# Tigo RS485 Monitor – Arduino (standalone)

[English](README.md) · **Italiano**

Variante **standalone** di [Tigo RS485 Monitor](../README.it.md): firmware Arduino per ESP32 che legge il bus RS-485
tra TAP e CCA e offre una dashboard web propria, un WebSocket e la persistenza su SPIFFS. Non richiede Home Assistant.

Per usarlo con Home Assistant: integrazione [tigosolar-local](https://github.com/Bobsilvio/tigosolar-local),
sorgente `ESP32_WS` (legge il WebSocket). Se vuoi solo Home Assistant, valuta la variante [ESPHome](../esphome/README.it.md).

<img src="../images/home.png" alt="dashboard" width="1000"/>
<img src="../images/spiffs.png" alt="spiffs" width="1000"/>

---

## 🛠️ Hardware

| Componente | Note |
|---|---|
| **ESP32 / ESP32-S3** | Flash minimo da 4 MB con partizione SPIFFS |
| **Convertitore TTL→RS485** | MAX485 o simile |
| **Regolatore 5 V** | Se alimentato dal bus RS-485 |

---

## ⚙️ Funzionalità

- 📡 **Lettura bus RS-485** (Tigo TAP/CCA, 38400 baud 8N1)
- 🔍 **Parsing frame**: potenza (0x31), topologia (0x09), NodeTable (0x27), stringa `Version` (0x07, per l'ID hardware)
- 📖 **Protocollo verificato sul firmware del CCA**: decodifica allineata al CCA 4.0.8, dettagli e correzioni a taptap in [docs/PROTOCOL-NOTES.md](../docs/PROTOCOL-NOTES.md)
- 🌐 **WebServer integrato**:
  - `/` – Dashboard live con grafici (ApexCharts) e card per modulo
  - `/debug` – Tabella raw (tensione, corrente, temperatura, RSSI, …)
  - `/panels` – Mappatura manuale longAddress → etichetta (es. A4)
  - `/spiffs` – File manager (upload / download / elimina file SPIFFS)
- 🔌 **WebSocket `/ws`** — aggiornamento real-time; payload per modulo:
  ```json
  {
    "id":       "A4",
    "panel":    "A4",
    "longaddr": "04C05B4000B1A688",
    "barcode":  "04C05B4000B1A688",
    "addr":     "001A",
    "watt":     250,
    "vin":      34.70,
    "vout":     34.40,
    "amp":      6.94,
    "temp":     34.4,
    "rssi":     126,
    "online":   true,
    "age":      3,
    "hwid":     45
  }
  ```
  `watt` = `vin` × `amp` (potenza in ingresso all'ottimizzatore). `rssi` è il valore grezzo 0–255, non dBm.
  `hwid` = ID hardware del modulo letto dalla risposta `Version` (es. `Mnode Version K8.0120 (2D)` → 0x2D), `-1` finché non visto.
  La scala della corrente dipende da `hwid` come nel CCA: ≤ 0x23 → 0,0025 A; 0x24–0x5F → 0,005 A; > 0x5F → 0,025 A
  (senza `hwid` si usa 0,005 A). `temp` si aggiorna solo quando il bit 0x20 della status word lo marca come temperatura.
  Se un modulo non invia dati da 5 minuti: `online: false` e `watt`/`vin`/`vout`/`amp` a 0. `age` = secondi dall'ultimo dato (`-1` = mai ricevuto).
- 💾 **NodeTable** — salvataggio automatico con debounce 30 s (`/nodetable.json`, include l'`hwid` di ogni modulo)
- 🔖 **Panel Map** — associazione persistente longAddress → etichetta (`/panel_map.json`)
- 🕒 **NTP sync** — orario preciso alla sincronizzazione; senza internet l'avvio prosegue dopo 10 s
- 🧵 **Accesso concorrente sicuro** — i dati condivisi tra lettura RS-485 e pagine web sono protetti da un mutex; buffer di ricezione RS-485 da 4 KB
- 🔁 **OTA** — aggiornamento firmware via rete
- 📊 **Dashboard grafica**:
  - Grafico area — potenza totale nel tempo (ultimi 60 campioni, in-memory)
  - Grafico a barre orizzontali — potenza istantanea per pannello, colorato W→verde
  - Card per ogni modulo con barra di progresso, Vin/Vout, corrente, temperatura, RSSI

---

## 📂 File SPIFFS

| File | Descrizione |
|---|---|
| `index.html` | Dashboard web |
| `nodetable.json` | Mapping addr corto ↔ longAddress (auto-generato) |
| `panel_map.json` | Mapping longAddress ↔ etichetta pannello (gestito da `/panels`) |

---

## 🔖 Mappatura pannelli

Vai su `/panels`: ogni riga mostra il longAddress del modulo (es. `04C05B4000B1A688`).  
Scrivi l'etichetta (es. `A4`) e clicca **Salva**.  
Da quel momento il WebSocket trasmette `"panel": "A4"` e `"id": "A4"` per quel modulo.  
Il longAddress rimane sempre disponibile nei campi `barcode` e `longaddr`.

---

## 🔄 Salvataggio automatico NodeTable

La `loop()` controlla ogni ciclo: se `NodeTable_changed == true` e sono passati ≥ 30 s  
dall'ultimo salvataggio → chiama `saveNodeTable()` e resetta il flag.  
Il salvataggio manuale è disponibile su `/debug` → bottone **Salva NodeTable ora**.

---

## 🧰 Installazione (Arduino IDE)

### 1. Prerequisiti
- **Arduino IDE 2.x**
- Supporto schede ESP32:  
  *File → Preferenze → URL aggiuntivi*:
  ```
  https://espressif.github.io/arduino-esp32/package_esp32_index.json
  ```
  Poi: *Strumenti → Scheda → Gestore schede → "esp32" → Installa*

### 2. Librerie richieste
Installa via *Sketch → Includi libreria → Gestisci librerie*:
- **ArduinoJson** (Benoit Blanchon) — versione 6 o 7
- **ESPAsyncWebServer** + **AsyncTCP**
- **PubSubClient** (MQTT)
- **WebSerial**

### 3. Configurazione scheda
*Strumenti*:
- **Board:** `ESP32 Dev Module` (o variante S3)
- **Partition Scheme:** `No OTA (1MB APP / 3MB SPIFFS)`
- **Upload Speed:** 921600

### 4. Apri lo sketch, Wi-Fi e pin
Apri `arduino/TigoServer/TigoServer.ino` (la cartella dello sketch deve chiamarsi `TigoServer`, la cartella `data/`
accanto contiene i file SPIFFS). In cima al file:
```cpp
const char* ssid     = "TuaReteWiFi";
const char* password = "TuaPassword";
#define RX_PIN 3  // pin collegato a RO del convertitore RS-485
#define TX_PIN 1
```
Imposta `RX_PIN`/`TX_PIN` sui pin che hai cablato. Il valore predefinito nel codice è 3/1, gli stessi della
seriale USB.

### 5. Flash firmware
1. Collega ESP32 via USB
2. Clicca **→ Carica**
3. Apri *Monitor Seriale* (115200 baud)

### 6. Carica i file SPIFFS
Installa il plugin **ESP32 Sketch Data Upload**:  
→ [https://github.com/me-no-dev/arduino-esp32fs-plugin](https://github.com/me-no-dev/arduino-esp32fs-plugin)  
Poi: *Strumenti → ESP32 Sketch Data Upload*

### 7. Primo accesso
Trova l'IP in Serial Monitor o nel router DHCP:
```
http://<IP_ESP32>
```
- `/` — Dashboard + grafici
- `/debug` — Dati raw + NodeTable
- `/panels` — Mappatura etichette
- `/spiffs` — File manager

---

## ⚡ Schema di collegamento

<img src="../images/wiring.it.svg" alt="Schema di collegamento" width="900"/>

| Modulo RS-485 | ESP32 / bus | Note |
|---|---|---|
| VCC | 3V3 | |
| GND | GND | |
| TXD | `RX_PIN` (predefinito GPIO 3) | dati dal bus verso l'ESP32 |
| RXD | non collegato | il monitor non trasmette |
| A (D+) / B (D−) | A / B del bus TAP ↔ CCA | in parallelo |

L'ESP32 è alimentato in USB-C da un convertitore DC-DC 12/24 V → 5 V collegato a +V/GND del bus.

---

Crediti e licenza: vedi il [README principale](../README.it.md).

