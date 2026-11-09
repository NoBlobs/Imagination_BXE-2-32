# Observed sequences (behaviour reference)

This document collects firmware behaviour sequences. They serve the clean team as an
**acceptance reference**: the reimplementation must produce equivalent sequences. Quoted
text is the matching **public** trace format string (`pvr_rogue_fwif_sf.h`, identified by
group and id, see [trace-ids](trace-ids.md)).

**Provenance.** Unmarked sections: **O**, observed in the firmware trace on the board.
Sections marked **[D]** come from firmware analysis and isolated execution in emulation,
and were not observed on the board. Timeout values and iteration limits in the [D] sections
are implementation parameters, not requirements.

> Addresses `CR[0x....]` here are observed **hardware registers**. Those with a public name
> are in [registers](registers.md) (`ROGUE_CR_*`); those without a public name are
> identified by the observed offset.

## Host notification (write barrier + IRQ) [D]

Sequence to publish data to the host (see
[execution-environment](execution-environment.md)):

```
barrier word (uncached) = 1
EHB ; SYNC
read the barrier word
read ROGUE_CR_CORE_REVISION (0x0020)        // read that drains the writes on the bus
rogue_fwif_osdata.interrupt_count[0]++      // offset 0x230 (560) in osdata
ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS = 1        // IRQ to the host
ROGUE_CR_MTS_SCHEDULE = 0                   // when the BG task must be rescheduled
```

## Job cycle (e.g. 3D transfer, TQ)

```
"Irq Task (EVENT_STATUS=…)"            // completion signalled by the GPU
"3D Transfer finished"                 // MAIN id 4
"UFO Updates for FWCtx … @ …"          // signal the finished job to the host
"UFO Checks for FWCtx … @ …"           // evaluate the fences of the next one
"Ready-to-run …"
"Client command header DM = …, client CCB = …, cmd = …"
"Activate MemCtx=…" / "Setup register set=…"
"Kick 3D TQ: …"                        // next job started (MAIN id 121)
```

## Preemption (context store / restore)

Happens when a higher-priority context waits behind a lower-priority one on a DM (see
[scheduler](scheduler.md)). Observed on compute (DM 4) and fragment (DM 3):

**Compute (DM 4):**
```
"Higher priority context scheduled for DM 4 …"              // CSW id 18/34/38
"*** CDM FWCtx store start"                                  // CSW id 5
"Multicore context store on DM4 active core mask 0x0001"     // CSW id 49
... the higher-priority job runs and finishes ...
"Multicore context resume on DM4 active core mask 0x0001"    // CSW id 48
"CDM FWCtx shared alloc size load 0x1"                       // CSW id 3
"*** CDM FWCtx <ctx> resume from snapshot buffer <addr>, shader state <n>"  // CSW id 29
"Kick Compute: FWCtx <ctx> @ <off>, …"                       // MAIN id 122
```

**Fragment (DM 3):**
```
"Higher priority context scheduled for DM 3 …"
"*** 3D context store start"  ->  "*** 3D context store complete"   // CSW id 11, 9
kick 3D of the higher-priority context
"*** 3D FWCtx <ctx> resume"   ->  "Kick 3D: … CSW resume:1"          // CSW id 8, MAIN id 120
```

Observed facts:
- **Geometry (DM 2) is not preempted**: a higher-priority request waits for the TA to
  finish.
- With the open firmware and the mainline driver, preemption only happens if the OS config
  flags enable CSW on the DM (`ROGUE_FWIF_INICFG_OS_CTXSWITCH_*_EN`; they default to `0`,
  so no CSW). See [data-structures](data-structures.md) (config flags).
- The context store saves into a *snapshot buffer*; the resume reloads the `shader state`
  and sets `CSW resume:1` in the next kick.

## PB out of memory and freelist grow (SPM)

Full sequence (see [partial-render-spm](partial-render-spm.md)):

```
EVENT_STATUS bit 7 (PM out of memory)
"OOM TA_cmd=<cmd>, U-FL <fl>, N-FL <fl>"                         // SPM id 7
"OOM TA/3D PR Check: [<ufo>] is <v> requires <v>"                // SPM id 66
"SPM State = wait for grow"                                      // SPM id 34
FW -> host  FWCCB ROGUE_FWIF_FWCCB_CMD_FREELIST_GROW (freelist id, …)
host allocates pages
host -> FW  KCCB ROGUE_FWIF_KCCB_CMD_FREELIST_GROW_UPDATE
"Received grow update, FL addr: <fl>, new pages: 32, ready pages: 4"   // SPM id 54
"PM pause TA ALLOC: PM_PAGE_MANAGEOP set to 0x1"                 // PM id 11 (CR 0x02A0)
"Freelist update completed [<fl>]: old total pages …, new total pages …"  // PM id 7
"PM unpause TA ALLOC: PM_PAGE_MANAGEOP set to 0x0"               // PM id 12
"OOM: Resumed TA with ready pages, FL addr: <fl>, current pages: <n>, SP : <n>"  // SPM id 44
"SPM State = PR avoided"
```

- OOM with a grow already pending: `"Wait for pending grow on Freelist <fl>"` (SPM id 41).
- No grow possible → **partial render**: `EVENT_STATUS 0x82` → "PR blocked" → PR checks
  (`>=`) → "wait for HW" → 3D kick with `Partial render:1` → "PR running" → "PR executed" →
  TA restart.

## Power

**Per job, without powering off (HW on/off request for the dusts):**
```
"Changing number of dusts from 0 to 1"                       // POW id 9
"HW Request On(1)/Off(0): 1, Units: 0x…603" -> "HW Request Completed(1)/Aborted(0): 1, …"
... job ...
"Changing number of dusts from 1 to 0"
"HW Request On(1)/Off(0): 0, Units: 0x…602" -> "HW Request Completed(1)/Aborted(0): 1, …"
"Idle timer start. …" -> 100 ms -> "GPU idle (might be powered down). …"   // POW id 21, 2
```

**Forced idle + power off + warm boot (mainline runtime PM):**
```
"OS requested forced IDLE, pow flags: 0x0"                               // POW id 19
"Check Pow state DM… int: 0x6, ext: 0x3, pow flags: 0x40000"             // POW id 1
"OS requested pow off (forced = 1), pow flags: 0x80000"                  // POW id 59
"Check Pow state DM… int: 0x6, ext: 0x2, pow flags: 0xa0000"
"GPU ready to be powered down. Pow state int: 0x6, ext: 0x2, flags: 0xa0000"   // POW id 6
(power domain turned off by the host)
... next job: domain powered on, firmware re-initialised ...
"Active PM latency set to 0ms. Core clock: 307200000 Hz"                 // POW id 23, warm boot
```

**Observed pow flags** (internal encoding, see [power-management](power-management.md)):
`0x10000` power-off request pending, `0x40000` forced idle, `0x80000` forced request,
`0xA0000 = 0x80000 | 0x20000`. A "cancel power off" happens if there was recent work:
`"Cancel power off request int: …, ext: …, pow flags: …"` (POW id 18).

## HWR (recovery)

```
MMU fault / failed poll → lockup
every DM "DM… ready for HWR"                       // HWR id 32
"Analysis DM…: Lockup FWCtx … Need to skip to next command"   // HWR id 28
"Begin hardware reset" ; "HWR: Reset HW" ; "GPU Units init" ; "Finished hardware reset"
recovery of each DM; the culprit skips the command  // HWR id 14, 24, 25
rescheduling
```
From lockup to recovery: about 0.43 ms observed. See
[hardware-recovery](hardware-recovery.md).

## USC event (`EVENT_STATUS` bit 15) [D]

```
ROGUE_CR_EVENT_CLEAR = 0x8000              // "GP USC triggered" (MAIN id 31)
rogue_fwif_osdata.interrupt_count[0]++
write barrier ; ROGUE_CR_MIPS_WRAPPER_IRQ_STATUS = 1
```
If `EVENT_STATUS` bit 12 (GPIO) is also set and no power request is pending, the GPIO is
handled first.

## GPIO handshake (`EVENT_STATUS` bits 12/13) [D]

Registers without a public name: `0x0150` = input value (8 bits), `0x0158` = ack output,
`0x0B90` = GPIO event re-arm/mask. Every wait has a timeout.

```
EVENT_STATUS bit 12 (GPIO request)
v = CR[0x0150]                             // "GPIO read 0x<v>" (MISC id 2)
EVENT_CLEAR = 0x1000 ; wait for bit 12 = 0
CR[0x0158] = 1                             // ack
wait for bit 12 = 1 ; EVENT_CLEAR = 0x1000 ; wait for bit 12 = 0
BCAST[0x0B90] = 0x1000                     // re-arm ; store v ; state = 1
on any timeout: EVENT_CLEAR = 0x3000 ; BCAST[0x0B90] = 0x1000 ;
                ROGUE_CR_MTS_DM0_INTERRUPT_ENABLE bit 12 = 0 ; "GPIO disabled" (MISC id 4)
```

## PM 3D timeout [D]

Primary-core registers without a public name:
```
disable: s = CR[0x03E0] ; CR[0x03E0] = 0
         CR[0x0388] = 1 ; CR[0x0298] = 0 ; CR[0x01C8] = 0
         "3D Timeout Now for FWCtx <ctx>"          // PM id 28
         EVENT_CLEAR = 0x100 ; CR[0x0380] = 1
restore: poll EVENT_STATUS bit 8 (PM 3D memory free), with a timeout
         EVENT_CLEAR = 0x100 if ok ; CR[0x03E0] = s
```

## Reference latencies (performance sanity check)

Observed medians (transfer job), useful as an order of magnitude. The vendor stack is
measured only as a black box (**O**):

| Stack | IRQ → completion | completion → next kick | kick → completion (GPU) |
|---|---|---|---|
| vendor stack, 614 MHz | 10.8 µs | 53.8 µs | 876 µs |
| mainline (open), 307 MHz | 14.2 µs | 180.8 µs | 2225 µs |
| mainline (open), 614 MHz | 8.8 µs | 118.8 µs | 1044 µs |
