/*
 * "E220_WOR_Transmitter_Gemini.ino"
 *  Expertly tuned for Dual-Core FreeRTOS with Deep Sleep & KY002S Latch
 *  01/09/2026 @ 06:02 EST  Arduino IDE 2.3.7   ESP32 Board Manager 3.3.5
 * 
 *   William Lucid and Team, AI: Copilot, Claude, and Gemini
 * 
 *   Microsoft Copilot's Dual-core refactor:
 *   ---------------------------------------
 *  - WiFi + web server on Core 0  
 *  - WOR / radio logic on Core 1
 *  - AUX interrupt (FALLING) -> task notification (no polling)
 *  - switchFlag consumed only in switchOne()
 *  - sendPreamble() and sendOutgoing() run unconditionally
 *  - No blocking loops on Core 0 (WiFi stays healthy)
 *
 */

#include <Arduino.h>
#include "LoRa_E220.h"
#include "driver/rtc_io.h"

// --- Hardware Pins ---
#define RXD2 16
#define TXD2 17
#define M0_PIN GPIO_NUM_19
#define M1_PIN GPIO_NUM_21
#define AUX_PIN GPIO_NUM_15  // EXT0 wake source  
#define KY002S_TRIGGER 32
#define KY002S_STATUS 33

LoRa_E220 e220ttl(&Serial2, AUX_PIN, M0_PIN, M1_PIN);

// --- Protocol ---
const int MAX_dateTime_LENGTH = 40;
struct Message {
  int switchState; // 1 = ON, 2 = OFF  
  char dateTime[MAX_dateTime_LENGTH];  
};

// --- FreeRTOS & Sync ---
volatile bool inboxReady = false;  
Message inbox; 
bool pulseActive = false;
uint32_t pulseStartMs = 0;
const uint16_t PULSE_MS = 200;  

// --- Forward Declarations ---
void enterDeepSleep();
void core0Task(void* parameter);
void core1Task(void* parameter);

// ================================================================
// Setup
// ================================================================
void setup() {
  Serial.begin(115200);
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);  

  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);
  pinMode(AUX_PIN, INPUT);  
  pinMode(KY002S_TRIGGER, OUTPUT);
  digitalWrite(KY002S_TRIGGER, LOW);
  pinMode(KY002S_STATUS, INPUT);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();  

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("EXT0 Wake: Data Incoming. Launching Tasks...");
    
    // Clear holds from sleep to allow mode changes  
    gpio_hold_dis(M0_PIN);
    gpio_hold_dis(M1_PIN);
    gpio_deep_sleep_hold_dis();

    xTaskCreatePinnedToCore(core0Task, "Comm", 4096, NULL, 2, NULL, 0);  
    xTaskCreatePinnedToCore(core1Task, "Logic", 4096, NULL, 1, NULL, 1);  
  } else {
    Serial.println("Power-On: Configuring Radio and Sleeping...");
    e220ttl.begin();
    delay(1000);
    enterDeepSleep();
  }
}

// ================================================================
// Core 0: COMM - E220 Buffer Management
// ================================================================
void core0Task(void* parameter) {
  // Wait for AUX to go HIGH (Signal that data is fully buffered)  
  uint32_t start = millis();
  while (digitalRead(AUX_PIN) == LOW) {
    if (millis() - start > 4000) { // 4s timeout for 2000ms WOR preamble
      Serial.println("Core0 Error: AUX Timeout");
      vTaskDelete(NULL);
      return;
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  e220ttl.setMode(MODE_2_WOR_RECEIVER); // Settle radio for reading  
  vTaskDelay(20 / portTICK_PERIOD_MS);

  if (e220ttl.available() > 0) {
    ResponseStructContainer rsc = e220ttl.receiveMessageRSSI(sizeof(Message));  
    if (rsc.status.code == 1) {
      Message* msgPtr = (Message*)rsc.data;
      inbox.switchState = msgPtr->switchState;  
      strncpy(inbox.dateTime, msgPtr->dateTime, MAX_dateTime_LENGTH);
      inbox.dateTime[MAX_dateTime_LENGTH - 1] = '\0';
      inboxReady = true;  
    }
    rsc.close();
  }
  vTaskDelete(NULL);
}

// ================================================================
// Core 1: LOGIC - KY002S Control & Deep Sleep
// ================================================================
void core1Task(void* parameter) {
  for (;;) {
    if (inboxReady) {
      inboxReady = false;
      Message msg = inbox;  

      bool isCurrentlyOn = (digitalRead(KY002S_STATUS) == HIGH);  
      
      // Logic: Switch if 1(ON) and currently OFF, or if 2(OFF) and currently ON
      if ((msg.switchState == 1 && !isCurrentlyOn) || (msg.switchState == 2 && isCurrentlyOn)) {
        Serial.println("Toggling KY002S...");
        digitalWrite(KY002S_TRIGGER, HIGH);  
        vTaskDelay(PULSE_MS / portTICK_PERIOD_MS);  
        digitalWrite(KY002S_TRIGGER, LOW);  
      }

      Serial.println("Cycle Complete. Sleeping...");
      enterDeepSleep();  
    }
    vTaskDelay(10 / portTICK_PERIOD_MS); // Yield to prevent WDT 
  }
}

// ================================================================
// Power Management
// ================================================================
void enterDeepSleep() {
  e220ttl.setMode(MODE_2_WOR_RECEIVER);  
  vTaskDelay(50 / portTICK_PERIOD_MS);

  // Maintain M0/M1 state during sleep  
  gpio_hold_en(M0_PIN);
  gpio_hold_en(M1_PIN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_ext0_wakeup(AUX_PIN, 0);  
  esp_deep_sleep_start();
}

void loop() {}
