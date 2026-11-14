#ifndef RTD_LEDS_H
#define RTD_LEDS_H

/*
 * Baseline NeoPixel ring and WS2812B strip animations (DESIGN.md 5.3, 8).
 * The animation parameters come only from HostGame::leds() and the settings;
 * no game state is read here.
 */

#include <stdint.h>
#include <NeoPixelBrightnessBus.h>
#include "Board.h"
#include "HostGame.h"

class Leds {
public:
  Leds();

  void begin();

  /* Ring/strip brightness and strip mode from the settings, applied at once. */
  void applySettings(const uint8_t* cfg);

  /* Advance the animations; call from the main loop. */
  void update(const LedView& view);

private:
  enum StripAnim : uint8_t {
    ANIM_RAINBOW = 0,
    ANIM_SPARKLE,
    ANIM_METEOR,
    ANIM_CHASE,
    ANIM_BREATHING,
    ANIM_FIRE,
    ANIM_HANZE,
    ANIM_BASELINE_COUNT = 6   /* animations the auto cycle picks from */
  };

  void setRing(uint8_t ring, const RgbColor& color);
  void showRings();
  void paintLimit(const LedView& view, uint32_t now);
  void updateRings(const LedView& view, uint32_t now);

  void selectStripAnim(StripAnim anim);
  void updateStrip();
  void stripRainbow();
  void stripSparkle();
  void stripMeteor();
  void stripChase();
  void stripBreathing();
  void stripFire();
  void stripHanze();

  /* Rings: I2S port 0 with DMA. The whole frame is encoded before it is sent,
   * so a late interrupt (flash read for the audio files, Wi-Fi) cannot corrupt
   * the bit stream. RMT refills its 64-bit buffer in an interrupt and did
   * glitch. The audio uses I2S port 1. The strip stays on RMT channel 1. */
  NeoPixelBrightnessBus<NeoGrbFeature, NeoEsp32I2s0800KbpsMethod> pixels_;
  NeoPixelBrightnessBus<NeoGrbFeature, NeoEsp32Rmt1800KbpsMethod> strip_;

  uint8_t ringBrightness_;
  uint8_t stripBrightness_;
  uint8_t stripMode_;
  bool stripOff_;

  uint8_t neoMode_;
  uint32_t neoLastMs_;
  uint32_t neoOffset_;
  bool neoBlink_;

  StripAnim stripAnim_;
  uint32_t stripLastMs_;
  uint32_t stripAnimStartMs_;
  uint32_t stripStep_;
  uint8_t stripHeat_[STRIP_LED_COUNT];
};

#endif /* RTD_LEDS_H */
