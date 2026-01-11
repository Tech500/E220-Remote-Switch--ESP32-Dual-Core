/*
 * E220_WOR_Diagnostic_Tool.ino     (E220 Module configuration tool; best one to use!!!
 * Tests WOR transmission and helps identify configuration issues
 * 01/09/2026
 * 
 * Upload this to your TRANSMITTER and watch Serial Monitor
 * It will test WOR and report detailed diagnostics
 */

#include "Arduino.h"
#include "LoRa_E220.h"

// CONFIGURATION - MODIFY THESE TO MATCH YOUR SETUP
#define MY_ADDRESS 0x03        // This device's address
#define DEST_ADDRESS 0x03      // Where to send (receiver's address)
#define CHANNEL 23
#define FREQUENCY_915

// Pin definitions for ESP32
#define RXD2 16
#define TXD2 17
#define M0_PIN 21
#define M1_PIN 19
#define AUX_PIN 15

LoRa_E220 e220ttl(&Serial2, AUX_PIN, M0_PIN, M1_PIN);

void waitForAux(int timeout = 5000) {
  uint32_t start = millis();
  Serial.print("  Waiting for AUX...");
  while (digitalRead(AUX_PIN) == LOW && (millis() - start < timeout)) {
    delay(10);
  }
  if (digitalRead(AUX_PIN) == HIGH) {
    Serial.println(" ✓ Ready");
  } else {
    Serial.println(" ✗ TIMEOUT!");
  }
}

void printConfiguration(Configuration config) {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     E220 CONFIGURATION DETAILS         ║");
  Serial.println("╚════════════════════════════════════════╝");
  
  Serial.print("Address High (ADDH):     0x");
  Serial.println(config.ADDH, HEX);
  Serial.print("Address Low (ADDL):      0x");
  Serial.println(config.ADDL, HEX);
  Serial.print("Channel (CHAN):          ");
  Serial.println(config.CHAN, DEC);
  
  Serial.println("\n--- UART Settings ---");
  Serial.print("UART Parity:             ");
  Serial.println(config.SPED.getUARTParityDescription());
  Serial.print("UART Baud Rate:          ");
  Serial.println(config.SPED.getUARTBaudRateDescription());
  Serial.print("Air Data Rate:           ");
  Serial.println(config.SPED.getAirDataRateDescription());
  
  Serial.println("\n--- Transmission Mode ---");
  Serial.print("Fixed Transmission:      ");
  Serial.print(config.TRANSMISSION_MODE.fixedTransmission, BIN);
  Serial.print(" (");
  Serial.print(config.TRANSMISSION_MODE.getFixedTransmissionDescription());
  Serial.println(")");
  
  Serial.print("WOR Period:              ");
  Serial.print(config.TRANSMISSION_MODE.WORPeriod, BIN);
  Serial.print(" (");
  Serial.print(config.TRANSMISSION_MODE.getWORPeriodByParamsDescription());
  Serial.println(")");
  
  Serial.print("Enable LBT:              ");
  Serial.println(config.TRANSMISSION_MODE.getLBTEnableByteDescription());
  Serial.print("Enable RSSI:             ");
  Serial.println(config.TRANSMISSION_MODE.getRSSIEnableByteDescription());
  
  Serial.println("\n--- Power & Options ---");
  Serial.print("Transmission Power:      ");
  Serial.println(config.OPTION.getTransmissionPowerDescription());
  Serial.print("Sub Packet Setting:      ");
  Serial.println(config.OPTION.getSubPacketSetting());
  Serial.print("RSSI Ambient Noise:      ");
  Serial.println(config.OPTION.getRSSIAmbientNoiseEnable());
  
  Serial.println("════════════════════════════════════════\n");
}

void testMode(OPERATING_MODE mode, const char* modeName) {
  Serial.print("Setting mode: ");
  Serial.println(modeName);
  
  ResponseStatus rs = e220ttl.setMode(mode);
  Serial.print("  Status: ");
  Serial.println(rs.getResponseDescription());
  
  delay(100);
  waitForAux();
  
  // Read back mode pins to verify
  bool m0 = digitalRead(M0_PIN);
  bool m1 = digitalRead(M1_PIN);
  Serial.print("  M0=");
  Serial.print(m0 ? "HIGH" : "LOW");
  Serial.print(", M1=");
  Serial.println(m1 ? "HIGH" : "LOW");
  Serial.println();
}

void testWORTransmission() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║       WOR TRANSMISSION TEST            ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  Serial.print("Transmitting FROM address: 0x");
  Serial.println(MY_ADDRESS, HEX);
  Serial.print("Sending TO address:        0x");
  Serial.println(DEST_ADDRESS, HEX);
  Serial.print("Channel:                   ");
  Serial.println(CHANNEL);
  
  if (MY_ADDRESS == DEST_ADDRESS) {
    Serial.println("\n⚠️  WARNING: You're sending to your own address!");
    Serial.println("   This won't work with Fixed Transmission.");
    Serial.println("   TX and RX must have DIFFERENT addresses.\n");
  }
  
  // Step 1: Switch to WOR Transmitter mode
  Serial.println("Step 1: Switching to MODE_1_WOR_TRANSMITTER...");
  testMode(MODE_1_WOR_TRANSMITTER, "WOR Transmitter");
  
  // Step 2: Send preamble
  Serial.println("Step 2: Sending WOR preamble...");
  Serial.println("  (This should wake the receiver if configured correctly)");
  
  ResponseStatus rs = e220ttl.sendFixedMessage(0, DEST_ADDRESS, CHANNEL, "WAKE");
  Serial.print("  Send status: ");
  Serial.println(rs.getResponseDescription());
  
  if (rs.code == 1) {
    Serial.println("  ✓ Preamble sent successfully");
  } else {
    Serial.println("  ✗ Preamble send FAILED!");
  }
  
  waitForAux();
  
  // Step 3: Wait for receiver to wake
  Serial.println("\nStep 3: Waiting 4 seconds for receiver to wake...");
  for (int i = 4; i > 0; i--) {
    Serial.print("  ");
    Serial.println(i);
    delay(1000);
  }
  
  // Step 4: Send data message
  Serial.println("\nStep 4: Sending data message...");
  struct TestMessage {
    int32_t testValue;
    char message[20];
  } __attribute__((packed));
  
  TestMessage msg;
  msg.testValue = 12345;
  strcpy(msg.message, "WOR Test Data");
  
  rs = e220ttl.sendFixedMessage(0, DEST_ADDRESS, CHANNEL, &msg, sizeof(TestMessage));
  Serial.print("  Send status: ");
  Serial.println(rs.getResponseDescription());
  
  if (rs.code == 1) {
    Serial.println("  ✓ Data sent successfully");
  } else {
    Serial.println("  ✗ Data send FAILED!");
  }
  
  waitForAux();
  
  // Return to normal mode
  Serial.println("\nReturning to MODE_0_NORMAL...");
  testMode(MODE_0_NORMAL, "Normal Mode");
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("WOR transmission test complete!");
  Serial.println("════════════════════════════════════════\n");
}

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n");
  Serial.println("╔════════════════════════════════════════╗");
  Serial.println("║   E220 WOR DIAGNOSTIC TOOL v1.0        ║");
  Serial.println("╚════════════════════════════════════════╝\n");
  
  // Initialize pins
  pinMode(AUX_PIN, INPUT);
  pinMode(M0_PIN, OUTPUT);
  pinMode(M1_PIN, OUTPUT);
  
  // Initialize UART
  Serial.println("Initializing UART (9600 baud)...");
  Serial2.begin(9600, SERIAL_8N1, RXD2, TXD2);
  delay(100);
  
  // Initialize E220
  Serial.println("Initializing E220 module...");
  e220ttl.begin();
  delay(100);
  
  // Set to normal mode for configuration reading
  Serial.println("Setting MODE_0_NORMAL for configuration...");
  e220ttl.setMode(MODE_0_NORMAL);
  delay(100);
  waitForAux();
  
  // Read configuration
  Serial.println("\nReading module configuration...");
  ResponseStructContainer c = e220ttl.getConfiguration();
  
  if (c.status.code == 1) {
    Configuration config = *(Configuration*) c.data;
    printConfiguration(config);
    
    // Verification checks
    Serial.println("╔════════════════════════════════════════╗");
    Serial.println("║          VERIFICATION CHECKS           ║");
    Serial.println("╚════════════════════════════════════════╝\n");
    
    bool hasErrors = false;
    
    // Check 1: Address
    if (config.ADDL == MY_ADDRESS) {
      Serial.print("✓ Address matches (0x");
      Serial.print(config.ADDL, HEX);
      Serial.println(")");
    } else {
      Serial.print("✗ Address MISMATCH! Config=0x");
      Serial.print(config.ADDL, HEX);
      Serial.print(", Expected=0x");
      Serial.println(MY_ADDRESS, HEX);
      hasErrors = true;
    }
    
    // Check 2: Channel
    if (config.CHAN == CHANNEL) {
      Serial.print("✓ Channel matches (");
      Serial.print(config.CHAN);
      Serial.println(")");
    } else {
      Serial.print("✗ Channel MISMATCH! Config=");
      Serial.print(config.CHAN);
      Serial.print(", Expected=");
      Serial.println(CHANNEL);
      hasErrors = true;
    }
    
    // Check 3: Fixed Transmission
    if (config.TRANSMISSION_MODE.fixedTransmission == FT_FIXED_TRANSMISSION) {
      Serial.println("✓ Fixed Transmission ENABLED");
    } else {
      Serial.println("✗ Fixed Transmission NOT enabled!");
      Serial.println("  WOR requires Fixed Transmission!");
      hasErrors = true;
    }
    
    // Check 4: WOR Period
    Serial.print("✓ WOR Period: ");
    Serial.println(config.TRANSMISSION_MODE.getWORPeriodByParamsDescription());
    Serial.println("  ** Receiver MUST have same WOR period! **");
    
    // Check 5: Air Data Rate
    Serial.print("✓ Air Data Rate: ");
    Serial.println(config.SPED.getAirDataRateDescription());
    Serial.println("  ** Receiver MUST have same Air Data Rate! **");
    
    if (hasErrors) {
      Serial.println("\n⚠️  CONFIGURATION ERRORS DETECTED!");
      Serial.println("Fix these before testing WOR.\n");
    } else {
      Serial.println("\n✓ All configuration checks passed!\n");
    }
    
  } else {
    Serial.println("✗ Failed to read configuration!");
    Serial.print("Error code: ");
    Serial.println(c.status.code);
  }
  
  c.close();
  
  Serial.println("\n════════════════════════════════════════");
  Serial.println("Press 'T' to test WOR transmission");
  Serial.println("Press 'C' to read configuration again");
  Serial.println("Press 'N' to set Normal mode");
  Serial.println("Press 'W' to set WOR Transmitter mode");
  Serial.println("Press 'R' to set WOR Receiver mode");
  Serial.println("════════════════════════════════════════\n");
}

void loop() {
  if (Serial.available() > 0) {
    char cmd = Serial.read();
    
    switch (cmd) {
      case 'T':
      case 't':
        testWORTransmission();
        break;
        
      case 'C':
      case 'c':
        e220ttl.setMode(MODE_0_NORMAL);
        delay(100);
        waitForAux();
        ResponseStructContainer c = e220ttl.getConfiguration();
        if (c.status.code == 1) {
          Configuration config = *(Configuration*) c.data;
          printConfiguration(config);
        }
        c.close();
        break;
        
      case 'N':
      case 'n':
        testMode(MODE_0_NORMAL, "Normal Mode");
        break;
        
      case 'W':
      case 'w':
        testMode(MODE_1_WOR_TRANSMITTER, "WOR Transmitter");
        break;
        
      case 'R':
      case 'r':
        testMode(MODE_2_WOR_RECEIVER, "WOR Receiver");
        break;
    }
  }
  
  delay(10);
}