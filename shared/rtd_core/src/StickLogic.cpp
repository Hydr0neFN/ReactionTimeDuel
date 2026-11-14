#include "StickLogic.h"

#include <string.h>

StickLogic::StickLogic(StickPorts& ports, uint8_t stickId)
    : ports_(ports),
      id_(stickId),
      state_(JS_IDLE),
      assignedSlot_(0),
      beaconMissCount_(0),
      hostPhase_(PH_IDLE),
      mode_(MODE_REACTION),
      shakeParam_(0),
      shakeTarget_(0),
      bootMs_(0),
      lastHostMs_(0),
      hbMs_(0),
      hbStarted_(false),
      joinActive_(false),
      reqSends_(0),
      reqNextMs_(0),
      resultActive_(false),
      resultCmd_(0),
      resultData_(0),
      lastResult_(0),
      resultSends_(0),
      resultNextMs_(0),
      doneMs_(0),
      goUs_(0),
      goMs_(0),
      sawFall_(false),
      lastFallUs_(0),
      blankSampled_(false),
      blankDone_(false),
      blankS1_(false),
      blankSampleMs_(0),
      shakeCount_(0),
      shakeHaveRef_(false),
      shakeGravInit_(false),
      shakeLastCountMs_(0),
      shakePeak2_(0),
      shakePeak_(0),
      shakeFinished_(0),
      shakeMaxPeak_(0),
      shakeMinPeak_(0),
      shakePeakSum_(0),
      shakeSoftMax_(0),
      shakeSoft2_(0),
      shakeStartMs_(0),
      lastAccelMs_(0),
      shakeLastReported_(0),
      motorOn_(false),
      motorStartMs_(0),
      motorDurMs_(0) {
  bootMs_ = ports_.millis();
  lastHostMs_ = bootMs_;
  hbMs_ = bootMs_;
}

uint16_t StickLogic::versionData() const {
  return (uint16_t)(((uint16_t)FW_VERSION_BYTE << 8) | FW_VERSION_PATCH);
}

void StickLogic::sendCmd(uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, ID_HOST, id_, cmd, data);
  ports_.send((const uint8_t*)&pkt, PACKET_SIZE);
}

void StickLogic::vibStart(uint16_t durationMs) {
  motorOn_ = true;
  motorStartMs_ = ports_.millis();
  motorDurMs_ = durationMs;
  ports_.motor(true);
}

void StickLogic::goIdle() {
  state_ = JS_IDLE;
  assignedSlot_ = 0;
  beaconMissCount_ = 0;
  joinActive_ = false;
  resultActive_ = false;
  goUs_ = 0;
  goMs_ = 0;
  sawFall_ = false;
  blankSampled_ = false;
  blankDone_ = false;
  shakeCount_ = 0;
  shakeHaveRef_ = false;
  shakeGravInit_ = false;
}

void StickLogic::startJoinRequest(uint32_t now) {
  joinActive_ = true;
  reqSends_ = 1;
  reqNextMs_ = now;
  sendCmd(CMD_REQ_ID, versionData());
}

void StickLogic::finish(uint8_t cmd, uint16_t data, uint32_t now) {
  resultCmd_ = cmd;
  resultData_ = data;
  lastResult_ = data;
  resultSends_ = 1;
  resultNextMs_ = now;
  resultActive_ = true;
  doneMs_ = now;
  state_ = JS_DONE;
  sendCmd(cmd, data);
  if (data != TIME_PENALTY) {
    vibStart(cmd == CMD_SHAKE_DONE ? SHAKE_DONE_VIBRATE_MS : RESULT_VIBRATE_MS);
  }
}

void StickLogic::onButtonFall(uint32_t us) {
  if (state_ == JS_WAITING_GO) {
    sawFall_ = true;
    lastFallUs_ = us;
    return;
  }
  if (state_ == JS_REACTION_TIMING) {
    int32_t sinceGo = (int32_t)(us - goUs_);
    if (sinceGo < 0) {
      /* the edge happened before GO: the false start window of DESIGN 4.2 applies */
      if ((uint32_t)(goUs_ - us) <= (uint32_t)(FALSE_START_WINDOW_MS * 1000u)) {
        finish(CMD_REACTION_DONE, TIME_PENALTY, ports_.millis());
      }
      return;
    }
    if ((uint32_t)sinceGo >= (uint32_t)(GO_BLANK_MS * 1000u)) {
      uint32_t ms = (uint32_t)sinceGo / 1000u;
      if (ms == 0) ms = 1;
      if (ms > 0xFFFEu) ms = 0xFFFEu;
      finish(CMD_REACTION_DONE, (uint16_t)ms, ports_.millis());
    }
    return;
  }
  if (state_ == JS_IDLE && assignedSlot_ == 0 && !joinActive_) {
    startJoinRequest(ports_.millis());
  }
}

void StickLogic::handleGo(const GamePacket& pkt, uint16_t data, uint32_t rxMicros) {
  (void)pkt;
  if (state_ != JS_WAITING_GO) return;
  if (assignedSlot_ == 0) return;

  /* fair GO instant of DESIGN 4.1: the receive time minus the age in the packet */
  uint32_t ageMs = (uint32_t)data;
  goUs_ = (uint32_t)(rxMicros - ageMs * 1000u);
  goMs_ = (uint32_t)(ports_.millis() - ageMs);
  blankSampled_ = false;
  blankDone_ = false;

  bool windowPress = sawFall_ &&
      (uint32_t)(goUs_ - lastFallUs_) <= (uint32_t)(FALSE_START_WINDOW_MS * 1000u);
  sawFall_ = false;
  lastFallUs_ = 0;

  bool buttonAtGo = ports_.buttonDown();

  if (mode_ == MODE_SHAKE) {
    vibStart(GO_VIBRATE_MS);
    shakeCount_ = 0;
    shakeHaveRef_ = false;
    shakeGravInit_ = false;
    shakeStatsClear();
    shakeLastReported_ = 0;
    shakeStartMs_ = goMs_;
    lastAccelMs_ = goMs_;
    state_ = JS_SHAKE_COUNTING;
    return;
  }

  bool falseStart = buttonAtGo || windowPress;

  vibStart(GO_VIBRATE_MS);

  if (falseStart) {
    finish(CMD_REACTION_DONE, TIME_PENALTY, goMs_);
  } else {
    state_ = JS_REACTION_TIMING;
  }
}

void StickLogic::onPacket(const uint8_t* data, int len, uint32_t rxMicros) {
  if (len != PACKET_SIZE) return;
  if (!validatePacket(data, len)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  if (pkt.dest_id != id_ && pkt.dest_id != ID_BROADCAST) return;

  uint16_t val = packetData(&pkt);
  uint32_t now = ports_.millis();
  lastHostMs_ = now;

  switch (pkt.cmd) {
    case CMD_ACK:
      if (resultActive_ && pkt.data_low == resultCmd_) resultActive_ = false;
      break;

    case CMD_OK:
      if (pkt.data_low >= 1 && pkt.data_low <= RTD_MAX_PLAYERS) {
        assignedSlot_ = pkt.data_low;
        beaconMissCount_ = 0;
        joinActive_ = false;
        vibStart(JOIN_VIBRATE_MS);
      }
      break;

    case CMD_IDLE:
      goIdle();
      if (pkt.dest_id != ID_BROADCAST) sendCmd(CMD_ACK, CMD_IDLE);
      break;

    case CMD_GAME_START:
      if (assignedSlot_ == 0) break;
      mode_ = pkt.data_high;
      shakeParam_ = pkt.data_low;
      shakeTarget_ = (uint16_t)pkt.data_low * SHAKE_FACTOR;
      state_ = JS_WAITING_GO;
      goUs_ = 0;
      goMs_ = 0;
      sawFall_ = false;
      blankSampled_ = false;
      blankDone_ = false;
      resultActive_ = false;
      sendCmd(CMD_ACK, CMD_GAME_START);
      break;

    case CMD_COUNTDOWN:
      vibStart(COUNTDOWN_VIBRATE_MS);
      if (pkt.dest_id != ID_BROADCAST) sendCmd(CMD_ACK, CMD_COUNTDOWN);
      break;

    case CMD_VIBRATE:
      vibStart((uint16_t)pkt.data_low * 10u);
      break;

    case CMD_GO:
      if (pkt.dest_id != ID_BROADCAST) sendCmd(CMD_ACK, CMD_GO);
      handleGo(pkt, val, rxMicros);
      break;

    case CMD_OTA_BEGIN: {
      uint8_t ver = pkt.data_high;
      uint8_t patch = pkt.data_low;
      sendCmd(CMD_ACK, CMD_OTA_BEGIN);
      if (ver == FW_VERSION_BYTE && patch == FW_VERSION_PATCH) {
        sendCmd(CMD_OTA_STATUS, ((uint16_t)OTA_ST_UPTODATE << 8) | 0);
      } else {
        sendCmd(CMD_OTA_STATUS, ((uint16_t)OTA_ST_STARTING << 8) | 0);
        state_ = JS_OTA;
        ports_.otaStart(ver, patch);
      }
      break;
    }

    case CMD_BEACON: {
      uint8_t joinedMask = (uint8_t)(val & 0x0F);
      hostPhase_ = (uint8_t)(val >> 8);
      bool joined = (joinedMask & (uint8_t)(1u << (id_ - ID_STICK1))) != 0;
      if (joined) {
        beaconMissCount_ = 0;
        break;
      }
      if (beaconMissCount_ < 0xFF) beaconMissCount_++;
      if (state_ == JS_OTA) break;
      if (state_ != JS_IDLE) {
        goIdle();
      } else if (assignedSlot_ != 0 && beaconMissCount_ >= BEACON_MISS_LIMIT) {
        /* a lost CMD_IDLE or a host reboot leaves a stale slot; two beacons confirm it */
        assignedSlot_ = 0;
        joinActive_ = false;
      }
      break;
    }

    default:
      break;
  }
}

void StickLogic::reactionTick(uint32_t now) {
  uint32_t elapsed = (uint32_t)(now - goMs_);

  if (!blankDone_ && elapsed >= GO_BLANK_MS) {
    if (!blankSampled_) {
      blankS1_ = ports_.buttonDown();
      blankSampled_ = true;
      blankSampleMs_ = now;
    } else if ((uint32_t)(now - blankSampleMs_) >= GO_BLANK_SAMPLE_MS) {
      blankDone_ = true;
      bool s2 = ports_.buttonDown();
      if (blankS1_ && s2) {
        finish(CMD_REACTION_DONE, TIME_PENALTY, now);
        return;
      }
    }
  }

  if (elapsed > TIMEOUT_REACTION) {
    finish(CMD_REACTION_DONE, TIME_PENALTY, now);
  }
}

static uint16_t isqrt32(uint32_t v) {
  uint32_t r = 0;
  for (uint32_t bit = 1u << 15; bit; bit >>= 1) {
    uint32_t t = r | bit;
    if (t * t <= v) r = t;
  }
  return (uint16_t)r;
}

void StickLogic::shakeStatsClear() {
  shakePeak2_ = 0;
  shakePeak_ = 0;
  shakeFinished_ = 0;
  shakeMaxPeak_ = 0;
  shakeMinPeak_ = 0xFFFF;
  shakePeakSum_ = 0;
  shakeSoftMax_ = 0;
  shakeSoft2_ = 0;
}

/* The running excursion is over: its peak joins the round statistics. */
void StickLogic::shakeEndExcursion() {
  shakePeak_ = isqrt32(shakePeak2_);
  shakeFinished_++;
  shakePeakSum_ += shakePeak_;
  if (shakePeak_ > shakeMaxPeak_) shakeMaxPeak_ = shakePeak_;
  if (shakePeak_ < shakeMinPeak_) shakeMinPeak_ = shakePeak_;
}

void StickLogic::shakeSoftEnd() {
  uint16_t soft = isqrt32(shakeSoft2_);
  if (soft > shakeSoftMax_) shakeSoftMax_ = soft;
  shakeSoft2_ = 0;
}

void StickLogic::shakeRoundEnd() {
  if (shakeHaveRef_) shakeEndExcursion();
}

void StickLogic::shakeTick(uint32_t now) {
  if ((uint32_t)(now - lastAccelMs_) < SHAKE_SAMPLE_MS) return;
  lastAccelMs_ = now;

  int16_t ax, ay, az;
  if (!ports_.readAccel(ax, ay, az)) return;

  const int32_t raw[3] = {ax, ay, az};
  if (!shakeGravInit_) {
    for (uint8_t i = 0; i < 3; i++) shakeGrav_[i] = raw[i] * (1 << SHAKE_GRAVITY_SHIFT);
    shakeGravInit_ = true;
  }
  /* squared length of the dynamic vector; each axis clamped so the sum fits uint32_t */
  int32_t d[3];
  uint32_t mag2 = 0;
  for (uint8_t i = 0; i < 3; i++) {
    int32_t g = shakeGrav_[i] / (1 << SHAKE_GRAVITY_SHIFT);
    d[i] = raw[i] - g;
    shakeGrav_[i] += d[i];
    if (d[i] > 32767) d[i] = 32767;
    if (d[i] < -32767) d[i] = -32767;
    mag2 += (uint32_t)(d[i] * d[i]);
  }

  /* A shake is counted at each reversal: the dynamic vector points against the direction of the
   * last counted half shake and is longer than SHAKE_HIGH_LSB. One back-and-forth = 2 counts,
   * in any direction, also when the magnitude does not drop to zero between the two halves. */
  const uint32_t hi2 = (uint32_t)SHAKE_HIGH_LSB * SHAKE_HIGH_LSB;
  const uint32_t lo2 = (uint32_t)SHAKE_LOW_LSB * SHAKE_LOW_LSB;
  int64_t dot = 0;
  if (shakeHaveRef_) {
    for (uint8_t i = 0; i < 3; i++) dot += (int64_t)d[i] * shakeRef_[i];
  }
  bool reversed = !shakeHaveRef_ || dot < 0;

  if (!reversed) {
    /* still the same half shake: follow its peak */
    if (mag2 > shakePeak2_) {
      shakePeak2_ = mag2;
      for (uint8_t i = 0; i < 3; i++) shakeRef_[i] = d[i];
    }
    /* a reversal that stayed too weak is over: remember how close it came */
    if (mag2 >= lo2) shakeSoftEnd();
  } else if (mag2 <= hi2) {
    if (mag2 >= lo2 && mag2 > shakeSoft2_) shakeSoft2_ = mag2;
    if (!shakeHaveRef_ && mag2 < lo2) shakeSoftEnd();
  } else if (shakeCount_ == 0 || (uint32_t)(now - shakeLastCountMs_) >= SHAKE_MIN_GAP_MS) {
    if (shakeHaveRef_) shakeEndExcursion();
    shakeHaveRef_ = true;
    for (uint8_t i = 0; i < 3; i++) shakeRef_[i] = d[i];
    shakeLastCountMs_ = now;
    shakePeak2_ = mag2;
    shakeSoft2_ = 0;   /* the rising edge of a counted shake is not a soft shake */
    if (shakeCount_ < 0xFFFF) shakeCount_++;
    uint16_t step = (uint16_t)(shakeTarget_ / SHAKE_PROGRESS_STEPS);
    if (step < 1) step = 1;
    uint16_t milestone = (uint16_t)((shakeCount_ / step) * step);
    if (milestone > shakeLastReported_ && milestone < shakeTarget_) {
      shakeLastReported_ = milestone;
      sendCmd(CMD_SHAKE_PROGRESS,
              (uint16_t)(((uint16_t)milestone << 8) | (shakeTarget_ & 0xFF)));
    }
  }

  if (shakeCount_ >= shakeTarget_) {
    shakeRoundEnd();
    uint32_t elapsed = (uint32_t)(now - shakeStartMs_);
    if (elapsed > 0xFFFEu) elapsed = 0xFFFEu;
    finish(CMD_SHAKE_DONE, (uint16_t)elapsed, now);
    return;
  }

  if ((uint32_t)(now - shakeStartMs_) > TIMEOUT_SHAKE) {
    shakeRoundEnd();
    finish(CMD_SHAKE_DONE, TIME_PENALTY, now);
  }
}

void StickLogic::tick() {
  uint32_t now = ports_.millis();

  if (motorOn_ && (uint32_t)(now - motorStartMs_) >= motorDurMs_) {
    motorOn_ = false;
    ports_.motor(false);
  }

  if (state_ != JS_OTA) {
    if (!hbStarted_) {
      if ((uint32_t)(now - bootMs_) >= (uint32_t)HEARTBEAT_FIRST_MS * (uint32_t)id_) {
        hbStarted_ = true;
        hbMs_ = now;
        sendCmd(CMD_HEARTBEAT, versionData());
      }
    } else if ((uint32_t)(now - hbMs_) >= HEARTBEAT_INTERVAL_MS) {
      hbMs_ = now;
      sendCmd(CMD_HEARTBEAT, versionData());
    }
  }

  if (resultActive_ && (uint32_t)(now - resultNextMs_) >= RESULT_RESEND_MS) {
    if (resultSends_ < RESULT_MAX_SENDS) {
      resultSends_++;
      resultNextMs_ = now;
      sendCmd(resultCmd_, resultData_);
    } else {
      resultActive_ = false;
    }
  }

  if (joinActive_ && (uint32_t)(now - reqNextMs_) >= REQID_RESEND_MS) {
    if (reqSends_ < REQID_MAX_SENDS) {
      reqSends_++;
      reqNextMs_ = now;
      sendCmd(CMD_REQ_ID, versionData());
    } else {
      joinActive_ = false;
    }
  }

  if (state_ != JS_IDLE && state_ != JS_OTA &&
      (uint32_t)(now - lastHostMs_) >= STICK_SILENCE_MS) {
    goIdle();
  }

  switch (state_) {
    case JS_REACTION_TIMING:
      reactionTick(now);
      break;
    case JS_SHAKE_COUNTING:
      shakeTick(now);
      break;
    case JS_DONE:
      if ((uint32_t)(now - doneMs_) > DONE_RESET_MS) goIdle();
      break;
    default:
      break;
  }
}
