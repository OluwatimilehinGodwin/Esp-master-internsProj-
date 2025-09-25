// /* Master — ESP32 (Optimized)
//  * - Fingerprint (Adafruit_Fingerprint) with non-blocking operations
//  * - WiFi + NTP (WAT, UTC+1)
//  * - Supabase REST calls with caching
//  * - ESP-NOW for display communication
//  * - UART communication support
//  */

// #include <Arduino.h>
// #include <esp_now.h>
// #include <WiFi.h>
// #include <WiFiClientSecure.h>
// #include <HTTPClient.h>
// #include <ArduinoJson.h>
// #include <time.h>
// #include <HardwareSerial.h>
// #include <Adafruit_Fingerprint.h>
// #include <vector>

// // ---------------------- USER CONFIG ----------------------
// // Replace with your display/slave MAC (6 bytes)
// uint8_t receiverMAC[] = {0x78, 0xEE, 0x4C, 0x02, 0x17, 0x54};

// // WiFi
// const char* ssid     = "Skill G Innovation";
// const char* password = "INNOV8HUB";

// // NTP / Time (Nigeria WAT = UTC+1)
// const char* ntpServer = "pool.ntp.org";
// const long  gmtOffset_sec = 3600;   // +1 hour
// const int   daylightOffset_sec = 0; // none

// // Supabase (replace with your values)
// const char* supabase_url    = "https://cskdjbpsiupasdhynazt.supabase.co";
// const char* supabase_apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImNza2RqYnBzaXVwYXNkaHluYXp0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTI3NDM2MTIsImV4cCI6MjA2ODMxOTYxMn0.n5V-Jl2njI3AdzWuXcFjFjqCdD4xdqUf7OCcfEA8Ahg";

// // Fingerprint pins (UART1). Adjust if needed.
// #define FP_RX 16
// #define FP_TX 17

// // UART Communication pins (UART2). Adjust if needed.
// #define UART_RX 18
// #define UART_TX 19
// #define UART_BAUD_RATE 115200

// // Buzzer pin (optional)
// #define BUZZER_PIN 13

// // ESP-NOW send interval for main heartbeat/time update
// const unsigned long sendInterval = 7000; // 7 seconds

// // Communication mode: 0 = ESP-NOW only, 1 = UART only, 2 = Both
// #define COMM_MODE 2

// // ---------------------------------------------------------

// // Networking / clients
// WiFiClientSecure tlsClient;
// HTTPClient http;

// // Fingerprint
// HardwareSerial fpSerial(1);
// Adafruit_Fingerprint finger(&fpSerial);

// // UART Communication
// HardwareSerial uartSerial(2);

// // State
// bool wifiConnected = false;
// bool espNowInitialized = false;
// unsigned long lastSendTime = 0;

// // control mode from Supabase: "collection" or "register"
// String mode = "collection";
// int staffidToRegister = -1;

// // Fingerprint state machine
// enum FingerprintState { IDLE, SCANNING, PROCESSING, COMPLETE };
// FingerprintState fpState = IDLE;
// unsigned long lastFpCheck = 0;
// const unsigned long fpCheckInterval = 100; // Check every 100ms

// // Collection cache
// std::vector<int> collectedToday;
// unsigned long lastCollectionRefresh = 0;
// const unsigned long collectionRefreshInterval = 300000; // Refresh every 5 min

// // Timing variables
// unsigned long lastControlPoll = 0;
// const unsigned long controlPollInterval = 5000; // Poll every 5 seconds
// unsigned long lastWifiAttempt = 0;
// const unsigned long wifiReconnectInterval = 30000; // Try reconnect every 30 seconds

// // ---------- Forward declarations ----------
// void initWiFi();
// bool syncTimeWithNTP();
// String hhmmNow();
// String isoTimeNowWAT();
// String getTodayDate();
// bool initializeESP_NOW();
// void sendInstruction(const char* instruction);
// void sendInstructionWithTime(const char* instruction);
// void onSent(const uint8_t *mac_addr, esp_now_send_status_t status);
// void sendViaUART(const char* instruction, bool withTime = true);

// String checkControlMode();
// void updateControlModeToCollection();

// bool staffExists(int staffid);
// bool updateStaffFingerprint(int staffid, int fid);
// int getTagByFingerprint(int fid);
// int getStaffIdByFingerprint(int fid);
// bool hasCollectedToday(int staffid);
// void refreshCollectionCache();
// int findNextAvailableID();
// bool enrollFingerprintFlow(int staffid, unsigned long timeoutMs = 30000);
// void handleFingerprintOperations(unsigned long now);
// void handleCollectionMode();
// void processFingerprint();
// void handleRegistrationMode();
// void successBeep();
// void errorBeep();
// void handleNetworkOperations(unsigned long now);

// // ------------------ ESP-NOW callback --------------------
// void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
//   Serial.print("ESP-NOW Send Status: ");
//   Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
// }

// // ------------------ UART Communication --------------------
// void sendViaUART(const char* instruction, bool withTime) {
//   String message;
//   if (withTime) {
//     String t = hhmmNow();
//     message = String(instruction) + "|" + t;
//   } else {
//     message = String(instruction);
//   }
  
//   uartSerial.println(message);
//   Serial.printf("UART Sent: %s\n", message.c_str());
// }

// // ------------------ Time helpers ------------------------
// bool syncTimeWithNTP() {
//   Serial.println("Syncing time with NTP...");
//   configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
//   unsigned long start = millis();
//   while (millis() - start < 10000) { // 10s wait (reduced from 15s)
//     struct tm timeinfo;
//     if (getLocalTime(&timeinfo)) {
//       Serial.println("NTP time obtained.");
//       char buf[64];
//       strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
//       Serial.print("Local time: ");
//       Serial.println(buf);
//       return true;
//     }
//     delay(200);
//   }
//   Serial.println("NTP sync timeout.");
//   return false;
// }

// String hhmmNow() {
//   struct tm timeinfo;
//   if (!getLocalTime(&timeinfo)) {
//     // fallback to millis-based approximate
//     time_t now = time(nullptr);
//     localtime_r(&now, &timeinfo);
//   }
//   char buf[6];
//   snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
//   return String(buf);
// }

// String isoTimeNowWAT() {
//   time_t now = time(nullptr);
//   struct tm t; localtime_r(&now, &t);
//   char tmp[32];
//   // create ISO like "2025-08-29T14:05:33+01:00"
//   strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &t); // e.g., +0100
//   String s(tmp);
//   if (s.length() >= 5) s = s.substring(0, s.length()-2) + ":" + s.substring(s.length()-2);
//   return s;
// }

// String getTodayDate() {
//   time_t now = time(nullptr);
//   struct tm t; localtime_r(&now, &t);
//   char buf[11];
//   snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year+1900, t.tm_mon+1, t.tm_mday);
//   return String(buf);
// }

// // ------------------ WiFi -------------------------------
// void initWiFi() {
//   WiFi.disconnect(true);
//   delay(100);
//   WiFi.mode(WIFI_STA);
//   WiFi.begin(ssid, password);
//   Serial.printf("Connecting to WiFi '%s' ...\n", ssid);
//   unsigned long start = millis();
//   const unsigned long timeout = 20000; // 20s initial timeout (reduced from 30s)
//   while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
//     Serial.print(".");
//     delay(500);
//   }
//   if (WiFi.status() == WL_CONNECTED) {
//     Serial.println("\nWiFi connected.");
//     Serial.print("IP: "); Serial.println(WiFi.localIP());
//     tlsClient.setInsecure();
//     wifiConnected = true;
//     // Refresh collection cache on WiFi connect
//     refreshCollectionCache();
//   } else {
//     Serial.println("\nWiFi connection failed.");
//     wifiConnected = false;
//   }
// }

// // ------------------ ESP-NOW init & send ----------------
// bool initializeESP_NOW() {
//   if (espNowInitialized) return true;

//   Serial.println("Initializing ESP-NOW...");
//   WiFi.mode(WIFI_STA);
//   Serial.print("MAC: "); Serial.println(WiFi.macAddress());

//   esp_err_t r = esp_now_init();
//   if (r != ESP_OK) {
//     Serial.printf("esp_now_init failed: 0x%X\n", r);
//     return false;
//   }
//   esp_now_register_send_cb(onSent);

//   esp_now_peer_info_t peer = {};
//   memcpy(peer.peer_addr, receiverMAC, 6);
//   peer.channel = 0;
//   peer.encrypt = false;

//   r = esp_now_add_peer(&peer);
//   if (r != ESP_OK && r != ESP_ERR_ESPNOW_EXIST) {
//     Serial.printf("esp_now_add_peer failed: 0x%X\n", r);
//     // Not fatal — may still send if interface accepts raw send
//   } else {
//     Serial.println("ESP-NOW peer added.");
//   }

//   espNowInitialized = true;
//   return true;
// }

// // Builds "instruction|HH:MM" and sends via esp_now
// void sendInstructionWithTime(const char* instruction) {
//   String t = hhmmNow();
//   char msg[64];
//   snprintf(msg, sizeof(msg), "%s|%s", instruction, t.c_str());
  
//   // Send via selected communication method
//   #if COMM_MODE == 0 || COMM_MODE == 2
//   // ESP-NOW
//   if (!espNowInitialized) {
//     initializeESP_NOW();
//   }
//   esp_err_t r = esp_now_send(receiverMAC, (uint8_t*)msg, strlen(msg));
//   Serial.printf("ESP-NOW Sent: %s\n", msg);
//   if (r != ESP_OK) {
//     Serial.printf("esp_now_send error: 0x%X\n", r);
//   }
//   #endif
  
//   #if COMM_MODE == 1 || COMM_MODE == 2
//   // UART
//   sendViaUART(instruction);
//   #endif
// }

// void sendInstruction(const char* instruction) {
//   sendInstructionWithTime(instruction);
// }

// // ------------------ Supabase control --------------------
// String checkControlMode() {
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/control?select=mode,staffid&processed=eq.false&limit=1";
//   if (!h.begin(tlsClient, url)) {
//     Serial.println("control GET: begin failed");
//     return mode;
//   }
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   h.addHeader("Accept", "application/json");

//   int code = h.GET();
//   String payload = h.getString();
//   h.end();

//   if (code != 200) {
//     // keep current mode
//     return mode;
//   }

//   StaticJsonDocument<512> doc;
//   auto err = deserializeJson(doc, payload);
//   if (err || !doc.is<JsonArray>()) {
//     Serial.println("control JSON parse error");
//     return mode;
//   }

//   if (doc.size() == 0) {
//     mode = "collection";
//     staffidToRegister = -1;
//     return mode;
//   }

//   JsonObject first = doc[0];
//   mode = String((const char*)(first["mode"] | "collection"));
//   staffidToRegister = first["staffid"] | -1;
//   Serial.printf("Control → mode=%s, staffid=%d\n", mode.c_str(), staffidToRegister);
//   return mode;
// }

// void updateControlModeToCollection() {
//   if (staffidToRegister <= 0) return;
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/control?staffid=eq." + String(staffidToRegister);
//   if (!h.begin(tlsClient, url)) {
//     Serial.println("control PATCH: begin failed");
//     return;
//   }
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   h.addHeader("Content-Type", "application/json");
//   h.addHeader("Prefer", "return=minimal");

//   StaticJsonDocument<128> doc;
//   doc["processed"] = true;
//   doc["mode"] = "collection";
//   String body; serializeJson(doc, body);

//   int code = h.PATCH(body);
//   Serial.printf("control PATCH → %d\n", code);
//   h.end();

//   staffidToRegister = -1;
//   mode = "collection";
// }

// // ------------------ Supabase helpers --------------------
// bool staffExists(int staffid) {
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/staff?staffid=eq." + String(staffid) + "&select=staffid&limit=1";
//   if (!h.begin(tlsClient, url)) return false;
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   int code = h.GET();
//   String payload = h.getString();
//   h.end();
//   if (code != 200) return false;

//   StaticJsonDocument<256> doc;
//   if (deserializeJson(doc, payload)) return false;
//   return doc.as<JsonArray>().size() > 0;
// }

// bool updateStaffFingerprint(int staffid, int fid) {
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/staff?staffid=eq." + String(staffid);
//   if (!h.begin(tlsClient, url)) return false;
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   h.addHeader("Content-Type", "application/json");
//   h.addHeader("Prefer", "return=minimal");

//   StaticJsonDocument<128> body;
//   body["fingerprintid"] = fid;
//   String out; serializeJson(body, out);

//   int code = h.PATCH(out);
//   h.end();
//   return (code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT);
// }

// int getStaffIdByFingerprint(int fid) {
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/staff?fingerprintid=eq." + String(fid) + "&select=staffid&limit=1";
//   if (!h.begin(tlsClient, url)) return -1;
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   int code = h.GET();
//   String payload = h.getString();
//   h.end();
//   if (code != 200) return -1;

//   StaticJsonDocument<256> doc;
//   if (deserializeJson(doc, payload)) return -1;
//   if (!doc.is<JsonArray>() || doc.size() == 0) return -1;
//   return (int)doc[0]["staffid"];
// }

// int getTagByFingerprint(int fid) {
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/staff?fingerprintid=eq." + String(fid) + "&select=tag&limit=1";
//   if (!h.begin(tlsClient, url)) return -1;
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   int code = h.GET();
//   String payload = h.getString();
//   h.end();
//   if (code != 200) return -1;

//   StaticJsonDocument<256> doc;
//   if (deserializeJson(doc, payload)) return -1;
//   if (!doc.is<JsonArray>() || doc.size() == 0) return -1;
//   return (int)doc[0]["tag"];
// }

// bool hasCollectedToday(int staffid) {
//   // Check local cache first (much faster)
//   for (int id : collectedToday) {
//     if (id == staffid) {
//       return true;
//     }
//   }
//   return false;
// }

// void refreshCollectionCache() {
//   if (WiFi.status() != WL_CONNECTED) return;
  
//   collectedToday.clear();
//   HTTPClient h;
//   String today = getTodayDate();
//   String url = String(supabase_url) + "/rest/v1/food_collections?select=staffid&time_collected=gte." + today + "T00:00:00";
  
//   if (!h.begin(tlsClient, url)) {
//     Serial.println("Collection cache refresh: HTTP begin failed");
//     return;
//   }
  
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  
//   int code = h.GET();
//   if (code == 200) {
//     String payload = h.getString();
//     DynamicJsonDocument doc(2048);
//     if (!deserializeJson(doc, payload)) {
//       for (JsonObject item : doc.as<JsonArray>()) {
//         collectedToday.push_back(item["staffid"].as<int>());
//       }
//       Serial.printf("Collection cache refreshed: %d entries\n", collectedToday.size());
//     }
//   }
  
//   h.end();
//   lastCollectionRefresh = millis();
// }

// // ------------------ Fingerprint ops ----------------------
// int findNextAvailableID() {
//   for (int id = 1; id <= 127; id++) {
//     if (finger.loadModel(id) != FINGERPRINT_OK) {
//       return id;
//     }
//   }
//   return -1;
// }

// // Enrollment: two scans, link to staff record in DB
// bool enrollFingerprintFlow(int staffid, unsigned long timeoutMs) {
//   if (!staffExists(staffid)) {
//     Serial.printf("Staff %d not found.\n", staffid);
//     return false;
//   }

//   int fid = findNextAvailableID();
//   if (fid < 0) { Serial.println("No free fingerprint slots."); return false; }

//   Serial.printf("Enroll staff %d -> fid %d\n", staffid, fid);

//   unsigned long start = millis();
//   uint8_t p;

//   // FIRST SCAN
//   sendInstruction("scan");
//   Serial.println("Place finger (first)...");
//   start = millis();


//   while ((p = finger.getImage()) != FINGERPRINT_OK) {
//     if (millis() - start > timeoutMs) { Serial.println("Timeout first scan."); return false; }
//     delay(50);
//   }
//   if (finger.image2Tz(1) != FINGERPRINT_OK) { Serial.println("image2Tz(1) failed"); return false; }
//   sendInstruction("successful"); successBeep();
//   delay(600);


//   // Wait remove
//   Serial.println("Remove finger...");
//   start = millis();
//   while (finger.getImage() != FINGERPRINT_NOFINGER) {
//     if (millis() - start > timeoutMs) { Serial.println("Timeout remove."); return false; }
//     delay(50);
//   }

//   // SECOND SCAN
//   sendInstruction("scan");
//   Serial.println("Place same finger (second)...");
//   start = millis();
//   while ((p = finger.getImage()) != FINGERPRINT_OK) {
//     if (millis() - start > timeoutMs) { Serial.println("Timeout second scan."); return false; }
//     delay(50);
//   }
//   if (finger.image2Tz(2) != FINGERPRINT_OK) { Serial.println("image2Tz(2) failed"); return false; }

//   if (finger.createModel() != FINGERPRINT_OK) { Serial.println("createModel mismatch."); return false; }
//   if (finger.storeModel(fid) != FINGERPRINT_OK) { Serial.println("storeModel failed."); return false; }

//   sendInstruction("successful"); successBeep();
//   Serial.println("Enrollment success, updating DB...");

//   if (!updateStaffFingerprint(staffid, fid)) {
//     Serial.println("DB link failed.");
//     errorBeep();
//     return false;
//   }

//   delay(800);
//   sendInstruction("main");
//   return true;
// }

// // ------------------ Fingerprint State Machine --------------------
// void handleFingerprintOperations(unsigned long now) {
//   if (now - lastFpCheck < fpCheckInterval) return;
//   lastFpCheck = now;
  
//   if (mode == "register" && staffidToRegister > 0) {
//     handleRegistrationMode();
//   } else {
//     handleCollectionMode();
//   }
// }

// void handleCollectionMode() {
//   switch (fpState) {
//     case IDLE:
//       if (finger.getImage() == FINGERPRINT_OK) {
//         fpState = SCANNING;
//         sendInstruction("scan");
//         Serial.println("Finger detected, processing...");
//       }
//       break;
      
//     case SCANNING:
//       processFingerprint();
//       break;
      
//     case COMPLETE:
//       // Reset after completion
//       delay(800);
//       sendInstruction("main");
//       fpState = IDLE;
//       break;
      
//     case PROCESSING:
//       // Processing in progress, do nothing
//       break;
//   }
// }

// void processFingerprint() {
//   fpState = PROCESSING;
  
//   uint8_t p = finger.image2Tz();
//   if (p != FINGERPRINT_OK) {
//     Serial.printf("image2Tz error: %u\n", p);
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }

//   p = finger.fingerFastSearch();
//   if (p != FINGERPRINT_OK) {
//     Serial.println("No match.");
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }

//   int fid = finger.fingerID;
//   Serial.printf("Fingerprint match: fid=%d confidence=%d\n", fid, finger.confidence);

//   int tag = getTagByFingerprint(fid);
//   if (tag < 0) {
//     Serial.println("Fingerprint has no staff record (tag).");
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }

//   int staffid = getStaffIdByFingerprint(fid);
//   if (staffid < 0) {
//     Serial.println("No staffid for fingerprint.");
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }

//   if (hasCollectedToday(staffid)) {
//     Serial.printf("Staff %d already collected today.\n", staffid);
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }

//   // Log collection to Supabase
//   HTTPClient h;
//   String url = String(supabase_url) + "/rest/v1/food_collections";
//   if (!h.begin(tlsClient, url)) {
//     Serial.println("HTTP begin failed (log POST).");
//     errorBeep();
//     sendInstruction("unsuccessful");
//     fpState = COMPLETE;
//     return;
//   }
//   h.addHeader("apikey", supabase_apikey);
//   h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
//   h.addHeader("Content-Type", "application/json");
//   h.addHeader("Prefer", "return=minimal");

//   String ts = isoTimeNowWAT();
//   StaticJsonDocument<256> body;
//   body["fingerprintid"]  = fid;
//   body["tag"]            = tag;
//   body["staffid"]        = staffid;
//   body["time_collected"] = ts;
//   String payload; serializeJson(body, payload);

//   int code = h.POST(payload);
//   h.end();

//   if (code == HTTP_CODE_CREATED) {
//     Serial.printf("Collection logged: staff %d @ %s\n", staffid, ts.c_str());
//     successBeep();
//     sendInstruction("successful");
//     // Add to local cache
//     collectedToday.push_back(staffid);
//   } else {
//     Serial.printf("Log failed: %d\n", code);
//     errorBeep();
//     sendInstruction("unsuccessful");
//   }

//   fpState = COMPLETE;
// }

// void handleRegistrationMode() {
//   Serial.printf("Entering registration mode for staff %d\n", staffidToRegister);
//   sendInstruction("main"); // ensure UI stable before starting
//   bool ok = enrollFingerprintFlow(staffidToRegister, 30000);
//   if (!ok) {
//     Serial.println("Enrollment failed or timed out.");
//     errorBeep();
//     sendInstruction("unsuccessful");
//     delay(800);
//     sendInstruction("main");
//   }
//   updateControlModeToCollection();
//   fpState = IDLE;
// }

// // ------------------ Network Operations --------------------
// void handleNetworkOperations(unsigned long now) {
//   // Check control mode every 5 seconds
//   if (now - lastControlPoll >= controlPollInterval) {
//     lastControlPoll = now;
//     checkControlMode();
//   }
  
//   // Refresh collection cache every minute
//   if (now - lastCollectionRefresh >= collectionRefreshInterval) {
//     refreshCollectionCache();
//   }
  
//   // Handle WiFi reconnection
//   if (WiFi.status() != WL_CONNECTED) {
//     if (now - lastWifiAttempt >= wifiReconnectInterval) {
//       lastWifiAttempt = now;
//       Serial.println("Attempting WiFi reconnect...");
//       initWiFi();
//     }
//   }
// }

// // ------------------ Simple beeps -------------------------
// void successBeep() {
//   #ifdef BUZZER_PIN
//   tone(BUZZER_PIN, 1000, 150);
//   #endif
// }

// void errorBeep() {
//   #ifdef BUZZER_PIN
//   tone(BUZZER_PIN, 500, 300);
//   #endif
// }

// // ------------------ Setup & Loop ------------------------
// void setup() {
//   Serial.begin(115200);
//   delay(100);

//   pinMode(BUZZER_PIN, OUTPUT);

//   // Fingerprint sensor UART
//   fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
//   finger.begin(57600);
//   if (!finger.verifyPassword()) {
//     Serial.println("Fingerprint sensor not found or wrong password!");
//     while (true) { delay(1000); }
//   } else {
//     Serial.println("Fingerprint sensor ready.");
//   }

//   // UART Communication setup
//   uartSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX, UART_TX);
//   Serial.println("UART communication ready.");

//   // WiFi + time
//   initWiFi();
//   if (wifiConnected) {
//     syncTimeWithNTP();
//   } else {
//     Serial.println("WiFi not connected. Attempting later.");
//   }

//   // ESP-NOW init (only if needed)
//   #if COMM_MODE == 0 || COMM_MODE == 2
//   initializeESP_NOW();
//   #endif

//   // Initial instruction (main with time)
//   sendInstruction("main");
// }

// void loop() {
//   unsigned long now = millis();
  
//   // Handle network operations (non-blocking)
//   handleNetworkOperations(now);
  
//   // Handle fingerprint operations (non-blocking)
//   handleFingerprintOperations(now);
  
//   // Handle ESP-NOW heartbeat
//   if (now - lastSendTime >= sendInterval) {
//     lastSendTime = now;
//     sendInstruction("main");
//   }
  
//   delay(10); // Short yield
// }