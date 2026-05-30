# BCM2711 firmware PCIe gate — disassembly + MMIO confirmation

Investigation arc into why xHCI / USB-A never comes up on bare-metal
Pi 4 even after replaying the CPRMAN PCIe-clock sequence extracted from
`start4.elf` disassembly.

## TL;DR

`start4.elf` firmware contains a single 1-bit "PCIe present" gate at
**`0xFE0000B4` bit 0** (VC4-view `0x7E0000B4`).  On bare-metal Pi 4 boot
this bit is **0**, causing firmware to skip the entire CPRMAN→PCIe
init block.  The gate is writable from ARM via direct MMIO and the
write sticks — but flipping it post-boot is necessary, not sufficient:
firmware's PCIe init also touches VC4-internal state at `gp+0x3c0d0`
(bits 24, 25, 26) which is not addressable from ARM.

## Evidence chain

### 1. Disassembly trail (`references/start4.S`)

* `ec63f00` is the board-dispatch function with a 37-case switch on
  `version r4 - 0x4000160`.  Active cases (r4 ∈ {0,1,2,16,32,33,34,35,36})
  set a cascading 4-flag chain ending at `st r9,(gp+5476)` (the PCIe
  flag).  Other 28 cases jump to default = no flag set.
* `ec63fec..ec63ff6`: regardless of which case fires, firmware
  subsequently reads `*(0x7E000080 + 0x34) = *(0x7E0000B4)` and
  branches away (`beq 0xec64052`) if bit 0 is clear, skipping the
  remaining CPRMAN/PCIe init.

### 2. MMIO confirmation (recorded on Pi 4 8GB rev d03115)

```
/mmio-read?addr=0xFE0000B4 → val=0x0 bit0=0       (gate is OFF)
/mmio-sweep?addr=0xFE000080&n=16 → mostly 0/1 flags + "MULT" sentinel
/mmio-write?addr=0xFE0000B4&val=0x1 → before=0 after=1 [STUCK]   (writable!)
/mmio-read?addr=0xFEE01000 (post-gate-force) → val=0 (was wedge before)
```

### 3. Necessary-but-not-sufficient

After gate force + `/cprman-init` (write `0x5A000016` → `CPRMAN+0x128`):

```
EMMC2CTL [+0x1d0] = 0x296   (bit 7 BUSY=1 — clock running, baseline good)
PCIE?CTL [+0x128] = 0x016   (bit 7 BUSY=0 — clock NOT running)
```

The CTL register accepts the SRC=6/ENAB write but the gate hardware
doesn't activate.  The full firmware sequence (`ec63fa8` onwards)
touches `gp+0x3c0d0` bits 24, 25, 26 between the gate check and the
CPRMAN write — those are VC4-internal addresses not reachable from
ARM, blocking us from completing the init sequence.

## Memory map findings (Pi 4 8GB)

| Range | Content | Notes |
|-------|---------|-------|
| 0xFE000000-7C | all 0 | reserved / header |
| 0xFE000080-BF | mixed flags + 0x4D554C54 ("MULT") @ 0x8C + 0x0A060000 @ 0xBC | firmware feature-config region |
| **0xFE0000B4** | **0** ← PCIe gate | bit 0 = "PCIe present" |
| 0xFE0000C0-C8 | 0/1/1 | adjacent feature group |
| 0xFE0000CC+ | "MULT" sentinel | unused entries |
| 0xFE102390 (CPRMAN+0x1390) | 0x300021 | accepts password write (sets bit 21, clears bit 24) |

## New HTTP routes added this arc

| Route | Purpose |
|-------|---------|
| `GET /mmio-read?addr=0xN` | Direct MMIO read via setjmp-fault-catch |
| `POST /mmio-write?addr=0xN&val=0xV` | Direct MMIO write + read-back STUCK check |
| `GET /mmio-sweep?addr=0xN&step=4&n=16` | Range sweep, fault tolerant |
| `GET /pcie-fw-probe` | Mailbox proxy probe (proven safe addresses) |
| `GET /pcie-fw-probe1?addr=0xN` | Single-address mailbox probe |
| `POST /pcie-fw-gate-force` | Mailbox-attempt to set the gate (turned out direct MMIO works better) |

## Dead-end signals (so future sessions don't repeat)

* `GET_PERIPH_REG` mailbox tag (`0x00030045`) returns `value=0` with
  `tag-resp=0x80000002` (a 2-byte error response, not the 4-byte
  HANDLED) for CPRMAN addresses on our firmware.  Mailbox-proxied
  reads are NOT a viable channel for probing peripheral state on this
  firmware version.  `GET_FIRMWARE_REVISION` works (tag-resp=0x80000004
  with a real value), so the mailbox plumbing itself is fine.
* `/pcie-clk-full` (the 11-step extracted sequence) WEDGES Pi 4 — one
  of the writes hits a peripheral whose AXI slave is not responding,
  stalling ARM instruction fetch too.  Step-by-step bisection needed
  to identify which step is the wedger.
* `0xFEE01000` reads as 0 but writes don't stick (probably read-only
  status, not a power-domain control register).

## What would actually fix it

To complete PCIe init from ARM we would need:

1. Continue disassembly past `ec64052` to map every `gp+0x3c0d0` touch
   and translate to its physical equivalent (or find an ARM-visible
   mirror).
2. Identify which step of `/pcie-clk-full` wedges (split into per-step
   HTTP routes, bisect across multiple flash cycles).
3. Failing that, accept that Pi 4 bare-metal cannot enable PCIe
   without firmware cooperation, and switch USB-A strategy to either
   (a) network HID input (already done — `/click`), or (b) a userland
   Linux helper invoked over the same HTTP gateway.
