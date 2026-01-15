/*
 * ATtiny85_BasicTest.ino - Minimal Hardware Test (1 MHz compatible)
 * 
 * NO serial, NO MPU - just tests:
 *   - Motor (vibration)
 *   - Button
 * 
 * Behavior:
 *   - Boot: 3 short buzzes (motor works)
 *   - Button press: 1 short buzz (button works)
 *   - Idle: no vibration
 * 
 * CORRECTED Pin mapping from schematic:
 *   PB0 (pin 5): RX + SDA
 *   PB1 (pin 6): TX + Motor (via transistor Q2 + R6)
 *   PB2 (pin 7): SCL
 *   PB3 (pin 2): GO signal input
 *   PB4 (pin 3): Button/SW (INPUT_PULLUP, press = LOW)
 *   PB5 (pin 1): RESET
 */

#define PIN_MOTOR   1   // PB1 - drives transistor base via R6 (1k)
#define PIN_GO      3   // PB3
#define PIN_BUTTON  4   // PB4

void vibrateMs(uint16_t ms) {
  digitalWrite(PIN_MOTOR, HIGH);
  delay(ms);
  digitalWrite(PIN_MOTOR, LOW);
}

void setup() {
  // Configure pins
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_MOTOR, OUTPUT);
  
  // Make sure motor is OFF
  digitalWrite(PIN_MOTOR, LOW);
  
  // Boot signal: 3 short buzzes
  for (int i = 0; i < 3; i++) {
    vibrateMs(100);
    delay(150);
  }
}

void loop() {
  // Button press test only (standalone - no ESP32 needed)
  if (digitalRead(PIN_BUTTON) == LOW) {
    vibrateMs(100);  // Short buzz on press
    // Wait for release (debounce)
    while (digitalRead(PIN_BUTTON) == LOW) {
      delay(10);
    }
    delay(50);  // Debounce
  }
  
  delay(10);
}
