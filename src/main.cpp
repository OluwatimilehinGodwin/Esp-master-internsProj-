// Full revised sketch — optimized for real-time scanning and queued network work.
// Target: ESP32
// Changes: waits for server ACK on registration before switching back to main,
// collection cache refresh = 30s, enrollment network-wait state + timeout.
// Additional: registration takes priority, deferred control retry on timeout.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <HardwareSerial.h>
#include <Adafruit_Fingerprint.h>
#include <vector>
#include <map>
#include <set>

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

// Intervals
const unsigned long sendInterval = 7000; // UART main heartbeat
const unsigned long controlPollInterval = 5000;
const unsigned long collectionRefreshInterval = 30000; // 30 seconds 

// Scan cooldowns
const unsigned long scanCooldownMs = 1200;        // after a complete scan, block new scans
const unsigned long perFidCooldownMs = 2000;      // avoid processing same fid repeatedly

// Enrollment network wait
const unsigned long enrollNetworkMaxWait = 60000; // wait up to 60s for server ack

// ---------------------------------------------------------

// New globals for deferred control handling / enrollment scan timeout
// Defer re-processing of a control row for some milliseconds after a timeout.
// key = hashed control id (same hash used in checkControlModeNetwork), value = millis() when allowed again
std::map<int, unsigned long> controlRetryTs;

// Enrollment scan timeout (ms)
const unsigned long enrollScanTimeout = 60000; // 60s before deferring/pausing enrollment
// Defer duration for timed-out register rows
const unsigned long controlRetryDelay = 60000; // 60s

// Networking / clients (used only in network task)
WiFiClientSecure tlsClient;

// Fingerprint (main thread)
HardwareSerial fpSerial(1);
Adafruit_Fingerprint finger(&fpSerial);

// UART Communication (main thread)
HardwareSerial uartSerial(2);

// State
volatile bool wifiConnected = false;
unsigned long lastSendTime = 0;

// control mode from Supabase: "collection" or "register"
String mode = "collection";
int staffidToRegister = -1;
int currentControlId = -1; // store control row id when a register command arrives

// Fingerprint state machine (collection)
enum FingerprintState { IDLE, SCANNING, PROCESSING, COMPLETE };
FingerprintState fpState = IDLE;
unsigned long lastFpCheck = 0;
const unsigned long fpCheckInterval = 80; // faster polling

// Enrollment state machine (non-blocking)
enum EnrollStep {
  ENROLL_IDLE = 0,
  ENROLL_WAIT_FIRST,
  ENROLL_WAIT_REMOVE,
  ENROLL_WAIT_SECOND,
  ENROLL_SECOND_CAPTURED,
  ENROLL_DONE,
  ENROLL_WAIT_NETWORK_ACK,
  ENROLL_FAILED
};
EnrollStep enrollStep = ENROLL_IDLE;
int enrollStaffId = -1;
int enrollFid = -1;
unsigned long enrollStepTime = 0;
unsigned long enrollNetworkStart = 0;
bool enrollNetworkAck = false; // set by network task when server ack arrives

// In-memory fingerprint map (fid -> {staffid, tag})
struct FpRecord { int staffid; int tag; };
std::map<int, FpRecord> fingerprintMap;

// Collection cache (staffid who collected today)
std::vector<int> collectedToday;

// Pending network queues (shared with network task)
std::vector<String> pendingLogs; // JSON payloads to POST or deferred ops
struct PendingResolve { int fid; unsigned long ts; };
std::vector<PendingResolve> pendingResolves;

// Aux sets to prevent duplicate payloads
std::set<String> pendingHashes; // dedupe by payload string

// track last processed time per fid to avoid duplicates & double messages
std::map<int, unsigned long> lastProcessedFidTs;

// Mutex for protecting shared structures
SemaphoreHandle_t sharedMutex = NULL;

// ---------- Forward declarations ----------
void sendInstruction(const char* instruction);
void sendViaUART(const char* instruction, bool withTime = true);
String hhmmNow();
String isoTimeNowWAT();
String getTodayDate();
void successBeep();
void errorBeep();

void networkTask(void* pvParameters);

// Utility (network-only) — run inside networkTask
bool refreshFingerprintMap(); // loads fingerprintMap from server
void refreshCollectionCache(); // loads collectedToday for today
String checkControlModeNetwork(); // polls control mode from server
bool updateStaffFingerprintNetwork(int staffid, int fid, int controlId);

// Enrollment helpers (main thread)
int findNextAvailableID();

// ---------- Implementation ----------

void sendViaUART(const char* instruction, bool withTime) {
  String message;
  if (withTime) {
    message = String(instruction) + "|" + hhmmNow();
  } else {
    message = String(instruction);
  }
  uartSerial.println(message);
  Serial.printf("UART Sent: %s\n", message.c_str());
}

void sendInstruction(const char* instruction) {
  sendViaUART(instruction, true);
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
  strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S%z", &t);
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

// Simple beeps
#ifdef BUZZER_PIN
void successBeep() { tone(BUZZER_PIN, 1000, 120); }
void errorBeep()   { tone(BUZZER_PIN, 500, 250);  }
#else
void successBeep() { }
void errorBeep()   { }
#endif

// Find next free fingerprint slot (uses sensor; main thread)
int findNextAvailableID() {
  for (int id = 1; id <= 127; ++id) {
    if (finger.loadModel(id) != FINGERPRINT_OK) {
      return id;
    }
  }
  return -1;
}

// ---------------- Fingerprint handling (main loop) ----------------
unsigned long lastScanCompleteTs = 0;

void handleCollectionMode(unsigned long now) {
  switch (fpState) {
    case IDLE:
      if (now - lastScanCompleteTs < scanCooldownMs) return;
      if (finger.getImage() == FINGERPRINT_OK) {
        fpState = SCANNING;
        sendInstruction("scan");
        Serial.println("Finger detected - capture starting...");
      }
      break;

    case SCANNING: {
      uint8_t p = finger.image2Tz();
      if (p != FINGERPRINT_OK) {
        Serial.printf("image2Tz error in collection: %u\n", p);
        errorBeep();
        sendInstruction("unsuccessful");
        fpState = COMPLETE;
        return;
      }
      fpState = PROCESSING;
      break;
    }

    case PROCESSING: {
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

      unsigned long lastTs = 0;
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        auto it = lastProcessedFidTs.find(fid);
        if (it != lastProcessedFidTs.end()) lastTs = it->second;
        xSemaphoreGive(sharedMutex);
      }
      if (millis() - lastTs < perFidCooldownMs) {
        Serial.printf("Ignoring repeated fid %d within cooldown.\n", fid);
        sendInstruction("main");
        fpState = COMPLETE;
        return;
      }

      bool foundLocally = false;
      int staffid = -1, tag = -1;
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        auto it = fingerprintMap.find(fid);
        if (it != fingerprintMap.end()) {
          foundLocally = true;
          staffid = it->second.staffid;
          tag = it->second.tag;
        }
        lastProcessedFidTs[fid] = millis();
        xSemaphoreGive(sharedMutex);
      }

      if (foundLocally) {
        bool already = false;
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          for (int id : collectedToday) if (id == staffid) { already = true; break; }
          xSemaphoreGive(sharedMutex);
        }

        if (already) {
          Serial.printf("Staff %d already collected (local cache)\n", staffid);
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        StaticJsonDocument<256> body;
        body["fingerprintid"] = fid;
        body["tag"] = tag;
        body["staffid"] = staffid;
        body["time_collected"] = isoTimeNowWAT();
        String payload; serializeJson(body, payload);

        bool willPush = false;
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          if (pendingHashes.find(payload) == pendingHashes.end()) {
            pendingLogs.push_back(payload);
            pendingHashes.insert(payload);
            willPush = true;
            collectedToday.push_back(staffid); // optimistic
          } else {
            Serial.println("Payload already queued, skipping duplicate enqueue.");
          }
          xSemaphoreGive(sharedMutex);
        }

        if (willPush) {
          successBeep();
          sendInstruction("successful");
        } else {
          errorBeep();
          sendInstruction("unsuccessful");
        }
        fpState = COMPLETE;
        return;
      } else {
        PendingResolve r; r.fid = fid; r.ts = millis();
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          bool already = false;
          for (auto &pr : pendingResolves) if (pr.fid == fid) { already = true; break; }
          if (!already) pendingResolves.push_back(r);
          else Serial.printf("Resolve for fid %d already queued.\n", fid);
          xSemaphoreGive(sharedMutex);
        }
        sendInstruction("processing");
        fpState = COMPLETE;
        return;
      }
    }

    case COMPLETE: {
      static unsigned long completeStarted = 0;
      if (completeStarted == 0) completeStarted = millis();
      if (millis() - completeStarted >= 600) {
        sendInstruction("main");
        lastScanCompleteTs = millis();
        fpState = IDLE;
        completeStarted = 0;
      }
      break;
    }
  }
}

// ---------------- Enrollment (main thread nonblocking) ----------------
// Modified to set mode under mutex and to integrate timeouts / deferral
void startEnrollmentNonBlocking(int staffid) {
  enrollStaffId = staffid;
  enrollFid = findNextAvailableID();
  if (enrollFid < 0) {
    Serial.println("No free fingerprint slots available.");
    errorBeep();
    sendInstruction("unsuccessful");
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      staffidToRegister = -1;
      mode = "collection";
      currentControlId = -1;
      xSemaphoreGive(sharedMutex);
    }
    return;
  }

  // mark as active enrollment so network task will not replace mode
  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
    mode = "register";
    staffidToRegister = staffid;
    xSemaphoreGive(sharedMutex);
  }

  enrollStep = ENROLL_WAIT_FIRST;
  enrollStepTime = millis();
  enrollNetworkAck = false;
  Serial.printf("Enroll start: staff %d -> fid %d\n", enrollStaffId, enrollFid);
  sendInstruction("scan");
}

void handleEnrollmentNonBlocking(unsigned long now) {
  switch (enrollStep) {
    case ENROLL_WAIT_FIRST: {
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        if (finger.image2Tz(1) == FINGERPRINT_OK) {
          sendInstruction("successful"); successBeep();
          Serial.println("First capture OK. Remove finger...");
          enrollStep = ENROLL_WAIT_REMOVE;
          enrollStepTime = millis();
        } else {
          Serial.println("image2Tz(1) failed.");
          enrollStep = ENROLL_FAILED;
        }
      } else if (millis() - enrollStepTime > enrollScanTimeout) {
        Serial.println("Timeout waiting for first finger. Deferring registration and returning to collection.");
        // defer reprocessing of this control row for controlRetryDelay
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          if (currentControlId > 0) controlRetryTs[currentControlId] = millis() + controlRetryDelay;
          // reset UI state to collection
          mode = "collection";
          staffidToRegister = -1;
          currentControlId = -1;
          xSemaphoreGive(sharedMutex);
        }
        // reset enrollment locally
        enrollStep = ENROLL_IDLE;
        enrollStaffId = -1;
        enrollFid = -1;
      }
      break;
    }

    case ENROLL_WAIT_REMOVE: {
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        sendInstruction("scan");
        enrollStep = ENROLL_WAIT_SECOND;
        enrollStepTime = millis();
      } else if (millis() - enrollStepTime > enrollScanTimeout) {
        Serial.println("Timeout waiting for removal. Deferring registration and returning to collection.");
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          if (currentControlId > 0) controlRetryTs[currentControlId] = millis() + controlRetryDelay;
          mode = "collection";
          staffidToRegister = -1;
          currentControlId = -1;
          xSemaphoreGive(sharedMutex);
        }
        enrollStep = ENROLL_IDLE;
        enrollStaffId = -1;
        enrollFid = -1;
      }
      break;
    }

    case ENROLL_WAIT_SECOND: {
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_OK) {
        if (finger.image2Tz(2) == FINGERPRINT_OK) {
          sendInstruction("successful"); successBeep();
          enrollStep = ENROLL_SECOND_CAPTURED;
        } else {
          Serial.println("image2Tz(2) failed.");
          enrollStep = ENROLL_FAILED;
        }
      } else if (millis() - enrollStepTime > enrollScanTimeout) {
        Serial.println("Timeout waiting for second scan. Deferring registration and returning to collection.");
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          if (currentControlId > 0) controlRetryTs[currentControlId] = millis() + controlRetryDelay;
          mode = "collection";
          staffidToRegister = -1;
          currentControlId = -1;
          xSemaphoreGive(sharedMutex);
        }
        enrollStep = ENROLL_IDLE;
        enrollStaffId = -1;
        enrollFid = -1;
      }
      break;
    }

    case ENROLL_SECOND_CAPTURED: {
      if (finger.createModel() == FINGERPRINT_OK) {
        Serial.println("Model created. Storing...");
        if (finger.storeModel(enrollFid) == FINGERPRINT_OK) {
          Serial.printf("Stored model at slot %d\n", enrollFid);
          // Queue DB update for network task, include control id so network can mark processed
          StaticJsonDocument<256> body;
          body["op"] = "update_staff_fingerprint";
          body["staffid"] = enrollStaffId;
          body["fingerprintid"] = enrollFid;
          body["control_id"] = currentControlId; // might be -1 if unknown
          String payload; serializeJson(body, payload);
          if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
            if (pendingHashes.find(payload) == pendingHashes.end()) {
              pendingLogs.push_back(payload);
              pendingHashes.insert(payload);
            }
            xSemaphoreGive(sharedMutex);
          }
          sendInstruction("successful"); successBeep();
          // go to WAIT_NETWORK_ACK: do not switch back to main until network confirms and marks control processed
          enrollStep = ENROLL_WAIT_NETWORK_ACK;
          enrollNetworkStart = millis();
          Serial.println("Enrollment stored - waiting for network ACK to finalize registration...");
        } else {
          Serial.println("storeModel failed.");
          enrollStep = ENROLL_FAILED;
        }
      } else {
        Serial.println("createModel failed.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_WAIT_NETWORK_ACK: {
      // If network ack arrives, finalize
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        if (enrollNetworkAck) {
          // finalise locally
          enrollNetworkAck = false;
          enrollStep = ENROLL_DONE;
        }
        xSemaphoreGive(sharedMutex);
      }
      // timeout fallback for network ack (keep same behavior)
      if (millis() - enrollNetworkStart > enrollNetworkMaxWait) {
        Serial.println("Network ACK timeout; finalizing locally and returning to main. Pending DB op will be retried by network task.");
        enrollStep = ENROLL_DONE;
      }
      break;
    }

    case ENROLL_DONE: {
      // Now safe to return to main UI (registration completed or timed out)
      sendInstruction("main");
      enrollStep = ENROLL_IDLE;
      // sanitize/clear control registration state
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        staffidToRegister = -1;
        mode = "collection";
        currentControlId = -1;
        xSemaphoreGive(sharedMutex);
      }
      enrollStaffId = -1;
      enrollFid = -1;
      Serial.println("Enrollment done and device returned to collection mode.");
      break;
    }

    case ENROLL_FAILED: {
      errorBeep();
      sendInstruction("unsuccessful");
      // reset and return to collection (but do not mark control processed so it will be retried normally)
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        mode = "collection";
        staffidToRegister = -1;
        // optionally defer immediate pickup slightly to avoid flapping
        if (currentControlId > 0) controlRetryTs[currentControlId] = millis() + controlRetryDelay;
        currentControlId = -1;
        xSemaphoreGive(sharedMutex);
      }
      enrollStep = ENROLL_IDLE;
      enrollStaffId = -1;
      enrollFid = -1;
      break;
    }

    default: break;
  }
}

// ----------------- Setup & main loop ----------------------
void setup() {
  Serial.begin(115200);
  delay(100);

  #ifdef BUZZER_PIN
  pinMode(BUZZER_PIN, OUTPUT);
  #endif

  // fingerprint UART
  fpSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);
  finger.begin(57600);
  if (!finger.verifyPassword()) {
    Serial.println("Fingerprint sensor not found or wrong password!");
    while (true) { delay(1000); }
  } else {
    Serial.println("Fingerprint sensor ready.");
  }

  // UART comm
  uartSerial.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX, UART_TX);
  Serial.println("UART ready.");

  // Create mutex for shared data
  sharedMutex = xSemaphoreCreateMutex();
  if (sharedMutex == NULL) {
    Serial.println("Failed to create mutex!");
    while (true) { delay(1000); }
  }

  // Connect WiFi (network task will also manage reconnects)
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.printf("Connecting to WiFi '%s'...\n", ssid);
  unsigned long start = millis();
  const unsigned long wifiTimeout = 15000;
  while (WiFi.status() != WL_CONNECTED && millis() - start < wifiTimeout) {
    Serial.print(".");
    delay(300);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWiFi connected.");
    wifiConnected = true;
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  } else {
    Serial.println("\nWiFi not connected (will be handled by network task).");
    wifiConnected = false;
  }

  // Start network task on core 1 (keeps HTTP off the main loop)
  xTaskCreatePinnedToCore(networkTask, "networkTask", 32*1024, NULL, 1, NULL, 1);

  // initial UI
  sendInstruction("main");
}

void loop() {
  unsigned long now = millis();

  // Handle enrollment trigger
  if (mode == "register" && staffidToRegister > 0 && enrollStep == ENROLL_IDLE) {
    Serial.println("Starting enrollment process...");
    startEnrollmentNonBlocking(staffidToRegister);
  }

  // Enrollment handling if active
  if (enrollStep != ENROLL_IDLE) {
    handleEnrollmentNonBlocking(now);
  } else {
    // Only do collection scanning when not in enrollment
    if (now - lastFpCheck >= fpCheckInterval) {
      lastFpCheck = now;
      handleCollectionMode(now);
    }
  }

  // Heartbeat main message (non-blocking)
  if (now - lastSendTime >= sendInterval) {
    lastSendTime = now;
    sendInstruction("main");
  }

  delay(1); // tiny yield
}

// ---------------- Network task (runs on other core) -------------
void networkTask(void* pvParameters) {
  tlsClient.setInsecure();

  unsigned long lastControlPoll = 0;
  unsigned long lastCollectionRefresh = 0;
  unsigned long lastFingerprintRefresh = 0;

  // On start, if WiFi connected, populate fingerprintMap and collection cache
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    refreshFingerprintMap();
    checkControlModeNetwork();
    refreshCollectionCache();
    // sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

    // IMMEDIATE control poll after initial refresh so new web "register" rows are detected quickly
    checkControlModeNetwork();
  }

  for (;;) {
    // ensure WiFi
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      Serial.println("Network task: WiFi disconnected, attempting reconnect...");
      WiFi.disconnect(false);
      WiFi.reconnect();
      unsigned long reconnectStart = millis();
      while (WiFi.status() != WL_CONNECTED && millis() - reconnectStart < 8000) {
        vTaskDelay(pdMS_TO_TICKS(300));
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("Network task: WiFi reconnected.");
        wifiConnected = true;
        // refresh caches quickly
        refreshFingerprintMap();
        checkControlModeNetwork();
        refreshCollectionCache();
        // immediate control poll to pick up any new register commands
        checkControlModeNetwork();
      }
    }

    unsigned long now = millis();

    // Poll control mode every controlPollInterval
    if (now - lastControlPoll >= controlPollInterval) {
      lastControlPoll = now;
      checkControlModeNetwork();
    }

    // Refresh fingerprint mapping every 10 minutes (if connected)
    if (now - lastFingerprintRefresh >= 600000 && wifiConnected) {
      lastFingerprintRefresh = now;
      refreshFingerprintMap();
      checkControlModeNetwork();
    }

    // Refresh today's collection cache every collectionRefreshInterval (30s)
    if (now - lastCollectionRefresh >= collectionRefreshInterval && wifiConnected) {
      lastCollectionRefresh = now;
      refreshCollectionCache();
    }

    // Process one pending network action (resolve -> create collection -> POST) per loop
    if (wifiConnected) {
      bool didOne = false;

      PendingResolve pr;
      bool haveResolve = false;
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        if (!pendingResolves.empty()) {
          pr = pendingResolves.front();
          pendingResolves.erase(pendingResolves.begin());
          haveResolve = true;
        }
        xSemaphoreGive(sharedMutex);
      }

      if (haveResolve) {
        int tag = -1, staffid = -1;
        {
          HTTPClient h;
          String url = String(supabase_url) + "/rest/v1/staff?fingerprintid=eq." + String(pr.fid) + "&select=staffid,tag&limit=1";
          if (h.begin(tlsClient, url)) {
            h.addHeader("apikey", supabase_apikey);
            h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
            int code = h.GET();
            String payload = h.getString();
            h.end();
            if (code == 200) {
              DynamicJsonDocument doc(512);
              DeserializationError err = deserializeJson(doc, payload);
              if (!err && doc.is<JsonArray>() && doc.size() > 0) {
                staffid = doc[0]["staffid"] | -1;
                tag = doc[0]["tag"] | -1;
              } else {
                Serial.println("Resolve: parse error or no results");
              }
            } else {
              Serial.printf("Resolve GET failed: %d\n", code);
            }
          } else {
            Serial.println("Resolve HTTP begin failed");
          }
        }

        if (staffid <= 0 || tag < 0) {
          errorBeep();
          sendViaUART("unsuccessful", true);
          didOne = true;
        } else {
          bool already = false;
          if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
            for (int s : collectedToday) if (s == staffid) { already = true; break; }
            if (!already) collectedToday.push_back(staffid);
            xSemaphoreGive(sharedMutex);
          }

          if (already) {
            errorBeep();
            sendViaUART("unsuccessful", true);
          } else {
            StaticJsonDocument<256> body;
            body["fingerprintid"] = pr.fid;
            body["tag"] = tag;
            body["staffid"] = staffid;
            body["time_collected"] = isoTimeNowWAT();
            String payload; serializeJson(body, payload);

            if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
              if (pendingHashes.find(payload) == pendingHashes.end()) {
                pendingLogs.push_back(payload);
                pendingHashes.insert(payload);
              } else {
                Serial.println("Pending collection already queued (from resolve path).");
              }
              xSemaphoreGive(sharedMutex);
            }
            successBeep();
            sendViaUART("successful", true);
          }
          didOne = true;
        }
      } else {
        String payload;
        bool havePayload = false;
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          if (!pendingLogs.empty()) {
            payload = pendingLogs.front();
            pendingLogs.erase(pendingLogs.begin());
            havePayload = true;
          }
          xSemaphoreGive(sharedMutex);
        }

        if (havePayload) {
          DynamicJsonDocument doc(512);
          DeserializationError err = deserializeJson(doc, payload);
          if (err) {
            Serial.println("Pending payload parse error — removing from pendingHashes");
            if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
              pendingHashes.erase(payload);
              xSemaphoreGive(sharedMutex);
            }
            didOne = true;
          } else {
            if (doc.containsKey("op") && String((const char*)doc["op"]) == "update_staff_fingerprint") {
              int staffid = doc["staffid"] | -1;
              int fid = doc["fingerprintid"] | -1;
              int controlId = doc["control_id"] | -1;
              bool ok = updateStaffFingerprintNetwork(staffid, fid, controlId);
              if (!ok) {
                if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
                  pendingLogs.push_back(payload);
                  xSemaphoreGive(sharedMutex);
                }
              } else {
                // success -> remove from pendingHashes is handled in updateStaffFingerprintNetwork
              }
              didOne = true;
            } else {
              HTTPClient h;
              String url = String(supabase_url) + "/rest/v1/food_collections";
              if (h.begin(tlsClient, url)) {
                h.addHeader("apikey", supabase_apikey);
                h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
                h.addHeader("Content-Type", "application/json");
                h.addHeader("Prefer", "return=minimal");
                int code = h.POST(payload);
                h.end();
                if (code == HTTP_CODE_CREATED || code == 201) {
                  Serial.println("Collection posted successfully.");
                  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdMS_TO_TICKS(10)) {
                    pendingHashes.erase(payload);
                    xSemaphoreGive(sharedMutex);
                  }
                } else {
                  Serial.printf("Collection POST failed: %d — will retry\n", code);
                  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
                    pendingLogs.push_back(payload);
                    xSemaphoreGive(sharedMutex);
                  }
                }
              } else {
                Serial.println("HTTP begin failed for collection; requeueing");
                if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
                  pendingLogs.push_back(payload);
                  xSemaphoreGive(sharedMutex);
                }
              }
              didOne = true;
            }
          }
        }
      }

      if (didOne) vTaskDelay(pdMS_TO_TICKS(150));
      else vTaskDelay(pdMS_TO_TICKS(200));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
  }
}

// ---------- Network helper implementations (networkTask only) -------------
bool refreshFingerprintMap() {
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.println("Refreshing fingerprint map from server...");
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?select=staffid,fingerprintid,tag&fingerprintid=is.not.null";
  if (!h.begin(tlsClient, url)) {
    Serial.println("Fingerprint map HTTP begin failed");
    return false;
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  int code = h.GET();
  String payload = h.getString();
  h.end();

  if (code != 200) {
    Serial.printf("Fingerprint map GET failed: %d\n", code);
    return false;
  }

  DynamicJsonDocument doc(8192);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Fingerprint map parse error");
    return false;
  }

  if (xSemaphoreTake(sharedMutex, (TickType_t)200/portTICK_PERIOD_MS) == pdTRUE) {
    fingerprintMap.clear();
    for (JsonObject item : doc.as<JsonArray>()) {
      int fid = item["fingerprintid"] | -1;
      int staffid = item["staffid"] | -1;
      int tag = item["tag"] | -1;
      if (fid > 0 && staffid > 0) {
        fingerprintMap[fid] = { staffid, tag };
      }
    }
    xSemaphoreGive(sharedMutex);
  }

  Serial.printf("Fingerprint map refreshed: %d entries\n", (int)fingerprintMap.size());
  return true;
}

void refreshCollectionCache() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Refreshing today's collection cache...");
  HTTPClient h;
  String today = getTodayDate();
  String url = String(supabase_url) + "/rest/v1/food_collections?select=staffid&time_collected=gte." + today + "T00:00:00";
  if (!h.begin(tlsClient, url)) {
    Serial.println("Collection cache begin failed");
    return;
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  int code = h.GET();
  String payload = h.getString();
  h.end();

  if (code != 200) {
    Serial.printf("Collection cache GET failed: %d\n", code);
    return;
  }

  DynamicJsonDocument doc(4096);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.println("Collection cache parse error");
    return;
  }

  if (xSemaphoreTake(sharedMutex, (TickType_t)200/portTICK_PERIOD_MS) == pdTRUE) {
    collectedToday.clear();
    for (JsonObject item : doc.as<JsonArray>()) {
      collectedToday.push_back(item["staffid"].as<int>());
    }
    xSemaphoreGive(sharedMutex);
  }

  Serial.printf("Collection cache refreshed: %d entries\n", (int)collectedToday.size());
}

// Modified: checkControlModeNetwork now skips deferred control rows and respects active enrollment
String checkControlModeNetwork() {
  if (WiFi.status() != WL_CONNECTED) return String();

  // If an enrollment is active on main thread, do not replace mode.
  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
    bool active = (enrollStep != ENROLL_IDLE);
    xSemaphoreGive(sharedMutex);
    if (active) {
      // preserve current mode while enrollment in progress
      Serial.println("checkControlModeNetwork: enrollment active — skipping control poll update.");
      return mode;
    }
  }

  HTTPClient h;
  // include id so we can mark processed later
  String url = String(supabase_url) + "/rest/v1/control?select=id,mode,staffid&processed=eq.false&limit=1";
  if (!h.begin(tlsClient, url)) {
    Serial.println("control GET begin failed");
    return String();
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  h.addHeader("Accept", "application/json");
  int code = h.GET();
  String payload = h.getString();
  h.end();

  Serial.printf("control GET code=%d payload_len=%d\n", code, (int)payload.length());
  if (payload.length() > 0) {
    String pshort = payload;
    if (pshort.length() > 512) pshort = pshort.substring(0, 512) + "...";
    Serial.println("control payload (truncated):");
    Serial.println(pshort);
  }

  if (code != 200) {
    Serial.printf("control GET returned %d\n", code);
    return String();
  }

  DynamicJsonDocument doc(2048);
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("control parse error: ");
    Serial.println(err.c_str());
    return String();
  }

  if (!doc.is<JsonArray>() || doc.size() == 0) {
    // default to collection
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      mode = "collection";
      staffidToRegister = -1;
      currentControlId = -1;
      xSemaphoreGive(sharedMutex);
    }
    Serial.println("control: no pending rows -> remain in collection");
    return mode;
  }

  JsonObject first = doc[0];
  String newMode = String((const char*)(first["mode"] | "collection"));
  int sid = first["staffid"] | -1;
  
  // FIX: Handle UUID string for control ID
  String controlIdStr = String((const char*)(first["id"] | ""));
  int cid = -1;
  if (controlIdStr.length() > 0) {
    // Store as string or hash it to an integer - we'll use a simple hash
    unsigned long hash = 0;
    for (size_t i = 0; i < controlIdStr.length(); i++) {
      hash = hash * 31 + controlIdStr.charAt(i);
    }
    cid = (int)(hash & 0x7FFFFFFF); // Ensure positive
  }

  unsigned long now = millis();
  // Check if this control has a retry timestamp in future; if so ignore it for now
  if (cid > 0) {
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      auto it = controlRetryTs.find(cid);
      if (it != controlRetryTs.end() && now < it->second) {
        // skip this control now (it is deferred)
        Serial.printf("control %s (hashed %d) is deferred until +%lu ms -> ignoring for now\n",
                      controlIdStr.c_str(), cid, it->second);
        xSemaphoreGive(sharedMutex);
        // treat as no pending rows -> remain collection
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          mode = "collection";
          staffidToRegister = -1;
          currentControlId = -1;
          xSemaphoreGive(sharedMutex);
        }
        return mode;
      }
      xSemaphoreGive(sharedMutex);
    }
  }

  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
    mode = newMode;
    staffidToRegister = sid;
    currentControlId = cid;
    xSemaphoreGive(sharedMutex);
  }
  Serial.printf("Control → mode=%s, staffid=%d, control_id=%d (from %s)\n", newMode.c_str(), sid, cid, controlIdStr.c_str());
  return newMode;
}

// Update staff fingerprint and then mark control processed if controlId provided.
// returns true if update + control patch (if needed) succeeded.
bool updateStaffFingerprintNetwork(int staffid, int fid, int controlId) {
  if (WiFi.status() != WL_CONNECTED) return false;
  
  // Update staff fingerprint first
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/staff?staffid=eq." + String(staffid);
  if (!h.begin(tlsClient, url)) {
    Serial.println("updateStaffFingerprint begin failed");
    return false;
  }
  h.addHeader("apikey", supabase_apikey);
  h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
  h.addHeader("Content-Type", "application/json");
  h.addHeader("Prefer", "return=minimal");

  StaticJsonDocument<128> body;
  body["fingerprintid"] = fid;
  String out; serializeJson(body, out);

  int code = h.PATCH(out);
  h.end();
  
  if (code == HTTP_CODE_NO_CONTENT || code == HTTP_CODE_OK) {
    // Update local fingerprint map
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      fingerprintMap[fid] = { staffid, -1 };
      xSemaphoreGive(sharedMutex);
    }
    Serial.printf("updateStaffFingerprint succeeded for staff %d -> fid %d\n", staffid, fid);

    // Mark control as processed if we have a valid controlId
    if (controlId > 0) {
      // Since we hashed the UUID, we need to find the control row by staffid and mode
      // This is a workaround since we can't directly query by the original UUID
      HTTPClient h2;
      String url2 = String(supabase_url) + "/rest/v1/control?mode=eq.register&staffid=eq." + String(staffid) + "&processed=eq.false";
      if (!h2.begin(tlsClient, url2)) {
        Serial.println("mark control processed begin failed");
      } else {
        h2.addHeader("apikey", supabase_apikey);
        h2.addHeader("Authorization", String("Bearer ") + supabase_apikey);
        h2.addHeader("Content-Type", "application/json");
        h2.addHeader("Prefer", "return=minimal");
        
        StaticJsonDocument<64> b2;
        b2["processed"] = true;
        String out2; serializeJson(b2, out2);
        
        int code2 = h2.PATCH(out2);
        h2.end();
        
        if (code2 == HTTP_CODE_NO_CONTENT || code2 == HTTP_CODE_OK) {
          Serial.printf("Control marked processed for staff %d\n", staffid);
        } else {
          Serial.printf("Failed to mark control processed: %d\n", code2);
        }
      }
    }

    // Set network ACK to allow enrollment to complete
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      enrollNetworkAck = true;
      xSemaphoreGive(sharedMutex);
    }

    return true;
  } else {
    Serial.printf("updateStaffFingerprint failed: %d\n", code);
    return false;
  }
}
