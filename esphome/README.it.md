# Tigo RS485 Monitor – ESPHome (Home Assistant)

[English](README.md) · **Italiano**

Variante **ESPHome** di [Tigo RS485 Monitor](../README.it.md): un componente esterno ESPHome (`tigo_rs485`) che ascolta
il bus RS-485 tra TAP e CCA e pubblica in Home Assistant, in modo nativo, un set di sensori per ogni ottimizzatore.
Niente integrazione custom, niente WebSocket: Wi-Fi, OTA e log sono quelli standard di ESPHome.

Per una dashboard web autonoma senza Home Assistant usa la variante [Arduino](../arduino/README.it.md).

---

## Cosa fa

- Ascolta il bus in **sola ricezione** (38400 baud 8N1): non trasmette mai, il CCA continua a funzionare come prima.
- Per ogni pannello configurato pubblica: **potenza**, tensione d'ingresso e d'uscita, **corrente**, **temperatura**,
  RSSI e duty cycle.
- Decodifica allineata al firmware del CCA (4.0.8), più accurata di taptap:
  - potenza = Vin × Iin, come la calcola il CCA;
  - scala della corrente secondo l'ID hardware del modulo (letto dalle risposte `Version`):
    ≤ 0x23 → 0,0025 A, 0x24–0x5F → 0,005 A, > 0x5F → 0,025 A; 0,005 A finché non è noto;
  - temperatura pubblicata solo quando il bit 0x20 della status word la marca come tale;
  - RSSI 0 ("valore non valido") scartato.

  Dettagli: [docs/PROTOCOL-NOTES.md](../docs/PROTOCOL-NOTES.md).
- Identifica i pannelli per **long address** (64 bit, fisso nell'hardware), non per l'ID corto che il CCA può
  riassegnare. L'abbinamento ID corto → long address e l'ID hardware arrivano sul bus di rado: vengono salvati in
  flash e sopravvivono ai riavvii.
- Un pannello che non trasmette per `offline_timeout` (predefinito 5 minuti) diventa "non disponibile" in Home Assistant.
- Un sensore di testo diagnostico elenca i pannelli che trasmettono ma non sono ancora configurati, con il loro long
  address pronto da copiare.

---

## Hardware

<img src="../images/wiring.it.svg" alt="Schema di collegamento" width="900"/>

| Componente | Note |
|---|---|
| **ESP32** | qualsiasi modulo supportato da ESPHome |
| **Convertitore TTL→RS485** | MAX485 o simile, in sola ricezione |
| **Regolatore 5 V** | se alimentato dal bus |

| Modulo RS-485 | ESP32 / bus | Note |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| TXD (o RO) | `rx_pin` (es. GPIO 16) | dati dal bus verso l'ESP32 |
| RXD (o DI) | non collegato | l'ESP32 non trasmette |
| RE/DE (se presenti) | GND | sola ricezione; i moduli ad auto-direzione non li hanno |
| A (D+) / B (D−) | A / B del bus TAP ↔ CCA | in parallelo |

Evita i pin della seriale USB (GPIO 1/3 su ESP32): il logger di ESPHome li usa.

---

## Installazione

Il componente si aggiunge a un dispositivo ESPHome passo per passo: ogni passo aggiunge poche righe al YAML del
dispositivo, non c'è un file da copiare.

### 1. Crea il dispositivo ESPHome

Nella dashboard ESPHome (add-on di Home Assistant o standalone): **+ New device** → ESP32, poi la prima
installazione via USB. Il YAML del dispositivo contiene già `esphome:`, `esp32:`, `wifi:`, `api:` e `ota:`: lasciali
come sono. Se hai già un ESP32 in ESPHome usa quello e salta questo passo.

### 2. Aggiungi il componente

Apri il YAML del dispositivo (**Edit**) e aggiungi in fondo queste righe:

```yaml
external_components:
  - source: github://Bobsilvio/tigo_server@main
    components: [tigo_rs485]

uart:
  id: tigo_bus
  rx_pin: GPIO16          # pin collegato al TXD del modulo
  baud_rate: 38400
  rx_buffer_size: 2048

tigo_rs485:
  uart_id: tigo_bus

text_sensor:
  - platform: tigo_rs485
    unconfigured_panels:
      name: Unconfigured panels
```

> Se il YAML ha già una sezione `text_sensor:`, aggiungi sotto solo la voce `- platform: tigo_rs485`: una sezione
> non può comparire due volte. Lo stesso vale per `sensor:` nei passi successivi.

### 3. Installa e leggi gli indirizzi dei pannelli

**Save** → **Install** → **Wirelessly**. Quando c'è sole gli ottimizzatori iniziano a trasmettere e i loro long
address compaiono:

- in Home Assistant, nell'entità **Unconfigured panels** del dispositivo;
- nei log di ESPHome: `Unconfigured panel reporting: 04C05B4000B1A688`.

Annota tutti gli indirizzi: ne serve uno per pannello.

### 4. Aggiungi i pannelli

Per ogni indirizzo aggiungi un blocco sotto `sensor:` con il nome che vuoi vedere in Home Assistant:

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

  - platform: tigo_rs485
    long_address: "04C05B4000B1A689"
    power:
      name: A2 Power
```

Tutti i sensori sono opzionali: tieni solo quelli che ti servono (ad esempio solo `power`, come per A2). Installa di
nuovo: i pannelli configurati spariscono da **Unconfigured panels** e compaiono come entità.

### 5. Energia (opzionale)

Per la dashboard Energia di Home Assistant dai un `id` al sensore di potenza e integralo sul dispositivo, sempre
sotto `sensor:`:

```yaml
  - platform: tigo_rs485
    long_address: "04C05B4000B1A688"
    power:
      name: A1 Power
      id: a1_power

  - platform: integration
    name: A1 Energy
    sensor: a1_power
    time_unit: h
    unit_of_measurement: Wh
    device_class: energy
    state_class: total_increasing
    restore: true
```

[`example.yaml`](example.yaml) è una configurazione completa, anche con un sensore di potenza totale: usala come
riferimento per controllare la tua, non come file da copiare.

### Long address non ancora noto

Il long address di un modulo si impara quando il CCA rilegge la tabella dei nodi, cosa che succede di rado (di
solito alla discovery). Finché non è noto il pannello compare come `node 001A` tra quelli non configurati; puoi
usarlo temporaneamente con l'ID corto, sapendo che il CCA può riassegnarlo:

```yaml
  - platform: tigo_rs485
    node_id: 0x001A
    power:
      name: A3 Power
```

---

## Riferimento configurazione

**`tigo_rs485`**

| Opzione | Predefinito | Descrizione |
|---|---|---|
| `uart_id` | — | UART collegato al bus (38400 baud) |
| `offline_timeout` | `5min` | dopo quanto un pannello silenzioso diventa non disponibile |

**`sensor` / `platform: tigo_rs485`** — esattamente uno tra `long_address` (16 cifre esadecimali) e `node_id`,
più uno o più tra `power` (W), `voltage_in` (V), `voltage_out` (V), `current` (A), `temperature` (°C),
`rssi` (valore grezzo 0–255, non dBm), `duty_cycle` (%).

**`text_sensor` / `platform: tigo_rs485`** — `unconfigured_panels`: pannelli che trasmettono ma non sono configurati.

---

## Sviluppo

Il decoder del protocollo (`components/tigo_rs485/tigo_protocol.*`) non dipende da ESPHome e si testa sul computer:

```bash
c++ -std=c++17 -I esphome/components/tigo_rs485 esphome/tests/host_test.cpp esphome/components/tigo_rs485/tigo_protocol.cpp -o host_test
```

```bash
./host_test issue_20_initial.bin
```

Senza argomenti esegue i test sintetici; con una cattura del bus (per esempio quella di
[taptap](https://github.com/willglynn/taptap), `src/test_data/issue_20_initial.bin`) stampa ogni power report.
