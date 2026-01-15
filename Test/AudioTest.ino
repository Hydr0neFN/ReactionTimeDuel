/*
 * AudioTest.ino - Test sketch for AudioManager
 * 
 * Tests:
 *   1. SD card initialization with HSPI
 *   2. I2S audio output
 *   3. Non-blocking queue
 *   4. Sequence playback
 * 
 * Wiring (per schematic):
 *   MAX98357A:
 *     DINS <- GPIO23
 *     BCLK <- GPIO26
 *     LRC  <- GPIO25
 *     VIN  <- 5V
 *     GND  <- GND
 *   
 *   SD Card Module:
 *     SCK  <- GPIO14 (HSPI)
 *     MISO -> GPIO12 (HSPI)
 *     MOSI <- GPIO13 (HSPI)
 *     CS   <- GPIO5
 *     VCC  <- 3.3V
 *     GND  <- GND
 * 
 * SD Card Contents:
 *   /1.mp3  - "One"
 *   /2.mp3  - "Two"
 *   /3.mp3  - "Three"
 *   /4.mp3  - "Four"
 *   /10.mp3 - "Player"
 *   /23.mp3 - "Wins"
 *   etc.
 */

#include "AudioManager.h"

AudioManager audio;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println(F("\n=== Audio Manager Test ==="));
  Serial.println(F("Pin Configuration:"));
  Serial.println(F("  I2S DOUT: GPIO23"));
  Serial.println(F("  I2S BCLK: GPIO26"));
  Serial.println(F("  I2S LRC:  GPIO25"));
  Serial.println(F("  SD SCK:   GPIO14 (HSPI)"));
  Serial.println(F("  SD MISO:  GPIO12 (HSPI)"));
  Serial.println(F("  SD MOSI:  GPIO13 (HSPI)"));
  Serial.println(F("  SD CS:    GPIO5"));
  Serial.println();
  
  // Initialize audio system
  if (!audio.begin(0.5)) {
    Serial.println(F("Audio init FAILED!"));
    Serial.println(F("Check:"));
    Serial.println(F("  - SD card inserted?"));
    Serial.println(F("  - Wiring correct?"));
    Serial.println(F("  - MP3 files present?"));
    while (1) delay(1000);
  }
  
  Serial.println(F("Audio system ready!"));
  Serial.println(F("\nCommands:"));
  Serial.println(F("  1-4: Play number"));
  Serial.println(F("  p:   Play 'Player 1'"));
  Serial.println(F("  w:   Play 'Player 2 Wins'"));
  Serial.println(F("  c:   Play countdown 3-2-1"));
  Serial.println(F("  s:   Stop all"));
  Serial.println(F("  q:   Queue status"));
  Serial.println();
  
  // Test: Play startup sound
  audio.queueSound(SND_GET_READY);
}

void loop() {
  // CRITICAL: Must call update() frequently for non-blocking playback
  audio.update();
  
  // Handle serial commands for testing
  if (Serial.available()) {
    char cmd = Serial.read();
    
    switch (cmd) {
      case '1':
      case '2':
      case '3':
      case '4':
        Serial.printf("Playing number %c\n", cmd);
        audio.queueSound(cmd - '0');
        break;
        
      case 'p':
        Serial.println(F("Playing: 'Player 1'"));
        audio.playPlayerNumber(1);
        break;
        
      case 'w':
        Serial.println(F("Playing: 'Player 2 Wins'"));
        audio.playPlayerWins(2);
        break;
        
      case 'c':
        Serial.println(F("Playing countdown: 3-2-1"));
        audio.playCountdown(3);
        audio.playCountdown(2);
        audio.playCountdown(1);
        break;
        
      case 's':
        Serial.println(F("Stopping..."));
        audio.stop();
        break;
        
      case 'q':
        Serial.printf("Queue: %d items, playing: %s\n", 
          audio.getQueueCount(),
          audio.isPlaying() ? "yes" : "no");
        break;
    }
  }
  
  delay(1);
}
