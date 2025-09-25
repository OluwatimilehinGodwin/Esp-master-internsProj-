/* Master — ESP32 (Non-blocking fingerprint + queued network)
 * - Fingerprint (Adafruit_Fingerprint) with non-blocking state machines
 * - WiFi + NTP (WAT, UTC+1)
 * - Supabase REST calls queued and processed asynchronously
 * - UART communication support
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <HardwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <vector>

// ---------------------- USER CONFIG ----------------------
// WiFi
const char* ssid     = "Skill G Innovation";
const char* password = "INNOV8HUB";

// NTP / Time (Nigeria WAT = UTC+1)
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;   // +1 hour
const int   daylightOffset_sec = 0; // none

// Supabase (replace with your values)
const char* supabase_url    = "https://cskdjbpsiupasdhynazt.supabase.co";
const char* supabase_apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImNza2RqYnBzaXVwYXNkaHluYXp0Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NTI3NDM2MTIsImV4cCI6MjA2ODMxOTYxMn0.n5V-Jl2njI3AdzWuXcFjFjqCdD4xdqUf7OCcfEA8Ahg";

// Fingerprint pins (UART1). Adjust if needed.
#define FP_RX 16
#define FP_TX 17

// UART Communication pins (UART2). Adjust if needed.
#define UART_RX 18
#define UART_TX 19
#define UART_BAUD_RATE 115200

// Buzzer pin (optional)
#define BUZZER_PIN 13

// UART send interval for main heartbeat/time update
const unsigned long sendInterval = 7000; // 7 seconds

// ---------------------------------------------------------

// Networking / clients
WiFiClientSecure tlsClient;
HTTPClient http;

// Fingerprint
HardwareSerial fpSerial(1);
Adafruit_Fingerprint finger(&fpSerial);

// UART Communication
HardwareSerial uartSerial(2);

// State
bool wifiConnected = false;
unsigned long lastSendTime = 0;

// control mode from Supabase: "collection" or "register"
String mode = "collection";
int staffidToRegister = -1;

// Fingerprint state machine (collection)
enum FingerprintState { IDLE, SCANNING, PROCESSING, COMPLETE };
FingerprintState fpState = IDLE;
unsigned long lastFpCheck = 0;
const unsigned long fpCheckInterval = 100; // Check every 100ms

// Enrollment state machine (non-blocking)
enum EnrollStep {
  ENROLL_IDLE = 0,
  ENROLL_WAIT_FIRST,
  ENROLL_FIRST_CAPTURED,
  ENROLL_WAIT_REMOVE,
  ENROLL_WAIT_SECOND,
  ENROLL_SECOND_CAPTURED,
  ENROLL_CREATE_MODEL,
  ENROLL_STORE_MODEL,
  ENROLL_DONE,
  ENROLL_FAILED
};
EnrollStep enrollStep = ENROLL_IDLE;
int enrollStaffId = -1;
int enrollFid = -1;
unsigned long enrollStartTime = 0;
const unsigned long enrollTimeout = 30000; // 30s for each waiting phase
unsigned long enrollStepTime = 0;

// Collection cache
std::vector<int> collectedToday;
unsigned long lastCollectionRefresh = 0;
const unsigned long collectionRefreshInterval = 300000; // Refresh every 5 min

// Timing variables
unsigned long lastControlPoll = 0;
const unsigned long controlPollInterval = 5000; // Poll every 5 seconds
unsigned long lastWifiAttempt = 0;
const unsigned long wifiReconnectInterval = 30000; // Try reconnect every 30 seconds

// Pending network log queue (non-blocking)
std::vector<String> pendingLogs; // JSON payload strings

// ---------- Forward declarations ----------
void initWiFi();
bool syncTimeWithNTP();
String hhmmNow();
String isoTimeNowWAT();
String getTodayDate();
void sendInstruction(const char* instruction);
void sendInstructionWithTime(const char* instruction);
void sendViaUART(const char* instruction, bool withTime = true);

String checkControlMode();
void updateControlModeToCollection();

bool staffExists(int staffid);
bool updateStaffFingerprint(int staffid, int fid);
int getTagByFingerprint(int fid);
int getStaffIdByFingerprint(int fid);
bool hasCollectedToday(int staffid);
void refreshCollectionCache();
int findNextAvailableID();
void handleFingerprintOperations(unsigned long now);
void handleCollectionMode(unsigned long now);
void startEnrollmentNonBlocking(int staffid);
void handleEnrollmentNonBlocking(unsigned long now);
void queueCollectionLog(int fid, int tag, int staffid);
void handleNetworkOperations(unsigned long now);
void sendPendingLogs();
void successBeep();
void errorBeep();

// ------------------ UART Communication --------------------
void sendViaUART(const char* instruction, bool withTime) {
  String message;
  if (withTime) {
    String t = hhmmNow();
    message = String(instruction) + "|" + t;
  } else {
    message = String(instruction);
  }
  
  uartSerial.println(message);
  Serial.printf("UART Sent: %s\n", message.c_str());
}

// ------------------ Time helpers ------------------------
bool syncTimeWithNTP() {
  Serial.println("Syncing time with NTP...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  unsigned long start = millis();
  while (millis() - start < 10000) { // 10s wait
    struct tm timeinfo;
    if (getLocalTime(&timeinfo)) {
      Serial.println("NTP time obtained.");
      char buf[64];
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &timeinfo);
      Serial.print("Local time: ");
      Serial.println(buf);
      return true;
    }
    delay(200);
  }
  Serial.println("NTP sync timeout.");
  return false;
}

String hhmmNow() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    time_t now = time(nullptr);
    localtime_r(&now, &timeinfo);
  }
  char buf[6];
  snprintf(buf, sizeof(buf), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);
  return String(buf);
}

String isoTimeNowWAT() {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  char tmp[32];
  strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &t); // e.g., +0100
  String s(tmp);
  if (s.length() >= 5) s = s.substring(0, s.length()-2) + ":" + s.substring(s.length()-2);
  return s;
}

String getTodayDate() {
  time_t now = time(nullptr);
  struct tm t; localtime_r(&now, &t);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", t.tm_year+1900, t.tm_mon+1, t.tm_mday);
  return String(buf);
}

// ------------------ WiFi -------------------------------
void initWiFi() {
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi '%s' ...\n", ssid);
  unsigned long start = millis();
  const unsigned long timeout = 20000; // 20s initial timeout
  while (WiFi.status() != WL_CONNECTED && millis() - start < timeout) {
    Serial.print(".");
    delay(500);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected.");
    Serial.print("IP: "); Serial.println(WiFi.localIP());
    tlsClient.setInsecure();
    wifiConnected = true;
    refreshCollectionCache(); // refresh immediately
  } else {
    Serial.println("\nWiFi connection failed.");
    wifiConnected = false;
  }
}

// Builds "instruction|HH:MM" and sends via UART
void sendInstructionWithTime(const char* instruction) {
  String t = hhmmNow();
  char msg[64];
  snprintf(msg, sizeof(msg), "%s|%s", instruction, t.c_str());
  
  // Send via UART
  sendViaUART(instruction);
}

void sendInstruction(const char* instruction) {
  sendInstructionWithTime(instruction);
}

// ------------------ Supabase control --------------------
String checkControlMode() {
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/control?select=mode,staffid&processed=eq.false&limit=1";
  if (!h.begin(tlsClient, url)) {
    Serial.println("control GET: begin failed");
    return mode;
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  h.addHeader("Accept", "application/json");

  int code = h.GET();
  String payload = h.getString();
  h.end();

  if (code != 200) {
    // keep current mode
    return mode;
  }

  StaticJsonDocument<512> doc;
  auto err = deserializeJson(doc, payload);
  if (err || !doc.is<JsonArray>()) {
    Serial.println("control JSON parse error");
    return mode;
  }

  if (doc.size() == 0) {
    mode = "collection";
    staffidToRegister = -1;
    return mode;
  }

  JsonObject first = doc[0];
  mode = String((const char*)(first["mode"] | "collection"));
  staffidToRegister = first["staffid"] | -1;
  Serial.printf("Control → mode=%s, staffid=%d\n", mode.c_str(), staffidToRegister);
  return mode;
}

void updateControlModeToCollection() {
  if (staffidToRegister <= 0) return;
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/control?staffid=eq." + String(staffidToRegister);
  if (!h.begin(tlsClient, url)) {
    Serial.println("control PATCH: begin failed");
    return;
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<128> doc;
  doc["processed"] = true;
  doc["mode"] = "collection";
  String body; serializeJson(doc, body);

  int code = h.PATCH(body);
  Serial.printf("control PATCH → %d\n", code);
  h.end();

  staffidToRegister = -1;
  mode = "collection";
}

// ------------------ Supabase helpers (used by non-blocking sender) --------------------
bool staffExists(int staffid) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?staffid=eq." + String(staffid) + "&select=staffid&limit=1";
  if (!h.begin(tlsClient, url)) return false;
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  int code = h.GET();
  String payload = h.getString();
  h.end();
  if (code != 200) return false;

  DynamicJsonDocument doc(256);
  if (deserializeJson(doc, payload)) return false;
  return doc.as<JsonArray>().size() > 0;
}

// NOTE: This is still synchronous; called from network worker (non-critical for fingerprint timing)
bool updateStaffFingerprint(int staffid, int fid) {
  if (WiFi.status() != WL_CONNECTED) return false;
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?staffid=eq." + String(staffid);
  if (!h.begin(tlsClient, url)) return false;
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<128> body;
  body["fingerprintid"] = fid;
  String out; serializeJson(body, out);

  int code = h.PATCH(out);
  h.end();
  return (code == HTTP_CODE_OK || code == HTTP_CODE_NO_CONTENT);
}

int getStaffIdByFingerprint(int fid) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?fingerprintid=eq." + String(fid) + "&select=staffid&limit=1";
  if (!h.begin(tlsClient, url)) return -1;
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  int code = h.GET();
  String payload = h.getString();
  h.end();
  if (code != 200) return -1;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload)) return -1;
  if (!doc.is<JsonArray>() || doc.size() == 0) return -1;
  return (int)doc[0]["staffid"];
}

int getTagByFingerprint(int fid) {
  if (WiFi.status() != WL_CONNECTED) return -1;
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?fingerprintid=eq." + String(fid) + "&select=tag&limit=1";
  if (!h.begin(tlsClient, url)) return -1;
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  int code = h.GET();
  String payload = h.getString();
  h.end();
  if (code != 200) return -1;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, payload)) return -1;
  if (!doc.is<JsonArray>() || doc.size() == 0) return -1;
  return (int)doc[0]["tag"];
}

bool hasCollectedToday(int staffid) {
  for (int id : collectedToday) {
    if (id == staffid) {
      return true;
    }
  }
  return false;
}

void refreshCollectionCache() {
  if (WiFi.status() != WL_CONNECTED) return;
  
  collectedToday.clear();
  HTTPClient h;
  String today = getTodayDate();
  String url = String(supabase_url) + "/rest/v1/food_collections?select=staffid&time_collected=gte." + today + "T00:00:00";
  
  if (!h.begin(tlsClient, url)) {
    Serial.println("Collection cache refresh: HTTP begin failed");
    return;
  }
  
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  
  int code = h.GET();
  if (code == 200) {
    String payload = h.getString();
    DynamicJsonDocument doc(4096);
    auto err = deserializeJson(doc, payload);
    if (!err) {
      for (JsonObject item : doc.as<JsonArray>()) {
        collectedToday.push_back(item["staffid"].as<int>());
      }
      Serial.printf("Collection cache refreshed: %d entries\n", collectedToday.size());
    } else {
      Serial.println("Collection cache parse error");
    }
  } else {
    Serial.printf("Collection cache refresh failed: %d\n", code);
  }
  
  h.end();
  lastCollectionRefresh = millis();
}

// ------------------ Fingerprint ops (utilities) ----------------------
int findNextAvailableID() {
  for (int id = 1; id <= 127; id++) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      return id;
    }
  }
  return -1;
}

// Queue a collection log JSON payload for asynchronous sending
void queueCollectionLog(int fid, int tag, int staffid) {
  StaticJsonDocument<256> body;
  body["fingerprintid"]  = fid;
  body["tag"]            = tag;
  body["staffid"]        = staffid;
  body["time_collected"] = isoTimeNowWAT();
  String payload; serializeJson(body, payload);

  pendingLogs.push_back(payload);
  Serial.printf("Queued log for staff %d (fid %d). Pending logs: %d\n", staffid, fid, (int)pendingLogs.size());
}

// ------------------ Fingerprint State Machine (non-blocking) --------------------
void handleFingerprintOperations(unsigned long now) {
  // run every fpCheckInterval
  if (now - lastFpCheck < fpCheckInterval) return;
  lastFpCheck = now;

  // If register mode requested from server and we aren't already enrolling -> start enroll
  if (mode == "register" && staffidToRegister > 0 && enrollStep == ENROLL_IDLE) {
    startEnrollmentNonBlocking(staffidToRegister);
    return; // enrollment will be handled in separate handler
  }

  // If currently in enrollment flow, handle it
  if (enrollStep != ENROLL_IDLE && enrollStep != ENROLL_DONE && enrollStep != ENROLL_FAILED) {
    handleEnrollmentNonBlocking(now);
    return;
  }

  // Otherwise handle collection mode
  handleCollectionMode(now);
}

void handleCollectionMode(unsigned long now) {
  switch (fpState) {
    case IDLE:
      // check quickly for finger presence (non-blocking)
      if (finger.getImage() == FINGERPRINT_OK) {
        fpState = SCANNING;
        sendInstruction("scan");
        Serial.println("Finger detected - capture starting...");
      }
      break;

    case SCANNING:
      // Convert image to char buffer (non-blocking single check)
      {
        uint8_t p = finger.image2Tz(); // some Adafruit libs default to slot 1 here
        if (p != FINGERPRINT_OK) {
          Serial.printf("image2Tz error in collection: %u\n", p);
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }
        fpState = PROCESSING;
      }
      break;

    case PROCESSING:
      // Search template
      {
        uint8_t p = finger.fingerFastSearch();
        if (p != FINGERPRINT_OK) {
          Serial.println("No match (collection).");
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }
        int fid = finger.fingerID;
        Serial.printf("Fingerprint match: fid=%d confidence=%d\n", fid, finger.confidence);

        // Retrieve tag and staffid via network (these are synchronous operations; to avoid blocking the scanner
        // we only enqueue the log and handle staff/tag retrieval in the network worker if desired).
        // But getTagByFingerprint/getStaffIdByFingerprint use HTTPClient and will block if WiFi used.
        // Instead, fetch tag/staffid only if WiFi is connected; otherwise mark as failure.
        if (WiFi.status() != WL_CONNECTED) {
          Serial.println("WiFi not connected — cannot verify staff. Queue unsuccessful.");
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        int tag = getTagByFingerprint(fid);
        if (tag < 0) {
          Serial.println("Fingerprint has no staff record (tag).");
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        int staffid = getStaffIdByFingerprint(fid);
        if (staffid < 0) {
          Serial.println("No staffid for fingerprint.");
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        if (hasCollectedToday(staffid)) {
          Serial.printf("Staff %d already collected today.\n", staffid);
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        // Queue the log for asynchronous HTTP POST (non-blocking for scanner)
        queueCollectionLog(fid, tag, staffid);

        // Mark cached as collected immediately to avoid duplicate scans while queue is pending
        collectedToday.push_back(staffid);

        successBeep();
        sendInstruction("successful");
        fpState = COMPLETE;
      }
      break;

    case COMPLETE:
      // short non-blocking dwell, then go back to IDLE
      static unsigned long completeStarted = 0;
      if (completeStarted == 0) completeStarted = millis();
      if (millis() - completeStarted >= 800) {
        sendInstruction("main");
        fpState = IDLE;
        completeStarted = 0;
      }
      break;
  }
}

// ------------------ Enrollment (non-blocking) --------------------
void startEnrollmentNonBlocking(int staffid) {
  // Start the non-blocking enrollment flow
  if (!staffExists(staffid)) {
    Serial.printf("Enrollment requested but staff %d does not exist.\n", staffid);
    errorBeep();
    sendInstruction("unsuccessful");
    staffidToRegister = -1;
    mode = "collection";
    return;
  }

  enrollStaffId = staffid;
  enrollFid = findNextAvailableID();
  if (enrollFid < 0) {
    Serial.println("No free fingerprint slots available.");
    errorBeep();
    sendInstruction("unsuccessful");
    staffidToRegister = -1;
    mode = "collection";
    return;
  }

  enrollStep = ENROLL_WAIT_FIRST;
  enrollStartTime = millis();
  enrollStepTime = millis();
  Serial.printf("Enroll start: staff %d -> fid %d\n", enrollStaffId, enrollFid);
  sendInstruction("scan");
  Serial.println("Place finger (first)...");
}

void handleEnrollmentNonBlocking(unsigned long now) {
  switch (enrollStep) {
    case ENROLL_WAIT_FIRST: {
      // Wait until first capture is available or timeout
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        // capture first
        uint8_t r = finger.image2Tz(1); // buffer 1
        if (r == FINGERPRINT_OK) {
          enrollStep = ENROLL_FIRST_CAPTURED;
          enrollStepTime = millis();
          sendInstruction("successful"); successBeep();
          Serial.println("First capture OK. Remove finger...");
          // prepare to wait for finger removal
          enrollStep = ENROLL_WAIT_REMOVE;
          enrollStepTime = millis();
        } else {
          Serial.printf("image2Tz(1) failed: %u\n", r);
          enrollStep = ENROLL_FAILED;
        }
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for first finger.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_WAIT_REMOVE: {
      // Wait until sensor reports NOFINGER
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        Serial.println("Finger removed. Prompting for second scan...");
        sendInstruction("scan");
        enrollStep = ENROLL_WAIT_SECOND;
        enrollStepTime = millis();
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for finger removal.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_WAIT_SECOND: {
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        uint8_t r = finger.image2Tz(2); // buffer 2
        if (r == FINGERPRINT_OK) {
          enrollStep = ENROLL_SECOND_CAPTURED;
          enrollStepTime = millis();
          sendInstruction("successful"); successBeep();
          Serial.println("Second capture OK. Creating model...");
        } else {
          Serial.printf("image2Tz(2) failed: %u\n", r);
          enrollStep = ENROLL_FAILED;
        }
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for second scan.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_SECOND_CAPTURED: {
      // create model
      uint8_t r = finger.createModel();
      if (r == FINGERPRINT_OK) {
        enrollStep = ENROLL_CREATE_MODEL;
        Serial.println("Model created. Storing...");
      } else {
        Serial.printf("createModel failed: %u\n", r);
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_CREATE_MODEL: {
      if (finger.storeModel(enrollFid) == FINGERPRINT_OK) {
        enrollStep = ENROLL_STORE_MODEL;
        Serial.printf("Stored model at slot %d\n", enrollFid);
      } else {
        Serial.println("storeModel failed.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_STORE_MODEL: {
      // Update DB asynchronously: we will attempt now (network worker will succeed if WiFi ok).
      // But to keep DB consistent faster, we try to call updateStaffFingerprint here (it's synchronous).
      bool ok = false;
      if (WiFi.status() == WL_CONNECTED) {
        ok = updateStaffFingerprint(enrollStaffId, enrollFid);
      } else {
        ok = false;
      }

      if (!ok) {
        Serial.println("DB link failed immediately. Scheduling retry via pendingLogs.");
        // If DB update failed because of no WiFi or PATCH failure, queue a retry payload with special endpoint.
        // We'll create a payload to indicate we need to update staff fingerprint later.
        StaticJsonDocument<128> body;
        body["op"] = "update_staff_fingerprint";
        body["staffid"] = enrollStaffId;
        body["fingerprintid"] = enrollFid;
        String payload; serializeJson(body, payload);
        pendingLogs.push_back(payload);
      } else {
        Serial.println("DB updated with fingerprint id.");
      }

      sendInstruction("successful");
      successBeep();
      enrollStep = ENROLL_DONE;
      enrollStepTime = millis();
      break;
    }

    case ENROLL_DONE: {
      // Non-blocking post-enroll delay, then return to collection mode
      if (millis() - enrollStepTime >= 800) {
        sendInstruction("main");
        enrollStep = ENROLL_IDLE;
        staffidToRegister = -1;
        mode = "collection";
        enrollStaffId = -1;
        enrollFid = -1;
        Serial.println("Enrollment completed - returning to collection mode.");
      }
      break;
    }

    case ENROLL_FAILED: {
      Serial.println("Enrollment failed. Sending unsuccessful and returning to collection mode.");
      errorBeep();
      sendInstruction("unsuccessful");
      // small non-blocking wait
      enrollStepTime = millis();
      enrollStep = ENROLL_DONE; // route to DONE so main resets
      break;
    }

    default:
      break;
  } // end switch
}

// ------------------ Network Operations (process queued logs) --------------------
void handleNetworkOperations(unsigned long now) {
  // Check control mode every 5 seconds
  if (now - lastControlPoll >= controlPollInterval) {
    lastControlPoll = now;
    checkControlMode();
  }
  
  // Refresh collection cache every interval
  if (now - lastCollectionRefresh >= collectionRefreshInterval) {
    refreshCollectionCache();
  }
  
  // Handle WiFi reconnection
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiConnected) { wifiConnected = false; }
    if (now - lastWifiAttempt >= wifiReconnectInterval) {
      lastWifiAttempt = now;
      Serial.println("Attempting WiFi reconnect...");
      initWiFi();
    }
  } else {
    wifiConnected = true;
  }

  // Try sending queued logs if connected
  if (WiFi.status() == WL_CONNECTED && pendingLogs.size() > 0) {
    sendPendingLogs();
  }
  
  // Handle other periodic send (UART heartbeat)
  if (now - lastSendTime >= sendInterval) {
    lastSendTime = now;
    sendInstruction("main");
  }
}

// Send pending logs from pendingLogs vector. Each entry is either:
// - A collection JSON to POST to /rest/v1/food_collections
// - Or a special op like update_staff_fingerprint (we detect via JSON "op")
void sendPendingLogs() {
  // We'll iterate and attempt to send each; on success we remove it; on failure we keep for later.
  for (int i = (int)pendingLogs.size() - 1; i >= 0; --i) {
    String payload = pendingLogs[i];
    DynamicJsonDocument doc(512);
    DeserializationError err = deserializeJson(doc, payload);
    if (err) {
      // treat as regular collection entry if cannot parse (fallback)
      Serial.println("Pending log parse error, attempting as collection payload.");
      // attempt to post directly
      HTTPClient h;
      String url = String(supabase_url) + "/rest/v1/food_collections";
      if (!h.begin(tlsClient, url)) {
        Serial.println("HTTP begin failed for pending log.");
        continue;
      }
      h.addHeader("apikey", supabase_apikey);
      h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
      h.addHeader("Content-Type", "application/json");
      h.addHeader("Prefer", "return=minimal");
      int code = h.POST(payload);
      h.end();
      if (code == HTTP_CODE_CREATED) {
        Serial.println("Pending collection posted successfully.");
        pendingLogs.erase(pendingLogs.begin() + i);
      } else {
        Serial.printf("Pending collection post failed: %d\n", code);
      }
      continue;
    }

    // If parsed successfully, we can check for special op
    if (doc.containsKey("op") && String((const char*)doc["op"]) == "update_staff_fingerprint") {
      int staffid = doc["staffid"] | -1;
      int fid = doc["fingerprintid"] | -1;
      if (staffid > 0 && fid > 0) {
        bool ok = updateStaffFingerprint(staffid, fid);
        if (ok) {
          Serial.printf("Deferred DB update applied for staff %d -> fid %d\n", staffid, fid);
          pendingLogs.erase(pendingLogs.begin() + i);
        } else {
          Serial.println("Deferred DB update failed; will retry.");
        }
      } else {
        Serial.println("Invalid deferred DB update payload; discarding.");
        pendingLogs.erase(pendingLogs.begin() + i);
      }
      continue;
    }

    // Otherwise treat as collection log - post to /rest/v1/food_collections
    HTTPClient h;
    String url = String(supabase_url) + "/rest/v1/food_collections";
    if (!h.begin(tlsClient, url)) {
      Serial.println("HTTP begin failed for pending collection.");
      continue;
    }
    h.addHeader("apikey", supabase_apikey);
    h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
    h.addHeader("Content-Type", "application/json");
    h.addHeader("Prefer", "return=minimal");
    String out;
    serializeJson(doc, out); // normalize payload
    int code = h.POST(out);
    h.end();
    if (code == HTTP_CODE_CREATED) {
      Serial.println("Pending collection posted successfully.");
      pendingLogs.erase(pendingLogs.begin() + i);
    } else {
      Serial.printf("Pending collection post failed: %d\n", code);
      // keep for retry
    }
  } // end for
}

// ------------------ Simple beeps -------------------------
void successBeep() {
  #ifdef BUZZER_PIN
  tone(BUZZER_PIN, 1000, 150);
  #endif
}

void errorBeep() {
  #ifdef BUZZER_PIN
  tone(BUZZER_PIN, 500, 300);
  #endif
}

// ------------------ Setup & Loop ------------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  pinMode(BUZZER_PIN, OUTPUT);

  // Fingerprint sensor UART
  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(57600);
  if (!finger.verifyPassword()) {
    Serial.println("Fingerprint sensor not found or wrong password!");
    while (true) { delay(1000); }
  } else {
    Serial.println("Fingerprint sensor ready.");
  }

  // UART Communication setup
  uartSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("UART communication ready.");

  // WiFi + time
  initWiFi();
  if (wifiConnected) {
    syncTimeWithNTP();
  } else {
    Serial.println("WiFi not connected. Attempting later.");
  }

  // Initial instruction (main with time)
  sendInstruction("main");
}

void loop() {
  unsigned long now = millis();
  
  // Handle network operations (non-blocking)
  handleNetworkOperations(now);
  
  // Handle fingerprint operations (non-blocking)
  handleFingerprintOperations(now);
  
  // tiny yield so background tasks and WiFi can run; avoids starving the system
  delay(1);
}