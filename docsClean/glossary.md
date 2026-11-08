# Glossary

| Term | Meaning |
|---|---|
| **APM** | *Active power management*: power the GPU off when idle and restart it (warm boot) when work arrives. See [power-management](power-management.md). |
| **BCAST[off]** | broadcast register write through the XPU broadcast bank (`0xCFA00000`). |
| **BIF** | the GPU bus interface / MMU; maps device addresses through page catalogues (`ROGUE_CR_BIF_CAT_BASE*`). |
| **BVNC** | identification of the Rogue core: Branch.Version.Number-of-scalable-units.Config (`36.29.52.182`). |
| **cCCB / Client CCB** | per-context command ring (render/compute work). See [client-ccb](client-ccb.md). |
| **CCB** | circular command buffer: a command ring in shared memory. |
| **CDM** | compute data master: the compute unit (DM 4). |
| **context store / CSW** | saving a context's state for preemption (context switch). |
| **CR[off]** | GPU control register at offset `off` in the primary core's bank. |
| **DDK** | the vendor's driver development kit, base of the closed firmware/driver. These documents do not use its internal names. |
| **DM** | data master: a GPU work unit with its own queue (geometry, 3D, compute, transfer); indices are `PVR_FWIF_DM_*`. |
| **DVFS** | dynamic voltage and frequency scaling. |
| **FWCCB / Firmware CCB** | notification ring from the firmware to the host. See [firmware-ccb](firmware-ccb.md). |
| **FW common context** | `rogue_fwif_fwcommoncontext`: the firmware's state for one work stream (one cCCB, one DM). |
| **FW memory context** | `rogue_fwif_fwmemcontext`: a GPU address space (one page catalogue). |
| **HCS** | hard context switch: a watchdog that forces the switch if a context store does not complete. |
| **HWPerf** | performance-counter subsystem (binary packets). Optional with mainline. See [hwperf-trace](hwperf-trace.md). |
| **HWR** | hardware recovery: detect and recover a hung GPU. See [hardware-recovery](hardware-recovery.md). |
| **HWRTData** | `rogue_fwif_hwrtdata`: hardware render target data, the state of a render target shared by geometry and fragment. |
| **KCCB / Kernel CCB** | command ring from the host to the firmware. See [kernel-ccb](kernel-ccb.md). |
| **LRU** | least recently used: one possible (not mandatory) policy for reusing BIF slots. |
| **MTS** | the GPU's micro task scheduler hardware, which delivers BG/IRQ tasks to the MIPS as interrupts (`ROGUE_CR_MTS_*`). |
| **NMI** | non-maskable interrupt; on MIPS it enters through the reset vector. |
| **OSID** | OS/guest identifier for virtualisation; each one has its own VA window. |
| **PB** | parameter buffer: geometry memory, allocated from a freelist. |
| **PM** | parameter management: the hardware that allocates PB pages; signals `PM_OUT_OF_MEMORY` (`ROGUE_CR_EVENT_STATUS` bit 7). |
| **PR** | partial render (`ROGUE_FWIF_CCB_CMD_TYPE_FRAG_PR`). |
| **rogue_fwif_\*** | the binary host↔firmware interface in shared memory (public headers). See [host-interface](host-interface.md). |
| **ROGUE_CR_\*** | GPU control registers as named in `pvr_rogue_cr_defs.h`. See [registers](registers.md). |
| **run list / wait list** | per-DM lists of ready / fence-blocked contexts. See [scheduler](scheduler.md). |
| **SFID** | trace entry ID word (group, id, parameter count). See [trace-ids](trace-ids.md). |
| **SLC** | system level cache of the GPU. |
| **SPM** | the firmware's out-of-memory / partial-render handling (trace group `SPM`): grow the freelist or run a partial render. See [partial-render-spm](partial-render-spm.md). |
| **TA** | tiling accelerator: the geometry unit (DM 2/6). "Kick TA" = geometry kick. |
| **TQ** | transfer queue: blit/resolve operations (through 3D, TDM or 2D). |
| **UFO** | a sync object `{address, value}` for fences and updates (`rogue_fwif_ufo`). See [client-ccb](client-ccb.md). |
| **USC** | unified shading cluster: where shaders run; target of the shader breakpoints. |
| **XPU broadcast** | a write that reaches every enabled core (multicore) through `0xCFA00000`. |
