void setupWebserver() {
  handleDebugPage();
  handlePanelsPage();
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // Elenco file SPIFFS
  server.on("/spiffs", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", generateFileListHTML());
  });

  // Gestione pannelli
  server.on("/panel_set", HTTP_POST, [](AsyncWebServerRequest *request) {
    if (request->hasParam("longAddress", true) && request->hasParam("label", true)) {
      String la  = request->getParam("longAddress", true)->value();
      String lbl = request->getParam("label", true)->value();
      la.trim(); lbl.trim();
      // Aggiorna se esiste già, altrimenti aggiungi
      bool found = false;
      for (int i = 0; i < panelMap_count; i++) {
        if (panelMap[i].longAddress == la) {
          panelMap[i].label = lbl;
          found = true; break;
        }
      }
      if (!found && panelMap_count < 150) {
        panelMap[panelMap_count].longAddress = la;
        panelMap[panelMap_count].label       = lbl;
        panelMap_count++;
      }
      savePanelMap();
    }
    request->redirect("/panels");
  });

  server.on("/panel_delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("longAddress")) {
      String la = request->getParam("longAddress")->value();
      for (int i = 0; i < panelMap_count; i++) {
        if (panelMap[i].longAddress == la) {
          for (int j = i; j < panelMap_count - 1; j++) panelMap[j] = panelMap[j+1];
          panelMap_count--;
          break;
        }
      }
      savePanelMap();
    }
    request->redirect("/panels");
  });

  handleFileUpload();

  // Eliminazione file
  server.on("/delete", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (request->hasParam("file")) {
      String fileToDelete = request->getParam("file")->value();
      if (SPIFFS.exists(fileToDelete)) {
        if (SPIFFS.remove(fileToDelete)) {
          request->redirect("/spiffs");
        } else {
          request->send(500, "text/plain", "Error deleting file");
        }
      } else {
        request->send(404, "text/plain", "File not found");
      }
    } else {
      request->send(400, "text/plain", "Missing parameter: file");
    }
  });

  // Download file
  server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing parameter: file");
      return;
    }

    String filePath = request->getParam("file")->value();
    if (!SPIFFS.exists(filePath)) {
      request->send(404, "text/plain", "File not found");
      return;
    }

    File *file = new File(SPIFFS.open(filePath, "r"));
    if (!*file) {
      request->send(500, "text/plain", "Failed to open file");
      delete file;
      return;
    }

    AsyncWebServerResponse *response = request->beginResponse(
      "application/octet-stream",
      file->size(),
      [file](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        size_t readLen = file->read(buffer, maxLen);
        if (readLen == 0) {
          file->close();
          delete file;
        }
        return readLen;
      }
    );

    String filename = filePath.substring(filePath.lastIndexOf('/') + 1);
    response->addHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
    request->send(response);
  });

  // Reboot
  server.on("/reboot", HTTP_POST, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "♻️ Rebooting ESP32...");
    request->client()->close();
    delay(100);
    ESP.restart();
  });

  // Salva NodeTable
  server.on("/save", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/plain", "Saving NodeTable...");
    delay(100);
    saveNodeTable();
    NodeTable_changed = false;
    WebSerial.println("✅ NodeTable saved via /save endpoint");
  });

  // Gestione 404
  server.onNotFound([](AsyncWebServerRequest *request) {
    request->send(404, "text/plain", "404: Not Found");
  });
}

String urlEncode(const String &str) {
  String encoded = "";
  char buf[4];
  for (size_t i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      encoded += c;
    } else {
      sprintf(buf, "%%%02X", (unsigned char)c);
      encoded += buf;
    }
  }
  return encoded;
}

String fixPath(const String &path) {
  if (!path.startsWith("/")) {
    return "/" + path;
  }
  return path;
}

String generateFileListHTML() {
  size_t total    = SPIFFS.totalBytes();
  size_t used     = SPIFFS.usedBytes();
  size_t freeBytes = total - used;
  size_t freeHeap = ESP.getFreeHeap();
  float usedKB    = used   / 1024.0;
  float totalKB   = total  / 1024.0;
  float usedPct   = total  > 0 ? (used * 100.0 / total) : 0;

  String html = "<!DOCTYPE html><html lang='en'><head>"
    "<meta charset='UTF-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>SPIFFS · TigoServer</title>"
    "<style>"
    "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
    ":root{--bg:#0d0d0d;--surface:#1a1a1a;--border:#2e2e2e;--accent:#f0a500;--green:#4caf50;--blue:#42a5f5;--red:#ef5350;--text:#e0e0e0;--muted:#777;--radius:12px}"
    "body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;padding:14px}"
    "nav{display:flex;align-items:center;gap:4px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:10px 16px;margin-bottom:16px;flex-wrap:wrap}"
    "nav a{color:var(--text);text-decoration:none;font-size:.85rem;font-weight:600;padding:5px 11px;border-radius:7px;transition:background .18s}"
    "nav a:hover{background:var(--border)}"
    "nav a.danger{color:var(--red)}"
    "nav a.danger:hover{background:rgba(239,83,80,.13)}"
    "h2{font-size:1rem;font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.07em;margin:20px 0 10px}"
    ".card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px;margin-bottom:16px}"
    "table{width:100%;border-collapse:collapse;font-size:.83rem}"
    "th{text-align:left;padding:8px 10px;color:var(--muted);font-size:.7rem;text-transform:uppercase;letter-spacing:.06em;border-bottom:1px solid var(--border)}"
    "td{padding:8px 10px;border-bottom:1px solid var(--border);vertical-align:middle;word-break:break-all}"
    "tr:last-child td{border-bottom:none}"
    "tr:hover td{background:rgba(255,255,255,.025)}"
    ".fname{color:var(--blue);font-weight:600}"
    ".fsize{color:var(--muted);white-space:nowrap}"
    ".btn{display:inline-block;padding:3px 10px;border-radius:5px;font-size:.75rem;font-weight:700;text-decoration:none;transition:background .17s}"
    ".btn-dl{background:rgba(66,165,245,.15);color:var(--blue)}"
    ".btn-dl:hover{background:rgba(66,165,245,.3)}"
    ".btn-del{background:rgba(239,83,80,.12);color:var(--red)}"
    ".btn-del:hover{background:rgba(239,83,80,.25)}"
    ".bar-wrap{height:8px;background:var(--border);border-radius:4px;overflow:hidden;margin:6px 0 4px}"
    ".bar{height:100%;border-radius:4px;background:linear-gradient(90deg,var(--green),var(--accent))}"
    ".stat-row{display:flex;gap:20px;flex-wrap:wrap;font-size:.82rem;color:var(--muted);margin-top:4px}"
    ".stat-row b{color:var(--text)}"
    "label{font-size:.8rem;color:var(--muted);display:block;margin-bottom:6px}"
    "input[type=file]{color:var(--text);font-size:.82rem;background:var(--border);border:1px solid var(--border);border-radius:6px;padding:5px 8px;width:100%}"
    ".btn-up{margin-top:10px;display:inline-block;padding:7px 20px;border-radius:7px;font-weight:700;font-size:.85rem;background:var(--accent);color:#000;border:none;cursor:pointer;transition:opacity .18s}"
    ".btn-up:hover{opacity:.82}"
    "</style></head><body>";

  html += "<nav>"
    "<a href='/'>🏠 Home</a>"
    "<a href='/debug'>🧠 Debug</a>"
    "<a href='/panels'>🔖 Pannelli</a>"
    "<a href='/spiffs'>💾 SPIFFS</a>"
    "<a href='#' class='danger' onclick=\"if(confirm('Riavviare?'))fetch('/reboot',{method:'POST'}).then(()=>setTimeout(()=>location.reload(),4000))\">🔁 Reboot</a>"
    "</nav>";

  // Tabella file
  html += "<div class='card'>";
  html += "<h2>File</h2>";
  html += "<table><thead><tr><th>Nome</th><th>Dimensione</th><th></th></tr></thead><tbody>";

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  bool hasFiles = false;
  while (file) {
    hasFiles = true;
    String fname = fixPath(file.name());
    size_t fsize  = file.size();
    html += "<tr>";
    html += "<td class='fname'>" + fname + "</td>";
    html += "<td class='fsize'>" + String(fsize) + " B &nbsp;(" + String(fsize / 1024.0, 1) + " KB)</td>";
    html += "<td style='white-space:nowrap'>"
            "<a class='btn btn-dl' href='/download?file=" + urlEncode(fname) + "'>⬇ Download</a> "
            "<a class='btn btn-del' href='/delete?file=" + urlEncode(fname) + "' "
            "onclick=\"return confirm('Eliminare &quot;" + fname + "&quot;?')\">✕ Elimina</a>";
    html += "</td></tr>";
    file = root.openNextFile();
  }
  if (!hasFiles) html += "<tr><td colspan='3' style='color:var(--muted);text-align:center;padding:20px'>Nessun file</td></tr>";
  html += "</tbody></table></div>";

  // Storage
  html += "<div class='card'>";
  html += "<h2>Storage</h2>";
  html += "<div class='bar-wrap'><div class='bar' style='width:" + String(usedPct, 0) + "%'></div></div>";
  html += "<div class='stat-row'>";
  html += "<span><b>" + String(usedPct, 0) + "%</b> usato</span>";
  html += "<span><b>" + String(usedKB, 1) + " KB</b> / " + String(totalKB, 1) + " KB</span>";
  html += "<span>liberi: <b>" + String(freeBytes / 1024.0, 1) + " KB</b></span>";
  html += "<span>RAM libera: <b>" + String(freeHeap / 1024) + " KB</b></span>";
  html += "</div></div>";

  // Upload
  html += "<div class='card'>";
  html += "<h2>Carica file</h2>";
  html += "<form method='POST' action='/save_upload' enctype='multipart/form-data'>";
  html += "<label>Seleziona un file da caricare su SPIFFS:</label>";
  html += "<input type='file' name='upload'>";
  html += "<button class='btn-up' type='submit'>⬆ Carica</button>";
  html += "</form></div>";

  html += "</body></html>";
  return html;
}



void handlePanelsPage() {
  server.on("/panels", HTTP_GET, [](AsyncWebServerRequest *request) {

    String h = "<!DOCTYPE html><html lang='en'><head>"
      "<meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Pannelli · TigoServer</title>"
      "<style>"
      "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
      ":root{--bg:#0d0d0d;--surface:#1a1a1a;--border:#2e2e2e;--accent:#f0a500;--green:#4caf50;--blue:#42a5f5;--red:#ef5350;--text:#e0e0e0;--muted:#777;--radius:12px}"
      "body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;padding:14px}"
      "nav{display:flex;align-items:center;gap:4px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:10px 16px;margin-bottom:16px;flex-wrap:wrap}"
      "nav a{color:var(--text);text-decoration:none;font-size:.85rem;font-weight:600;padding:5px 11px;border-radius:7px;transition:background .18s}"
      "nav a:hover{background:var(--border)}"
      "nav a.danger{color:var(--red)}"
      "nav a.danger:hover{background:rgba(239,83,80,.13)}"
      "h2{font-size:1rem;font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.07em;margin:20px 0 10px}"
      ".card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px;margin-bottom:16px;overflow-x:auto}"
      "table{width:100%;border-collapse:collapse;font-size:.83rem}"
      "th{text-align:left;padding:8px 10px;color:var(--muted);font-size:.7rem;text-transform:uppercase;letter-spacing:.06em;border-bottom:1px solid var(--border)}"
      "td{padding:8px 10px;border-bottom:1px solid var(--border);vertical-align:middle;white-space:nowrap}"
      "tr:last-child td{border-bottom:none}"
      "tr:hover td{background:rgba(255,255,255,.025)}"
      ".la{font-family:monospace;font-size:.82rem;color:var(--blue)}"
      ".lbl{font-weight:700;color:var(--accent)}"
      ".lbl-empty{color:var(--muted);font-style:italic}"
      "input[type=text]{background:#111;border:1px solid var(--border);color:var(--text);border-radius:6px;padding:5px 9px;font-size:.82rem;width:90px;transition:border .18s}"
      "input[type=text]:focus{outline:none;border-color:var(--accent)}"
      ".btn{display:inline-block;padding:4px 12px;border-radius:6px;font-size:.77rem;font-weight:700;border:none;cursor:pointer;transition:opacity .17s;text-decoration:none}"
      ".btn:hover{opacity:.8}"
      ".btn-save{background:var(--accent);color:#000}"
      ".btn-del{background:rgba(239,83,80,.15);color:var(--red)}"
      ".note{font-size:.75rem;color:var(--muted);margin-top:8px}"
      "</style></head><body>";

    h += "<nav>"
      "<a href='/'>🏠 Home</a>"
      "<a href='/debug'>🧠 Debug</a>"
      "<a href='/panels'>🔖 Pannelli</a>"
      "<a href='/spiffs'>💾 SPIFFS</a>"
      "<a href='#' class='danger' onclick=\"if(confirm('Riavviare?'))fetch('/reboot',{method:'POST'}).then(()=>setTimeout(()=>location.reload(),4000))\">🔁 Reboot</a>"
      "</nav>";

    // Tabella nodi noti (da NodeTable) con etichette
    h += "<div class='card'>";
    h += "<h2>Associazione Pannelli (&nbsp;" + String(NodeTable_count) + "&nbsp; nodi)</h2>";
    h += "<p class='note' style='margin-bottom:12px'>Assegna un'etichetta (es. A4) al long address di ogni pannello. "
         "L'etichetta viene trasmessa via WebSocket nel campo <code style='color:var(--blue)'>panel</code>.</p>";

    if (NodeTable_count == 0) {
      h += "<p style='color:var(--muted);padding:16px'>Nessun nodo in NodeTable. Attendi che i pannelli si connettano.</p>";
    } else {
      h += "<table><thead><tr><th>#</th><th>Long Address</th><th>Addr</th><th>Etichetta attuale</th><th>Imposta</th><th></th></tr></thead><tbody>";
      for (int i = 0; i < NodeTable_count; i++) {
        String la  = NodeTable[i].longAddress;
        String sa  = NodeTable[i].addr;
        String lbl = getPanelLabel(la);
        h += "<tr>";
        h += "<td style='color:var(--muted)'>" + String(i+1) + "</td>";
        h += "<td class='la'>" + la + "</td>";
        h += "<td style='color:var(--muted)'>" + sa + "</td>";
        if (lbl != "") {
          h += "<td class='lbl'>" + lbl + "</td>";
        } else {
          h += "<td class='lbl-empty'>non assegnato</td>";
        }
        // Form set
        h += "<td><form method='POST' action='/panel_set' style='display:flex;gap:6px;align-items:center'>"
             "<input type='hidden' name='longAddress' value='" + la + "'>"
             "<input type='text' name='label' placeholder='es. A4' value='" + lbl + "' maxlength='10'>"
             "<button class='btn btn-save' type='submit'>✓ Salva</button>"
             "</form></td>";
        // Delete
        if (lbl != "") {
          h += "<td><a class='btn btn-del' href='/panel_delete?longAddress=" + urlEncode(la) + "' "
               "onclick=\"return confirm('Rimuovere etichetta &quot;" + lbl + "&quot;?')\">× Rimuovi</a></td>";
        } else {
          h += "<td></td>";
        }
        h += "</tr>";
      }
      h += "</tbody></table>";
    }
    h += "</div>";

    // Mostra lo stato del mapping corrente (pannelli mappati)
    h += "<div class='card'>";
    h += "<h2>Mapping salvato (&nbsp;" + String(panelMap_count) + "&nbsp; voci)</h2>";
    if (panelMap_count == 0) {
      h += "<p style='color:var(--muted);padding:8px'>Nessun mapping salvato.</p>";
    } else {
      h += "<table><thead><tr><th>Etichetta</th><th>Long Address</th><th></th></tr></thead><tbody>";
      for (int i = 0; i < panelMap_count; i++) {
        h += "<tr>";
        h += "<td class='lbl'>" + panelMap[i].label + "</td>";
        h += "<td class='la'>" + panelMap[i].longAddress + "</td>";
        h += "<td><a class='btn btn-del' href='/panel_delete?longAddress=" + urlEncode(panelMap[i].longAddress) + "' "
             "onclick=\"return confirm('Rimuovere?')\">×</a></td>";
        h += "</tr>";
      }
      h += "</tbody></table>";
    }
    h += "</div>";

    h += "</body></html>";
    request->send(200, "text/html", h);
  });
}

void handleDebugPage() {
  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request){

    String h = "<!DOCTYPE html><html lang='en'><head>"
      "<meta charset='UTF-8'>"
      "<meta name='viewport' content='width=device-width,initial-scale=1'>"
      "<title>Debug · TigoServer</title>"
      "<style>"
      "*,*::before,*::after{box-sizing:border-box;margin:0;padding:0}"
      ":root{--bg:#0d0d0d;--surface:#1a1a1a;--border:#2e2e2e;--accent:#f0a500;--green:#4caf50;--blue:#42a5f5;--red:#ef5350;--text:#e0e0e0;--muted:#777;--radius:12px}"
      "body{background:var(--bg);color:var(--text);font-family:'Segoe UI',system-ui,sans-serif;padding:14px}"
      "nav{display:flex;align-items:center;gap:4px;background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:10px 16px;margin-bottom:16px;flex-wrap:wrap}"
      "nav a{color:var(--text);text-decoration:none;font-size:.85rem;font-weight:600;padding:5px 11px;border-radius:7px;transition:background .18s}"
      "nav a:hover{background:var(--border)}"
      "nav a.danger{color:var(--red)}"
      "nav a.danger:hover{background:rgba(239,83,80,.13)}"
      "h2{font-size:1rem;font-weight:700;color:var(--muted);text-transform:uppercase;letter-spacing:.07em;margin:20px 0 10px}"
      ".card{background:var(--surface);border:1px solid var(--border);border-radius:var(--radius);padding:16px;margin-bottom:16px;overflow-x:auto}"
      "table{width:100%;border-collapse:collapse;font-size:.8rem;min-width:600px}"
      "th{text-align:center;padding:7px 10px;color:var(--muted);font-size:.68rem;text-transform:uppercase;letter-spacing:.06em;border-bottom:1px solid var(--border)}"
      "td{padding:7px 10px;border-bottom:1px solid var(--border);text-align:center;white-space:nowrap}"
      "tr:last-child td{border-bottom:none}"
      "tr:hover td{background:rgba(255,255,255,.025)}"
      ".hl-blue{color:var(--blue);font-weight:600}"
      ".hl-green{color:var(--green);font-weight:700}"
      ".hl-muted{color:var(--muted)}"
      ".warn-box{background:rgba(239,83,80,.1);border:1px solid rgba(239,83,80,.3);border-radius:8px;padding:10px 14px;color:var(--red);font-size:.85rem;margin-bottom:12px}"
      ".btn-save{display:inline-block;padding:8px 22px;border-radius:8px;font-weight:700;font-size:.85rem;background:var(--accent);color:#000;text-decoration:none;transition:opacity .18s}"
      ".btn-save:hover{opacity:.82}"
      "</style></head><body>";

    h += "<nav>"
      "<a href='/'>🏠 Home</a>"
      "<a href='/debug'>🧠 Debug</a>"
      "<a href='/panels'>🔖 Pannelli</a>"
      "<a href='/spiffs'>💾 SPIFFS</a>"
      "<a href='#' class='danger' onclick=\"if(confirm('Riavviare?'))fetch('/reboot',{method:'POST'}).then(()=>setTimeout(()=>location.reload(),4000))\">🔁 Reboot</a>"
      "</nav>";

    // Helper: ricava la label pannello dato l'addr del device
    auto getPanelLabel = [](const String& addr) -> String {
      // 1) trova longAddress in NodeTable
      String la = "";
      for (int k = 0; k < NodeTable_count; k++) {
        if (NodeTable[k].addr == addr) { la = NodeTable[k].longAddress; break; }
      }
      if (la.isEmpty()) return "";
      // 2) cerca in panelMap
      for (int k = 0; k < panelMap_count; k++) {
        if (panelMap[k].longAddress == la) return panelMap[k].label;
      }
      return "";
    };

    // Ordina gli indici dei device per label pannello, senza label in fondo
    int order[100];
    for (int i = 0; i < deviceCount; i++) order[i] = i;
    // bubble sort (deviceCount tipicamente piccolo)
    for (int a = 0; a < deviceCount - 1; a++) {
      for (int b = a + 1; b < deviceCount; b++) {
        String la = getPanelLabel(devices[order[a]].addr);
        String lb = getPanelLabel(devices[order[b]].addr);
        // chi non ha label va dopo
        if (la.isEmpty() && !lb.isEmpty()) { int t = order[a]; order[a] = order[b]; order[b] = t; }
        else if (!la.isEmpty() && !lb.isEmpty() && lb < la) { int t = order[a]; order[a] = order[b]; order[b] = t; }
      }
    }

    // Tabella dispositivi
    h += "<div class='card'>";
    h += "<h2>Dispositivi Tigo (&nbsp;" + String(deviceCount) + "&nbsp;)</h2>";
    h += "<table><thead><tr>"
      "<th>#</th><th>Pannello</th><th>Node ID</th><th>Addr</th>"
      "<th>Vin</th><th>Vout</th><th>Duty</th>"
      "<th>Corrente</th><th>Watt</th><th>Temp</th>"
      "<th>Slot</th><th>RSSI</th><th>Barcode</th>"
      "</tr></thead><tbody>";

    for (int ii = 0; ii < deviceCount; ii++) {
      int i = order[ii];
      int watt = round(devices[i].voltage_out * devices[i].current_in);
      String label = getPanelLabel(devices[i].addr);
      h += "<tr>";
      h += "<td class='hl-muted'>" + String(ii+1) + "</td>";
      h += "<td class='hl-green'>" + (label.isEmpty() ? "<span class='hl-muted'>—</span>" : label) + "</td>";
      h += "<td>" + devices[i].pv_node_id + "</td>";
      h += "<td class='hl-blue'>" + devices[i].addr + "</td>";
      h += "<td>" + String(devices[i].voltage_in, 2)  + " V</td>";
      h += "<td>" + String(devices[i].voltage_out, 2) + " V</td>";
      h += "<td class='hl-muted'>" + String(devices[i].duty_cycle, HEX) + "</td>";
      h += "<td>" + String(devices[i].current_in, 2)  + " A</td>";
      h += "<td class='hl-green'>" + String(watt) + " W</td>";
      h += "<td>" + String(devices[i].temperature, 1) + " &deg;C</td>";
      h += "<td class='hl-muted'>" + devices[i].slot_counter + "</td>";
      h += "<td>" + String(devices[i].rssi) + "</td>";
      h += "<td>" + devices[i].barcode + "</td>";
      h += "</tr>";
    }
    h += "</tbody></table></div>";

    // Tabella NodeTable
    h += "<div class='card'>";
    h += "<h2>Node Table (&nbsp;" + String(NodeTable_count) + "&nbsp;)</h2>";
    if (NodeTable_changed) {
      h += "<div class='warn-box'>⚠️ NodeTable modificata e non ancora salvata</div>";
    }
    h += "<table><thead><tr><th>#</th><th>Addr</th><th>Long Address</th><th>Checksum</th></tr></thead><tbody>";
    for (int i = 0; i < NodeTable_count; i++) {
      h += "<tr>";
      h += "<td class='hl-muted'>" + String(i+1) + "</td>";
      h += "<td class='hl-blue'>" + NodeTable[i].addr + "</td>";
      h += "<td>" + NodeTable[i].longAddress + "</td>";
      h += "<td class='hl-muted'>" + NodeTable[i].checksum + "</td>";
      h += "</tr>";
    }
    h += "</tbody></table>";
    h += "<br><a class='btn-save' href='/save'>💾 Salva NodeTable ora</a>";
    h += "</div>";

    h += "</body></html>";
    request->send(200, "text/html", h);
  });
}


void handleFileUpload() {
  server.on(
    "/save_upload", HTTP_POST,
    [](AsyncWebServerRequest *request) {
      request->redirect("/spiffs");
    },
    [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      static bool spaceChecked = false;

      if (!index) {
        spaceChecked = false;
        Serial.printf("Upload beginnt: %s\n", filename.c_str());
        String path = "/" + filename;
        if (SPIFFS.exists(path)) {
          SPIFFS.remove(path);
        }
        uploadFile = SPIFFS.open(path, FILE_WRITE);
      }

      if (!spaceChecked) {
        if (SPIFFS.totalBytes() - SPIFFS.usedBytes() < len) {
          Serial.println("❌ Not enough space on SPIFFS.");
          if (uploadFile) uploadFile.close();
          return;
        }
        spaceChecked = true;
      }

      if (uploadFile) {
        uploadFile.write(data, len);
      }

      if (final) {
        Serial.printf("Upload abgeschlossen: %s (%u Bytes)\n", filename.c_str(), index + len);
        if (uploadFile) uploadFile.close();
      }
    }
  );
}
