#include "sim.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* sound lengths, roughly the MP3 lengths of firmware/host/data        */
/* ------------------------------------------------------------------ */
static uint32_t soundDurationUs(RtdSound s) {
  switch (s) {
    case SND_BUTTON_CLICK:     return 100000;
    case SND_GET_READY:        return 990000;
    case SND_PRESS_TO_JOIN:    return 1500000;
    case SND_READY:            return 500000;
    case SND_REACTION_MODE:    return 1220000;
    case SND_REACTION_INSTRUCT:return 4700000;
    case SND_SHAKE_IT:         return 820000;
    case SND_YOU_WILL_SHAKE:   return 4300000;
    case SND_NUM_1:            return 600000;
    case SND_NUM_2:            return 600000;
    case SND_NUM_3:            return 600000;
    case SND_NUM_10:           return 1270000;
    case SND_NUM_15:           return 1270000;
    case SND_NUM_20:           return 1270000;
    case SND_BEEP:             return 300000;
    case SND_PLAYER_1:         return 1420000;
    case SND_PLAYER_2:         return 1420000;
    case SND_PLAYER_3:         return 1420000;
    case SND_PLAYER_4:         return 1420000;
    case SND_FASTEST:          return 500000;
    case SND_WINS:             return 500000;
    case SND_VICTORY_FANFARE:  return 10530000;
    case SND_GAME_OVER:        return 1230000;
    case SND_ERROR_TONE:       return 500000;
    default:                   return 0;
  }
}

#define AUDIO_GAP_US 250000u

/* ------------------------------------------------------------------ */
/* port implementations                                                */
/* ------------------------------------------------------------------ */

struct Sim::HostImpl : public HostPorts {
  Sim* s;

  uint32_t millis() { return s->nowMs(); }
  uint32_t micros() { return (uint32_t)s->nowUs_; }
  uint32_t random(uint32_t n) { return s->randn(n); }
  void send(uint8_t destId, const uint8_t* data, uint8_t len) { s->hostSend(destId, data, len); }
  void audioPlay(RtdSound x) { s->audioPlay(x); }
  void audioStop() { s->audioStop(); }
  bool audioIdle() { return s->audioIdle(); }
  void applySettings(const uint8_t* cfg) { (void)cfg; }
  void saveSettings(const uint8_t* cfg) {
    (void)cfg;
    s->saveSettingsCalls++;
  }
  void saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal) {
    (void)bestAlltimeMs;
    (void)gamesTotal;
    s->saveStatsCalls++;
  }
  bool stickImage(uint8_t* verByte, uint8_t* patch) {
    if (!s->imagePresent_) return false;
    *verByte = s->imageVer_;
    *patch = s->imagePatch_;
    return true;
  }
  void maintEnter() { s->maintEnterCalls++; }
  void maintExit() { s->maintExitCalls++; }
};

struct Sim::StickImpl : public StickPorts {
  Sim* s;
  uint8_t id;

  uint32_t millis() { return s->nowMs(); }
  uint32_t micros() { return (uint32_t)s->nowUs_; }
  void send(const uint8_t* data, uint8_t len) { s->stickSend(id, data, len); }
  void motor(bool on) { (void)on; }
  bool buttonDown() { return s->buttonDown_[id]; }
  bool readAccel(int16_t& ax, int16_t& ay, int16_t& az) { return s->readAccel(id, ax, ay, az); }
  void otaStart(uint8_t verByte, uint8_t patch) { s->otaStart(id, verByte, patch); }
};

struct Sim::DispImpl : public DisplayPorts {
  Sim* s;

  uint32_t millis() { return s->nowMs(); }
  void send(const uint8_t* data, uint8_t len) { s->displaySend(data, len); }
};

/* ------------------------------------------------------------------ */
/* construction                                                        */
/* ------------------------------------------------------------------ */

Sim::Sim(uint32_t seed, uint64_t startUs)
    : nowUs_(startUs),
      seed_(seed),
      rng_(seed ? seed : 1u),
      displayLoss_(0),
      dupPercent_(0),
      jitterMs_(3),
      dropBroadcastGo_(false),
      broadcastGoDropped_(false),
      dropBroadcastIdle_(false),
      bestAlltimeMs_(TIME_NONE),
      gamesTotal_(0),
      frameCount_(0),
      buttonCount_(0),
      imagePresent_(false),
      imageVer_(0),
      imagePatch_(0),
      hostGoT0Us_(0),
      lastPhase_(0),
      otaBeginCount_(0),
      audioHead_(0),
      audioTail_(0),
      audioPlaying_(false),
      audioEndUs_(0),
      audioLastEndUs_(0),
      audioHasPlayed_(false),
      hostPorts_(0),
      dispPorts_(0),
      host_(0),
      display_(0) {
  saveSettingsCalls = 0;
  saveStatsCalls = 0;
  maintEnterCalls = 0;
  maintExitCalls = 0;
  for (uint32_t k = 0; k < CFG_SLOTS; k++) cfg_[k] = RTD_CFG_DEF[k];
  memset(player, 0, sizeof(player));
  memset(goDrop_, 0, sizeof(goDrop_));
  memset(hostToStickLoss_, 0, sizeof(hostToStickLoss_));
  memset(stickToHostLoss_, 0, sizeof(stickToHostLoss_));
  memset(buttonDown_, 0, sizeof(buttonDown_));
  memset(alive_, 0, sizeof(alive_));
  memset(fwVer_, 0, sizeof(fwVer_));
  memset(fwPatch_, 0, sizeof(fwPatch_));
  memset(otaComesBack_, 0, sizeof(otaComesBack_));
  memset(otaRebootAt_, 0, sizeof(otaRebootAt_));
  memset(otaRebootPending_, 0, sizeof(otaRebootPending_));
  memset(otaNewVer_, 0, sizeof(otaNewVer_));
  memset(otaNewPatch_, 0, sizeof(otaNewPatch_));
  memset(stickGoUs_, 0, sizeof(stickGoUs_));
  memset(goRecorded_, 0, sizeof(goRecorded_));
  memset(reactScheduled_, 0, sizeof(reactScheduled_));
  memset(earlyScheduled_, 0, sizeof(earlyScheduled_));
  memset(shakeStartUs_, 0, sizeof(shakeStartUs_));
  memset(hostSent_, 0, sizeof(hostSent_));
  memset(hostRecv_, 0, sizeof(hostRecv_));
  memset(displaySent_, 0, sizeof(displaySent_));
  memset(sounds_, 0, sizeof(sounds_));
  memset(otaBeginOrder_, 0, sizeof(otaBeginOrder_));
  memset(audioQ_, 0, sizeof(audioQ_));
  memset(stickPorts_, 0, sizeof(stickPorts_));

  uint32_t i;
  for (i = 0; i < SIM_STICKS; i++) {
    alive_[i + 1] = true;
    otaComesBack_[i + 1] = true;
    fwVer_[i + 1] = FW_VERSION_BYTE;
    fwPatch_[i + 1] = FW_VERSION_PATCH;
  }

  hostPorts_ = new HostImpl();
  hostPorts_->s = this;
  for (i = 0; i < SIM_STICKS; i++) {
    stickPorts_[i] = new StickImpl();
    stickPorts_[i]->s = this;
    stickPorts_[i]->id = (uint8_t)(i + 1);
  }
  dispPorts_ = new DispImpl();
  dispPorts_->s = this;

  host_ = new HostGame(*hostPorts_);
  for (i = 0; i < SIM_STICKS; i++) {
    sticks_[i] = new StickLogic(*stickPorts_[i], (uint8_t)(i + 1));
  }
  display_ = new DisplayLink(*dispPorts_);

  lastPhase_ = host_->phase();
  host_->begin(cfg_, bestAlltimeMs_, gamesTotal_);
}

Sim::~Sim() {
  delete display_;
  for (uint32_t i = 0; i < SIM_STICKS; i++) delete sticks_[i];
  delete host_;
  delete dispPorts_;
  for (uint32_t i = 0; i < SIM_STICKS; i++) delete stickPorts_[i];
  delete hostPorts_;
}

void Sim::beginWith(const uint8_t* cfg, uint16_t bestAlltimeMs, uint16_t gamesTotal) {
  for (uint8_t k = 0; k < CFG_SLOTS; k++) cfg_[k] = cfg ? cfg[k] : RTD_CFG_DEF[k];
  bestAlltimeMs_ = bestAlltimeMs;
  gamesTotal_ = gamesTotal;
  host_->begin(cfg_, bestAlltimeMs_, gamesTotal_);
  lastPhase_ = host_->phase();
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

uint32_t Sim::rnd() {
  rng_ = rng_ * 1664525u + 1013904223u;
  return rng_ >> 8;
}

uint32_t Sim::randn(uint32_t n) {
  if (n == 0) return 0;
  return rnd() % n;
}

bool Sim::maybeLose(uint8_t percent) {
  if (percent == 0) return false;
  return randn(100) < percent;
}

void Sim::setLoss(uint8_t percent) {
  for (uint32_t i = 0; i < SIM_STICKS; i++) {
    hostToStickLoss_[i] = percent;
    stickToHostLoss_[i] = percent;
  }
  displayLoss_ = percent;
}

void Sim::setHostToStickLoss(uint8_t id, uint8_t percent) {
  if (id >= 1 && id <= SIM_STICKS) hostToStickLoss_[id - 1] = percent;
}

void Sim::setStickToHostLoss(uint8_t id, uint8_t percent) {
  if (id >= 1 && id <= SIM_STICKS) stickToHostLoss_[id - 1] = percent;
}

void Sim::setDisplayLoss(uint8_t percent) { displayLoss_ = percent; }
void Sim::setDuplicatePercent(uint8_t percent) { dupPercent_ = percent; }
void Sim::setJitterMs(uint32_t maxJitterMs) { jitterMs_ = maxJitterMs; }
void Sim::dropNextBroadcastGo() { dropBroadcastGo_ = true; }
void Sim::dropNextBroadcastIdle() { dropBroadcastIdle_ = true; }

void Sim::rebootHost() {
  delete host_;
  host_ = new HostGame(*hostPorts_);
  host_->begin(cfg_, bestAlltimeMs_, gamesTotal_);
  lastPhase_ = host_->phase();
}

void Sim::dropGoFrames(uint8_t id, uint8_t count) {
  if (id >= 1 && id <= SIM_STICKS) goDrop_[id] = count;
}

void Sim::setPlayer(uint8_t id, uint8_t kind, uint32_t reactMs, uint32_t shakeUs) {
  if (id < 1 || id > SIM_STICKS) return;
  player[id].kind = kind;
  player[id].reactMs = reactMs;
  player[id].shakeUs = shakeUs;
}

void Sim::setStickFw(uint8_t id, uint8_t ver, uint8_t patch) {
  if (id < 1 || id > SIM_STICKS) return;
  fwVer_[id] = ver;
  fwPatch_[id] = patch;
}

void Sim::setStickImage(bool present, uint8_t ver, uint8_t patch) {
  imagePresent_ = present;
  imageVer_ = ver;
  imagePatch_ = patch;
}

void Sim::setOtaComesBack(uint8_t id, bool comesBack) {
  if (id >= 1 && id <= SIM_STICKS) otaComesBack_[id] = comesBack;
}

void Sim::setStickAlive(uint8_t id, bool alive) {
  if (id >= 1 && id <= SIM_STICKS) alive_[id] = alive;
}

void Sim::rebootStick(uint8_t id) {
  if (id < 1 || id > SIM_STICKS) return;
  delete sticks_[id - 1];
  sticks_[id - 1] = new StickLogic(*stickPorts_[id - 1], id);
  buttonDown_[id] = false;
  goRecorded_[id - 1] = false;
  reactScheduled_[id - 1] = false;
  earlyScheduled_[id - 1] = false;
}

bool Sim::allSticksIdle() const {
  for (uint32_t i = 0; i < SIM_STICKS; i++) {
    if (!alive_[i + 1]) continue;
    if (sticks_[i]->state() != JS_IDLE) return false;
    if (sticks_[i]->assignedSlot() != 0) return false;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* audio                                                               */
/* ------------------------------------------------------------------ */

void Sim::audioPlay(RtdSound s) {
  if (s <= SND_NONE || s >= SND_COUNT) return;
  sounds_[s]++;
  uint8_t next = (uint8_t)((audioTail_ + 1) % 16);
  if (next == audioHead_) return;
  audioQ_[audioTail_] = s;
  audioTail_ = next;
}

void Sim::audioStop() {
  audioHead_ = 0;
  audioTail_ = 0;
  audioPlaying_ = false;
  audioEndUs_ = 0;
  audioLastEndUs_ = 0;
  audioHasPlayed_ = false;
}

bool Sim::audioIdle() const {
  return !audioPlaying_ && audioHead_ == audioTail_;
}

void Sim::updateAudio() {
  if (audioPlaying_) {
    if (nowUs_ < audioEndUs_) return;
    audioPlaying_ = false;
    audioLastEndUs_ = nowUs_;
    audioHasPlayed_ = true;
  }
  if (audioHead_ == audioTail_) return;
  if (audioHasPlayed_ && (uint32_t)(nowUs_ - audioLastEndUs_) < AUDIO_GAP_US) return;
  RtdSound s = audioQ_[audioHead_];
  audioHead_ = (uint8_t)((audioHead_ + 1) % 16);
  audioPlaying_ = true;
  audioEndUs_ = nowUs_ + soundDurationUs(s);
}

/* ------------------------------------------------------------------ */
/* accelerometer                                                       */
/* ------------------------------------------------------------------ */

bool Sim::readAccel(uint8_t id, int16_t& ax, int16_t& ay, int16_t& az) {
  ax = 0;
  ay = 0;
  az = 16384;
  uint32_t period = player[id].shakeUs;
  if (period < 20) return true;
  uint64_t t = nowUs_ - shakeStartUs_[id - 1];
  if ((t % period) < (period / 2u)) {
    ax = 30000;
    ay = 30000;
    az = 30000;
  }
  return true;
}

/* ------------------------------------------------------------------ */
/* radio                                                               */
/* ------------------------------------------------------------------ */

void Sim::pushFrame(uint8_t to, const uint8_t* data, uint8_t len, uint64_t atUs) {
  if (frameCount_ >= SIM_MAX_FRAMES) return;
  Frame& f = frames_[frameCount_++];
  f.atUs = atUs;
  f.to = to;
  f.len = len;
  if (len > STATE_SIZE) len = STATE_SIZE;
  memcpy(f.data, data, len);
}

void Sim::hostSend(uint8_t dest, const uint8_t* data, uint8_t len) {
  if (len == PACKET_SIZE) {
    hostSent_[data[3]]++;
    if (data[3] == CMD_GO && dest == ID_BROADCAST) {
      hostGoT0Us_ = nowUs_;
      if (dropBroadcastGo_) {
        dropBroadcastGo_ = false;
        broadcastGoDropped_ = true;
        return;
      }
    }
    if (data[3] == CMD_IDLE && dest == ID_BROADCAST && dropBroadcastIdle_) {
      dropBroadcastIdle_ = false;
      return;
    }
  }

  uint32_t delayMs = jitterMs_ ? randn(jitterMs_ + 1) : 0;

  bool isGo = (len == PACKET_SIZE && data[3] == CMD_GO);

  if (dest == ID_BROADCAST) {
    for (uint32_t i = 0; i < SIM_STICKS; i++) {
      if (!alive_[i + 1]) continue;
      if (isGo && goDrop_[i + 1] > 0) {
        goDrop_[i + 1]--;
        continue;
      }
      if (maybeLose(hostToStickLoss_[i])) continue;
      pushFrame((uint8_t)(i + 1), data, len, nowUs_ + (uint64_t)delayMs * 1000u);
      if (dupPercent_ && maybeLose(dupPercent_)) {
        pushFrame((uint8_t)(i + 1), data, len,
                  nowUs_ + (uint64_t)(delayMs + 1 + randn(3)) * 1000u);
      }
    }
    return;
  }

  if (dest == ID_DISPLAY) {
    if (maybeLose(displayLoss_)) return;
    pushFrame(ID_DISPLAY, data, len, nowUs_ + (uint64_t)delayMs * 1000u);
    return;
  }

  if (dest >= ID_STICK1 && dest <= ID_STICK4) {
    if (!alive_[dest]) return;
    if (isGo && goDrop_[dest] > 0) {
      goDrop_[dest]--;
      return;
    }
    if (maybeLose(hostToStickLoss_[dest - 1])) return;
    pushFrame(dest, data, len, nowUs_ + (uint64_t)delayMs * 1000u);
    if (dupPercent_ && maybeLose(dupPercent_)) {
      pushFrame(dest, data, len, nowUs_ + (uint64_t)(delayMs + 1 + randn(3)) * 1000u);
    }
  }
}

void Sim::stickSend(uint8_t id, const uint8_t* data, uint8_t len) {
  if (!alive_[id]) return;
  uint8_t buf[STATE_SIZE];
  if (len > STATE_SIZE) len = STATE_SIZE;
  memcpy(buf, data, len);

  if (len == PACKET_SIZE && (buf[3] == CMD_HEARTBEAT || buf[3] == CMD_REQ_ID)) {
    /* the simulated firmware version, which changes after an OTA */
    buf[4] = fwVer_[id];
    buf[5] = fwPatch_[id];
    buf[6] = calcCRC8(buf, PACKET_SIZE - 1);
  }

  if (maybeLose(stickToHostLoss_[id - 1])) return;
  pushFrame(ID_HOST, buf, len, nowUs_);
  if (dupPercent_ && maybeLose(dupPercent_)) {
    pushFrame(ID_HOST, buf, len, nowUs_ + (uint64_t)(1 + randn(3)) * 1000u);
  }
}

void Sim::displaySend(const uint8_t* data, uint8_t len) {
  if (len == PACKET_SIZE) displaySent_[data[3]]++;
  if (maybeLose(displayLoss_)) return;
  pushFrame(ID_HOST, data, len, nowUs_);
}

void Sim::deliverFrame(const Frame& f) {
  if (f.to == ID_HOST) {
    if (f.len == PACKET_SIZE) hostRecv_[f.data[3]]++;
    host_->onPacket(f.data, f.len, (uint32_t)f.atUs);
    return;
  }
  if (f.to == ID_DISPLAY) {
    display_->onPacket(f.data, f.len, (uint32_t)(f.atUs / 1000u));
    return;
  }
  uint8_t id = f.to;
  if (!alive_[id]) return;

  if (f.len == PACKET_SIZE && f.data[3] == CMD_GO && !goRecorded_[id - 1] &&
      sticks_[id - 1]->state() == JS_WAITING_GO) {
    uint16_t age = (uint16_t)(((uint16_t)f.data[4] << 8) | f.data[5]);
    stickGoUs_[id - 1] = f.atUs - (uint64_t)age * 1000u;
    /* the scripted shake is physical: it runs from the real GO, not from the frame arrival */
    shakeStartUs_[id - 1] = stickGoUs_[id - 1];
    goRecorded_[id - 1] = true;
  }

  if (f.len == PACKET_SIZE && f.data[3] == CMD_OTA_BEGIN) {
    if (otaBeginCount_ < SIM_STICKS &&
        (otaBeginCount_ == 0 || otaBeginOrder_[otaBeginCount_ - 1] != id)) {
      otaBeginOrder_[otaBeginCount_++] = id;
    }
    uint8_t ver = f.data[4];
    uint8_t patch = f.data[5];
    if (ver != fwVer_[id] || patch != fwPatch_[id]) {
      /* the simulated stick runs another firmware: ACK, report starting, reboot into it */
      GamePacket ack;
      buildPacket(&ack, ID_HOST, id, CMD_ACK, CMD_OTA_BEGIN);
      pushFrame(ID_HOST, (const uint8_t*)&ack, PACKET_SIZE, nowUs_);
      buildPacket(&ack, ID_HOST, id, CMD_OTA_STATUS, (uint16_t)(OTA_ST_STARTING << 8));
      pushFrame(ID_HOST, (const uint8_t*)&ack, PACKET_SIZE, nowUs_);
      otaStart(id, ver, patch);
      return;
    }
  }

  sticks_[id - 1]->onPacket(f.data, f.len, (uint32_t)f.atUs);
}

void Sim::deliverDue() {
  for (;;) {
    int best = -1;
    for (uint8_t i = 0; i < frameCount_; i++) {
      if (frames_[i].atUs > nowUs_) continue;
      if (best < 0 || frames_[i].atUs < frames_[best].atUs) best = (int)i;
    }
    if (best < 0) break;
    Frame f = frames_[best];
    frames_[best] = frames_[--frameCount_];
    deliverFrame(f);
  }
}

/* ------------------------------------------------------------------ */
/* buttons                                                             */
/* ------------------------------------------------------------------ */

void Sim::schedulePress(uint8_t id, uint64_t atUs, uint32_t holdMs) {
  if (buttonCount_ >= SIM_MAX_BUTTONS) return;
  buttons_[buttonCount_].atUs = atUs;
  buttons_[buttonCount_].id = id;
  buttons_[buttonCount_].down = true;
  buttonCount_++;
  if (holdMs > 0) scheduleRelease(id, atUs + (uint64_t)holdMs * 1000u);
}

void Sim::scheduleRelease(uint8_t id, uint64_t atUs) {
  if (buttonCount_ >= SIM_MAX_BUTTONS) return;
  buttons_[buttonCount_].atUs = atUs;
  buttons_[buttonCount_].id = id;
  buttons_[buttonCount_].down = false;
  buttonCount_++;
}

void Sim::press(uint8_t id, uint32_t delayMs, uint32_t holdMs) {
  schedulePress(id, nowUs_ + (uint64_t)delayMs * 1000u, holdMs);
}

void Sim::pressAtUs(uint8_t id, uint64_t atUs, uint32_t holdMs) {
  schedulePress(id, atUs, holdMs);
}

void Sim::holdButton(uint8_t id, bool down) {
  if (id < 1 || id > SIM_STICKS) return;
  if (buttonCount_ >= SIM_MAX_BUTTONS) return;
  buttons_[buttonCount_].atUs = nowUs_;
  buttons_[buttonCount_].id = id;
  buttons_[buttonCount_].down = down;
  buttonCount_++;
}

void Sim::processButtons() {
  for (;;) {
    int best = -1;
    for (uint8_t i = 0; i < buttonCount_; i++) {
      if (buttons_[i].atUs > nowUs_) continue;
      if (best < 0 || buttons_[i].atUs < buttons_[best].atUs) best = (int)i;
    }
    if (best < 0) break;
    Button b = buttons_[best];
    buttons_[best] = buttons_[--buttonCount_];

    bool prev = buttonDown_[b.id];
    buttonDown_[b.id] = b.down;
    if (b.down && !prev) sticks_[b.id - 1]->onButtonFall((uint32_t)b.atUs);
  }
}

/* ------------------------------------------------------------------ */
/* OTA                                                                 */
/* ------------------------------------------------------------------ */

void Sim::otaStart(uint8_t id, uint8_t ver, uint8_t patch) {
  alive_[id] = false;
  if (!otaComesBack_[id]) {
    otaRebootPending_[id] = false;
    return;
  }
  otaRebootPending_[id] = true;
  otaRebootAt_[id] = nowUs_ + 2000000u;
  otaNewVer_[id] = ver;
  otaNewPatch_[id] = patch;
}

/* ------------------------------------------------------------------ */
/* main loop                                                           */
/* ------------------------------------------------------------------ */

void Sim::updateScripts() {
  uint32_t eta = host_->msUntilGo();

  for (uint8_t id = 1; id <= SIM_STICKS; id++) {
    PlayerScript& p = player[id];
    if (p.kind == PL_NONE) continue;

    if ((p.kind == PL_EARLY || p.kind == PL_HOLD) && !earlyScheduled_[id - 1]) {
      if (eta != 0 && eta <= p.earlyMs) {
        uint64_t t = nowUs_ + (uint64_t)(p.earlyMs - eta) * 1000u;
        schedulePress(id, t, p.kind == PL_HOLD ? 0u : 100u);
        earlyScheduled_[id - 1] = true;
      }
    }

    if (goRecorded_[id - 1] && !reactScheduled_[id - 1]) {
      reactScheduled_[id - 1] = true;
      if (p.kind == PL_NORMAL) {
        schedulePress(id, stickGoUs_[id - 1] + (uint64_t)p.reactMs * 1000u, 100);
      } else if (p.kind == PL_BLANK) {
        schedulePress(id, stickGoUs_[id - 1] + 20000u, 0);
      } else if (p.kind == PL_EARLY && p.secondMs != 0) {
        schedulePress(id, stickGoUs_[id - 1] + (uint64_t)p.secondMs * 1000u, 100);
      }
    }
  }
}

void Sim::step() {
  nowUs_ += 1000;

  updateAudio();
  deliverDue();
  processButtons();

  for (uint32_t i = 0; i < SIM_STICKS; i++) {
    if (alive_[i + 1]) sticks_[i]->tick();
  }
  host_->tick();
  display_->tick();

  for (uint8_t id = 1; id <= SIM_STICKS; id++) {
    if (otaRebootPending_[id] && nowUs_ >= otaRebootAt_[id]) {
      otaRebootPending_[id] = false;
      fwVer_[id] = otaNewVer_[id];
      fwPatch_[id] = otaNewPatch_[id];
      rebootStick(id);
      alive_[id] = true;
    }
  }

  updateScripts();

  uint8_t phase = host_->phase();
  if (phase != lastPhase_) {
    if (phase == PH_ANNOUNCE) {
      for (uint32_t i = 0; i < SIM_STICKS; i++) {
        goRecorded_[i] = false;
        reactScheduled_[i] = false;
        earlyScheduled_[i] = false;
        stickGoUs_[i] = 0;
      }
    }
    lastPhase_ = phase;
  }
}

void Sim::run(uint32_t ms) {
  for (uint32_t i = 0; i < ms; i++) step();
}

bool Sim::runUntilPhase(uint8_t phase, uint32_t maxMs) {
  for (uint32_t i = 0; i < maxMs; i++) {
    if (host_->phase() == phase) return true;
    step();
  }
  return host_->phase() == phase;
}

bool Sim::runUntilIdle(uint32_t maxMs, uint32_t stableMs) {
  uint32_t stable = 0;
  for (uint32_t i = 0; i < maxMs; i++) {
    step();
    if (host_->phase() == PH_IDLE && allSticksIdle()) {
      stable++;
      if (stable >= stableMs) return true;
    } else {
      stable = 0;
    }
  }
  return false;
}
