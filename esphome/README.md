# Tigo RS485 Monitor – ESPHome (Home Assistant)

**English** · [Italiano](README.it.md)

**ESPHome** variant of [Tigo RS485 Monitor](../README.md): an ESPHome external component (`tigo_rs485`) that listens to
the RS-485 bus between the TAPs and the CCA and publishes a set of sensors for each optimizer natively in Home
Assistant. No custom integration, no WebSocket: Wi-Fi, OTA and logs are ESPHome's standard ones.

For a standalone web dashboard without Home Assistant, use the [Arduino](../arduino/README.md) variant.

---

## What it does

- Listens to the bus **receive-only** (38400 baud 8N1): it never transmits, and the CCA keeps working as before.
- For each configured panel it publishes: **power**, input and output voltage, **current**, **temperature**,
  RSSI and duty cycle.
- Decoding matches the CCA firmware (4.0.8), more accurate than taptap:
  - power = Vin × Iin, as the CCA computes it;
  - current scale by the module hardware ID (read from the `Version` answers):
    ≤ 0x23 → 0.0025 A, 0x24–0x5F → 0.005 A, > 0x5F → 0.025 A; 0.005 A until known;
  - temperature published only when bit 0x20 of the status word marks it as such;
  - RSSI 0 ("no usable value") discarded.

  Details: [docs/PROTOCOL-NOTES.md](../docs/PROTOCOL-NOTES.md).
- Identifies panels by their **long address** (64 bits, fixed in hardware), not by the short ID the CCA can
  reassign. The short ID → long address mapping and the hardware ID show up on the bus rarely: they are saved to
  flash and survive reboots.
- A panel that stops reporting for `offline_timeout` (5 minutes by default) becomes "unavailable" in Home Assistant.
- A diagnostic text sensor lists the panels that report but are not configured yet, with their long address ready
  to copy.

---

## Hardware

<img src="../images/wiring.svg" alt="Wiring diagram" width="900"/>

| Component | Notes |
|---|---|
| **ESP32** | any module supported by ESPHome |
| **TTL→RS485 converter** | MAX485 or similar, receive-only |
| **5 V regulator** | when powered from the bus |

| RS-485 module | ESP32 / bus | Notes |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| TXD (or RO) | `rx_pin` (e.g. GPIO 16) | data from the bus to the ESP32 |
| RXD (or DI) | not connected | the ESP32 never transmits |
| RE/DE (if present) | GND | receive-only; auto-direction modules don't have them |
| A (D+) / B (D−) | A / B of the TAP ↔ CCA bus | in parallel |

Avoid the USB serial pins (GPIO 1/3 on the ESP32): the ESPHome logger uses them.

---

## Installation

1. Copy [`example.yaml`](example.yaml) into your ESPHome configuration and set Wi-Fi and `rx_pin`.
2. The component is downloaded from GitHub:
   ```yaml
   external_components:
     - source: github://Bobsilvio/tigo_server@main
       components: [tigo_rs485]
   ```
3. Configure the UART and the component:
   ```yaml
   uart:
     id: tigo_bus
     rx_pin: GPIO16
     baud_rate: 38400
     rx_buffer_size: 2048

   tigo_rs485:
     uart_id: tigo_bus
     offline_timeout: 5min   # optional
   ```
4. Flash once with only the unconfigured panels sensor:
   ```yaml
   text_sensor:
     - platform: tigo_rs485
       unconfigured_panels:
         name: Unconfigured panels
   ```
   Once the sun is up, the long addresses appear there and in the logs (`Unconfigured panel reporting: 04C05B4000B1A688`).
5. Add one block per panel:
   ```yaml
   sensor:
     - platform: tigo_rs485
       long_address: "04C05B4000B1A688"
       power:
         name: A1 Power
       voltage_in:
         name: A1 Voltage In
       voltage_out:
         name: A1 Voltage Out
       current:
         name: A1 Current
       temperature:
         name: A1 Temperature
       rssi:
         name: A1 RSSI
       duty_cycle:
         name: A1 Duty Cycle
   ```
   All sensors are optional.

### Long address not known yet

A module's long address is learned when the CCA reads the node table again, which happens rarely (usually at
discovery). Until then the panel shows up as `node 001A` among the unconfigured ones; you can use it temporarily by
its short ID, knowing that the CCA may reassign it:

```yaml
  - platform: tigo_rs485
    node_id: 0x001A
    power:
      name: A3 Power
```

### Energy

Energy is obtained by integrating power on the device with the standard `integration` platform
(see `example.yaml`), usable in the Home Assistant Energy dashboard.

---

## Configuration reference

**`tigo_rs485`**

| Option | Default | Description |
|---|---|---|
| `uart_id` | — | UART connected to the bus (38400 baud) |
| `offline_timeout` | `5min` | how long a silent panel takes to become unavailable |

**`sensor` / `platform: tigo_rs485`** — exactly one of `long_address` (16 hex digits) and `node_id`,
plus one or more of `power` (W), `voltage_in` (V), `voltage_out` (V), `current` (A), `temperature` (°C),
`rssi` (raw 0–255 value, not dBm), `duty_cycle` (%).

**`text_sensor` / `platform: tigo_rs485`** — `unconfigured_panels`: panels that report but are not configured.

---

## Development

The protocol decoder (`components/tigo_rs485/tigo_protocol.*`) does not depend on ESPHome and is tested on the host:

```bash
c++ -std=c++17 -I esphome/components/tigo_rs485 esphome/tests/host_test.cpp esphome/components/tigo_rs485/tigo_protocol.cpp -o host_test
```

```bash
./host_test issue_20_initial.bin
```

Without arguments it runs the synthetic tests; with a bus capture (for example
[taptap](https://github.com/willglynn/taptap)'s `src/test_data/issue_20_initial.bin`) it prints every power report.
