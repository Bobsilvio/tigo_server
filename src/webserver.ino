void setupWebserver() {
  handleDebugPage();
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);
  server.serveStatic("/", SPIFFS, "/").setDefaultFile("index.html");

  // Elenco file SPIFFS
  server.on("/spiffs", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", generateFileListHTML());
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

  // Log viewer
  server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request){
    String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Logs</title>";
    html += "<style>";
    html += "body { font-family: Arial; margin: 20px; }";
    html += "nav { background:#333; padding:10px; border-radius:8px; margin-bottom:20px; }";
    html += "nav a { color:white; margin-right:15px; text-decoration:none; font-weight:bold; }";
    html += "nav a:hover { text-decoration:underline; }";
    html += "</style></head><body>";

    html += "<nav>";
    html += "<a href='/'>🏠 HOME</a>";
    html += "<a href='/debug'>🧠 Debug</a>";
    html += "<a href='/spiffs'>💾 SPIFFS</a>";
    html += "<a href='/logs'>📜 Logs</a>";
    html += "<a href='/logviewer.html'>📊 Log Viewer</a>";
    html += "<a href='#' onclick=\"fetch('/reboot',{method:'POST'}).then(()=>location.reload());\">🔁 Reboot</a>";
    html += "</nav>";

    html += "<h1>Log Viewer</h1>";
    html += "<p>Open specific logs via <a href='/listlogs'>/listlogs</a> or view a file with /logdata?file=log_xxx.txt</p>";
    html += "</body></html>";

    request->send(200, "text/html", html);
  });


  // Elenco file log
  server.on("/listlogs", HTTP_GET, [](AsyncWebServerRequest *request) {
    String json = "[";
    File root = SPIFFS.open("/");
    File file = root.openNextFile();
    bool first = true;
    while (file) {
      String name = file.name();
      if (name.startsWith("log_")) {
        if (!first) json += ",";
        json += "\"" + name + "\"";
        first = false;
      }
      file = root.openNextFile();
    }
    root.close();
    json += "]";
    request->send(200, "application/json", json);
  });

  // Lettura singolo file log
  server.on("/logdata", HTTP_GET, [](AsyncWebServerRequest *request) {
    if (!request->hasParam("file")) {
      request->send(400, "text/plain", "Missing file parameter");
      return;
    }

    String file = request->getParam("file")->value();
    if (!file.startsWith("/")) file = "/" + file;

    if (!SPIFFS.exists(file)) {
      request->send(404, "text/plain", "File not found");
      return;
    }

    request->send(SPIFFS, file, "text/plain");
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
  size_t total = SPIFFS.totalBytes();
  size_t used = SPIFFS.usedBytes();
  size_t free = total - used;
  size_t freeHeap = ESP.getFreeHeap();

  // Conversione in KB e MB
  float usedKB = used / 1024.0;
  float totalKB = total / 1024.0;
  float freeKB = free / 1024.0;

  float usedMB = usedKB / 1024.0;
  float totalMB = totalKB / 1024.0;
  float freeMB = freeKB / 1024.0;

  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>SPIFFS Files</title>";
  html += "<style>";
  html += "body { font-family: Arial; margin: 20px; }";
  html += "nav { background:#333; padding:10px; border-radius:8px; margin-bottom:20px; }";
  html += "nav a { color:white; margin-right:15px; text-decoration:none; font-weight:bold; }";
  html += "nav a:hover { text-decoration:underline; }";
  html += "table { border-collapse: collapse; width:100%; }";
  html += "th, td { border:1px solid #ccc; padding:6px; }";
  html += "th { background:#f0f0f0; }";
  html += "</style></head><body>";

  // 🔹 Menu
  html += "<nav>";
  html += "<a href='/'>🏠 HOME</a>";
  html += "<a href='/debug'>🧠 Debug</a>";
  html += "<a href='/spiffs'>💾 SPIFFS</a>";
  html += "<a href='/logs'>📜 Logs</a>";
  html += "<a href='/logviewer.html'>📊 Log Viewer</a>";
  html += "<a href='#' onclick=\"fetch('/reboot',{method:'POST'}).then(()=>location.reload());\">🔁 Reboot</a>";
  html += "</nav>";

  html += "<h1>SPIFFS File List</h1><ul>";

  File root = SPIFFS.open("/");
  File file = root.openNextFile();
  while (file) {
    String fname = fixPath(file.name());
    size_t fsize = file.size();
    html += "<li>" + fname + " (" + String(fsize) + " Bytes) ";
    html += "<a href=\"/download?file=" + urlEncode(fname) + "\">[Download]</a> ";
    html += "<a href=\"/delete?file=" + urlEncode(fname) + "\" onclick=\"return confirm('Delete this file?');\">[Delete]</a>";
    html += "</li>";
    file = root.openNextFile();
  }

  html += "</ul>";

  // 🔹 SPIFFS Usage con Bytes, KB e MB
  html += "<p><b>SPIFFS Usage:</b> " 
        + String(used) + " / " + String(total) + " Bytes used (" + String(free) + " free)<br>"
        + String(usedKB, 2) + " / " + String(totalKB, 2) + " KB used (" + String(freeKB, 2) + " KB free)<br>"
        + String(usedMB, 2) + " / " + String(totalMB, 2) + " MB used (" + String(freeMB, 2) + " MB free)</p>";

  html += "<p><b>Free RAM:</b> " + String(freeHeap) + " Bytes</p>";

  html += "<form method='POST' action='/save_upload' enctype='multipart/form-data'>";
  html += "<input type='file' name='upload'><br><br><input type='submit' value='Upload'>";
  html += "</form></body></html>";

  return html;
}



void handleDebugPage() {
  server.on("/debug", HTTP_GET, [](AsyncWebServerRequest *request){
    String htmlContent = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>Tigo Debug</title>";
    htmlContent += "<style>";
    htmlContent += "body { font-family: Arial; margin: 20px; }";
    htmlContent += "nav { background:#333; padding:10px; border-radius:8px; margin-bottom:20px; }";
    htmlContent += "nav a { color:white; margin-right:15px; text-decoration:none; font-weight:bold; }";
    htmlContent += "nav a:hover { text-decoration:underline; }";
    htmlContent += "table { border-collapse: collapse; width:100%; }";
    htmlContent += "th, td { border:1px solid #ccc; padding:6px; text-align:center; }";
    htmlContent += "th { background:#f0f0f0; }";
    htmlContent += "</style></head><body>";

    htmlContent += "<nav>";
    htmlContent += "<a href='/'>🏠 HOME</a>";
    htmlContent += "<a href='/debug'>🧠 Debug</a>";
    htmlContent += "<a href='/spiffs'>💾 SPIFFS</a>";
    htmlContent += "<a href='/logs'>📜 Logs</a>";
    htmlContent += "<a href='/logviewer.html'>📊 Log Viewer</a>";
    htmlContent += "<a href='#' onclick=\"fetch('/reboot',{method:'POST'}).then(()=>location.reload());\">🔁 Reboot</a>";
    htmlContent += "</nav>";

    htmlContent += "<h1>Tigo Data</h1>"; 
    htmlContent += "<table><tr><th>No.</th><th>PV Node ID</th><th>Addr</th><th>Voltage In</th><th>Voltage Out</th><th>Duty Cycle</th><th>Current In</th><th>Watt</th><th>Temperature</th><th>Slot Counter</th><th>RSSI</th><th>Barcode</th></tr>";

    for (int i = 0; i < deviceCount; i++) {
      htmlContent += "<tr>";
      htmlContent += "<td>" + String(i+1) + "</td>";
      htmlContent += "<td>" + devices[i].pv_node_id + "</td>";
      htmlContent += "<td>" + devices[i].addr + "</td>";
      htmlContent += "<td>" + String(devices[i].voltage_in, 2) + " V</td>";
      htmlContent += "<td>" + String(devices[i].voltage_out, 2) + " V</td>";
      htmlContent += "<td>" + String(devices[i].duty_cycle, HEX) + "</td>";
      htmlContent += "<td>" + String(devices[i].current_in, 2) + " A</td>";
      int watt = round(devices[i].voltage_out * devices[i].current_in);
      htmlContent += "<td>" + String(watt) + " W</td>";
      htmlContent += "<td>" + String(devices[i].temperature, 1) + " &deg;C</td>";
      htmlContent += "<td>" + devices[i].slot_counter + "</td>";
      htmlContent += "<td>" + String(devices[i].rssi) + "</td>";
      htmlContent += "<td>" + devices[i].barcode + "</td>";
      htmlContent += "</tr>";
    }

    htmlContent += "</table><br><h1>Node Table</h1>";
    htmlContent += "<table><tr><th>No.</th><th>Addr</th><th>Long Address</th><th>Checksum</th></tr>";

    for (int i = 0; i < NodeTable_count; i++){
      htmlContent += "<tr>";
      htmlContent += "<td>" + String(i+1) + "</td>";
      htmlContent += "<td>" + NodeTable[i].addr + "</td>";
      htmlContent += "<td>" + NodeTable[i].longAddress + "</td>";
      htmlContent += "<td>" + NodeTable[i].checksum + "</td>";
      htmlContent += "</tr>";
    }

    htmlContent += "</table><br>";
    if (NodeTable_changed) {
      htmlContent += "<b style='color:red;'>⚠️ NodeTable has changed and not saved!</b><br>";
    }

    htmlContent += "<br><a href='/save'><button>Save NodeTable</button></a><br><br>";
    htmlContent += "</body></html>";

    request->send(200, "text/html", htmlContent);
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
