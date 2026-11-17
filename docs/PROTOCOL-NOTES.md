# Power report: what the CCA firmware really does

Notes on how the Tigo CCA decodes TS4 power reports (PV packet type `0x31`), and where
[willglynn/taptap](https://github.com/willglynn/taptap) — the protocol reference this
firmware is based on — is wrong or incomplete.

## Method and sources

taptap's `docs/protocol.md` was written from passive observation of the RS-485 bus only
("based on the author's observations"). Fields it could not explain are marked `???`.

These notes come from the CCA's own decoder instead: firmware **4.0.8-ct**, binary
`mgmtu/bin/meshdcd` (ARM Thumb-2, stripped), disassembled with `llvm-objdump` and radare2.

| What | Where in `meshdcd` 4.0.8 |
|---|---|
| Wire payload → raw struct (`CReportDataRaw`) | function at `0x18350` |
| Raw → engineering units (`CDataScaled`) | function at `0x195cc` |
| Hardware ID parsed from the Version string | function at `0x3fb54` |
| Hardware family selection (`CNodeVersion::DetermineHwScaling`) | function at `0x3fbf4` |
| `CCA::TS4Utils::IsUnitHigherRated` | `libmgmtu-0.0.2.so`, `0x1d2f8` |

Results were cross-checked against the real capture shipped with taptap
(`src/test_data/issue_20_initial.bin`, 716 power reports): the decoding below matches all of
them, and every report in that capture has the temperature flag set.

Tooling caveat: radare2 labels the ARM PLT one entry off and shows Thumb literal-pool
addresses 4 bytes late. Use `llvm-objdump --triple=thumbv7` and `llvm-objdump -R`
(relocations) to resolve calls and constants.

## Payload layout

Offsets are payload bytes (after the 7-byte PV packet header). Multi-byte fields are big endian.

| Byte | Field | Conversion |
|---|---|---|
| 0–1.5 | Vin, 12 bit | × 0.05 V |
| 1.5–2 | Vout, 12 bit | × 0.1 V |
| 3 | PWM / DC-DC duty cycle | 0–255 |
| 4–5.5 | Iin, 12 bit | **depends on hardware ID**, see below |
| 5.5–6 | Temperature **or** "Details", 12 bit | if temperature: two's complement × 0.1 °C |
| 7 | Status byte (the CCA logs a `Jstatus %02X`; mapping to this byte is likely but not proven) | — |
| 8–9 | Status word (`Status %04X`) | bit `0x20` = byte 5.5–6 holds a temperature |
| 10–11 | Slot counter | — |
| 12 | RSSI | raw 0–255; **0 = no usable value**, the CCA discards it |
| 13 | Bits 0–1 = slot counter bits 16–17 | used only when a global mode flag is set (probably the extended slot clock — inference) |
| 14 | Extra byte, stored but meaning not identified | — |

Accepted lengths: the CCA requires at least 12 bytes (a 12-byte report has no RSSI).
Firmware 4.0.8 counts reports longer than 14 bytes as "surplus" but still decodes them.

Power, as the CCA computes it: **Pin = Vin × Iin**.

### Input current scale

The CCA reads a hardware ID from the module's answer to the `^00Version` string request,
e.g. `Mnode Version K8.0120 (2D)`: the hex number in parentheses (here `0x2D`).

| Hardware ID | Iin per unit |
|---|---|
| ≤ `0x23` | 0.0025 A |
| `0x24`–`0x5F` | 0.005 A |
| > `0x5F` ("higher rated" units) | raw × 5 × 0.005 = 0.025 A |
| unknown (no Version answer yet) | the CCA does **not** scale the data at all |

## Where taptap is wrong or incomplete

| # | taptap | Firmware | Impact |
|---|---|---|---|
| 1 | Iin always × 0.005 A | Scale depends on hardware ID (0.0025 / 0.005 / 0.025) | Current and power 2× too high or 5× too low on modules outside `0x24`–`0x5F`. The only Version answer in taptap's docs is `0x2D`, inside the range where 0.005 is right. |
| 2 | Temperature always decoded | Only a temperature when Status bit `0x20` is set; otherwise it is a "Details" value | Garbage temperatures whenever the bit is clear |
| 3 | `???` 3 bytes | Byte 7 = status byte, bytes 8–9 = status word | Status bits other than `0x20` still unnamed |
| 4 | 15-byte report: 2 bytes `unknown` | Byte 13 = high slot-counter bits, byte 14 = extra byte | Slot counter can be 18 bits wide |
| 5 | RSSI 0 passed through | 0 means "unusable" | Spurious 0 readings |
| 6 | Sign extension of temperature marked `XXX` (unsure) | Confirmed: two's complement | taptap is right |
| 7 | No power value | Pin = Vin × Iin | taptap-mqtt computes Vout × I, which under-reports buck-mode (shaded) panels; this firmware also did until this change |

Confirmed correct in taptap: Vin × 0.05, Vout × 0.1, duty cycle byte, the field being the
**input** current, slot counter and RSSI positions.

## What this firmware does with it

- `watt = vin × amp`.
- Iin scale chosen from the hardware ID, read from passing Version answers, stored in
  `nodetable.json` (`hwid`) and sent on the WebSocket. Until it is seen, 0.005 A is assumed
  (unlike the CCA, which shows nothing).
- Temperature updated only when bit `0x20` is set; the last valid value is kept otherwise.
- RSSI 0 ignored.

## Still unknown

- Meaning of the status word bits other than `0x20`, of the status byte and of the "Details" value.
- Byte 14 of the 15-byte report.
- The global flag that enables the slot-counter extension.
- Not looked at in the firmware yet: topology report (`0x09`) unknown fields, packet
  `0x41`, gateway radio configuration, network status, gateway frame types `0x0010`/`0x0011`.
