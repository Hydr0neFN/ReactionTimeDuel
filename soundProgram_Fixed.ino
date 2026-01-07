/*
 * soundProgram_Fixed.ino - Corrected pin assignments per schematic
 * 
 * CHANGES FROM ORIGINAL:
 *   I2S DOUT: 25 → 23 (per schematic)
 *   I2S LRC:  22 → 25 (per schematic)
 *   SD SPI:   VSPI → HSPI (avoid CC1/CC2 conflict on GPIO18/19)
 */

#include "Arduino.h"
#include "SD.h"
#include "FS.h"
#include "AudioFileSourceSD.h"
#include "AudioGeneratorMP3.h"
#include "AudioOutputI2S.h"

// I2S pins for MAX98357A (CORRECTED per schematic)
#define I2S_DOUT 23  // DIN  (was 25)
#define I2S_BCLK 26  // BCLK (unchanged)
#define I2S_LRC  25  // LRC  (was 22)

// SD Card pins (HSPI to avoid GPIO18/19 conflict with CC1/CC2)
#define SD_CS    5
#define SD_SCK   14  // was 18
#define SD_MISO  12  // was 19
#define SD_MOSI  13  // was 23

SPIClass hspi(HSPI);

AudioGeneratorMP3 *mp3;
AudioFileSourceSD *file;
AudioOutputI2S *out;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("Starting MP3 player (FIXED pins)...");
  Serial.println("I2S: DOUT=23, BCLK=26, LRC=25");
  Serial.println("SD:  SCK=14, MISO=12, MOSI=13, CS=5");
  
  // Initialize SD card with HSPI
  hspi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  
  if(!SD.begin(SD_CS, hspi)) {
    Serial.println("SD Card Mount Failed!");
    return;
  }
  
  Serial.println("SD Card initialized.");
  
  // List files on SD card (for debugging)
  File root = SD.open("/");
  printDirectory(root, 0);
  
  // Configure I2S output with CORRECTED pins
  out = new AudioOutputI2S();
  out->SetPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  out->SetGain(0.5);
  
  // Open MP3 file from SD card
  file = new AudioFileSourceSD("/test.mp3");
  
  // Create MP3 decoder
  mp3 = new AudioGeneratorMP3();
  
  // Start playing
  mp3->begin(file, out);
  
  Serial.println("Playing MP3...");
}

void loop() {
  if (mp3->isRunning()) {
    if (!mp3->loop()) {
      mp3->stop();
      Serial.println("MP3 playback finished.");
    }
  } else {
    Serial.println("MP3 stopped.");
    delay(1000);
  }
}

void printDirectory(File dir, int numTabs) {
  while (true) {
    File entry = dir.openNextFile();
    if (!entry) {
      break;
    }
    for (uint8_t i = 0; i < numTabs; i++) {
      Serial.print('\t');
    }
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      printDirectory(entry, numTabs + 1);
    } else {
      Serial.print("\t\t");
      Serial.println(entry.size(), DEC);
    }
    entry.close();
  }
}
