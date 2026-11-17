# Tigo bus protocol: what the CCA firmware really does

Notes on how the Tigo CCA decodes and builds the RS-485 gateway traffic, and where
[willglynn/taptap](https://github.com/willglynn/taptap) — the protocol reference this
firmware is based on — is wrong or incomplete.

## Method and sources

taptap's `docs/protocol.md` was written from passive observation of the RS-485 bus only
("based on the author's observations"). Fields it could not explain are marked `???`.

These notes come from the CCA's own code instead: firmware **4.0.8-ct**, binary
`mgmtu/bin/meshdcd` (ARM Thumb-2, stripped), disassembled with `llvm-objdump` and radare2.
Field names below are the ones the CCA itself uses in its log and dump formats.

| What | Where in `meshdcd` 4.0.8 |
|---|---|
| PV packet type → name table | function at `0x2a048` |
| Power report: wire → raw struct (`CReportDataRaw`) | `0x18350` |
| Power report: raw → units (`CDataScaled`) | `0x195cc` |
| Hardware ID parsed from the Version string | `0x3fb54` |
| Hardware family selection (`CNodeVersion::DetermineHwScaling`) | `0x3fbf4` |
| `CCA::TS4Utils::IsUnitHigherRated` | `libmgmtu-0.0.2.so`, `0x1d2f8` |
| Topology report parser (`CReportNetDiag`) | `0x1bda8` (log format at `0x1b7c8`) |
| Gateway config response, common block | `0x47b56` |
| DiscState Write builder | `0x58314` |
| DiscState Resp parser | around `0x59f70` |
| Gateway broadcast commands (Herding, Baud, Get MAC, Reset) | `0x20fdc`, `0x21090`, `0x21144`, `0x211e0` |

Cross-checks against the real capture shipped with taptap (`src/test_data/issue_20_initial.bin`):
all 716 power reports decode as described below, and the three DiscState responses in it
match the layout given here.

Tooling caveat: radare2 labels the ARM PLT one entry off and shows Thumb literal-pool
addresses 4 bytes late. Use `llvm-objdump --triple=thumbv7` and `llvm-objdump -R`
(relocations) to resolve calls and constants.

## PV packet types: official names

From the CCA's own name table. "taptap" is taptap's name where it differs or is missing.

| Type | CCA name | taptap |
|---|---|---|
| `06` / `07` | EncapString Req / Resp | String request / response |
| `09` | Report NetDiag | Topology report |
| `0D` / `0E` | GatewayCfg Req / Resp | Gateway radio configuration |
| `0F` | Discovery Solicit | — |
| `10` / `15` | DiscCfg Req / Resp | — |
| `11` | Discovery Enable | — |
| `13` | PeriodicCfg Write | "PV configuration request" |
| `14` | Report Combined | — |
| `17` / `18` | PeriodicCfg Req / Resp | `18` = "PV configuration response" (paired with `13`) |
| `1D` / `1E` | ResetNode Req / Resp | — |
| `22` / `23` | **SamplingRate Req / Resp** | "Broadcast" / "Broadcast ack" |
| `24` | Discovery Alien | — |
| `26` / `27` | GetTable Req / Resp | Node table request / response |
| `29` | SetTable Req | — |
| `2C` | EraseFlash Resp | — |
| `2D` | **DiscState Write** | "Long network status request" |
| `2E` / `2F` | DiscState Req / Resp | Network status request / response |
| `30` | Report NetDebug | — |
| `31` | Report Power | Power report |
| `40` | StarDisc Found | — |
| `41` | **StarDisc SetPgw** | "Unknown 0x41" |
| `42`–`44` | StarDisc Own / Ignore / Kick | — |
| `45` / `46` | Firmware Request / Delivery | — |
| `47` | StarDisc Status | — |
| `50` | Fetch ChunkMap | — |
| `E1` / `E2` | SetKnownNetworks Req / Resp | — |
| `E3`–`E6` | Get / Erase StackTrace Req / Resp | — |

Notes on some of these types:

- **`40`–`47` StarDisc** belong to "star discovery" (`cstardiscovery.cpp`). A TS4 can run in
  legacy *star* mode or in *mesh* mode; a mesh node can act as **PGW** for nearby star
  nodes. The CCA picks the next message per node: `41` SetPgw (two variants, with a flag
  0 or 1), `42` Own, `43` Ignore, `44` Kick a star from a PGW. That fits taptap's observation
  that `41` appears when modules come online at sunrise and go offline at sunset. What
  "PGW" stands for is not spelled out in the binary, and the `41` payload is not decoded yet.
- **`14` Report Combined** has a name but no parser in `meshdcd` 4.0.8: the CCA does not use it.
  The periodic reports it processes are `09`, `30` and `31` only.
- **`30` Report NetDebug** is a radio debug report (FrameCt, JoinReq/JoinRsp, SyncLoss,
  LowPower, RapidShut, reselection and TX counters…). Its field layout is not fixed: the CCA
  reads it from `/mnt/ffs/mgmtu/conf/netdebug.ini` and falls back to a built-in map.

## Power report (`31`)

Offsets are payload bytes (after the 7-byte PV packet header). Multi-byte fields are big endian.

| Byte | Field (CCA name) | Conversion |
|---|---|---|
| 0–1.5 | Vin, 12 bit | × 0.05 V |
| 1.5–2 | Vout, 12 bit | × 0.1 V |
| 3 | Pwm (DC-DC duty cycle) | 0–255 |
| 4–5.5 | Iin, 12 bit | **depends on hardware ID**, see below |
| 5.5–6 | Temperature **or** Details, 12 bit | if temperature: two's complement × 0.1 °C |
| 7 | Jstatus | raw byte |
| 8–9 | Status | bit `0x20` = byte 5.5–6 holds a temperature |
| 10–11 | Slot counter | — |
| 12 | Rssi | raw 0–255; **0 = no usable value**, the CCA discards it |
| 13 | bits 0–1 = slot counter bits 16–17 | used only when a global mode flag is set (probably the extended slot clock — inference) |
| 14 | PLC RSSI | raw byte |

Accepted lengths: the CCA requires at least 12 bytes (a 12-byte report has no RSSI).
Firmware 4.0.8 counts reports longer than 14 bytes as "surplus" but still decodes them.

Power, as the CCA computes it: **Pin = Vin × Iin**.

Apart from bit `0x20`, `meshdcd` only logs Jstatus and Status: it never tests other bits,
so their meaning cannot be recovered from it.

### Input current scale

The CCA reads a hardware ID from the module's answer to the `^00Version` string request,
e.g. `Mnode Version K8.0120 (2D)`: the hex number in parentheses (here `0x2D`).

| Hardware ID | Iin per unit |
|---|---|
| ≤ `0x23` | 0.0025 A |
| `0x24`–`0x5F` | 0.005 A |
| > `0x5F` ("higher rated" units) | raw × 5 × 0.005 = 0.025 A |
| unknown (no Version answer yet) | the CCA does **not** scale the data at all |

## Topology report (`09`, Report NetDiag)

| Byte | CCA name | taptap |
|---|---|---|
| 0–1 | Nwid | short address |
| 2–3 | *not read by the CCA* (it takes Tuid from elsewhere) | PV node ID |
| 4–5 | ParentNwid | next hop |
| 6–7 | Status | `unknown_1` |
| 8–15 | Mac (long address) | long address |
| 16 | ParentRssi (RSSI of the link to the parent) | rssi |
| 17 | Alignment | part of `unknown_2` |
| 18–19 | Slot counter of the measurement ("Measured at") | part of `unknown_2` |
| 20–23 | two 16-bit fields, present in longer reports, not logged | part of `unknown_2` |

Accepted lengths: 20 to 24 bytes.

## Gateway config response (`0E`, GatewayCfg Resp)

Exactly 37 bytes. Bytes 0–35 are a "common block"; byte 36 is extra.

| Byte | CCA name | taptap | taptap example |
|---|---|---|---|
| 0–1 | Channel | `???` + Radio channel | `00 15` = 21 |
| 2–3 | PAN | PAN ID | `24 F6` |
| 4–13 | Map (bytes 4–11 + byte 12 + byte 13, combined) | CAP? CFP? BOP? IAP? + `???` | `18 04 02 01 00 00 00 00 1C 01` |
| 14 | SltThresh | part of `???` | `5A` = 90 |
| 15–30 | Key ("SSL Encryption Security Key", 16 bytes) | Encryption key | — |
| 31–34 | Optimization (u32) | `???` | `00 00 02 00` |
| 35 | SltMin | `???` | `3C` = 60 |
| 36 | not in the common block | `???` | `00` |

taptap's superframe guesses (CAP/CFP/BOP/IAP) fall inside what the CCA treats as a single
"Map" field. The internal structure of Map is not decoded yet.

## Discovery state (`2D`, `2E`, `2F`)

**`2D` DiscState Write** (taptap: "long network status request") is a command, not a query.
It enables or disables discovery of new modules:

| Byte | CCA name | taptap example |
|---|---|---|
| 0–1 | fixed constant `0xBABE` | `BA BE` |
| 2 | Discovery: `02` = Enable, `00` = Disable | `02` |
| 3–4 | Limit Time | `03 84` = 900 |
| 5–6 | Count (node limit) | `00 87` = 135 |
| 7 | `01` = Whitelist, `00` = Promiscuous | `01` |
| 8 | extra byte (firmware 4.0.8 sends 9 bytes; taptap's capture shows 8) | — |

**`2F` DiscState Resp**:

| Byte | CCA name | taptap |
|---|---|---|
| 0 | state | `???` |
| 1–2 | Goal Time | "Counter?" |
| 3–4 | Nodes (goal) | "Node count?" |
| 5–6 | Count Discovered | "Node count?" |
| 7–8 | Count All | "Node count?" |
| 9 | optional extra byte | — |

In the real capture, Goal Time counts down between polls (256 → 236 → 216), so it is a timer,
not a counter.

## Node table response (`27`, GetTable Resp)

The CCA logs `%04X starting TUID, %u number of records`: the response starts with a 16-bit
start node ID, then the entry count. Upstream taptap omits the start field;
[litinoveweedle/taptap](https://github.com/litinoveweedle/taptap) already fixed this. This
firmware (`process27frame`) skips it correctly.

## Gateway frame types

Frame types the CCA builds. A gateway response uses the request type + 1.

| Type | Meaning | taptap |
|---|---|---|
| `000A` | Version request | same |
| `0010` | **Reset** (broadcast to `0000`, payload = constant `37 24 92 66`) | "Unknown 0010" |
| `0011` | **Reset response** (from the gateway) | "Unknown 0011" |
| `0012` / `0013` | **Store** (persist gateway configuration) | — |
| `0014` | Enumeration start, called "Herding" by the CCA (payload `37 24 92 66` + enumeration address `12 35`) | same |
| `0038` / `0039` | Get MAC at the enumeration address | identify |
| `003A` / `003B` | Get MAC / validation of an assigned ID | identify |
| `003C` / `003D` | Assign ID | same |
| `005A` | **Baud rate set** (broadcast) | — |
| `0148` / `0149` | Receive request / response | same |
| `0B00` / `0B01` | Ping | same |
| `0B0F` / `0B10` | Command request / response | same |

`37 24 92 66` is a fixed constant in the firmware (`0x37249266`), not data.

taptap contradicts itself on `0011`: it says the gateway answers `0010` with `003D`, then shows
`0011` coming *from* the gateway (address `8000`), then says the controller sends `0011` in
broadcast. The CCA never builds a `0011` frame, so `0011` is the gateway's answer to Reset.

## Summary: where taptap is wrong or incomplete

| # | Topic | taptap | Firmware |
|---|---|---|---|
| 1 | Power report Iin | always × 0.005 A | scale depends on hardware ID (0.0025 / 0.005 / 0.025) |
| 2 | Power report temperature | always decoded | only when Status bit `0x20` is set; otherwise "Details" |
| 3 | Power report `???` | 3 unknown bytes | Jstatus + Status |
| 4 | 15-byte power report | 2 unknown bytes | slot counter bits 16–17 + PLC RSSI |
| 5 | RSSI 0 | passed through | means "unusable" |
| 6 | Power | not computed (taptap-mqtt: Vout × I) | Pin = Vin × Iin |
| 7 | Packet `22`/`23` | Broadcast / ack | SamplingRate Req / Resp |
| 8 | Packet `2D` | long network status request | DiscState Write: discovery enable, time limit, node limit, whitelist |
| 9 | Packet `2F` | counter + 3 node counts | state, Goal Time (countdown), goal nodes, discovered, all |
| 10 | Packet `41` | unknown | StarDisc SetPgw (preferred gateway) |
| 11 | Packet `09` | 7 unknown bytes | Status, Alignment, measurement slot counter |
| 12 | Packet `0E` | CAP/CFP/BOP/IAP guesses, 7+6 unknown bytes | Map, SltThresh, Optimization, SltMin |
| 13 | Packet `27` | no start field (upstream) | start node ID + count |
| 14 | Gateway `0010`/`0011` | unknown, contradictory | Reset / Reset response |
| 15 | Gateway `0012`, `005A` | missing | Store, Baud rate set |
| 16 | Many PV packet types | missing | names listed above |

Confirmed correct in taptap: Vin × 0.05, Vout × 0.1, duty cycle byte, the field being the
**input** current, temperature sign extension (marked as unsure in its code), slot counter
and RSSI positions. The escaping scheme and frame CRC were checked on the capture only
(11 638 frames, no CRC error), not in the firmware.

## What this firmware does with it

- `watt = vin × amp`.
- Iin scale chosen from the hardware ID, read from passing Version answers, stored in
  `nodetable.json` (`hwid`) and sent on the WebSocket. Until it is seen, 0.005 A is assumed
  (unlike the CCA, which shows nothing).
- Temperature updated only when bit `0x20` is set; the last valid value is kept otherwise.
- RSSI 0 ignored.

## Still unknown

- Meaning of the Status and Jstatus bits (other than `0x20`) and of the Details value. The
  CCA does not interpret them, so another source is needed (Tigo cloud, module firmware).
- Payload of `41` StarDisc SetPgw.
- Internal structure of the gateway config "Map" field and of the Optimization bits.
- The global flag that enables the 18-bit slot counter.
- The built-in field map of `30` Report NetDebug.
- Payloads of the other packet types taptap never saw (`1D`/`1E`, firmware delivery…).
