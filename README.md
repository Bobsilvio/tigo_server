# Tigo RS485 Monitor

Monitor locale per ottimizzatori solari **Tigo TS4**: un ESP32 ascolta il bus RS-485 tra i TAP e il CCA e legge,
per ogni pannello, potenza, tensioni, corrente e temperatura. Nessun cloud, nessuna modifica al CCA: l'ESP32 è in
sola ricezione.

<img src="images/home.png" alt="dashboard" width="1000"/>

## Scegli la variante

| | [**Arduino** (standalone)](arduino/README.md) | [**ESPHome** (Home Assistant)](esphome/README.md) |
|---|---|---|
| Per chi | vuole un dispositivo autonomo con dashboard propria | usa Home Assistant |
| Interfaccia | dashboard web, pagine debug/pannelli/file, WebSocket | entità native in Home Assistant |
| Home Assistant | tramite l'integrazione [tigosolar-local](https://github.com/Bobsilvio/tigosolar-local) (sorgente `ESP32_WS`) | nativo, nessuna integrazione custom |
| Configurazione | Arduino IDE, Wi-Fi nel codice | YAML ESPHome |
| Aggiornamenti | OTA Arduino | OTA ESPHome |
| Nomi pannelli | pagina `/panels` | nel YAML |

Entrambe usano la stessa decodifica, verificata sul firmware del CCA.

## Hardware comune

- ESP32
- modulo TTL↔RS485 (MAX485 o ad auto-direzione) in **sola ricezione**: l'uscita dati del modulo va al pin RX
  dell'ESP32, l'ingresso dati del modulo resta scollegato
- collegamento A/B in parallelo al bus TAP ↔ CCA, 38400 baud 8N1
- convertitore DC-DC 12/24 V → 5 V (USB-C) per alimentare l'ESP32 dal bus

<img src="images/wiring.svg" alt="Schema di collegamento" width="900"/>

## Protocollo

[docs/PROTOCOL-NOTES.md](docs/PROTOCOL-NOTES.md) descrive il protocollo del bus come lo decodifica il firmware del
CCA (4.0.8): layout del power report, scala della corrente per ID hardware, nomi ufficiali dei pacchetti e i punti
in cui [taptap](https://github.com/willglynn/taptap) è sbagliato o incompleto.

## Struttura del repository

```
arduino/    variante Arduino: sketch TigoServer/ e README
esphome/    variante ESPHome: componente tigo_rs485, example.yaml, test, README
docs/       note sul protocollo
images/     immagini
```

## Crediti

- Reverse engineering del protocollo: [willglynn/taptap](https://github.com/willglynn/taptap)
- Progetto originale: [tictactom/tigo_server](https://github.com/tictactom/tigo_server)
- Correzioni al protocollo ricavate dal firmware del CCA: [docs/PROTOCOL-NOTES.md](docs/PROTOCOL-NOTES.md)

## Licenza

MIT — libero adattamento per il proprio impianto.
