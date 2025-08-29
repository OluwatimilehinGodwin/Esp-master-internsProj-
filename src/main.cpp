#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>
#include "time.h"

// Receiver MAC address
uint8_t receiverMAC[] = {0x78, 0xEE, 0x4C, 0x02, 0x17, 0x54};

// WiFi credentials
const char* ssid = "Skill G Innovation";
const char* password = "INNOV8HUB";

// WiFi connection state
bool wifiConnected = false;
bool espNowInitialized = false;

// ESP-NOW send callback
void onSent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  Serial.print("Send Status: ");
  Serial.println(status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
}

// List of instructions to cycle through
const char *instructions[] = {"main", "scan", "successful", "unsuccessful"};
const int numInstructions = sizeof(instructions) / sizeof(instructions[0]);

int currentIndex = 0;        // start with "main"
unsigned long lastSendTime = 0;
const unsigned long interval = 7000;  // 7 seconds

// NTP server configuration - NIGERIA SETTINGS
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 3600;       // UTC+1 for Nigeria (1 hour = 3600 seconds)
const int   daylightOffset_sec = 0;     // No daylight saving time in Nigeria

// Time variables
struct tm timeinfo;
unsigned long lastTimeSync = 0;
const unsigned long timeSyncInterval = 3600000; // Sync time every hour (1 hour)

/************************************************************************************************/
bool initializeESP_NOW() {
  if (espNowInitialized) return true;
  
  Serial.println("Initializing ESP-NOW...");
  
  // Ensure WiFi is in STA mode
  WiFi.mode(WIFI_STA);
  
  // Print MAC address for reference
  Serial.print("MAC Address: ");
  Serial.println(WiFi.macAddress());
  
  // Initialize ESP-NOW
  esp_err_t result = esp_now_init();
  if (result != ESP_OK) {
    Serial.printf("Error initializing ESP-NOW: 0x%X\n", result);
    return false;
  }
  
  // Register callback
  esp_now_register_send_cb(onSent);
  
  // Add peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;
  
  result = esp_now_add_peer(&peerInfo);
  if (result != ESP_OK) {
    Serial.printf("Failed to add peer: 0x%X\n", result);
    // Continue anyway - we might be able to send without adding peer
  } else {
    Serial.println("Peer added successfully");
  }
  
  espNowInitialized = true;
  Serial.println("ESP-NOW initialized successfully");
  return true;
}

bool connectToWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.disconnect(true);
  delay(100);
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  unsigned long startTime = millis();
  const unsigned long timeout = 300000; // 300 second timeout
  
  while (WiFi.status() != WL_CONNECTED && millis() - startTime < timeout) {
    delay(500);
    Serial.print(".");
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected to WiFi");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());
    return true;
  } else {
    Serial.println("\nWiFi connection failed");
    return false;
  }
}

bool syncTimeWithNTP() {
  Serial.println("Attempting time synchronization...");
  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  
  // Wait for time sync with timeout
  unsigned long startTime = millis();
  const unsigned long timeout = 300000; // 300 second timeout
  
  while (!getLocalTime(&timeinfo) && millis() - startTime < timeout) {
    delay(500);
    Serial.print(".");
  }
  
  if (getLocalTime(&timeinfo)) {
    Serial.println("\nTime synchronized successfully");
    lastTimeSync = millis();
    return true;
  } else {
    Serial.println("\nTime synchronization failed");
    return false;
  }
}

void updateLocalTime() {
  unsigned long now = millis();
  
  // Check if we need to re-sync time (after 1 hour)
  if (now - lastTimeSync >= timeSyncInterval) {
    Serial.println("Hourly time sync check...");
    if (WiFi.status() == WL_CONNECTED) {
      if (syncTimeWithNTP()) {
        Serial.println("Time re-synchronized successfully");
      } else {
        Serial.println("Time re-synchronization failed, continuing with local time");
      }
    } else {
      Serial.println("Cannot re-sync time - WiFi not connected");
      // Attempt to reconnect WiFi for time sync
      if (connectToWiFi()) {
        syncTimeWithNTP();
      }
    }
  } else {
    // Update time locally (increment seconds)
    static unsigned long lastSecondUpdate = 0;
    if (now - lastSecondUpdate >= 1000) {
      lastSecondUpdate = now;
      timeinfo.tm_sec++;
      
      // Handle time rollover
      if (timeinfo.tm_sec >= 60) {
        timeinfo.tm_sec = 0;
        timeinfo.tm_min++;
        
        if (timeinfo.tm_min >= 60) {
          timeinfo.tm_min = 0;
          timeinfo.tm_hour++;
          
          if (timeinfo.tm_hour >= 24) {
            timeinfo.tm_hour = 0;
          }
        }
      }
    }
  }
}

//*****************************************************************************************************/
void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP-NOW Sender starting...");

  // STEP 1: Connect to WiFi FIRST (blocking)
  Serial.println("=== SETUP PHASE: CONNECTING TO WIFI ===");
  if (!connectToWiFi()) {
    Serial.println("FATAL: Could not connect to WiFi. Using default time.");
    // Set default time
    timeinfo.tm_hour = 12;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
  } else {
    wifiConnected = true;
  }

  // STEP 2: Sync time with NTP (blocking)
  Serial.println("=== SETUP PHASE: SYNCING TIME ===");
  if (wifiConnected) {
    if (!syncTimeWithNTP()) {
      Serial.println("Time sync failed. Using default time.");
      timeinfo.tm_hour = 12;
      timeinfo.tm_min = 0;
      timeinfo.tm_sec = 0;
    }
  }

  // STEP 3: Initialize ESP-NOW
  Serial.println("=== SETUP PHASE: INITIALIZING ESP-NOW ===");
  if (!initializeESP_NOW()) {
    Serial.println("Warning: ESP-NOW initialization failed. Will retry in loop.");
  }

  Serial.println("=== SETUP COMPLETE ===");
  Serial.println("Sender ready. Starting main loop...");
}

void loop() {
  unsigned long now = millis();
  
  // Update local time continuously (includes hourly sync check)
  updateLocalTime();
  
  if (now - lastSendTime >= interval) {
    lastSendTime = now;

    // Get current time string in HH:MM format
    char timeStr[16];
    snprintf(timeStr, sizeof(timeStr), "%02d:%02d", timeinfo.tm_hour, timeinfo.tm_min);

    // Create message with format: "instruction|HH:MM"
    char message[32];
    const char *instruction = instructions[currentIndex];
    snprintf(message, sizeof(message), "%s|%s", instruction, timeStr);

    // Only try to send if ESP-NOW is initialized
    if (espNowInitialized) {
      esp_err_t result = esp_now_send(receiverMAC, (uint8_t *)message, strlen(message));
      Serial.printf("Sending: %s (Time: %s)\n", instruction, timeStr);

      if (result != ESP_OK) {
        Serial.printf("Error sending message: 0x%X\n", result);
        // Try to reinitialize ESP-NOW if sending fails
        static int sendFailCount = 0;
        sendFailCount++;
        if (sendFailCount > 3) {
          Serial.println("Multiple send failures, reinitializing ESP-NOW...");
          esp_now_deinit();
          espNowInitialized = false;
          delay(100);
          initializeESP_NOW();
          sendFailCount = 0;
        }
      } else {
        // Reset fail count on successful send
        static int sendFailCount = 0;
        sendFailCount = 0;
      }
    } else {
      Serial.printf("Message ready but ESP-NOW not initialized: %s (Time: %s)\n", instruction, timeStr);
      // Try to initialize ESP-NOW if not initialized
      initializeESP_NOW();
    }

    // Move to next instruction
    currentIndex = (currentIndex + 1) % numInstructions;
  }

  // Small delay to prevent WiFi issues
  delay(10);
}