// Full revised sketch — optimized for real-time scanning and queued network work.
// Target: ESP32

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
const unsigned long collectionRefreshInterval = 300000; // 5 min

// ---------------------------------------------------------

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
  ENROLL_CREATE_MODEL,
  ENROLL_STORE_MODEL,
  ENROLL_DONE,
  ENROLL_FAILED
};
EnrollStep enrollStep = ENROLL_IDLE;
int enrollStaffId = -1;
int enrollFid = -1;
unsigned long enrollStepTime = 0;
const unsigned long enrollTimeout = 30000;

// In-memory fingerprint map (fid -> {staffid, tag})
struct FpRecord { int staffid; int tag; };
std::map<int, FpRecord> fingerprintMap;

// Collection cache (staffid who collected today)
std::vector<int> collectedToday;

// Pending network queues (shared with network task)
std::vector<String> pendingLogs; // JSON payloads to POST or deferred ops
struct PendingResolve { int fid; unsigned long ts; };
std::vector<PendingResolve> pendingResolves;

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

// Network task
void networkTask(void* pvParameters);

// Utility (network-only) — run inside networkTask
bool refreshFingerprintMap(); // loads fingerprintMap from server
void refreshCollectionCache(); // loads collectedToday for today
String checkControlModeNetwork(); // polls control mode from server
bool sendOnePendingNetworkAction(); // processes one pending action (resolve or post)
bool updateStaffFingerprintNetwork(int staffid, int fid);

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

// Simple beeps - FIXED: moved preprocessor directives outside functions
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
void handleCollectionMode(unsigned long now) {
  switch (fpState) {
    case IDLE:
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

      // Try local lookup first (fast)
      bool foundLocally = false;
      int staffid = -1, tag = -1;
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        auto it = fingerprintMap.find(fid);
        if (it != fingerprintMap.end()) {
          foundLocally = true;
          staffid = it->second.staffid;
          tag = it->second.tag;
        }
        xSemaphoreGive(sharedMutex);
      }

      if (foundLocally) {
        // check if already collected today
        bool already = false;
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          for (int id : collectedToday) if (id == staffid) { already = true; break; }
          if (!already) collectedToday.push_back(staffid); // optimistic
          xSemaphoreGive(sharedMutex);
        }

        if (already) {
          Serial.printf("Staff %d already collected (local cache)\n", staffid);
          errorBeep();
          sendInstruction("unsuccessful");
          fpState = COMPLETE;
          return;
        }

        // create payload and enqueue for network posting
        StaticJsonDocument<256> body;
        body["fingerprintid"] = fid;
        body["tag"] = tag;
        body["staffid"] = staffid;
        body["time_collected"] = isoTimeNowWAT();
        String payload; serializeJson(body, payload);

        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          pendingLogs.push_back(payload); // network task will post ASAP
          xSemaphoreGive(sharedMutex);
        }

        successBeep();
        sendInstruction("successful"); // immediate positive feedback
        fpState = COMPLETE;
        return;
      } else {
        // not in local map — enqueue resolve and give "processing" UI so user knows it's working
        PendingResolve r; r.fid = fid; r.ts = millis();
        if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
          pendingResolves.push_back(r);
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
        fpState = IDLE;
        completeStarted = 0;
      }
      break;
    }
  }
}

// ---------------- Enrollment (main thread nonblocking) ----------------
void startEnrollmentNonBlocking(int staffid) {
  // To avoid blocking, we will start enrollment immediately (assume server sent valid staffid).
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
  enrollStepTime = millis();
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
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for first finger.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_WAIT_REMOVE: {
      uint8_t p = finger.getImage();
      if (p == FINGERPRINT_NOFINGER) {
        sendInstruction("scan");
        enrollStep = ENROLL_WAIT_SECOND;
        enrollStepTime = millis();
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for removal.");
        enrollStep = ENROLL_FAILED;
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
      } else if (millis() - enrollStepTime > enrollTimeout) {
        Serial.println("Timeout waiting for second scan.");
        enrollStep = ENROLL_FAILED;
      }
      break;
    }

    case ENROLL_SECOND_CAPTURED: {
      if (finger.createModel() == FINGERPRINT_OK) {
        Serial.println("Model created. Storing...");
        if (finger.storeModel(enrollFid) == FINGERPRINT_OK) {
          Serial.printf("Stored model at slot %d\n", enrollFid);
          // Queue DB update for network task
          StaticJsonDocument<128> body;
          body["op"] = "update_staff_fingerprint";
          body["staffid"] = enrollStaffId;
          body["fingerprintid"] = enrollFid;
          String payload; serializeJson(body, payload);
          if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
            pendingLogs.push_back(payload);
            xSemaphoreGive(sharedMutex);
          }
          sendInstruction("successful"); successBeep();
          enrollStep = ENROLL_DONE;
          enrollStepTime = millis();
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

    case ENROLL_DONE: {
      if (millis() - enrollStepTime >= 700) {
        sendInstruction("main");
        enrollStep = ENROLL_IDLE;
        staffidToRegister = -1;
        mode = "collection";
        enrollStaffId = -1;
        enrollFid = -1;
        Serial.println("Enrollment done.");
      }
      break;
    }

    case ENROLL_FAILED: {
      errorBeep();
      sendInstruction("unsuccessful");
      enrollStep = ENROLL_DONE;
      enrollStepTime = millis();
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

  // Poll control mode only by reading mode variable (set by network task)
  if (mode == "register" && staffidToRegister > 0 && enrollStep == ENROLL_IDLE) {
    startEnrollmentNonBlocking(staffidToRegister);
  }

  // Enrollment handling if active
  if (enrollStep != ENROLL_IDLE && enrollStep != ENROLL_DONE && enrollStep != ENROLL_FAILED) {
    handleEnrollmentNonBlocking(now);
  }

  // Fingerprint scanning (tight, non-blocking)
  if (now - lastFpCheck >= fpCheckInterval) {
    lastFpCheck = now;
    handleCollectionMode(now);
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
    refreshCollectionCache();
    // sync time
    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  }

  for (;;) {
    // ensure WiFi
    if (WiFi.status() != WL_CONNECTED) {
      wifiConnected = false;
      // Try reconnect periodically
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
        refreshCollectionCache();
      }
    }

    unsigned long now = millis();

    // Poll control mode every controlPollInterval
    if (now - lastControlPoll >= controlPollInterval) {
      lastControlPoll = now;
      String newMode = checkControlModeNetwork();
      if (newMode.length()) {
        // newMode and staffidToRegister are set inside checkControlModeNetwork via shared mutex
      }
    }

    // Refresh fingerprint mapping every 10 minutes (if connected)
    if (now - lastFingerprintRefresh >= 600000 && wifiConnected) {
      lastFingerprintRefresh = now;
      refreshFingerprintMap();
    }

    // Refresh today's collection cache every collectionRefreshInterval
    if (now - lastCollectionRefresh >= collectionRefreshInterval && wifiConnected) {
      lastCollectionRefresh = now;
      refreshCollectionCache();
    }

    // Process one pending network action (resolve -> create collection -> POST) per loop
    if (wifiConnected) {
      bool didOne = false;
      if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
        if (!pendingResolves.empty()) {
          // pop front
          PendingResolve pr = pendingResolves.front();
          pendingResolves.erase(pendingResolves.begin());
          xSemaphoreGive(sharedMutex);

          // Resolve fid -> tag/staffid by querying DB
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
                if (!deserializeJson(doc, payload) && doc.is<JsonArray>() && doc.size() > 0) {
                  staffid = doc[0]["staffid"] | -1;
                  tag = doc[0]["tag"] | -1;
                }
              } else {
                Serial.printf("Resolve GET failed: %d\n", code);
              }
            } else {
              Serial.println("Resolve HTTP begin failed");
            }
          }

          if (staffid <= 0 || tag < 0) {
            // couldn't resolve — notify "unsuccessful" to device
            errorBeep();
            sendViaUART("unsuccessful", true);
            didOne = true;
          } else {
            // Check collectedToday cache: if not collected, enqueue collection POST
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
              // create collection payload and push to pendingLogs head for immediate post
              StaticJsonDocument<256> body;
              body["fingerprintid"] = pr.fid;
              body["tag"] = tag;
              body["staffid"] = staffid;
              body["time_collected"] = isoTimeNowWAT();
              String payload; serializeJson(body, payload);
              if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
                pendingLogs.push_back(payload);
                xSemaphoreGive(sharedMutex);
              }
              // Immediately send "successful" to device - final confirmation will happen after POST
              successBeep();
              sendViaUART("successful", true);
            }
            didOne = true;
          }
        } else if (!pendingLogs.empty()) {
          // Pop first pendingLogs and POST it
          String payload = pendingLogs.front();
          // We must release mutex before doing HTTP
          pendingLogs.erase(pendingLogs.begin());
          xSemaphoreGive(sharedMutex);

          // Detect deferred ops (like update_staff_fingerprint)
          DynamicJsonDocument doc(512);
          if (!deserializeJson(doc, payload) && doc.containsKey("op") && String((const char*)doc["op"]) == "update_staff_fingerprint") {
            int staffid = doc["staffid"] | -1;
            int fid = doc["fingerprintid"] | -1;
            bool ok = updateStaffFingerprintNetwork(staffid, fid);
            if (!ok) {
              // requeue for retry (put back end)
              if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
                pendingLogs.push_back(payload);
                xSemaphoreGive(sharedMutex);
              }
            }
            didOne = true;
          } else {
            // Post to /food_collections
            HTTPClient h;
            String url = String(supabase_url) + "/rest/v1/food_collections";
            if (h.begin(tlsClient, url)) {
              h.addHeader("apikey", supabase_apikey);
              h.addHeader("Authorization", String("Bearer ") + supabase_apikey);
              h.addHeader("Content-Type", "application/json");
              h.addHeader("Prefer", "return=minimal");
              int code = h.POST(payload);
              h.end();
              if (code == HTTP_CODE_CREATED) {
                Serial.println("Collection posted successfully.");
              } else {
                Serial.printf("Collection POST failed: %d — will retry\n", code);
                // requeue
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
        } else {
          xSemaphoreGive(sharedMutex); // nothing to do
        }
      } else {
        // couldn't get mutex; skip this cycle
      }

      // If we did one network action, give a short delay so we don't hammer the server
      if (didOne) vTaskDelay(pdMS_TO_TICKS(150));
      else vTaskDelay(pdMS_TO_TICKS(200)); // idle short wait
    } else {
      // Not connected: wait and try reconnect occasionally
      vTaskDelay(pdMS_TO_TICKS(1500));
    }
  } // end for
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
  auto err = deserializeJson(doc, payload);
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
  auto err = deserializeJson(doc, payload);
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

String checkControlModeNetwork() {
  if (WiFi.status() != WL_CONNECTED) return String();
  HTTPClient h;
  String url = String(supabase_url) + "/rest/v1/control?select=mode,staffid&processed=eq.false&limit=1";
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
  if (code != 200) return String();

  StaticJsonDocument<512> doc;
  if (deserializeJson(doc, payload)) {
    Serial.println("control parse error");
    return String();
  }
  if (!doc.is<JsonArray>() || doc.size() == 0) {
    // default to collection
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      mode = "collection";
      staffidToRegister = -1;
      xSemaphoreGive(sharedMutex);
    }
    return mode;
  }
  JsonObject first = doc[0];
  String newMode = String((const char*)(first["mode"] | "collection"));
  int sid = first["staffid"] | -1;

  if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
    mode = newMode;
    staffidToRegister = sid;
    xSemaphoreGive(sharedMutex);
  }
  Serial.printf("Control → mode=%s, staffid=%d\n", newMode.c_str(), sid);
  return newMode;
}

bool updateStaffFingerprintNetwork(int staffid, int fid) {
  if (WiFi.status() != WL_CONNECTED) return false;
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
    // Add to local fingerprint map quickly (so the new enrollment is usable immediately)
    if (xSemaphoreTake(sharedMutex, (TickType_t)10/portTICK_PERIOD_MS) == pdTRUE) {
      fingerprintMap[fid] = { staffid, -1 }; // tag may be null/unknown; refresh map later
      xSemaphoreGive(sharedMutex);
    }
    Serial.printf("updateStaffFingerprint succeeded for staff %d -> fid %d\n", staffid, fid);
    return true;
  } else {
    Serial.printf("updateStaffFingerprint failed: %d\n", code);
    return false;
  }
}