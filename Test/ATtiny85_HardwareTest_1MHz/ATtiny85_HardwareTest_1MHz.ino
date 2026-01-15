/*
 * ATtiny85_HardwareTest_1MHz.ino
 * 
 * Hardware test - NO SERIAL (works at any clock speed)
 * Use this because PB1 is shared between TX and Motor!
 * 
 * Tests: Motor, Button, GO signal
 * 
 * Behavior:
 *   Boot: 3 short buzzes
 *   Button press: 1 buzz
 *   GO pin HIGH: vibrate while held
 *   Idle: silence
 * 
 * Pin mapping (from schematic):
 *   PB0 (pin 5): RX + SDA (not used in this test)
 *   PB1 (pin 6): TX + Motor (motor only in this test)
 *   PB2 (pin 7): SCL (not used)
 *   PB3 (pin 2): GO signal input
 *   PB4 (pin 3): Button
 *   PB5 (pin 1): RESET
 */

#define PIN_MOTOR   1   // PB1
#define PIN_GO      3   // PB3
#define PIN_BUTTON  4   // PB4

void vibrateMs(uint16_t ms) {
  digitalWrite(PIN_MOTOR, HIGH);
  delay(ms);
  digitalWrite(PIN_MOTOR, LOW);
}

void setup() {
  // Configure pins
  pinMode(PIN_MOTOR, OUTPUT);
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_GO, INPUT);  // Will float if not connected
  
  // Ensure motor OFF
  digitalWrite(PIN_MOTOR, LOW);
  
  // Boot indicator: 3 buzzes
  for (int i = 0; i < 3; i++) {
    vibrateMs(100);
    delay(150);
  }
  
  delay(500);
}

void loop() {
  // Read GO signal from ESP32 (GPIO19 → PB3)
  // Only check if we want to test with host connected
  bool goSignal = (digitalRead(PIN_GO) == HIGH);
  
  // Read button
  bool buttonPressed = (digitalRead(PIN_BUTTON) == LOW);
  
  if (goSignal) {
    // GO signal active - vibrate continuously
    digitalWrite(PIN_MOTOR, HIGH);
  } 
  else if (buttonPressed) {
    // Button pressed - single buzz
    vibrateMs(100);
    // Wait for release
    while (digitalRead(PIN_BUTTON) == LOW) {
      delay(10);
    }
    delay(50);  // Debounce
  }
  else {
    // Idle - motor OFF
    digitalWrite(PIN_MOTOR, LOW);
  }
  
  delay(10);
}
