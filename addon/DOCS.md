# Tigo RS485 Monitor add-on

Puts the dashboard of the [Tigo RS485 Monitor](https://github.com/Bobsilvio/tigo_server) (ESPHome variant) in the
Home Assistant sidebar and keeps months of energy history per panel.

- **Sidebar panel** through Home Assistant ingress: it works remotely, over HTTPS and in the companion app, with no
  port opened on the network.
- **Same dashboard as the ESP32**: live array map, insights, table with CSV export.
- **Long history**: the add-on records the hourly energy of each panel in its own database, so the Analysis tab
  adds 7 days, 30 days and 12 months views: daily and monthly energy stacked by panel, energy ranking against the
  median, and a heatmap of each panel's yield relative to the best one (shading that comes with the seasons, a panel
  that degrades or gets dirty).

## Requirements

An ESP32 with the ESPHome variant (`tigo_rs485` component) and `web_server:` in its YAML. The add-on reads the same
data as the device's own page (`/tigo/live`, `/tigo/history`).

## Configuration

| Option | Description |
|---|---|
| `host` | IP address of the ESP32, e.g. `192.168.1.50`. A `.local` name may not resolve inside the add-on: prefer the IP (set a DHCP reservation for the ESP32). |
| `port` | port of the ESPHome `web_server`, `80` by default |
| `username` / `password` | only if `web_server` has `auth:` |
| `retention_days` | how many days of hourly energy to keep (default 730) |

Start the add-on and open **Tigo** in the sidebar.

## How the history is recorded

The ESP32 integrates each panel's power on every bus report and exposes the energy since boot. The add-on reads it
every 30 seconds and stores the increments hour by hour, so the result does not depend on the poll rate. A reboot of
the ESP32 or a reset of the slot assignments is recognised and does not lose energy.

The history starts when the add-on is installed. While the add-on is stopped nothing is recorded: after a gap longer
than two hours the energy produced meanwhile is not attributed to the hour of the restart. Panels are identified by
their long address, so renaming entities or moving slots does not split the history.

The database is in the add-on data folder and is included in Home Assistant backups.

## Data for other programs

Through the ingress URL the add-on answers `tigo/live`, `tigo/history`, `tigo/info` and `tigo/range?span=7d|30d|12m`
(Wh per panel per hour, day or month). Only the Home Assistant ingress gateway can reach it.
