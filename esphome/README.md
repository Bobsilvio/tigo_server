# Tigo RS485 Monitor – ESPHome (Home Assistant)

Variante **ESPHome** di [Tigo RS485 Monitor](../README.md): un componente esterno ESPHome (`tigo_rs485`) che ascolta
il bus RS-485 tra TAP e CCA e pubblica in Home Assistant, in modo nativo, un set di sensori per ogni ottimizzatore.
Niente integrazione custom, niente WebSocket: Wi-Fi, OTA e log sono quelli standard di ESPHome.

Per una dashboard web autonoma senza Home Assistant usa la variante [Arduino](../arduino/README.md).

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

<img src="../images/wiring.svg" alt="Schema di collegamento" width="900"/>

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

1. Copia [`example.yaml`](example.yaml) nella tua configurazione ESPHome e imposta Wi-Fi e `rx_pin`.
2. Il componente si scarica da GitHub:
   ```yaml
   external_components:
     - source: github://Bobsilvio/tigo_server@main
       components: [tigo_rs485]
   ```
3. Configura UART e componente:
   ```yaml
   uart:
     id: tigo_bus
     rx_pin: GPIO16
     baud_rate: 38400
     rx_buffer_size: 2048

   tigo_rs485:
     uart_id: tigo_bus
     offline_timeout: 5min   # opzionale
   ```
4. Flasha una prima volta con solo il sensore dei pannelli non configurati:
   ```yaml
   text_sensor:
     - platform: tigo_rs485
       unconfigured_panels:
         name: Unconfigured panels
   ```
   Appena il sole è alto i long address compaiono lì e nei log (`Unconfigured panel reporting: 04C05B4000B1A688`).
5. Aggiungi un blocco per ogni pannello:
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
   Tutti i sensori sono opzionali.

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

### Energia

L'energia si ottiene integrando la potenza sul dispositivo con la piattaforma standard `integration`
(vedi `example.yaml`), utilizzabile nella dashboard Energia di Home Assistant.

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
