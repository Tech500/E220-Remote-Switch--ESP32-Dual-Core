#include <Arduino.h>
#include "LoRa_E220.h"
#include "driver/rtc_io.h"

// --- Hardware Pins (Update to match your wiring) ---
#define RXD2 16
#define TXD2 17
#define M0_PIN GPIO_NUM_19
#define M1_PIN GPIO_NUM_21
#define AUX_PIN GPIO_NUM_15 
#define TRIGGER_PIN 32
#define STATE_PIN 33

LoRa_E220 e220ttl(&Serial2, AUX_PIN, M0_PIN, M1_PIN);

// --- Protocol Struct ---
const int MAX_dateTime_LENGTH = 40;
struct Message {
  int switchState; // 1 = ON, 2 = OFF
  char dateTime[MAX_dateTime_LENGTH];
};

volatile bool inboxReady = false;
Message inbox;

// --- Forward Declarations ---
void enterDeepSleep();
void core0Task(void* parameter);
void core1Task(void* parameter);

void setup() {
  Serial.begin(115200);
  
  // Initialize radio immediately to prevent null-pointer errors
  e220ttl.begin(); 
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);

  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);
  pinMode(AUX_PIN, INPUT);
  pinMode(TRIGGER_PIN, OUTPUT);
  digitalWrite(TRIGGER_PIN, LOW);
  pinMode(STATE_PIN, INPUT);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  
  if (cause != ESP_SLEEP_WAKEUP_EXT0) {
	  Serial.println("\nPower-on or Reset Wakeup");
	  Serial.println("Waiting on Web Request");
	  enterDeepSleep();
  }	  

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("EXT0 Wake: Launching Tasks...");
    
    // Release pins from sleep hold
    gpio_hold_dis(M0_PIN);      
	gpio_hold_dis(M1_PIN);     
	gpio_deep_sleep_hold_dis();  
	
    xTaskCreatePinnedToCore(core0Task, "Comm", 4096, NULL, 2, NULL, 0);      
	xTaskCreatePinnedToCore(core1Task, "Logic", 4096, NULL, 1, NULL, 1);    } else {
    
	Serial.println("Initial Power-On → Entering Sleep Mode");
    enterDeepSleep(); 
  }
}

// ================================================================
// Core 0: Communicates with E220
// ================================================================
void core0Task(void* parameter) {
  uint32_t start = millis();
  
  // Wait for AUX to return HIGH (end of LoRa transmission)    while (digitalRead(AUX_PIN) == LOW && (millis() - start < 4000)) {
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }

  // Brief mode switch to ensure Serial buffer is ready
  e220ttl.setMode(MODE_0_NORMAL);    vTaskDelay(50 / portTICK_PERIOD_MS);

  if (e220ttl.available() > 0) {      ResponseStructContainer rsc = e220ttl.receiveMessageRSSI(sizeof(Message));      
    // Safety check: Ensure status is valid and data is NOT NULL      if (rsc.status.code == 1 && rsc.data != nullptr) {
      Message* msgPtr = (Message*)rsc.data;        
      inbox.switchState = msgPtr->switchState;        strncpy(inbox.dateTime, msgPtr->dateTime, MAX_dateTime_LENGTH);        inbox.dateTime[MAX_dateTime_LENGTH - 1] = '\0';
      
      inboxReady = true;      }
    rsc.close();    }
  
  vTaskDelete(NULL); 
}

// ================================================================
// Core 1: Logic and KY002S Trigger
// ================================================================
void core1Task(void* parameter) {
  for (;;) {
    if (inboxReady) {
      inboxReady = false;        
      bool isCurrentlyOn = (digitalRead(STATE_PIN) == HIGH);        
      // Execute toggle only if state needs to change
      if ((inbox.switchState == 1 && !isCurrentlyOn) || (inbox.switchState == 2 && isCurrentlyOn)) {
        Serial.println("Triggering KY002S...");
        digitalWrite(TRIGGER_PIN, HIGH);          vTaskDelay(200 / portTICK_PERIOD_MS);          digitalWrite(TRIGGER_PIN, LOW);        }

      Serial.println("Processing complete. Returning to sleep.");
      enterDeepSleep();      }
    vTaskDelay(10 / portTICK_PERIOD_MS);    }
}

// ================================================================
// Sleep Management
// ================================================================
void enterDeepSleep() {
  e220ttl.setMode(MODE_2_WOR_RECEIVER);    
  // Use delay() if scheduler isn't running to prevent crashes    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING) {
    delay(100);
  } else {
    vTaskDelay(100 / portTICK_PERIOD_MS);
  }

  // Enable Pin Holds for M0/M1 to maintain radio state    gpio_hold_en(M0_PIN); 
  gpio_hold_en(M1_PIN);
  gpio_deep_sleep_hold_en();

  // Wake up when E220 pulls AUX LOW    esp_sleep_enable_ext0_wakeup(AUX_PIN, 0); 
  esp_deep_sleep_start();  }

void loop() {}