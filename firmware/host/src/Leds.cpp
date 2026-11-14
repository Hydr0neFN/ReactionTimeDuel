#include "Leds.h"

#include <Arduino.h>
#include <string.h>

/* Ring mapping of the baseline: player slots left to right are 1,2,center,3,4. */
static const uint8_t PLAYER_TO_RING[RTD_MAX_PLAYERS] = {4, 3, 1, 0};

#define STRIP_ANIM_DURATION 15000  /* auto cycle: switch animation every 15 s */

static const RgbColor RGB_OFF(0);
static const RgbColor RGB_RED(255, 0, 0);
static const RgbColor RGB_GREEN(0, 255, 0);
static const RgbColor RGB_YELLOW(255, 255, 0);
static const RgbColor RGB_WHITE(255, 255, 255);
static const RgbColor RGB_HANZE(255, 148, 48);

static RgbColor rgb(uint32_t value) {
  return RgbColor((uint8_t)((value >> 16) & 0xFF), (uint8_t)((value >> 8) & 0xFF),
                  (uint8_t)(value & 0xFF));
}

Leds::Leds()
    : pixels_(NEOPIXEL_COUNT, PIN_NEOPIXEL),
      strip_(STRIP_LED_COUNT, PIN_STRIP),
      ringBrightness_(16 * RTD_CFG_DEF[CFG_RING_BRIGHT]),
      stripBrightness_(16 * RTD_CFG_DEF[CFG_STRIP_BRIGHT]),
      stripMode_(RTD_CFG_DEF[CFG_STRIP_MODE]),
      stripOff_(false),
      neoMode_(0xFF),
      neoLastMs_(0),
      neoOffset_(0),
      neoBlink_(false),
      stripAnim_(ANIM_RAINBOW),
      stripLastMs_(0),
      stripAnimStartMs_(0),
      stripStep_(0) {
  memset(stripHeat_, 0, sizeof(stripHeat_));
}

void Leds::begin() {
  pixels_.Begin();
  pixels_.SetBrightness(ringBrightness_);
  pixels_.ClearTo(RGB_OFF);
  pixels_.Show();

  strip_.Begin();
  strip_.SetBrightness(stripBrightness_);
  strip_.ClearTo(RGB_OFF);
  strip_.Show();
  stripAnimStartMs_ = millis();
}

void Leds::applySettings(const uint8_t* cfg) {
  uint8_t ring = (uint8_t)(16 * cfg[CFG_RING_BRIGHT]);
  if (ring != ringBrightness_) {
    ringBrightness_ = ring;
    pixels_.SetBrightness(ringBrightness_);
  }

  uint8_t strip = (uint8_t)(16 * cfg[CFG_STRIP_BRIGHT]);
  if (strip != stripBrightness_) {
    stripBrightness_ = strip;
    strip_.SetBrightness(stripBrightness_);
    stripOff_ = false;
  }
  if (stripBrightness_ == 0) {
    if (!stripOff_) {
      strip_.ClearTo(RGB_OFF);
      strip_.Show();
      stripOff_ = true;
    }
    return;
  }

  uint8_t mode = cfg[CFG_STRIP_MODE];
  if (mode != stripMode_) {
    stripMode_ = mode;
    selectStripAnim(mode == 0 ? ANIM_RAINBOW : (StripAnim)(mode - 1));
  }
}

void Leds::setRing(uint8_t ring, const RgbColor& color) {
  uint8_t start = (uint8_t)(ring * LEDS_PER_RING);
  for (uint8_t i = 0; i < LEDS_PER_RING; i++) pixels_.SetPixelColor(start + i, color);
}

void Leds::showRings() {
  if (pixels_.CanShow()) pixels_.Show();
}

/* Centre ring: the time left of the round limit. One LED goes off per 1/12 of
 * the limit; green, then yellow, then red. No limit: the ring is left as it is. */
void Leds::paintLimit(const LedView& view, uint32_t now) {
  if (view.limitMs == 0) return;

  uint32_t elapsed = now - view.limitStartMs;
  int remaining = 0;
  if (elapsed < view.limitMs) {
    remaining = (int)(((view.limitMs - elapsed) * LEDS_PER_RING + view.limitMs - 1) / view.limitMs);
  }
  if (remaining > LEDS_PER_RING) remaining = LEDS_PER_RING;

  RgbColor color = RGB_RED;
  if (remaining > 8) color = RGB_GREEN;
  else if (remaining > 4) color = RGB_YELLOW;

  uint8_t start = (uint8_t)(CENTER_RING * LEDS_PER_RING);
  for (uint8_t i = 0; i < LEDS_PER_RING; i++) {
    pixels_.SetPixelColor((uint16_t)(start + i), i < remaining ? color : RGB_OFF);
  }
}

/* ------------------------------------------------------------------ rings */

static RgbColor wheel(uint8_t pos) {
  pos = (uint8_t)(255 - pos);
  if (pos < 85) return RgbColor((uint8_t)(255 - pos * 3), 0, (uint8_t)(pos * 3));
  if (pos < 170) {
    pos = (uint8_t)(pos - 85);
    return RgbColor(0, (uint8_t)(pos * 3), (uint8_t)(255 - pos * 3));
  }
  pos = (uint8_t)(pos - 170);
  return RgbColor((uint8_t)(pos * 3), (uint8_t)(255 - pos * 3), 0);
}

void Leds::update(const LedView& view) {
  uint32_t now = millis();

  /* Countdown flash: bright white on all rings, synced with audio/vibration. */
  if (view.flashUntilMs != 0 && (int32_t)(now - view.flashUntilMs) < 0) {
    pixels_.ClearTo(RGB_WHITE);
    showRings();
  } else {
    updateRings(view, now);
  }

  updateStrip();
}

void Leds::updateRings(const LedView& view, uint32_t now) {
  if (view.neoMode != neoMode_) {
    neoMode_ = view.neoMode;
    neoLastMs_ = 0;
    neoBlink_ = false;
    neoOffset_ = 0;
  }

  switch (view.neoMode) {
    case NEO_OFF:
      if (now - neoLastMs_ > 50) {
        neoLastMs_ = now;
        pixels_.ClearTo(RGB_OFF);
        showRings();
      }
      break;

    case NEO_IDLE_RAINBOW:
      if (now - neoLastMs_ > 50) {
        neoLastMs_ = now;
        for (int i = 0; i < NEOPIXEL_COUNT; i++) {
          pixels_.SetPixelColor((uint16_t)i,
                                wheel((uint8_t)(i * 256 / NEOPIXEL_COUNT + neoOffset_)));
        }
        neoOffset_++;
        showRings();
      }
      break;

    case NEO_RANDOM_FAST:
      if (now - neoLastMs_ > 30) {
        neoLastMs_ = now;
        for (uint8_t r = 0; r < NUM_RINGS; r++) {
          setRing(r, wheel((uint8_t)(neoOffset_ + r * 51)));
        }
        neoOffset_ += 3;
        showRings();
      }
      break;

    case NEO_FIXED_COLOR:
      /* Re-render so a skipped Show() due to a busy I2S DMA is repaired. */
      if (now - neoLastMs_ > 50) {
        neoLastMs_ = now;
        for (uint8_t r = 0; r < NUM_RINGS; r++) {
          setRing(r, view.ringColor[r] != RTD_RGB_OFF ? rgb(view.ringColor[r]) : RGB_OFF);
        }
        paintLimit(view, now);
        showRings();
      }
      break;

    case NEO_COUNTDOWN:
      if (now - neoLastMs_ > 250) {
        neoLastMs_ = now;
        neoBlink_ = !neoBlink_;
        pixels_.ClearTo(neoBlink_ ? RGB_RED : RGB_OFF);
        showRings();
      }
      break;

    case NEO_STATUS: {
      if (now - neoLastMs_ > 50) {
        neoLastMs_ = now;
        bool blinkOn = ((now / 300) % 2) == 0;
        for (uint8_t r = 0; r < NUM_RINGS; r++) {
          if (view.ringColor[r] == RTD_RGB_OFF) {
            setRing(r, RGB_OFF);
          } else if (view.ringBlink[r] && !blinkOn) {
            setRing(r, RGB_OFF);
          } else {
            setRing(r, rgb(view.ringColor[r]));
          }
        }
        paintLimit(view, now);
        showRings();
      }
      break;
    }

    case NEO_BLINK_SLOT: {
      if (now - neoLastMs_ > 300) {
        neoLastMs_ = now;
        neoBlink_ = !neoBlink_;
        for (uint8_t r = 0; r < NUM_RINGS; r++) {
          setRing(r, view.ringColor[r] != RTD_RGB_OFF ? rgb(view.ringColor[r]) : RGB_OFF);
        }
        setRing(PLAYER_TO_RING[view.blinkSlot & 0x03], neoBlink_ ? RGB_GREEN : RGB_OFF);
        showRings();
      }
      break;
    }

    case NEO_SHAKE_COUNTDOWN:
      if (now - neoLastMs_ > 30) {
        neoLastMs_ = now;

        for (uint8_t r = 0; r < NUM_RINGS; r++) {
          if (r == CENTER_RING) continue;

          int8_t slot = -1;
          for (uint8_t p = 0; p < RTD_MAX_PLAYERS; p++) {
            if ((view.joinedMask & (uint8_t)(1u << p)) && PLAYER_TO_RING[p] == r) {
              slot = (int8_t)p;
              break;
            }
          }
          if (slot < 0 || !(view.activeMask & (uint8_t)(1u << slot))) {
            setRing(r, RGB_OFF);
            continue;
          }

          /* A result paints the ring green, a penalty red+blink (HostGame). */
          if (view.ringBlink[r]) {
            setRing(r, ((now / 300) % 2) ? RGB_RED : RGB_OFF);
            continue;
          }
          if (view.ringColor[r] == RTD_RGB_GREEN) {
            setRing(r, RGB_GREEN);
            continue;
          }

          /* White background, green progress overlay (progress is a percentage). */
          uint8_t ledsLit =
              (uint8_t)((uint16_t)view.progress[slot] * LEDS_PER_RING / 100u);
          if (ledsLit > LEDS_PER_RING) ledsLit = LEDS_PER_RING;
          uint8_t start = (uint8_t)(r * LEDS_PER_RING);
          for (uint8_t i = 0; i < LEDS_PER_RING; i++) {
            pixels_.SetPixelColor((uint16_t)(start + i), i < ledsLit ? RGB_GREEN : RGB_WHITE);
          }
        }

        paintLimit(view, now);
        showRings();
      }
      break;

    default:
      break;
  }

  neoMode_ = view.neoMode;
}

/* ------------------------------------------------------------------ strip */

static RgbColor stripWheel(uint8_t pos) {
  pos = (uint8_t)(255 - pos);
  if (pos < 85) return RgbColor((uint8_t)(255 - pos * 3), 0, (uint8_t)(pos * 3));
  if (pos < 170) {
    pos = (uint8_t)(pos - 85);
    return RgbColor(0, (uint8_t)(pos * 3), (uint8_t)(255 - pos * 3));
  }
  pos = (uint8_t)(pos - 170);
  return RgbColor((uint8_t)(pos * 3), (uint8_t)(255 - pos * 3), 0);
}

static RgbColor dimColor(const RgbColor& color, uint8_t bright) {
  return RgbColor((uint8_t)((uint16_t)color.R * bright / 255),
                  (uint8_t)((uint16_t)color.G * bright / 255),
                  (uint8_t)((uint16_t)color.B * bright / 255));
}

void Leds::selectStripAnim(StripAnim anim) {
  stripAnim_ = anim;
  stripStep_ = 0;
  stripAnimStartMs_ = millis();
  stripLastMs_ = 0;
  memset(stripHeat_, 0, sizeof(stripHeat_));
  strip_.ClearTo(RGB_OFF);
  strip_.Show();
}

void Leds::updateStrip() {
  if (stripBrightness_ == 0) return;

  uint32_t now = millis();
  if (stripMode_ == 0 && now - stripAnimStartMs_ > STRIP_ANIM_DURATION) {
    StripAnim next;
    do {
      next = (StripAnim)random(ANIM_BASELINE_COUNT);
    } while (next == stripAnim_ && ANIM_BASELINE_COUNT > 1);
    selectStripAnim(next);
  }

  switch (stripAnim_) {
    case ANIM_RAINBOW:   stripRainbow();   break;
    case ANIM_SPARKLE:   stripSparkle();   break;
    case ANIM_METEOR:    stripMeteor();    break;
    case ANIM_CHASE:     stripChase();     break;
    case ANIM_BREATHING: stripBreathing(); break;
    case ANIM_FIRE:      stripFire();      break;
    case ANIM_HANZE:     stripHanze();     break;
    default:             stripRainbow();   break;
  }
}

void Leds::stripRainbow() {
  if (millis() - stripLastMs_ < 30) return;
  stripLastMs_ = millis();
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    strip_.SetPixelColor((uint16_t)i,
                         stripWheel((uint8_t)(i * 256 / STRIP_LED_COUNT + stripStep_)));
  }
  if (strip_.CanShow()) strip_.Show();
  stripStep_++;
}

void Leds::stripSparkle() {
  if (millis() - stripLastMs_ < 50) return;
  stripLastMs_ = millis();
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    strip_.SetPixelColor((uint16_t)i, dimColor(strip_.GetPixelColor((uint16_t)i), 200));
  }
  for (int s = 0; s < 3; s++) {
    strip_.SetPixelColor((uint16_t)random(STRIP_LED_COUNT), stripWheel((uint8_t)random(256)));
  }
  if (strip_.CanShow()) strip_.Show();
}

void Leds::stripMeteor() {
  if (millis() - stripLastMs_ < 25) return;
  stripLastMs_ = millis();
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    if (random(10) > 4) {
      strip_.SetPixelColor((uint16_t)i, dimColor(strip_.GetPixelColor((uint16_t)i), 160));
    }
  }
  uint32_t head = stripStep_ % (STRIP_LED_COUNT + 20);
  for (int j = 0; j < 6; j++) {
    int pos = (int)head - j;
    if (pos >= 0 && pos < STRIP_LED_COUNT) {
      strip_.SetPixelColor((uint16_t)pos,
                           dimColor(RgbColor(200, 80, 255), (uint8_t)(255 - j * 40)));
    }
  }
  if (strip_.CanShow()) strip_.Show();
  stripStep_++;
}

void Leds::stripChase() {
  if (millis() - stripLastMs_ < 60) return;
  stripLastMs_ = millis();
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t seg = (uint8_t)((i + stripStep_) % 18);
    if (seg < 6) strip_.SetPixelColor((uint16_t)i, RgbColor(255, 0, 0));
    else if (seg < 12) strip_.SetPixelColor((uint16_t)i, RgbColor(0, 255, 0));
    else strip_.SetPixelColor((uint16_t)i, RgbColor(0, 0, 255));
  }
  if (strip_.CanShow()) strip_.Show();
  stripStep_++;
}

void Leds::stripBreathing() {
  if (millis() - stripLastMs_ < 20) return;
  stripLastMs_ = millis();
  uint8_t phase = (uint8_t)(stripStep_ & 0xFF);
  uint8_t level = (phase < 128) ? (uint8_t)(phase * 2) : (uint8_t)((255 - phase) * 2);
  uint8_t bright = (uint8_t)((uint16_t)level * level / 255);
  uint8_t hue = (uint8_t)((stripStep_ / 4) & 0xFF);
  strip_.ClearTo(dimColor(stripWheel(hue), bright));
  if (strip_.CanShow()) strip_.Show();
  stripStep_++;
}

void Leds::stripFire() {
  if (millis() - stripLastMs_ < 30) return;
  stripLastMs_ = millis();

  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t cooldown = (uint8_t)random(0, 20);
    stripHeat_[i] = stripHeat_[i] > cooldown ? (uint8_t)(stripHeat_[i] - cooldown) : 0;
  }
  for (int i = STRIP_LED_COUNT - 1; i >= 2; i--) {
    stripHeat_[i] = (uint8_t)((stripHeat_[i - 1] + stripHeat_[i - 2] + stripHeat_[i - 2]) / 3);
  }
  if (random(255) < 160) {
    int pos = (int)random(7);
    long value = (long)stripHeat_[pos] + random(160, 255);
    stripHeat_[pos] = (uint8_t)(value > 255 ? 255 : value);
  }
  for (int i = 0; i < STRIP_LED_COUNT; i++) {
    uint8_t t = stripHeat_[i];
    RgbColor color;
    if (t < 85) color = RgbColor((uint8_t)(t * 3), 0, 0);
    else if (t < 170) color = RgbColor(255, (uint8_t)((t - 85) * 3), 0);
    else color = RgbColor(255, 255, (uint8_t)((t - 170) * 3));
    strip_.SetPixelColor((uint16_t)i, color);
  }
  if (strip_.CanShow()) strip_.Show();
}

void Leds::stripHanze() {
  if (millis() - stripLastMs_ < 20) return;
  stripLastMs_ = millis();
  uint8_t phase = (uint8_t)(stripStep_ & 0xFF);
  uint8_t level = (phase < 128) ? (uint8_t)(phase * 2) : (uint8_t)((255 - phase) * 2);
  uint8_t bright = (uint8_t)((uint16_t)level * level / 255);
  strip_.ClearTo(dimColor(RGB_HANZE, bright));
  if (strip_.CanShow()) strip_.Show();
  stripStep_++;
}
