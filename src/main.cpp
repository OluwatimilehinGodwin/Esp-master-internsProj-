#include <Arduino.h>
#include <esp_now.h>
#include <WiFi.h>

// Receiver MAC address (replace with your CYD ESP32's MAC)
uint8_t receiverMAC[] = {0x78, 0xEE, 0x4C, 0x02, 0x17, 0x54};

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
const unsigned long interval = 5000;  // 5 seconds

void setup() {
  Serial.begin(115200);
  Serial.println();
  Serial.println("ESP-NOW Sender starting...");

  // Set WiFi to station mode
  WiFi.mode(WIFI_STA);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    while (true) { delay(1000); }
  }

  // Register send callback
  esp_now_register_send_cb(onSent);

  // Add peer
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;  
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("Failed to add peer");
    while (true) { delay(1000); }
  }

  Serial.println("Sender ready.");
}

void loop() {
  unsigned long now = millis();
  if (now - lastSendTime >= interval) {
    lastSendTime = now;

    // Pick current instruction
    const char *msg = instructions[currentIndex];
    esp_err_t result = esp_now_send(receiverMAC, (uint8_t *)msg, strlen(msg));

    Serial.printf("Sending: %s\n", msg);

    if (result != ESP_OK) {
      Serial.println("Error sending the message");
    }

    // Move to next instruction
    currentIndex = (currentIndex + 1) % numInstructions;
  }
}
