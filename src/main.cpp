#include <Arduino.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <WebSocketsClient.h>

// --- Configuration ---
bool debugMode = false;
int hp = 0;
bool dead = false;
const int hpLed = 18; // Single LED for HP gradient
int MAX_HP = 0;

// Game State
bool withServer = true;
bool gameStarted = false;

// Game data
uint16_t playerID = 0x0002;
uint8_t teamID = 0x00;

// WiFi Configuration
const char* ssid = "ASDF";
const char* password = "dfrttkj1";

// WebSocket Configuration
const char* websocket_server = "10.73.85.161";
constexpr uint16_t websocket_port = 8080;
const char* websocket_path = "/";

// Ping Configuration
const unsigned long PING_INTERVAL = 5000; // 5 seconds
unsigned long lastPingTime = 0;

// Hardware Pins
const int irLedPin = 4;
const int irReceiverPin = 5;
const int triggerPin = 15;

// Shooting Configuration
unsigned long SHOT_COOLDOWN = 500; // Default 500ms cooldown between shots
unsigned long lastShotTime = 0;
bool lastTriggerState = HIGH;

// IR Modulation Constants
const int carrierFrequency = 38000;
const int pwmChannel = 0;
const int pwmChannelHP = 2;
const int cycle = 64;

// NEC protocol timing
const unsigned long NEC_LEADING_PULSE = 9000;
const unsigned long NEC_LEADING_SPACE = 4500;
const unsigned long NEC_PULSE = 560;
const unsigned long NEC_ZERO_SPACE = 560;
const unsigned long NEC_ONE_SPACE = 1690;

// Global Objects
WebSocketsClient webSocket;

// --- HP LED Gradient Update ---
void updateHPLed() {
  int brightness = map(hp, 0, MAX_HP, 0, 255);
  ledcWrite(pwmChannelHP, brightness);
}

// --- Game State Handlers ---
void handleGameStart(JsonDocument& doc) {
  teamID = doc["tid"];
  SHOT_COOLDOWN = doc["cooldown"];
  hp = doc["lives"];
  MAX_HP = hp;

  gameStarted = true;
  updateHPLed();

  Serial.println("=== GAME STARTED ===");
  Serial.printf("Team ID: 0x%02X\n", teamID);
  Serial.printf("Cooldown: %lu ms\n", SHOT_COOLDOWN);
  Serial.printf("Lives: %d\n", hp);
}

void handleEndGame() {
  teamID = 0x00;
  SHOT_COOLDOWN = 0;
  hp = 0;
  MAX_HP = 1;

  gameStarted = false;
  updateHPLed();

  Serial.println("=== GAME ENDED ===");
  Serial.println("All values reset to 0");
}

// --- WebSocket Event Handler ---
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED:
            Serial.println("[WS] Disconnected!");
            break;
        case WStype_CONNECTED:
            Serial.println("[WS] Connected!");
            {
                // Send join message to server
                StaticJsonDocument<256> doc;
                doc["type"] = "connect";
                doc["pid"] = playerID;

                String output;
                serializeJson(doc, output);
                webSocket.sendTXT(output);
                Serial.println("[WS] Sent: " + output);
            }
            break;
        case WStype_TEXT:
            Serial.printf("[WS] Received: %s\n", payload);
            {
                // Parse incoming JSON
                StaticJsonDocument<256> doc;
                DeserializationError error = deserializeJson(doc, payload);

                if (!error) {
                  if (!doc["type"].is<const char*>()) return;
                  const char* msgType = doc["type"];

                  if (strcmp(msgType, "ping") == 0) {
                    digitalWrite(2, HIGH);
                    lastPingTime = millis();
                    {
                      StaticJsonDocument<256> back;
                      back["type"] = "pong";
                      back["pid"] = playerID;

                      String output;
                      serializeJson(back, output);
                      webSocket.sendTXT(output);
                      Serial.println("[WS] Sent: " + output);
                    }
                  } else if (strcmp(msgType, "startgame") == 0) {
                    handleGameStart(doc);
                  } else if (strcmp(msgType, "endgame") == 0) {
                    handleEndGame();
                  }
                }
            }
            break;
        case WStype_ERROR:
            Serial.println("[WS] Error");
            break;
        default:
            break;
    }
}


// --- IR Sending Logic ---
void sendNEC(uint16_t address, uint8_t command) {
  uint8_t command_inv = ~command;

  // Header
  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_LEADING_PULSE);
  ledcWrite(pwmChannel, 0);
  delayMicroseconds(NEC_LEADING_SPACE);

  uint32_t data = ((uint32_t)address) | ((uint32_t)command << 16) | ((uint32_t)command_inv << 24);

  for (int i = 0; i < 32; i++) {
    ledcWrite(pwmChannel, cycle);
    delayMicroseconds(NEC_PULSE);
    ledcWrite(pwmChannel, 0);

    if (data & (1UL << i)) {
      delayMicroseconds(NEC_ONE_SPACE);
    } else {
      delayMicroseconds(NEC_ZERO_SPACE);
    }
  }

  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_PULSE);
  ledcWrite(pwmChannel, 0);

  Serial.println("IR Shot Fired!");
}

// --- IR Receiving Logic ---
volatile unsigned long lastEdgeTime = 0;
volatile unsigned int bitCount = 0;
volatile uint32_t receivedData = 0;
volatile bool messageReady = false;
volatile bool waitingForStart = true;
volatile uint32_t lastReceivedData;

void IRAM_ATTR handleReceivedIR() {
  unsigned long currentTime = micros();
  unsigned long duration = currentTime - lastEdgeTime;
  int state = digitalRead(irReceiverPin);

  // TSOP38438 inverts the signal: LOW = IR detected, HIGH = no IR

  if (state == HIGH) {
    // Rising edge - IR burst ended, this is the start of a space
    lastEdgeTime = currentTime;
    return;
  }

  // Falling edge - space ended, IR burst starting
  // The duration variable contains the length of the space

  if (waitingForStart) {
    // Looking for the leading space (4.5ms)
    if (duration > 4000 && duration < 5000) {
      waitingForStart = false;
      bitCount = 0;
      receivedData = 0;
    }
  } else {
    // Receiving data bits
    if (duration > 400 && duration < 800) {
      // Zero bit (560µs space)
      receivedData &= ~(1UL << bitCount);
      bitCount++;
    } else if (duration > 1500 && duration < 1900) {
      // One bit (1690µs space)
      receivedData |= (1UL << bitCount);
      bitCount++;
    } else if (duration > 2000 && duration < 2500) {
      // Repeat code detected
      waitingForStart = true;
      bitCount = 0;
    } else {
      // Invalid timing, reset
      waitingForStart = true;
      bitCount = 0;
    }

    if (bitCount == 32) {
      // Complete message received
      lastReceivedData = receivedData;
      messageReady = true;
      waitingForStart = true;
      bitCount = 0;
    }
  }

  lastEdgeTime = currentTime;
}

void processHit(uint16_t shooterPID, uint8_t shooterTID) {
  if (shooterPID == playerID) return;
  if (shooterTID != 0xFF && shooterTID == teamID) {
    Serial.println("Friendly fire blocked.");
  } else if (hp > 0) {
    hp--;
    Serial.printf("Hit by pid: 0x%04X tid: 0x%02X! HP: %d\n", shooterPID, shooterTID, hp);

    // Update LED gradient
    updateHPLed();

    // Notify Server via WebSocket
    StaticJsonDocument<200> doc;
    doc["type"] = "hit";
    doc["victim"] = playerID;
    doc["shooter"] = shooterPID;

    String output;
    serializeJson(doc, output);
    webSocket.sendTXT(output);
  }
}

// --- UI & Control ---
void checkSerialCommands() {
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command.startsWith("set hp ")) {
      hp = command.substring(7).toInt();
      MAX_HP = hp;
      updateHPLed();
      Serial.println("HP Updated to " + String(hp));
    } else if (command == "noS") {
      gameStarted = true;
      withServer = false;

      teamID = 0x01;
      hp = 5;
      SHOT_COOLDOWN = 500;
      MAX_HP = hp;

      updateHPLed();

      Serial.println("Game start wait overridden!");
    }
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(2, OUTPUT);
  // Pin Modes
  pinMode(irLedPin, OUTPUT);
  pinMode(triggerPin, INPUT_PULLUP);
  pinMode(irReceiverPin, INPUT);
  pinMode(hpLed, OUTPUT);

  // IR PWM Setup
  ledcSetup(pwmChannel, carrierFrequency, 8);
  ledcAttachPin(irLedPin, pwmChannel);
  ledcWrite(pwmChannel, 0);

  // HP LED PWM Setup (for gradient)
  ledcSetup(pwmChannelHP, 5000, 8); // 5kHz frequency for LED brightness
  ledcAttachPin(hpLed, pwmChannelHP);
  updateHPLed();

  if (!debugMode) {
    // WiFi Setup
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) {
      delay(500);
      Serial.print(".");
    }
    Serial.println("\nWiFi Connected. IP: " + WiFi.localIP().toString());

    // WebSocket Setup
    webSocket.begin(websocket_server, websocket_port, websocket_path);
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
  }

  // IR Interrupt
  attachInterrupt(digitalPinToInterrupt(irReceiverPin), handleReceivedIR, CHANGE);

  Serial.println("Laser Tag System Online");
  Serial.println("Waiting for game start...");
  Serial.println("Type 'noS' to override");
}

void loop() {
  if (withServer && !debugMode) webSocket.loop();
  checkSerialCommands();

  unsigned long currentTime = millis();

  // Periodic Ping

  if (currentTime - lastPingTime >= PING_INTERVAL) {
    digitalWrite(2, LOW);
  }

  // If waiting for game start, skip main game logic
  if (!gameStarted) {
    return;
  }

  // Handle Received Hits
  if (messageReady) {
    messageReady = false;
    uint16_t address = lastReceivedData & 0xFFFF;
    uint8_t command = (lastReceivedData >> 16) & 0xFF;
    uint8_t command_inv = (lastReceivedData >> 24) & 0xFF;

    if ((uint8_t)(~command) == command_inv) {
      processHit(address, command);
    }
  }

  // Handle Trigger (click-to-shoot with cooldown)
  bool currentTriggerState = digitalRead(triggerPin);

  // Detect trigger press (transition from HIGH to LOW)
  if (lastTriggerState == HIGH && currentTriggerState == LOW) {
    // Check if cooldown has elapsed and player is alive
    if (hp > 0 && (currentTime - lastShotTime >= SHOT_COOLDOWN)) {
      sendNEC(playerID, teamID);
      lastShotTime = currentTime;
    }
  }

  lastTriggerState = currentTriggerState;
}

// [WS] Received: {"type":"activeWeapons","weapons":[1,4,2]}
