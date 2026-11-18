# Tigo RS485 Monitor add-on

Puts the dashboard of the [Tigo RS485 Monitor](https://github.com/Bobsilvio/tigo_server) (ESPHome variant) in the
Home Assistant sidebar, with the energy history of each panel taken from what Home Assistant already records.

- **Sidebar panel** through Home Assistant ingress: it works remotely, over HTTPS and in the companion app, with no
  port opened on the network.
- **Same dashboard as the ESP32**: live array map, insights, table with CSV export. Panels show the names you gave
  them in Home Assistant.
- **Long history**: the Analysis tab adds 7 days, 30 days and 12 months views: daily and monthly energy stacked by
  panel, energy ranking against the median, and a heatmap of each panel's yield relative to the best one (shading
  that comes with the seasons, a panel that degrades or gets dirty).

## Requirements

An ESP32 with the ESPHome variant (`tigo_rs485` component), `web_server:` in its YAML and the `power` sensor of each
panel (created by default). The add-on reads the live data from the device (`/tigo/live`, `/tigo/history`).

## Configuration

| Option | Description |
|---|---|
| `host` | IP address of the ESP32, e.g. `192.168.1.50`. A `.local` name may not resolve inside the add-on: prefer the IP (set a DHCP reservation for the ESP32). |
| `port` | port of the ESPHome `web_server`, `80` by default |
| `username` / `password` | only if `web_server` has `auth:` |

Start the add-on and open **Tigo** in the sidebar.

## Where the history comes from

The add-on stores nothing. The power sensors of the panels are measurements, so the Home Assistant recorder keeps
their hourly mean forever in its long-term statistics, the same data behind the statistics graphs. The add-on reads
them through the Home Assistant API: the energy of an hour is the mean power of that hour times one hour.

- The history starts when the panel sensors first appeared in Home Assistant, not when the add-on was installed.
- It is part of the Home Assistant database and of its backups; there is nothing to keep or to clean up.
- The ESP32 tells the add-on the name of each panel's power entity; the add-on finds it in the entity registry.
  Renaming the entity in Home Assistant keeps its history.
- The history belongs to the entity. With automatic slots, after **Reset panel assignments** a slot may get a
  different optimizer and its history continues from the previous one.
- Sensors excluded from the recorder, or a panel without the `power` sensor, have no history.

## Data for other programs

Through the ingress URL the add-on answers `tigo/live`, `tigo/history`, `tigo/info` and `tigo/range?span=7d|30d|12m`
(Wh per panel per hour, day or month). Only the Home Assistant ingress gateway can reach it.
