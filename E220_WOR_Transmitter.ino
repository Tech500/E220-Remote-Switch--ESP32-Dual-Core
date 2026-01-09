/*
 * "E220_WOR_Transmitter_Gemini.ino"
 * Expertly tuned for Dual-Core FreeRTOS with Deep Sleep & KY002S Latch
 *  "C:\Users\William\Desktop\Gemini\E220_WOR_Transmitter_Gemini\E220_WOR_Transmitter_Gemini.ino"
 *  01/08/2026 @ 04:05 EST  Arduino IDE 2.3.7   ESP32 Board Manager 3.3.5
 * 
 *  Work from this copy!
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
 * EBYTE LoRa E220
 * Stay in sleep mode and wait a wake up WOR message
 *
 * You must configure the address with 0 2 23 (FIXED RECEIVER configuration)
 * and pay attention that WOR period must be the same of sender.
 *
 * E220       ----- WeMos D1 mini   ----- esp32           ----- Arduino Nano 33 IoT
 * M0         ----- D7 (or GND)     ----- 19 (or GND)
 * M1         ----- D6 (or 3.3v)    ----- 21 (or 3.3v)
 * TX         ----- D3 (PullUP)     ----- TX2 (PullUP)
 * RX         ----- D4 (PullUP)     ----- RX2 (PullUP)
 * AUX        ----- D5 (PullUP)     ----- 15  (PullUP)
 * VCC        ----- 3.3v/5v         ----- 3.3v/5v
 * GND        ----- GND             ----- GND
 */

// With FIXED SENDER configuration
#define DESTINATION_ADDL 3

#define FREQUENCY_915
#define CHANNEL 23

#include "Arduino.h"
#include "LoRa_E220.h"
#include "WiFi.h"
#include <WiFiUdp.h>
#include <HTTPClient.h>
#include <time.h>
#include <Ticker.h>
#include <AsyncTCP.h>
#include "ESPAsyncWebServer.h"
#include "soc/rtc_cntl_reg.h"
#include "soc/rtc.h"
#include "driver/rtc_io.h"

#include "index7.h"

// ---------------------------
// E220 wiring: RX AUX M0 M1
// ---------------------------
LoRa_E220 e220ttl(&Serial2, 15, 21, 19);

#define RXD2 16
#define TXD2 17

#define M0_PIN GPIO_NUM_21
#define M1_PIN GPIO_NUM_19
#define AUX_PIN GPIO_NUM_15  // GPIO_NUM_15 is used for AUX and deep-sleep wake

#define TRIGGER 32     // KY002S MOSFET Bi-Stable Switch (drive)
#define KY002S_PIN 33  // KY002S MOSFET Bi-Stable Switch (sense)
#define ALERT 4        // ina226 Battery Monitor (input)

RTC_DATA_ATTR int bootCount = 0;
RTC_DATA_ATTR int activations = 0;

// ---------------------------
// Network / NTP
// ---------------------------
const char *ssid = "R2D2";
const char *password = "Sky7388500";

WiFiClient client;
boolean connected = false;

AsyncWebServer server(80);

// Define the maximum length for the dateTime
const int MAX_dateTime_LENGTH = 40;

char time_output[MAX_dateTime_LENGTH];

volatile int switchData = 0;

struct Message {
  int32_t switchData;
  char dateTime[40];
} __attribute__((packed));

// Transmitter Variables
Message outgoing;                  // Already in your code [cite: 16]
volatile bool switchFlag = false;  // Already in your code [cite: 19]

// Add this to fix the compiler error if you are using the new Task logic
volatile bool inboxReady = false;
Message inbox;

// Used by processor7() for index7.h
String linkAddress = "192.168.12.27:80";

WiFiUDP udp;
const int udpPort = 1157;
char incomingPacket[255];
char replyPacket[] = "Hi there! Got the message :-)";
// NTP Time Servers
const char *udpAddress1 = "pool.ntp.org";
const char *udpAddress2 = "time.nist.gov";

#define TZ "EST+5EDT,M3.2.0/2,M11.1.0/2"  // Time zone set to Indianapolis

int delayTime = 1000;  // setMode delay duration

int receiverWakeTime = 2000;

int pulseDuration = 100;

// Struct to hold date and time components
struct DateTime {
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;
};

// Tickers and timing
Ticker oneTick;
Ticker onceTick;

int cameraPowerOff = 0;

// 1. Volatile flags for ISR contexts
volatile bool interruptExecuted = false;
volatile bool countdownExpired = false;

// 2. Countdown management
int needAnotherCountdown = 0;

// ---------------------------
// Dual-core WOR / task state
// ---------------------------

// WOR task handle
TaskHandle_t worTaskHandle = NULL;

// Cross-core event flags
//volatile int switchData = 0;       // payload/state indicator for switchOne  (part of struct outgoing)

// Critical section mutex
portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;

// ---------------------------
// Forward declarations
// ---------------------------
void wifi_Start();
void configTimeCustom();
String get_time();
DateTime getCurrentDateTime();
void webInterface();
void enterDeepSleep();

// ---------------------------
// ISR: countdown expiry
// ---------------------------
void IRAM_ATTR countdownTrigger() {
  countdownExpired = true;
}

// ---------------------------
// ISR: generic wake flag (legacy)
// ---------------------------
void IRAM_ATTR wakeUp() {
  interruptExecuted = true;
}

// ---------------------------
// AUX ISR: FALLING edge (HIGH -> LOW)
// Used as "module idle / TX complete" notifier
// ---------------------------
void IRAM_ATTR auxISR() {
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  if (worTaskHandle != NULL) {
    vTaskNotifyGiveFromISR(worTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
  }
}

// ---------------------------
// Utility: updateTimestamp into time_output
// ---------------------------
void updateTimestamp() {
  time_t now;
  time(&now);
  strftime(time_output, MAX_dateTime_LENGTH, "%a  %m/%d/%y   %T", localtime(&now));
}

// ---------------------------
// Optional: print reset reason
// ---------------------------
void printResetReason() {
  esp_reset_reason_t reason = esp_reset_reason();
  switch (reason) {
    case ESP_RST_POWERON: Serial.println("Reset due to power-on"); break;
    case ESP_RST_SW: Serial.println("Software reset"); break;
    case ESP_RST_PANIC: Serial.println("Software reset due to panic"); break;
    case ESP_RST_INT_WDT: Serial.println("Interrupt watchdog reset"); break;
    case ESP_RST_TASK_WDT: Serial.println("Task watchdog reset"); break;
    case ESP_RST_WDT: Serial.println("Other watchdog reset"); break;
    case ESP_RST_DEEPSLEEP: Serial.println("Reset after deep sleep"); break;
    case ESP_RST_BROWNOUT: Serial.println("Brownout reset"); break;
    case ESP_RST_SDIO: Serial.println("SDIO reset"); break;
    default: Serial.println("Unknown reset reason"); break;
  }
}

// ================================================================
// Power Management
// ================================================================
void enterDeepSleep() {
  e220ttl.setMode(MODE_2_WOR_RECEIVER);
  vTaskDelay(50 / portTICK_PERIOD_MS);

  waitForAux();  //Wait until AUX_PIN is ready (HIGH)

  // Maintain M0/M1 state during sleep
  gpio_hold_en(M0_PIN);
  gpio_hold_en(M1_PIN);
  gpio_deep_sleep_hold_en();

  esp_sleep_enable_ext0_wakeup(AUX_PIN, 0);
  esp_deep_sleep_start();
}

// ---------------------------
// WOR: wait for AUX (polling)
// ---------------------------
void waitForAux() {
  uint32_t start = millis();
  while (digitalRead(AUX_PIN) == LOW && (millis() - start < 4000)) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}


// ---------------------------
// WOR: send preamble (no switchFlag logic)
// ---------------------------
void sendPreamble() {
  Serial.println("\nEntering WOR transmitter mode...");

  //waitForAux();
  e220ttl.setMode(MODE_1_WOR_TRANSMITTER);
  vTaskDelay(pdMS_TO_TICKS(100));

  Serial.println("Sending WOR preamble...");
  ResponseStatus rs = e220ttl.sendFixedMessage(0, DESTINATION_ADDL, CHANNEL,
                                               "Hello, world? WOR!");
  Serial.println(rs.getResponseDescription());

  // ADD THIS: Wait for receiver to boot and be ready
  Serial.println("Waiting 3 seconds for receiver to boot...");
  vTaskDelay(pdMS_TO_TICKS(3000));
  Serial.println("Proceeding to send data message");

  Serial.print("SwitchData value:  " + String(switchData));
  sendOutgoing(switchData);
}

// ---------------------------
// Send Payload
// ---------------------------
int sendOutgoing(int switchData) {
  Serial.println("\n\n--- sendOutgoing() START ---");

  Serial.print("Value of switchData sending payload: ");
  Serial.println(switchData);

  // Ensure module is idle
  waitForAux();

  // Prepare payload
  memset(&outgoing, 0, sizeof(Message));
  outgoing.switchData = switchData;

  // Get time and copy into struct (TX has NTP)
  get_time();  // fills time_output
  strncpy(outgoing.dateTime, time_output, MAX_dateTime_LENGTH - 1);
  outgoing.dateTime[MAX_dateTime_LENGTH - 1] = '\0';

  Serial.print("Sending outgoing message for switchData: ");
  Serial.println(outgoing.switchData);
  Serial.print("Timestamp: ");
  Serial.println(outgoing.dateTime);

  // ---- ACTUAL SEND: struct in transparent mode ----
  ResponseStatus rs = e220ttl.sendMessage(&outgoing, sizeof(Message));

  Serial.print("sendMessage status: ");
  Serial.println(rs.getResponseDescription());

  // Wait for AUX to confirm TX complete
  waitForAux();

  Serial.println("--- sendOutgoing() END ---\n");
  return (rs.code == 1) ? 1 : 0;
}





// ---------------------------
// Dispatcher: the ONLY consumer of switchFlag
// ---------------------------
int switchOne(int switchData) {

  Serial.print("SwitchData value:  " + String(switchData));

  //Prevent duplicate WOR sends
  if (!switchFlag) {
    Serial.println("WOR send blocked: switchFlag is false");
    return 0;
  }

  // Consume the event
  switchFlag = false;

  sendPreamble();
  waitForAux();

  if (switchData == 1) {
    onceTick.once(60, countdownTrigger);  //Start countdown timer
    Serial.println("Echoing Receiver messaging");
    Serial.println("\nESP32 waking from Deep Sleep");
    Serial.println("Battery Switch is ON\n");
  }

  if (switchData == 2) {
    Serial.println("Echoing Receiver messaging");
    Serial.println("\nBattery power switched OFF");
    Serial.println("ESP32 going to Deep Sleep\n");
  }

  Serial.print("Value of switchData:  ");
  Serial.println(switchData);
  Serial.println("switchData forwarded to preamble");

  return switchData;
}

// ---------------------------
// Template processor for index7.h
// ---------------------------
String processor7(const String &var) {

  // index7.h placeholders
  if (var == F("LINK"))
    return linkAddress;

  return String();
}

// ---------------------------
// E220 configuration printer (from library)
// ---------------------------
void printParameters(struct Configuration configuration) {
  DEBUG_PRINTLN("----------------------------------------");

  DEBUG_PRINT(F("HEAD : "));
  DEBUG_PRINT(configuration.COMMAND, HEX);
  DEBUG_PRINT(" ");
  DEBUG_PRINT(configuration.STARTING_ADDRESS, HEX);
  DEBUG_PRINT(" ");
  DEBUG_PRINTLN(configuration.LENGHT, HEX);
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINT(F("AddH : "));
  DEBUG_PRINTLN(configuration.ADDH, HEX);
  DEBUG_PRINT(F("AddL : "));
  DEBUG_PRINTLN(configuration.ADDL, HEX);
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINT(F("Chan : "));
  DEBUG_PRINT(configuration.CHAN, DEC);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.getChannelDescription());
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINT(F("SpeedParityBit     : "));
  DEBUG_PRINT(configuration.SPED.uartParity, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.SPED.getUARTParityDescription());
  DEBUG_PRINT(F("SpeedUARTDatte     : "));
  DEBUG_PRINT(configuration.SPED.uartBaudRate, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.SPED.getUARTBaudRateDescription());
  DEBUG_PRINT(F("SpeedAirDataRate   : "));
  DEBUG_PRINT(configuration.SPED.airDataRate, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.SPED.getAirDataRateDescription());
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINT(F("OptionSubPacketSett: "));
  DEBUG_PRINT(configuration.OPTION.subPacketSetting, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.OPTION.getSubPacketSetting());
  DEBUG_PRINT(F("OptionTranPower    : "));
  DEBUG_PRINT(configuration.OPTION.transmissionPower, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.OPTION.getTransmissionPowerDescription());
  DEBUG_PRINT(F("OptionRSSIAmbientNo: "));
  DEBUG_PRINT(configuration.OPTION.RSSIAmbientNoise, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.OPTION.getRSSIAmbientNoiseEnable());
  DEBUG_PRINTLN(F(" "));
  DEBUG_PRINT(F("TransModeWORPeriod : "));
  DEBUG_PRINT(configuration.TRANSMISSION_MODE.WORPeriod, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.TRANSMISSION_MODE.getWORPeriodByParamsDescription());
  DEBUG_PRINT(F("TransModeEnableLBT : "));
  DEBUG_PRINT(configuration.TRANSMISSION_MODE.enableLBT, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.TRANSMISSION_MODE.getLBTEnableByteDescription());
  DEBUG_PRINT(F("TransModeEnableRSSI: "));
  DEBUG_PRINT(configuration.TRANSMISSION_MODE.enableRSSI, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.TRANSMISSION_MODE.getRSSIEnableByteDescription());
  DEBUG_PRINT(F("TransModeFixedTrans: "));
  DEBUG_PRINT(configuration.TRANSMISSION_MODE.fixedTransmission, BIN);
  DEBUG_PRINT(" -> ");
  DEBUG_PRINTLN(configuration.TRANSMISSION_MODE.getFixedTransmissionDescription());

  DEBUG_PRINTLN("----------------------------------------");
}

void core0Task(void *parameter) {
  uint32_t start = millis();

  // 1. Wait for AUX to return HIGH (end of LoRa transmission)
  while (digitalRead(AUX_PIN) == LOW && (millis() - start < 4000)) {
    vTaskDelay(10 / portTICK_PERIOD_MS);  //[cite: 144]
  }

  // 2. Switch to NORMAL mode to enable the Serial port
  e220ttl.setMode(MODE_0_NORMAL);
  vTaskDelay(100 / portTICK_PERIOD_MS);  // Increased from 50ms for stability

  // 3. Read the message
  if (e220ttl.available() > 0) {                                                //[cite: 146]
    ResponseStructContainer rsc = e220ttl.receiveMessageRSSI(sizeof(Message));  //[cite: 146]
    if (rsc.status.code == 1 && rsc.data != nullptr) {                          //[cite: 147]
      Message *msgPtr = (Message *)rsc.data;
      outgoing.switchData = msgPtr->switchData;
      strncpy(outgoing.dateTime, msgPtr->dateTime, MAX_dateTime_LENGTH);  //[cite: 148]
      outgoing.dateTime[MAX_dateTime_LENGTH - 1] = '\0';

      inboxReady = true;
      Serial.println("Packet Processed Successfully!");
    }
    rsc.close();  //[cite: 148]
  } else {
    Serial.println("No data found in E220 buffer.");
  }

  vTaskDelete(NULL);
}

// ---------------------------
// WOR Task on Core 1
// ---------------------------
void WORTask(void *pvParameters) {
  Serial.print("WORTask running on core ");
  Serial.println(xPortGetCoreID());

  switchFlag = true;

  for (;;) {

    // Sleep until AUX or external event notifies us
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    bool localFlag;
    int localData;

    // Safely copy shared state
    portENTER_CRITICAL(&mux);
    localFlag = switchFlag;
    localData = switchData;
    portEXIT_CRITICAL(&mux);

    if (localFlag) {
      switchOne(localData);
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ---------------------------
// Event trigger: called from web or countdown
// ---------------------------
void triggerSwitchEvent(int value) {
  portENTER_CRITICAL(&mux);
  switchData = value;
  switchFlag = true;
  portEXIT_CRITICAL(&mux);

  if (worTaskHandle != NULL) {
    xTaskNotifyGive(worTaskHandle);  // wake WOR task
  }
}

// ---------------------------
// WiFi setup with static IP
// ---------------------------
void wifi_Start() {
  // Static IP configuration
  IPAddress local_IP(192, 168, 12, 27);  // your chosen IP
  IPAddress gateway(192, 168, 12, 1);    // router gateway
  IPAddress subnet(255, 255, 255, 0);    // subnet mask
  IPAddress primaryDNS(8, 8, 8, 8);      // optional
  IPAddress secondaryDNS(8, 8, 4, 4);

  // Configure static IP BEFORE WiFi.begin()
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    // Serial.println("STA Failed to configure");
  }

  WiFi.begin(ssid, password);

  Serial.print("Connecting");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  connected = true;
  server.begin();
}

void initRadio() {
  Serial.println("Initializing E220 radio...");

  pinMode(AUX_PIN, INPUT);
  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);

  // Start UART
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  vTaskDelay(pdMS_TO_TICKS(50));

  // Start library
  e220ttl.begin();
  vTaskDelay(pdMS_TO_TICKS(50));

  // Put radio into NORMAL mode first
  e220ttl.setMode(MODE_0_NORMAL);
  vTaskDelay(pdMS_TO_TICKS(50));

  // Optional: read module configuration
  // ResponseStructContainer c = e220ttl.getConfiguration();
  // c.close();

  Serial.println("E220 Ready.");
}

// ---------------------------
// Setup
// ---------------------------
void setup() {

  Serial.begin(115200);
  delay(200);

  // Initialize E220
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(50);

  initRadio();

  Serial.begin(115200);
  delay(200);

  wifi_Start();

  server.begin();

  configTimeCustom();
  delay(1000);

  esp_reset_reason_t rr = esp_reset_reason();
  //esp_sleep_wakeup_cause_t wc = esp_sleep_get_wakeup_cause();

  if (rr != ESP_RST_DEEPSLEEP) {
    Serial.println("Waiting for Web Request!");
  }

  e220ttl.begin();
  vTaskDelay(pdMS_TO_TICKS(50));

  pinMode(AUX_PIN, INPUT);
  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);

  int value = digitalRead(KY002S_PIN);  // KY002S, Vo pin
  if (value == HIGH) {
    digitalWrite(TRIGGER, HIGH);  // Toggle MOSFET Bistable Switch
    delay(pulseDuration);
    digitalWrite(TRIGGER, LOW);
  }

  // Web request: server ipAddress/relay with HTML from index7.h
  server.on("/relay", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send_P(200, PSTR("text/html"), HTML7, processor7);
    switchData = 1;
    switchFlag = true;
    switchOne(switchData);
    needAnotherCountdown = 1;
  });

  yield();

  esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
  //Serial.print("Wakeup cause: ");
  //Serial.println((int)cause);

  if (cause == ESP_SLEEP_WAKEUP_EXT0) {
    // Woke from WOR preamble via AUX
    Serial.println("Woke from WOR preamble, switching to NORMAL mode...");
    e220ttl.setMode(MODE_0_NORMAL);
    vTaskDelay(pdMS_TO_TICKS(50));

    if (e220ttl.available() > 0) {
      ResponseStructContainer rsc = e220ttl.receiveMessageRSSI(sizeof(Message));
      if (rsc.status.code == 1 && rsc.data != nullptr) {
        Message *msg = (Message *)rsc.data;
        rsc.close();

        Serial.println("Received Message:");
        Serial.print("  switchData: ");
        Serial.println(msg->switchData);
        Serial.print("  dateTime:   ");
        Serial.println(msg->dateTime);

        // TODO: act on msg->switchData here
      } else {
        Serial.println("Receive error or empty data.");
      }
      rsc.close();
    } else {
      Serial.println("No data available after WOR wake.");
    }

    // After processing, go back to deep sleep with WOR RX armed
    //enterDeepSleep();  //kills wifi

  } else {
    // Cold boot or other wake: just arm WOR RX and sleep
    Serial.println("Power-on or Reset Wake up");
    //enterDeepSleep();  //kills wifi
  }
}

// ---------------------------
// Main loop (Core 0) — keep light for WiFi
// ---------------------------
void loop() {

  // Countdown expiry logic: schedule switchState = 2 event
  if (countdownExpired) {
    countdownExpired = false;

    switchData = 2;
    switchFlag = true;
    switchOne(switchData);

    //triggerSwitchEvent(2);

    if (needAnotherCountdown == 1) {
      needAnotherCountdown = 0;
    }
  }

  delay(1);  // yield to WiFi / system tasks
}

// ---------------------------
// Get formatted time string
// ---------------------------
String get_time() {

  time_t now;
  time(&now);
  strftime(time_output, MAX_dateTime_LENGTH, "%a  %m/%d/%y   %T", localtime(&now));
  return String(time_output);  // returns dateTime in the specified format
}

// ---------------------------
// Custom configTime wrapper + NTP wait
// ---------------------------
void configTimeCustom() {

  configTime(0, 0, udpAddress1, udpAddress2);
  setenv("TZ", TZ, 1);
  tzset();

  if (connected) {
    udp.beginPacket(udpAddress1, udpPort);
    udp.printf("Seconds since boot: %u", millis() / 1000);
    udp.endPacket();
  }

  Serial.print("wait for first valid dateTime");

  while (time(nullptr) < 100000ul) {
    Serial.print(".");
    delay(5000);
  }

  Serial.println("\nSystem Time set\n");

  get_time();

  Serial.println(outgoing.dateTime);
}

// ---------------------------
// Get current date and time components
// ---------------------------
DateTime getCurrentDateTime() {
  DateTime currentDateTime;
  time_t now = time(nullptr);
  struct tm *ti = localtime(&now);

  currentDateTime.year = ti->tm_year + 1900;
  currentDateTime.month = ti->tm_mon + 1;
  currentDateTime.day = ti->tm_mday;
  currentDateTime.hour = ti->tm_hour;
  currentDateTime.minute = ti->tm_min;
  currentDateTime.second = ti->tm_sec;

  return currentDateTime;
}

// ---------------------------
// Example web client to remote URL (not core to WOR)
// ---------------------------
void webInterface() {

  String data = "http://192.123.12.27/relay";

  if (WiFi.status() == WL_CONNECTED) {
    HTTPClient http;

    http.begin(data);

    int httpCode = http.GET();

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Serial.print("HttpCode: ");
      Serial.print(httpCode);
      Serial.println("\n");
      // Serial.println(payload);
      http.end();
    } else {
      Serial.print("HttpCode: ");
      Serial.print(httpCode);
      Serial.println("  URL Request failed.");
      http.end();
    }

  } else {
    Serial.println("Error in WiFi connection");
  }
  switchFlag = true;
}
