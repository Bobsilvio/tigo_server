#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
#include <ArduinoOTA.h>
#include <PubSubClient.h>
#include <FS.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <time.h>

const char* hostname = "TigoServer";
const char* TZ_STRING = "CET-1CEST,M3.5.0/2,M10.5.0/3"; // Modifica per il tuo fuso orario
const char* ssid = ""; //SSID
const char* password = ""; //passwort
const char* MQTT_BROKER = ""; //MQTT Server IP
const char* mqtt_user = "";
const char* mqtt_pass = "";

constexpr uint16_t POLYNOMIAL = 0x8408;  // Reversed polynomial (0x1021 reflected)
constexpr size_t TABLE_SIZE = 256;
uint16_t CRC_TABLE[TABLE_SIZE];

#define RX_PIN 3  // Define the RX pin
#define TX_PIN 1  // Define the TX pin

String incomingData = "";
String completeFrame = "";
char address_complete[50];
char* address;

void setupNTP();
void setupWebserver();
void loadNodeTable();
void saveNodeTable();
void loadPanelMap();
void savePanelMap();
void handleRoot();
String generateFileListHTML();
void handleFileUpload();

File uploadFile;

struct DeviceData {
  String pv_node_id;
  String addr;
  float voltage_in;
  float voltage_out;
  byte duty_cycle;
  float current_in;
  float temperature;
  String slot_counter;
  int rssi;
  String barcode;
  bool changed = false;
};
DeviceData devices[100]; // Array to store data for up to 100 devices
int deviceCount = 0; // To keep track of how many devices are being tracked

struct NodeTableData {
  String longAddress;
  String addr;
  String checksum;
};
NodeTableData NodeTable[100];
int NodeTable_count = 0;
bool NodeTable_changed = false;

struct frame09Data {
  String node_id;
  String addr;
  String barcode;
};
frame09Data frame09[100];
int frame09_count = 0;

// ── Panel Map: associazione manuale longAddress → etichetta (es. "A4") ──
struct PanelMapEntry {
  String longAddress;  // es. "04C05B4000B1A688"
  String label;        // es. "A4"
};
PanelMapEntry panelMap[150];
int panelMap_count = 0;

WiFiClient espClient;
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
PubSubClient MQTT_Client(espClient);

unsigned long currentMillis = millis();
unsigned long previousMillis = millis();
unsigned long interval = 2000;

const char crc_char_map[] = "GHJKLMNPRSTVWXYZ";
const uint8_t crc_table[256] = {
  0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,
  0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,
  0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,
  0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,
  0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,
  0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,
  0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4,0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,
  0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,
  0xE,0xD,0x8,0xB,0x2,0x1,0x4,0x7,0x5,0x6,0x3,0x0,0x9,0xA,0xF,0xC,
  0xB,0x8,0xD,0xE,0x7,0x4,0x1,0x2,0x0,0x3,0x6,0x5,0xC,0xF,0xA,0x9,
  0x4,0x7,0x2,0x1,0x8,0xB,0xE,0xD,0xF,0xC,0x9,0xA,0x3,0x0,0x5,0x6,
  0x1,0x2,0x7,0x4,0xD,0xE,0xB,0x8,0xA,0x9,0xC,0xF,0x6,0x5,0x0,0x3,
  0x9,0xA,0xF,0xC,0x5,0x6,0x3,0x0,0x2,0x1,0x4,0x7,0xE,0xD,0x8,0xB,
  0xC,0xF,0xA,0x9,0x0,0x3,0x6,0x5,0x7,0x4,0x1,0x2,0xB,0x8,0xD,0xE,
  0x3,0x0,0x5,0x6,0xF,0xC,0x9,0xA,0x8,0xB,0xE,0xD,0x4,0x7,0x2,0x1,
  0x6,0x5,0x0,0x3,0xA,0x9,0xC,0xF,0xD,0xE,0xB,0x8,0x1,0x2,0x7,0x4
};

char computeTigoCRC4(const char* hexString) {
  uint8_t crc = 0x2;
  while (*hexString && *(hexString + 1)) {
    char byteStr[3] = { hexString[0], hexString[1], '\0' };
    uint8_t byteVal = strtoul(byteStr, NULL, 16);
    crc = crc_table[byteVal ^ (crc << 4)];
    hexString += 2;
  }
  return crc_char_map[crc];
}

void processFrame(String frame);
String removeEscapeSequences(const String& frame);

void hexStringToBytes(const String& hexString, uint8_t* byteArray, size_t length) {
  for (size_t i = 0; i < length; i++) {
    String byteString = hexString.substring(i * 2, i * 2 + 2);
    byteArray[i] = strtol(byteString.c_str(), NULL, 16);
  }
}

// Function to generate CRC table
void generateCRCTable() {
  for (uint16_t i = 0; i < TABLE_SIZE; ++i) {
    uint16_t crc = i;
    for (uint8_t j = 8; j > 0; --j) {
      if (crc & 1) {
        crc = (crc >> 1) ^ POLYNOMIAL;
      } else {
        crc >>= 1;
      }
    }
    CRC_TABLE[i] = crc;
  }
}

// Function to compute CRC-16/CCITT using the precomputed table
// Initial value 0x8408 is intentional per Tigo protocol (non-standard)
uint16_t computeCRC16CCITT(const uint8_t* data, size_t length) {
  uint16_t crc = 0x8408;  // Initial value
  for (size_t i = 0; i < length; i++) {
    uint8_t index = (crc ^ data[i]) & 0xFF;
    crc = (crc >> 8) ^ CRC_TABLE[index];
  }
  return crc;
}

bool verifyChecksum(const String& frame) {
  if (frame.length() < 2) return false;
  // Checksum is stored little-endian in the frame (e.g. 0x85A3 is stored as bytes A3 85)
  const uint8_t* raw = (const uint8_t*)frame.c_str();
  uint16_t extractedChecksum = (uint16_t)raw[frame.length() - 2]
                             | ((uint16_t)raw[frame.length() - 1] << 8);
  uint16_t computedChecksum = computeCRC16CCITT(raw, frame.length() - 2);
  return extractedChecksum == computedChecksum;
}

// Restituisce il longAddress di un dispositivo tramite il suo addr corto (via NodeTable)
String getLongAddress(const String& addr) {
  for (int i = 0; i < NodeTable_count; i++) {
    if (NodeTable[i].addr == addr) return NodeTable[i].longAddress;
  }
  return "";
}

// Restituisce l'etichetta pannello per un dato longAddress ("" se non mappato)
String getPanelLabel(const String& longAddress) {
  if (longAddress == "") return "";
  for (int i = 0; i < panelMap_count; i++) {
    if (panelMap[i].longAddress == longAddress) return panelMap[i].label;
  }
  return "";
}

void WebsocketSend(bool send_all = false) {
  StaticJsonDocument<8192> doc; 
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].changed || send_all == true) {
      DeviceData& d = devices[i];
      JsonObject obj = array.createNestedObject();
      String longAddr = getLongAddress(d.addr);
      String label    = getPanelLabel(longAddr);
      obj["id"]       = label != "" ? label : (d.barcode != "" ? d.barcode : "mod#" + String(i + 1));
      obj["panel"]    = label;      // stringa vuota se non mappato
      obj["longaddr"] = longAddr;   // utile per la pagina /panels
      obj["watt"] = round(d.voltage_out * d.current_in);
      obj["vin"] = d.voltage_in;
      obj["vout"] = d.voltage_out;
      obj["amp"] = d.current_in;
      obj["temp"] = d.temperature;
      obj["rssi"] = d.rssi;
      obj["barcode"] = d.barcode;
      obj["addr"] = d.addr;
      d.changed = false;
    }
  }
  if (array.size() > 0) {
    String json;
    serializeJson(array, json);
    ws.textAll(json);
  }
}

void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    WebSerial.printf("WebSocket client connected: %u\n", client->id());
    WebsocketSend(true);
  } else if (type == WS_EVT_DISCONNECT) {
    WebSerial.printf("WebSocket client disconnected: %u\n", client->id());
  }
}

void initWiFi() {
  WiFi.disconnect();
  //WiFi.reset();
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi ..");
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print('.');
    delay(1000);
  }
  Serial.println(WiFi.localIP());
  WiFi.setAutoReconnect(true);
  WiFi.persistent(true);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);
  Serial1.begin(38400, SERIAL_8N1, RX_PIN, TX_PIN);
  generateCRCTable();  // Generate the CRC lookup table
  initWiFi();
  setupNTP(); 
  setupWebserver();
  WebSerial.begin(&server);
  server.begin();
  MQTT_Client.setServer(MQTT_BROKER, 1883);
  ArduinoOTA.setHostname(hostname);
  ArduinoOTA.begin();
  SPIFFS.begin(true);
  loadNodeTable();
  loadPanelMap();
}

void loop() {
  currentMillis = millis();
  ArduinoOTA.handle();
  MQTT_Client.loop();
  yield();
  delay(10);

  static String incomingData = "";
  static bool frameStarted = false;

  if(WiFi.status() == WL_CONNECTED){
    if(!MQTT_Client.connected()){
      MQTT_Client.connect(hostname, mqtt_user, mqtt_pass);
      WebSerial.println(WiFi.localIP());
      sprintf(address_complete, "%s%s%s", "TIGO/server/", WiFi.localIP().toString().c_str(),"/startup");
      MQTT_Client.publish(address_complete, "Hello");
    }
  }

  // Lettura seriale RS485: eseguita ad ogni ciclo di loop per non perdere byte
  while (Serial1.available()) {
    char incomingByte = Serial1.read();
    incomingData += incomingByte;
    // Check if frame starts
    if (!frameStarted && incomingData.endsWith("\x7E\x08")) {
      WebSerial.println("Paket verpasst!");
    }
    if (!frameStarted && incomingData.endsWith("\x7E\x07")) {
        // Start of a new frame detected
        frameStarted = true;
        incomingData = "\x7E\x07";  // Reset buffer to only contain start delimiter
    }
    // Check if frame ends
    else if (frameStarted && incomingData.endsWith("\x7E\x08")) {
        // End of frame detected
        frameStarted = false; // Reset flag for the next frame
        // Remove start (0x7E 0x07) and end (0x7E 0x08) sequences
        String frame = incomingData.substring(2, incomingData.length() - 2);
        incomingData = ""; // Clear buffer for next potential frame
        // Process the frame
        processFrame(frame);
    }
    // Reset if the buffer grows too large (safety mechanism)
    if (incomingData.length() > 1024) {
        incomingData = "";
        frameStarted = false;
        WebSerial.println("Buffer zu klein!");
    }
  }

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    WebsocketSend();

    // Auto-save NodeTable se modificata (debounce 30 secondi)
    static unsigned long lastAutoSave = 0;
    if (NodeTable_changed && (millis() - lastAutoSave > 30000)) {
      lastAutoSave = millis();
      saveNodeTable();
      NodeTable_changed = false;
      WebSerial.println("✅ NodeTable salvata automaticamente.");
      Serial.println("NodeTable saved automatically.");
    }
  }
}


void loadPanelMap() {
  File file = SPIFFS.open("/panel_map.json", "r");
  if (!file) { Serial.println("No panel_map.json, starting empty."); return; }
  StaticJsonDocument<4096> doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) { Serial.println("panel_map.json: parse error"); return; }
  panelMap_count = 0;
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (panelMap_count < 150) {
      panelMap[panelMap_count].longAddress = obj["longAddress"].as<String>();
      panelMap[panelMap_count].label       = obj["label"].as<String>();
      panelMap_count++;
    }
  }
  Serial.printf("PanelMap caricata: %d voci\n", panelMap_count);
}

void savePanelMap() {
  File file = SPIFFS.open("/panel_map.json", FILE_WRITE);
  if (!file) { Serial.println("Impossibile scrivere panel_map.json"); return; }
  StaticJsonDocument<4096> doc;
  JsonArray arr = doc.to<JsonArray>();
  for (int i = 0; i < panelMap_count; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["longAddress"] = panelMap[i].longAddress;
    obj["label"]       = panelMap[i].label;
  }
  serializeJson(doc, file);
  file.close();
  Serial.printf("PanelMap salvata: %d voci\n", panelMap_count);
  WebSerial.println("✅ PanelMap salvata.");
}

void loadNodeTable() {
  File file = SPIFFS.open("/nodetable.json", "r");
  if (!file) {
    Serial.println("Failed to open file for reading");
    return;
  }
  StaticJsonDocument<8192> doc;
  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println("Failed to parse JSON");
    return;
  }
  NodeTable_count = 0;
  memset(NodeTable, 0, sizeof(NodeTable));
  for (JsonObject obj : doc.as<JsonArray>()) {
    if (NodeTable_count < 100) {
      NodeTable[NodeTable_count].longAddress = obj["longAddress"].as<String>();
      NodeTable[NodeTable_count].addr = obj["addr"].as<String>();
      // CRC4 is computed over the full 64-bit long address, not the short addr
      NodeTable[NodeTable_count].checksum = computeTigoCRC4(obj["longAddress"].as<String>().c_str());
      NodeTable_count++;
      bool found = false;
      for (int i = 0; i < deviceCount; i++) {
        if (devices[i].addr == obj["addr"].as<String>()) {
          devices[i].barcode = obj["longAddress"].as<String>();
          found = true;
          break;
        }
      }
      if (!found && deviceCount < 100) {
        devices[deviceCount].addr = obj["addr"].as<String>();
        devices[deviceCount].barcode = obj["longAddress"].as<String>();
        devices[deviceCount].pv_node_id = "";
        deviceCount++;
      }

    }
  }
  file.close();
}

void saveNodeTable() {
  WebSerial.println("🔄 Saving NodeTable...");

  if (NodeTable_count == 0) {
    WebSerial.println("⚠️ NodeTable is empty, nothing to save.");
    return;
  }

  // Check SPIFFS mounted
  if (!SPIFFS.begin(true)) {
    WebSerial.println("❌ SPIFFS not mounted - cannot save NodeTable.");
    return;
  }

  // Remove old file if exists
  if (SPIFFS.exists("/nodetable.json")) {
    SPIFFS.remove("/nodetable.json");
  }

  File file = SPIFFS.open("/nodetable.json", FILE_WRITE);
  if (!file) {
    WebSerial.println("❌ Failed to open /nodetable.json for writing.");
    return;
  }

  // Document size: aumentato per sicurezza (ogni entry ~ 64-128 bytes)
  StaticJsonDocument<16384> doc; // aumentato da 4096 -> 16384
  JsonArray arr = doc.to<JsonArray>();

  for (int i = 0; i < NodeTable_count; i++) {
    JsonObject obj = arr.createNestedObject();
    obj["addr"] = NodeTable[i].addr;
    obj["longAddress"] = NodeTable[i].longAddress;
    obj["checksum"] = NodeTable[i].checksum;
  }

  size_t written = serializeJson(doc, file); // meno overhead rispetto a pretty
  file.flush();
  file.close();

  if (written == 0) {
    WebSerial.println("❌ Failed to write JSON to file (0 bytes written).");
  } else {
    WebSerial.println("✅ NodeTable saved to /nodetable.json. bytes: " + String(written));
  }
}




String byteToHex(byte b) {
  const char hexChars[] = "0123456789ABCDEF";
  String hexStr;
  hexStr += hexChars[(b >> 4) & 0x0F];
  hexStr += hexChars[b & 0x0F];
  return hexStr;
}

String frameToHexString(const String& frame) {
  String hexStr;
  for (unsigned int i = 0; i < frame.length(); i++) {
    hexStr += byteToHex(frame[i]);
  }
  return hexStr;
}

void process09frame(String frame){
  // Topology report: header is 14 hex chars (type+PVnodeID+shortAddr+DSN+len)
  // Payload layout: shortAddr(4) + PVnodeID(4) + nextHop(4) + ???(4) + longAddr(16) + RSSI(2)
  String addr    = frame.substring(14, 18); // short address of node
  String node_id = frame.substring(18, 22); // PV node ID of node
  String barcode = frame.substring(30, 46); // full 64-bit long address (8 bytes = 16 hex chars)
  bool found = false;
  for(int i=0; i < frame09_count; i++){
    if (frame09[i].barcode == barcode){ // == not =
      //update existing
      frame09[i].node_id = node_id;
      frame09[i].addr = addr;
      found = true;
      break;
    }
  }
  if(!found && frame09_count < 100){
    frame09[frame09_count].node_id = node_id;
    frame09[frame09_count].addr = addr;
    frame09[frame09_count].barcode = barcode;
    frame09_count++;
  }
}

void process27frame(String frame){
  int numEntries = strtol(frame.substring(4, 8).c_str(), NULL, 16);
  //WebSerial.println("Frame 27 erhalten, Einträge: " + String(numEntries));
  int pos = 8;
  for (int i=0; i < numEntries && pos + 20 <= frame.length(); i++){
    String longAddr = frame.substring(pos, pos + 16);
    String addr = frame.substring(pos + 16, pos + 20);
    pos += 20;
    bool found = false;
    for (int j = 0; j < NodeTable_count; j++) {
      if (NodeTable[j].longAddress == longAddr) {
        // Update existing entry
        if(NodeTable[j].addr != addr){
          NodeTable[j].addr = addr;
          NodeTable_changed = true;
        }
        
        found = true;
        break;
      }
    }
    if (!found && NodeTable_count < 100) {
      // Add new entry
      NodeTable[NodeTable_count].longAddress = longAddr;
      NodeTable[NodeTable_count].addr = addr;
      NodeTable_count++;
      NodeTable_changed = true;
    }
  }

}

void processPowerFrame(String frame) {
  // pv_node_id (2 bytes)
  String addr = frame.substring(2, 6);   // Example: "001A"

  // Address (2 bytes)
  String pv_node_id = frame.substring(6, 10);        // Example: "000F"
  
  // Voltage In (2 bytes): Convert from hex to integer, then scale by 0.05
  int voltage_in_raw = strtol(frame.substring(14, 17).c_str(), nullptr, 16);
  float voltage_in = voltage_in_raw * 0.05;   // Scale factor is 0.05

  // Voltage Out (2 bytes): Convert from hex to integer, then scale by 0.10
  int voltage_out_raw = strtol(frame.substring(17, 20).c_str(), nullptr, 16);
  float voltage_out = voltage_out_raw * 0.10;  // Scale factor is 0.10

  // Duty Cycle (1 byte): Convert from hex to integer
  byte duty_cycle = strtol(frame.substring(20, 22).c_str(), nullptr, 16);

  // Current In (2 bytes + 1 nibble): Convert from hex to integer, then scale by 0.005
  int current_in_raw = strtol(frame.substring(22, 25).c_str(), nullptr, 16);  // 2.5 bytes (5 nibbles)
  float current_in = current_in_raw * 0.005;  // Scale factor is 0.005

  // Temperature (12 bit, two's complement): scale by 0.1°C/unit
  // Sign-extend 12-bit value: if bit 11 is set, temperature is negative
  int temperature_raw = strtol(frame.substring(25, 28).c_str(), nullptr, 16);
  if (temperature_raw & 0x800) temperature_raw |= 0xFFFFF000; // sign extend to 32-bit
  float temperature = temperature_raw * 0.1;

  // Slot Counter (4 bytes): Keep this in hex for display
  String slot_counter_value = frame.substring(34, 38);  // 4 bytes (8 hex digits)

  // RSSI (1 byte): Convert from hex to integer

  int rssi = strtol(frame.substring(38, 40).c_str(), nullptr, 16);

  // Find the device in the list or add a new one if not found
  bool found = false;

  String barcode = "";
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].addr == addr) { // && devices[i].pv_node_id == pv_node_id
      // Update existing device data
      if(devices[i].barcode == ""){
        //Finde barcode in NodeTable
        for (int j = 0; j < NodeTable_count; j++) {
          if (NodeTable[j].addr == devices[i].addr) {
            devices[i].barcode = NodeTable[j].longAddress;
            break;
          }
        }
      }
      devices[i].pv_node_id = pv_node_id;
      devices[i].voltage_in = voltage_in;
      devices[i].voltage_out = voltage_out;
      devices[i].duty_cycle = duty_cycle;
      devices[i].current_in = current_in;
      devices[i].temperature = temperature;
      devices[i].slot_counter = slot_counter_value;
      devices[i].rssi = rssi;
      devices[i].changed = true;
      found = true;
      break;
    }
  }
  // If device is new, add it to the list
  if (!found && deviceCount < 100) {
    for (int j = 0; j < NodeTable_count; j++) {
      if (NodeTable[j].addr == addr) {
         barcode = NodeTable[j].longAddress;
        break;
      }
    }
    devices[deviceCount] = {pv_node_id, addr, voltage_in, voltage_out, duty_cycle, current_in, temperature, slot_counter_value, rssi, barcode, true};
    deviceCount++;
  }
}


int calculateHeaderLength(String hexFrame) {
  // First two bytes = 4 characters in hex string (e.g. "00EE")
  // Convert from little-endian: swap bytes
  String lowByte  = hexFrame.substring(0, 2); // "00"
  String highByte = hexFrame.substring(2, 4); // "EE"
  String statusHex = lowByte + highByte;      // "EE00"

  // Convert hex string to integer
  unsigned int status = (unsigned int) strtol(statusHex.c_str(), NULL, 16);

  int length = 2; // Status word is always 2 bytes

  // Bit 0: Rx buffers used (1 byte)
  if ((status & (1 << 0)) == 0) length += 1;
  // Bit 1: Tx buffers free (1 byte)
  if ((status & (1 << 1)) == 0) length += 1;
  // Bit 2: ??? A (2 bytes)
  if ((status & (1 << 2)) == 0) length += 2;
  // Bit 3: ??? B (2 bytes)
  if ((status & (1 << 3)) == 0) length += 2;
  // Bit 4: Packet # high (1 byte)
  if ((status & (1 << 4)) == 0) length += 1;
  // Bit 5: Packet # low (1 byte)
  length += 1;
  // Bit 6: Slot counter (2 bytes)
  length += 2;
  // Bit 7: Packets field — variable length (handle elsewhere)
  length = length * 2;
  return length;
}

void processFrame(String frame) {
    frame = removeEscapeSequences(frame);
    if (verifyChecksum(frame)) {
      //Checksum is valid so remove it
      frame = frame.substring(0, frame.length() - 2);
      String hexFrame = frameToHexString(frame);
      if (hexFrame.length() >= 10) {
        String segment = hexFrame.substring(4, 8); // Get substring from position 5 to 8 (0-based index)
        String slot_counter = "";
        int start_payload = 0;
        //Sortieren nach Typen:
        if (segment == "0149") {
          //WebSerial.println("Gesamt: " + hexFrame);
          //gefunden dann den Header anschauen:
          //String segment = hexFrame.substring(8, 12); // Assuming segment is already defined correctly
          start_payload = 8 + calculateHeaderLength(hexFrame.substring(8, 12));

          //Payload:
          String segment = hexFrame.substring(start_payload, hexFrame.length());
          int pos = 0;
          int i = 0;

          while (pos < segment.length()) {
            // Sicherstellen, dass noch genug Daten für Type + Länge da sind
            if (pos + 14 > segment.length()) {
              Serial.println("Unvollständiges Paket, Abbruch.");
              break;
            }

            // Typ auslesen (2 Zeichen = 1 Byte hex)
            String type = segment.substring(pos, pos + 2);

            // Länge auslesen (2 Zeichen an Position pos+12..pos+14)
            String lengthHex = segment.substring(pos + 12, pos + 14);
            int length = (int) strtol(lengthHex.c_str(), NULL, 16);

            // Gesamtlänge des Pakets in Zeichen (Hex-String: Länge in Bytes * 2)
            int packetLengthInChars = length * 2 + 14;

            // Prüfen, ob das ganze Paket noch im segment ist
            if (pos + packetLengthInChars > segment.length()) {
              WebSerial.println(String(pos + packetLengthInChars) + " Unvollständiges Paket, Abbruch! länge: " + String(segment.length()));
              break;
            }

            // Paket extrahieren
            String packet = segment.substring(pos, pos + packetLengthInChars);
            //WebSerial.println("Paket " + String(i) + ": " + packet);

            if(type == "31"){
              //PowerFrame
              processPowerFrame(packet);
            }else if(type == "09"){
              //PowerFrame
              process09frame(packet);
              //WebSerial.println("09er: " + packet);
            }else if(type == "07"){
              //do nothing
            }else if(type == "18"){
              //do nothing
            }else{
              WebSerial.println("Unbekannt: " + packet);
            }
            // Weiter zum nächsten Paket
            pos += packetLengthInChars;
            i = i + 1;
          }
        }else if(segment == "0B10" || segment == "0B0F"){
          //Command request or Response
          String type = hexFrame.substring(14, 16);
          if(type == "27"){
            process27frame(hexFrame.substring(18, hexFrame.length()));
          }else if(type == "06"){
            //String request
          }else if(type == "07"){
            //String response
          }else if(type == "0E"){
            //Gateway radio configuration response
          }else if(type == "2F"){
            //Network status response
          }else if(type == "22"){
            //Broadcast
          }else if(type == "23"){
            //Broadcast ack
          }else if(type == "41"){
            //unknown
          }else if(type == "2E"){
            //unknown
          }else{
            WebSerial.println("Unknown Type: " + type);
            WebSerial.println(hexFrame);
          }

        }else if(segment == "0148"){
          //Receive request Packet
        }else{
          //ohne 0149
          WebSerial.println("Ohne 0149: " + hexFrame);
        }
      }
      // Further processing here...
    } else {
      WebSerial.println("Checksum Invalid" + frameToHexString(frame));
    }
}

String removeEscapeSequences(const String& frame) {
    String result = "";
    for (size_t i = 0; i < frame.length(); ++i) {
        if (frame[i] == '\x7E' && i < frame.length() - 1) {
            char nextByte = frame[i + 1];
            switch (nextByte) {
                case '\x00': result += '\x7E'; break; // Escaped 7E -> raw 7E
                case '\x01': result += '\x24'; break; // Escaped 7E 01 -> raw 24
                case '\x02': result += '\x23'; break; // Escaped 7E 02 -> raw 23
                case '\x03': result += '\x25'; break; // Escaped 7E 03 -> raw 25
                case '\x04': result += '\xA4'; break; // Escaped 7E 04 -> raw A4
                case '\x05': result += '\xA3'; break; // Escaped 7E 05 -> raw A3
                case '\x06': result += '\xA5'; break; // Escaped 7E 06 -> raw A5
                default:
                    // This default case should not happen according to protocol,
                    // but if it does, add both bytes to result to preserve data
                    result += frame[i];
                    result += nextByte;
                    break;
            }
            i++; // Skip the next byte after escape
        } else {
            result += frame[i];
        }
    }
    return result;
}

// Inserire nel setup() dopo la connessione WiFi:
void setupNTP() {
  setenv("TZ", TZ_STRING, 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("Sync NTP...");
    delay(1000);
  }
}


