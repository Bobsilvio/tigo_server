# Tigo RS485 Monitor – add-on di Home Assistant

[English](README.md) · **Italiano**

La dashboard della [variante ESPHome](../esphome/README.it.md) nella sidebar di Home Assistant, con lo storico
dell'energia di ogni pannello preso da quello che Home Assistant registra già.

<img src="../images/example-addon-history.it.jpg" alt="Add-on in Home Assistant: storico di 30 giorni" width="900"/>

<sub>Screenshot con dati inventati: B4 in ombra, A6 che perde resa nell'ultima settimana.</sub>

## Installazione

1. Home Assistant → **Impostazioni** → **Componenti aggiuntivi** → **Raccolta di componenti aggiuntivi** → ⋮ →
   **Repository**, aggiungi `https://github.com/Bobsilvio/tigo_server`.
2. Installa **Tigo RS485 Monitor**.
3. In **Configurazione** imposta `host` con l'indirizzo IP dell'ESP32 (e `username`/`password` se `web_server` ha
   `auth:`).
4. Avvialo e apri **Tigo** nella sidebar.

L'ESP32 deve avere il componente `tigo_rs485` con `web_server:` attivo.

## Opzioni

| Opzione | Descrizione |
|---|---|
| `host` | indirizzo IP dell'ESP32, ad esempio `192.168.1.50`. Un nome `.local` può non risolversi dentro l'add-on: meglio l'IP (con una prenotazione DHCP per l'ESP32). |
| `port` | porta del `web_server` ESPHome, `80` di default |
| `username` / `password` | solo se `web_server` ha `auth:` |

## Cosa aggiunge alla pagina dell'ESP32

| | Pagina ESP32 | Add-on |
|---|---|---|
| Dove | `http://<esp32>/`, rete locale | sidebar di Home Assistant, anche da remoto e nell'app |
| Storico | ultime 24 h, perse al riavvio | 7 giorni, 30 giorni, 12 mesi, dalle statistiche di Home Assistant |
| Analisi | live e 24 h | anche energia giornaliera/mensile, classifica e mappa di calore relativa sul periodo |

## Da dove viene lo storico

L'add-on non salva niente. I sensori di potenza dei pannelli sono misure, quindi il recorder di Home Assistant
conserva per sempre la loro media oraria nelle statistiche a lungo termine, le stesse dei grafici statistici. L'add-on
le legge tramite l'API di Home Assistant: l'energia di un'ora è la potenza media di quell'ora per un'ora.

- Lo storico parte da quando i sensori dei pannelli sono comparsi in Home Assistant, non dall'installazione
  dell'add-on.
- Sta nel database di Home Assistant e nei suoi backup: non c'è niente da conservare o da pulire.
- L'ESP32 comunica all'add-on il nome dell'entità di potenza di ogni pannello e l'add-on la trova nel registro delle
  entità. Rinominare l'entità in Home Assistant non perde lo storico, e la dashboard mostra il nome nuovo.
- Lo storico appartiene all'entità. Con i posti automatici, dopo **Reset panel assignments** un posto può
  ricevere un altro ottimizzatore e il suo storico prosegue da quello precedente.
- I sensori esclusi dal recorder, o un pannello senza il sensore `power`, non hanno storico.

## Sviluppo

`server.py` usa solo la libreria standard di Python. La dashboard è una copia di
`esphome/components/tigo_rs485/dashboard/index.html`: dopo averla modificata esegui `./sync-dashboard.sh`.
