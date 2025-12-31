📄 ESP32 Dual Core WOR Architecture — Technical Reference
William Lucid — E220 WOR Transmitter Architecture
________________________________________
Table of Contents
•	Introduction
•	ESP32 Dual Core Model
•	Core Assignment Strategy
•	Task Creation & Scheduling
•	Task Notifications
•	Interrupt Handling
•	Critical Sections
•	Deep Sleep Integration
•	WOR Radio Pipeline
•	Flowcharts
•	Glossary
•	Appendix: Code Snippets
________________________________________
1. Introduction
This document describes the dual core architecture used in the ESP32 E220 WOR Remote Switch Transmitter. It explains how Core 0 and Core 1 cooperate to deliver deterministic radio timing, non blocking WiFi, and instant AUX driven wakeups.
________________________________________
2. ESP32 Dual Core Model
The ESP32 contains two independent CPU cores:
•	Core 0 → WiFi, TCP/IP, system tasks
•	Core 1 → Application logic (WOR radio task)
FreeRTOS runs across both cores and provides the primitives used in this architecture.
________________________________________
3. Core Assignment Strategy
Core	Responsibilities
Core 0	WiFi, web server, HTTP handlers, triggerSwitchEvent()
Core 1	WORTask, switchOne(), sendPreamble(), sendOutgoing()
This separation prevents WiFi starvation and ensures deterministic radio behavior.
________________________________________
4. Task Creation & Scheduling
xTaskCreatePinnedToCore()
Creates a FreeRTOS task pinned to a specific core.
xTaskCreatePinnedToCore(
    WORTask,
    "WORTask",
    4096,
    NULL,
    1,
    &worTaskHandle,
    1
);
Purpose:
Guarantees WOR logic always runs on Core 1.
________________________________________
5. Task Notifications
xTaskNotifyGive()
Wakes a task from another task.
xTaskNotifyGive(worTaskHandle);
vTaskNotifyGiveFromISR()
ISR safe version.
vTaskNotifyGiveFromISR(worTaskHandle, &xHigherPriorityTaskWoken);
ulTaskNotifyTake()
Waits for a notification.
ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
Purpose:
Replaces all AUX polling loops.
________________________________________
6. Interrupt Handling
attachInterrupt()
attachInterrupt(digitalPinToInterrupt(AUX_PIN), auxISR, FALLING);
Why FALLING?
E220 AUX goes LOW when:
•	TX completes
•	Module becomes idle
•	WOR preamble is received
________________________________________
7. Critical Sections
Used to safely share data between cores.
portENTER_CRITICAL(&mux);
switchFlag = true;
switchData = 1;
portEXIT_CRITICAL(&mux);
________________________________________
8. Deep Sleep Integration
esp_sleep_enable_ext0_wakeup()
Wake on AUX LOW.
gpio_hold_en()
Retains pin state during deep sleep.
esp_deep_sleep_start()
Enters deep sleep.
________________________________________
9. WOR Radio Pipeline
switchFlag set → WORTask wakes → switchOne() → sendPreamble() → sendOutgoing()
switchFlag
Consumed only once in switchOne().
sendPreamble()
Unconditional.
sendOutgoing()
Unconditional.
________________________________________
10. Flowcharts
A. System Flow (Mermaid)
flowchart LR

    subgraph Core0["Core 0"]
        A[WiFi / Web Server]
        B["triggerSwitchEvent()"]
    end

    subgraph Core1["Core 1"]
        C[WORTask]
        D["switchOne()"]
        E["sendPreamble()"]
        F["sendOutgoing()"]
    end

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F
    
B. Architecture Diagram (Mermaid)
flowchart LR

    subgraph Core0["Core 0"]
        A[WiFi / Web Server]
        B["triggerSwitchEvent()"]
    end

    subgraph Core1["Core 1"]
        C[WORTask]
        D["switchOne()"]
        E["sendPreamble()"]
        F["sendOutgoing()"]
    end

    A --> B
    B --> C
    C --> D
    D --> E
    E --> F

C. ASCII Flowchart
[Web Request]
      |
      v
[triggerSwitchEvent()]
      |
      v
[xTaskNotifyGive]
      |
      v
[Core 1: WORTask]
      |
      v
[switchOne()]
      |
      v
[sendPreamble()]
      |
      v
[sendOutgoing()]
________________________________________
11. Glossary
Term	Meaning
WOR	Wake On Radio
AUX	E220 status pin
ISR	Interrupt Service Routine
Core 0	WiFi + system core
Core 1	Application core
Task Notification	Lightweight inter task signal
Critical Section	Atomic access region
Deep Sleep	Ultra low power mode
________________________________________
12. Appendix: Key Code Snippets
AUX ISR
void IRAM_ATTR auxISR() {
    BaseType_t xHigherPriorityTaskWoken = pdFALSE;
    vTaskNotifyGiveFromISR(worTaskHandle, &xHigherPriorityTaskWoken);
    portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}
WOR Task
void WORTask(void *pvParameters) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        bool localFlag;
        int localData;

        portENTER_CRITICAL(&mux);
        localFlag = switchFlag;
        localData = switchData;
        portEXIT_CRITICAL(&mux);

        if (localFlag)
            switchOne(localData);

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
Event Trigger
void triggerSwitchEvent(int value) {
    portENTER_CRITICAL(&mux);
    switchData = value;
    switchFlag = true;
    portEXIT_CRITICAL(&mux);

    xTaskNotifyGive(worTaskHandle);
}
________________________________________

