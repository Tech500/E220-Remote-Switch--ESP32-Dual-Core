#include <Arduino.h>

#define FREQUENCY_915
#include "LoRa_E220.h"
#include "driver/rtc_io.h"

#define CHANNEL 23
#define DESTINATION_ADDL 2

// --- Hardware Pins ---
#define RXD2 16
#define TXD2 17
#define M0_PIN GPIO_NUM_19
#define M1_PIN GPIO_NUM_21
#define AUX_PIN GPIO_NUM_15 
#define KY002S_TRIGGER 32
#define STATE_PIN 33

LoRa_E220 e220ttl(&Serial2, AUX_PIN, M0_PIN, M1_PIN);

// --- Protocol Struct ---
const int MAX_dateTime_LENGTH = 40;
struct Message {
  int switchState; // 1 = ON, 2 = OFF [cite: 2, 3]
  char dateTime[MAX_dateTime_LENGTH];
};

volatile bool inboxReady = false;
Message inbox;
int delayTime = 1000; [cite: 4]

// --- Forward Declarations ---
void enterDeepSleep();

void initRadio() {
  Serial.println("Initializing E220 radio..."); [cite: 5]

  pinMode(AUX_PIN, INPUT);
  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);

  // Start UART [cite: 6]
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  vTaskDelay(pdMS_TO_TICKS(50));

  // Start library
  e220ttl.begin();
  vTaskDelay(pdMS_TO_TICKS(50));

  // Set mode to NORMAL for configuration or active receiving [cite: 7]
  e220ttl.setMode(MODE_0_NORMAL);
  vTaskDelay(pdMS_TO_TICKS(50));

  Serial.println("E220 Ready.");
}

void setup() {
  Serial.begin(115200);
  
  initRadio(); [cite: 8]

  pinMode(KY002S_TRIGGER, OUTPUT);
  digitalWrite(KY002S_TRIGGER, LOW); [cite: 9]
  pinMode(STATE_PIN, INPUT);

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  
  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    Serial.println("EXT0 Wake: Handling Message..."); [cite: 11]
    
    // Release pins from sleep hold
    gpio_hold_dis(M0_PIN);      
    gpio_hold_dis(M1_PIN);     
    gpio_deep_sleep_hold_dis(); 

    // Receive the message that triggered the wake
    ResponseStructContainer rsc = e220ttl.receiveMessage(sizeof(Message)); [cite: 11]
    
    if (rsc.status.code == 1) {  // Success [cite: 12]
      Message message = *(Message*)rsc.data;
      rsc.close(); [cite: 14]

      // Send Acknowledgement [cite: 15]
      e220ttl.sendFixedMessage(0, DESTINATION_ADDL, CHANNEL, "Message Received!");
      
      if (message.switchState == 1) {
        Serial.println("Action: Power ON"); [cite: 19]
        digitalWrite(KY002S_TRIGGER, HIGH);
        delay(200); // Pulse for latch
        digitalWrite(KY002S_TRIGGER, LOW);
      } 
      else if (message.switchState == 2) {
        Serial.println("Action: Power OFF"); [cite: 20]
        digitalWrite(KY002S_TRIGGER, LOW);
      }
      
      Serial.println(message.dateTime); [cite: 19]
    }
    
    // Return to sleep after processing
    enterDeepSleep();
  } 
  else {
    Serial.println("Power-on/Reset -> Entering Sleep Mode"); [cite: 9, 10]
    enterDeepSleep(); 
  }
}

void enterDeepSleep() {
  Serial.println("Preparing for Deep Sleep...");
  
  // Set radio to WOR Receiver mode so it can wake the ESP32
  e220ttl.setMode(MODE_2_WOR_RECEIVER); 
  vTaskDelay(pdMS_TO_TICKS(50));

  // Enable Pin Holds for M0/M1 to maintain radio mode during sleep [cite: 32]
  gpio_hold_en(M0_PIN);
  gpio_hold_en(M1_PIN);

  // Wake up when E220 pulls AUX LOW 
  esp_sleep_enable_ext0_wakeup(AUX_PIN, 0); 
  
  Serial.println("Zzz...");
  Serial.flush(); // Fixes broken CR/LF by ensuring text is sent before power-down
  
  esp_deep_sleep_start(); // Proper deep sleep command 
}

void loop() {
  // Loop is not used with Deep Sleep
}
