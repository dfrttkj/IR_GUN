#include <Arduino.h>

// Laser Tag Code for ESP32

int hp = 3;
const int hpLeds[3] = {18, 19, 21};

const int irLedPin = 4;       // GPIO for the TSAL6200 (Gun)
const int irReceiverPin = 5;  // GPIO for the TSOP38438 (Vest)
const int triggerPin = 15;    // GPIO for a trigger button

// Variables for IR modulation
const int carrierFrequency = 38000;   // TSOP38438 is tuned to 38kHz
const int pwmChannel = 0;             // ESP32 has 16 PWM channels (0-15)
const int cycle = 64;   // cycle default=127 (0-254) (Best: 64-127)

// NEC protocol timing constants (in microseconds)
const unsigned long NEC_LEADING_PULSE = 9000;
const unsigned long NEC_LEADING_SPACE = 4500;
const unsigned long NEC_PULSE = 560;
const unsigned long NEC_ZERO_SPACE = 560;
const unsigned long NEC_ONE_SPACE = 1690;
const unsigned long NEC_REPEAT_SPACE = 2250;
const unsigned long NEC_REPEAT_DELAY = 108000; // 108ms between repeats

// Game data - customize these for your players/teams
uint16_t playerID = 0x1234;  // 16-bit player ID
uint8_t teamID = 0x99;       // 8-bit team ID

void sendNEC(uint16_t address, uint8_t command) {
  // Calculate command inverse for error checking
  uint8_t command_inv = ~command;
  
  // Send leading burst
  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_LEADING_PULSE);
  ledcWrite(pwmChannel, 0);
  delayMicroseconds(NEC_LEADING_SPACE);
  
  // Send 32-bit data: 16-bit address + 8-bit command + 8-bit inverted command
  // NEC sends LSB first, so we need to reverse the bit order
  uint32_t data = ((uint32_t)address) | ((uint32_t)command << 16) | ((uint32_t)command_inv << 24);
  
  for (int i = 0; i < 32; i++) {
    // Send pulse
    ledcWrite(pwmChannel, cycle);
    delayMicroseconds(NEC_PULSE);
    ledcWrite(pwmChannel, 0);
    
    // Send space (determines if bit is 0 or 1)
    if (data & (1UL << i)) {
      delayMicroseconds(NEC_ONE_SPACE);
    } else {
      delayMicroseconds(NEC_ZERO_SPACE);
    }
  }
  
  // Send stop pulse
  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_PULSE);
  ledcWrite(pwmChannel, 0);
  
  Serial.println("NEC IR Signal Sent!");
}

void sendNECRepeat() {
  // Send NEC repeat code
  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_LEADING_PULSE);
  ledcWrite(pwmChannel, 0);
  delayMicroseconds(NEC_REPEAT_SPACE);
  
  // Send stop pulse
  ledcWrite(pwmChannel, cycle);
  delayMicroseconds(NEC_PULSE);
  ledcWrite(pwmChannel, 0);
  
  Serial.println("NEC Repeat Sent!");
}

void sendIRSignal() {
  // Send NEC protocol with player ID as address and team ID as command
  sendNEC(playerID, teamID);
  
  // Optional: Send repeat codes (uncomment if needed)
  // delay(40); // Standard NEC repeat delay
  // sendNECRepeat();
}

// Variables for IR decoding
volatile unsigned long lastEdgeTime = 0;
volatile unsigned int bitCount = 0;
volatile uint32_t receivedData = 0;
volatile bool messageReady = false;
volatile bool waitingForStart = true;

// Temporary storage for complete message
uint32_t lastReceivedData = 0;

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
      receivedData |= (0UL << bitCount);
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

void processHit(uint16_t shooterID, uint8_t shooterTeam) {
  // Add your game logic here
  Serial.println("Player Hit! Processing damage...");
  
  // Example game logic:
  if (shooterTeam == teamID) {
    Serial.println("Friendly fire! No damage.");
  } else {
    Serial.println("Enemy hit! -10 health");
    // Subtract health, check for elimination, etc.
  }
}

void setup() {
  Serial.begin(115200);
  
  pinMode(irLedPin, OUTPUT);
  pinMode(triggerPin, INPUT_PULLUP);
  pinMode(irReceiverPin, INPUT);

  for (int pin : hpLeds) {
    pinMode(pin, OUTPUT);
    digitalWrite(pin, HIGH);
  }

  // Configure ESP32 LEDC PWM for 38kHz modulation
  ledcSetup(pwmChannel, carrierFrequency, 8);
  ledcAttachPin(irLedPin, pwmChannel);
  ledcWrite(pwmChannel, 0);

  // Attach interrupt to the IR receiver pin
  attachInterrupt(digitalPinToInterrupt(irReceiverPin), handleReceivedIR, CHANGE);
  
  Serial.println("Laser Tag System Ready");
  Serial.print("Player ID: 0x");
  Serial.println(playerID, HEX);
  Serial.print("Team ID: 0x");
  Serial.println(teamID, HEX);
}

void loop() {
  // Check for received IR messages
  if (messageReady) {
    messageReady = false;
    
    // Extract address and command (LSB first)
    uint16_t address = lastReceivedData & 0xFFFF;
    uint8_t command = (lastReceivedData >> 16) & 0xFF;
    uint8_t command_inv = (lastReceivedData >> 24) & 0xFF;
    
    // Verify data integrity
    if ((uint8_t)(~command) == command_inv) {
      Serial.print("Valid NEC Signal Received - Player: 0x");
      Serial.print(address, HEX);
      Serial.print(", Team: 0x");
      Serial.println(command, HEX);
      
      processHit(address, command);
    } else {
      Serial.print("Invalid NEC Signal - Checksum Error: cmd=0x");
      Serial.print(command, HEX);
      Serial.print(", inv=0x");
      Serial.print(command_inv, HEX);
      Serial.print(", expected inv=0x");
      Serial.println((uint8_t)(~command), HEX);
    }
  }
  
  // Check if trigger is pulled
  if (digitalRead(triggerPin) == LOW) {
    sendIRSignal();
    delay(100); // Debounce delay
  }
  
  // Add other game logic here (health display, respawning, etc.)
}