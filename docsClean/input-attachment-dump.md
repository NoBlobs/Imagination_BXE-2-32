# Register dump: input-attachment render (answers Q16m item 1, O)

Black-box register values of the open v1.1 image during
`dEQP-VK.renderpasses.renderpass1.suballocation.formats.d32_sfloat.input.clear.dont_care.draw`
(passes). The data was read from the host, with no writes, in eight runs of the same test.
Each run sampled 63 consecutive registers plus `ROGUE_CR_ISP_RENDER` (`0x0F08`), every
5–15 µs.

The **render window** runs from `0x0F08` 3 → 0 to `0x0F08` 0 → 3. It covers **both**
fragment renders of the render pass, because `0x0F08` stays 0 between them. The values
listed for that window are in order of appearance: where a register takes two values, the
first belongs to the first render (depth) and the last to the second render (input
attachment). For the second render alone, see the table in
[kick-compute-and-3d](kick-compute-and-3d.md) ("Input-attachment renders").

Only registers that are non-zero at some point are listed. `0x0800–0x08FF` is not
included. Ranges: `0x0600–0x07FC`, `0x0F00–0x0FF8`, `0x1500–0x17F0`, `0x4000–0x40F8`.

| Register | Before | During the render window | After |
|---|---|---|---|
| `0x0600` | `403F3F00` | `403F3F00` | `403F3F00` |
| `0x0604` | `800000` | `800000` | `800000` |
| `0x0614` | `DA` | `DA` | `DA` |
| `0x0618` | `2C60` | `2C60` → `80` | `2CE0` |
| `0x0620` | `2C40` | `2C40` → `2A80` → `2BC0` | `2CC0` |
| `0x0628` | `202` | `202` → `201` | `202` |
| `0x0640` | `9` | `9` | `9` |
| `0x0658` | `B` | `B` | `B` |
| `0x0698` | `B` | `B` | `B` |
| `0x06A0` | `0` | `0` → `41000` | `41000` |
| `0x06A4` | `0` | `0` → `1E40` | `1E40` |
| `0x06AC` | `0` | `0` → `2C80` | `2C80` |
| `0x06B8` | `0` | `0` → `10001` | `10001` |
| `0x06BC` | `0` | `0` → `1000000` | `1000000` |
| `0x06C0` | `D` | `D` | `D` |
| `0x06D0` | `7F` | `7F` | `7F` |
| `0x06E8` | `7` | `7` → `6` → `7` → `6` → `7` | `7` |
| `0x0700` | `1` | `1` | `1` |
| `0x0708` | `1` | `1` | `1` |
| `0x0720` | `F` | `F` | `F` |
| `0x0F18` | `40004` | `20002` | `40004` |
| `0x0F20` | `D4080` | `D4000` → `D4080` | `D4080` |
| `0x0F24` | `80` | `80` | `80` |
| `0x0F28` | `10` | `10` | `10` |
| `0x0F38` | `21000` | `21000` → `80031000` → `80011000` | `21000` |
| `0x0F48` | `0` | `0` → `180A201C` → `0` | `0` |
| `0x0F50` | `0` | `0` → `B7000` → `0` | `0` |
| `0x0F54` | `0` | `0` → `80` → `0` | `0` |
| `0x0F58` | `0` | `0` → `B7000` → `0` | `0` |
| `0x0F5C` | `0` | `0` → `80` → `0` | `0` |
| `0x0F80` | `3F800000` | `0` → `3F800000` | `3F800000` |
| `0x0F88` | `0` | `0` → `1FF` → `200` | `200` |
| `0x0FA0` | `88` | `88` | `88` |
| `0x0FA8` | `88` | `88` | `88` |
| `0x0FB0` | `5B480` | `532C0` → `5B480` | `5B480` |
| `0x0FB4` | `80` | `80` | `80` |
| `0x0FD8` | `2` | `2` | `2` |
| `0x1500` | `80` | `80` | `80` |
| `0x1508` | `FFFF` | `FFFF` | `FFFF` |
| `0x1510` | `68800000` | `68800000` → `0` → `68800000` → `96800000` | `96800000` |
| `0x1514` | `200007D` | `200007D` → `0` → `200007D` → `200007C` | `200007C` |
| `0x1550` | `6000003F` | `6000003F` → `0` → `6000003F` → `3F` | `3F` |
| `0x1554` | `6000003F` | `6000003F` → `0` → `6000003F` → `3F` | `3F` |
| `0x1738` | `AB40` | `AB40` | `AB40` |
| `0x1744` | `80` | `80` → `180` → `80` → `180` | `180` |
| `0x1780` | `21` | `21` | `21` |
| `0x1788` | `20` | `20` | `20` |
| `0x178C` | `20` | `20` | `20` |
| `0x1790` | `1C400` | `1C400` | `1C400` |
| `0x1794` | `20` | `20` | `20` |
| `0x1798` | `7F` | `7F` → `7E` → `7F` | `7E` |
| `0x17A0` | `1C400` | `1C400` | `1C400` |
| `0x17A4` | `20` | `20` | `20` |
| `0x17B8` | `A` | `A` | `A` |
| `0x17C0` | `2` | `2` | `2` |
| `0x400C` | `E0` | `E0` | `E0` |
| `0x4014` | `E0` | `E0` | `E0` |
| `0x402C` | `E0` | `E0` | `E0` |
| `0x4050` | `2000` | `2000` | `2000` |
| `0x4058` | `1` | `1` | `1` |
| `0x4070` | `7F8` | `7F8` | `7F8` |
| `0x40D8` | `1402A` | `1402A` → `14071` → `4089` → `140AC` | `40D5` |
| `0x40E0` | `1005A` | `1005A` → `5E` → `1005E` | `1005E` |
| `0x40E8` | `10000` | `10000` | `10000` |
| `0x40F0` | `10000` | `10000` | `10000` |
| `0x40F8` | `10000` | `10000` | `10000` |

`0x40D8` and `0x40E0` are counters that change continuously; their values show only
that they run.
