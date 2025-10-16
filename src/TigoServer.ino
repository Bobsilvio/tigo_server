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

#define MAX_SIZE_BYTES (300 * 1024) // 300 KB
#define MAX_LOG_FILE_SIZE 1100000  // in byte
#define MAX_SPIFFS_USAGE 1250000  // soglia oltre cui inizia a cancellare

unsigned long lastLogTime = 0;
const unsigned long logInterval = 30000; // 30 seconds

const char* hostname = "TigoServer";
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
void loopLogging();
bool allBarcodesKnown();
void saveNodeTable();
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
uint16_t computeCRC16CCITT(const uint8_t* data, size_t length) {
  uint16_t crc = 0x8408;  // Initial value
  for (size_t i = 0; i < length; i++) {
    uint8_t index = (crc ^ data[i]) & 0xFF;
    crc = (crc >> 8) ^ CRC_TABLE[index];
  }
  crc = (crc >> 8) | (crc << 8);
  return crc;  // Final XOR (inverted CRC as per CRC-16/CCITT spec)
}

bool verifyChecksum(const String& frame) {
  if (frame.length() < 2) return false;
  String checksumStr = frame.substring(frame.length() - 2);
  uint16_t extractedChecksum = (checksumStr[0] << 8) | checksumStr[1];
  uint16_t computedChecksum = computeCRC16CCITT((const uint8_t*)frame.c_str(), frame.length() - 2);
  return extractedChecksum == computedChecksum;
}

void WebsocketSend(bool send_all = false) {
  StaticJsonDocument<8192> doc; 
  JsonArray array = doc.to<JsonArray>();
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].changed || send_all == true) {
      DeviceData& d = devices[i];
      JsonObject obj = array.createNestedObject();
      obj["id"] = d.barcode != "" ? d.barcode : "mod#" + String(i + 1);
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
}

void loop() {
  currentMillis = millis();
  ArduinoOTA.handle();
  MQTT_Client.loop();
  yield();
  delay(10);

  static String incomingData = "";
  static bool frameStarted = false;
  static bool barcodeSaved = false;

  if (millis() - lastLogTime >= logInterval) {
    loopLogging();  // funzione esistente che scrive il log
    lastLogTime = millis();
  }

  if(WiFi.status() == WL_CONNECTED){
    if(!MQTT_Client.connected()){
      MQTT_Client.connect(hostname, mqtt_user, mqtt_pass);
      WebSerial.println(WiFi.localIP());
      sprintf(address_complete, "%s%s%s", "TIGO/server/", WiFi.localIP().toString().c_str(),"/startup");
      MQTT_Client.publish(address_complete, "Hello");
    }
  }

  if (currentMillis - previousMillis >= interval) {
    previousMillis = currentMillis;
    WebsocketSend();


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
            // Check the length of the raw frame
            //if (frame.length() < 6) { // Must have at least address (2), type (2), checksum (2)
                //Serial.println("Frame too short!");
                //continue; // Skip to the next iteration to avoid processing
            //}
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
    loopLogging();

    static bool barcodeSaved = false;
    if (!barcodeSaved && allBarcodesKnown() && NodeTable_count >= deviceCount) {
      saveNodeTable();
      NodeTable_changed = false;
      barcodeSaved = true;
      Serial.println("NodeTable saved automatically.");
    }
  }
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
      NodeTable[NodeTable_count].checksum = computeTigoCRC4(obj["addr"].as<String>().c_str());
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
  String addr = frame.substring(14,18);
  String node_id = frame.substring(18,22);
  String barcode = frame.substring(40,46);
  bool found = false;
  for(int i=0; i < frame09_count; i++){
    if (frame09[i].barcode = barcode){
      //udate existing
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

  // Temperature (2 bytes): Convert from hex to integer, then scale by 0.1
  int temperature_raw = strtol(frame.substring(25, 28).c_str(), nullptr, 16);
  float temperature = temperature_raw * 0.1;  // Scale factor is 0.1

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

String getDateFilename() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "/log_unknown.json";
  char buf[32];
  strftime(buf, sizeof(buf), "/log_%Y-%m-%d.json", &timeinfo);
  return String(buf);
}

String getTimestampISO8601() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return "1970-01-01T00:00:00";
  char buf[25];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &timeinfo);
  return String(buf);
}

void logAllDevices() {
  String filename = getDateFilename();
  File file = SPIFFS.open(filename, FILE_APPEND);
  if (!file) return;
  String timestamp = getTimestampISO8601();
  for (int i = 0; i < deviceCount; i++) {
    int watt = round(devices[i].voltage_out * devices[i].current_in);
    String line = "{";
    line += "\"ts\":\"" + timestamp + "\",";
    line += "\"barcode\":\"" + devices[i].barcode + "\",";
    line += "\"vin\":" + String(devices[i].voltage_in, 2) + ",";
    line += "\"vout\":" + String(devices[i].voltage_out, 2) + ",";
    line += "\"amp\":" + String(devices[i].current_in, 2) + ",";
    line += "\"watt\":" + String(watt) + ",";
    line += "\"temp\":" + String(devices[i].temperature, 1);
    line += "}\n";
    file.print(line);
  }
  file.close();
}

// Inserire nel setup() dopo la connessione WiFi:
void setupNTP() {
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  struct tm timeinfo;
  while (!getLocalTime(&timeinfo)) {
    Serial.println("Sync NTP...");
    delay(1000);
  }
}

void loopLogging() {
    static unsigned long lastLogTime = 0;
    unsigned long currentMillis = millis();
    if (currentMillis - lastLogTime < 30000) return; // ogni 30s
    lastLogTime = currentMillis;

    String filename = getDateFilename(); // log del giorno
    ensureFreeSpaceExceptToday(filename); // rimuove i log vecchi

    File logFile = SPIFFS.open(filename, FILE_APPEND);
    if (!logFile) {
        Serial.println("❌ Non posso aprire il file log!");
        return;
    }

    const size_t MAX_LOG_SIZE = 1100000; // 1.1 MB massimo
    if (logFile.size() > MAX_LOG_SIZE) {
        logFile.close();
        Serial.println("📦 File log troppo grande, non scrivo più oggi.");
        return;
    }

    String timestamp = getTimestampISO8601();
    for (int i = 0; i < deviceCount; i++) {
        int watt = round(devices[i].voltage_out * devices[i].current_in);
        String line = "{";
        line += "\"ts\":\"" + timestamp + "\",";
        line += "\"barcode\":\"" + devices[i].barcode + "\",";
        line += "\"vin\":" + String(devices[i].voltage_in, 2) + ",";
        line += "\"vout\":" + String(devices[i].voltage_out, 2) + ",";
        line += "\"amp\":" + String(devices[i].current_in, 2) + ",";
        line += "\"watt\":" + String(watt) + ",";
        line += "\"temp\":" + String(devices[i].temperature, 1);
        line += "}\n";
        logFile.print(line);
    }

    logFile.close();
}




bool allBarcodesKnown() {
  for (int i = 0; i < deviceCount; i++) {
    if (devices[i].barcode.length() < 5) {
      return false;
    }
  }
  return true;
}

void ensureFreeSpaceExceptToday(String todayFilename) {
    if (!SPIFFS.begin(true)) {
        Serial.println("❌ SPIFFS non montato!");
        return;
    }

    File root = SPIFFS.open("/");
    if (!root) return;

    File entry;
    while (entry = root.openNextFile()) {
        String name = entry.name();
        if (name.startsWith("/log_") && name.endsWith(".json") && name != todayFilename) {
            SPIFFS.remove(name);
            Serial.println("🗑️ Cancellato log vecchio: " + name);
        }
    }
}

void ensureFreeSpace() {
  size_t used = SPIFFS.usedBytes();
  size_t total = SPIFFS.totalBytes();
  if ((total - used) < 10 * 1024) { // meno di 10 KB liberi
    File root = SPIFFS.open("/");
    String oldestLog;
    time_t oldestTime = time(nullptr);

    File file = root.openNextFile();
    while (file) {
      String name = file.name();
      if (name.startsWith("/log_") && name.endsWith(".json")) {
        struct stat file_stat;
        if (!stat(("/spiffs" + name).c_str(), &file_stat)) {
          if (file_stat.st_mtime < oldestTime) {
            oldestTime = file_stat.st_mtime;
            oldestLog = name;
          }
        }
      }
      file = root.openNextFile();
    }

    if (oldestLog.length()) {
      SPIFFS.remove(oldestLog);
      Serial.println("🗑️ Cancellato file log più vecchio: " + oldestLog);
    }
  }
}
