#include "HostGame.h"

#include <string.h>

/* Baseline ring mapping: player order left to right is P1=ring4, P2=ring3, P3=ring1, P4=ring0. */
static const uint8_t PLAYER_TO_RING[RTD_MAX_PLAYERS] = {4, 3, 1, 0};

static uint8_t playerToRing(uint8_t player) {
  return PLAYER_TO_RING[player & 0x03];
}

static uint32_t stickColor(uint8_t stickId) {
  switch (stickId) {
    case ID_STICK1: return RTD_RGB_WHITE;
    case ID_STICK2: return RTD_RGB_BLUE;
    case ID_STICK3: return RTD_RGB_RED;
    case ID_STICK4: return RTD_RGB_YELLOW;
    default:        return RTD_RGB_GREEN;
  }
}

static RtdSound playerSound(uint8_t slot) {
  switch (slot) {
    case 1: return SND_PLAYER_1;
    case 2: return SND_PLAYER_2;
    case 3: return SND_PLAYER_3;
    default: return SND_PLAYER_4;
  }
}

static RtdSound numberSound(uint8_t number) {
  switch (number) {
    case 1:  return SND_NUM_1;
    case 2:  return SND_NUM_2;
    case 3:  return SND_NUM_3;
    case 10: return SND_NUM_10;
    case 15: return SND_NUM_15;
    case 20: return SND_NUM_20;
    default: return SND_NONE;
  }
}

HostGame::HostGame(HostPorts& ports)
    : ports_(ports),
      bestSessionMs_(TIME_NONE),
      bestAlltimeMs_(TIME_NONE),
      gamesTotal_(0),
      newRecord_(false),
      phase_(PH_IDLE),
      phaseStart_(0),
      nowMs_(0),
      round_(0),
      totalRounds_(TOTAL_ROUNDS),
      gameModeCfg_(0),
      scoringCfg_(RTD_CFG_DEF[CFG_SCORING]),
      bonusCfg_(RTD_CFG_DEF[CFG_BONUS]),
      mode_(MODE_NONE),
      countdown_(0),
      shakeTarget_(0),
      promptSlot_(0),
      winner_(0),
      flags_(0),
      bonusRound_(false),
      announceBaseMs_(0),
      announceFirst_(false),
      reactionInstructPlayed_(false),
      shakeInstructPlayed_(false),
      delayIdx_(0),
      targetIdx_(0),
      lastDelayIdx_(0xFF),
      lastTargetIdx_(0xFF),
      modeBagIdx_(0),
      reactDelayMs_(0),
      waitRng_(0),
      promptTimerStart_(0),
      soloTimeouts_(0),
      starting_(false),
      startShownAt_(0),
      goMicros_(0),
      goMs_(0),
      shakeStartMs_(0),
      exitCause_(0),
      exitSrc_(0),
      rxSrc_(0),
      idleRepeatLeft_(0),
      idleRepeatAt_(0),
      deuce_(false),
      deuceCount_(0),
      consecutiveTimeouts_(0),
      scoresShown_(false),
      scoresAt_(0),
      idlePromptAt_(0),
      beaconAt_(0),
      lastSnapAt_(0),
      phaseSnapAt_(0),
      phaseSnapPending_(false),
      seq_(0),
      stateChanged_(false),
      flashUntilMs_(0),
      otaState_(OTA_NONE),
      otaTarget_(0),
      otaProgress_(0),
      otaListCount_(0),
      otaIndex_(0),
      otaStartMs_(0),
      otaFailed_(false),
      imageValid_(false),
      imageVer_(0),
      imagePatch_(0),
      imageCheckAt_(0) {
  memset(cfg_, 0, sizeof(cfg_));
  memset(slotStick_, 0, sizeof(slotStick_));
  memset(score_, 0, sizeof(score_));
  memset(status_, 0, sizeof(status_));
  memset(timeMs_, 0, sizeof(timeMs_));
  memset(progress_, 0, sizeof(progress_));
  memset(lastSeenMs_, 0, sizeof(lastSeenMs_));
  memset(stickVer_, 0, sizeof(stickVer_));
  memset(stickPatch_, 0, sizeof(stickPatch_));
  memset(deuceSlots_, 0, sizeof(deuceSlots_));
  memset(&pending_, 0, sizeof(pending_));
  memset(&snap_, 0, sizeof(snap_));
  memset(&lastSnap_, 0, sizeof(lastSnap_));
  memset(&leds_, 0, sizeof(leds_));
  memset(otaList_, 0, sizeof(otaList_));
  memset(otaStatus_, 0, sizeof(otaStatus_));
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    timeMs_[i] = TIME_NONE;
    leds_.ringColor[playerToRing(i)] = RTD_RGB_OFF;
  }
  for (uint8_t k = 0; k < CFG_SLOTS; k++) cfg_[k] = RTD_CFG_DEF[k];
}

void HostGame::begin(const uint8_t* cfg, uint16_t bestAlltimeMs, uint16_t gamesTotal) {
  for (uint8_t k = 1; k <= CFG_COUNT; k++) {
    cfg_[k] = cfgClamp(k, cfg ? cfg[k] : RTD_CFG_DEF[k]);
  }
  cfg_[0] = 0;
  for (uint8_t k = (uint8_t)(CFG_COUNT + 1); k < CFG_SLOTS; k++) cfg_[k] = 0;

  nowMs_ = ports_.millis();
  bestAlltimeMs_ = bestAlltimeMs;
  bestSessionMs_ = TIME_NONE;
  gamesTotal_ = gamesTotal;
  newRecord_ = false;

  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    lastSeenMs_[i] = nowMs_ - STICK_PRESENCE_MS - 1;
    stickVer_[i] = 0;
    stickPatch_[i] = 0;
  }
  seq_ = 0;
  memset(&lastSnap_, 0, sizeof(lastSnap_));
  lastSnapAt_ = nowMs_ - STATE_MAX_INTERVAL - 1;
  phaseSnapPending_ = false;
  beaconAt_ = nowMs_ - BEACON_INTERVAL - 1;
  idleRepeatLeft_ = 0;

  phase_ = PH_IDLE;
  enterPhase(PH_IDLE);
  ports_.applySettings(cfg_);
  buildSnapshot();
}

/* ============================ small helpers ============================ */

uint8_t HostGame::lowestFreeSlot() const {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] == 0) return (uint8_t)(i + 1);
  }
  return 0xFF;
}

uint8_t HostGame::joinedCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0) n++;
  }
  return n;
}

uint8_t HostGame::joinedMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0) mask |= (uint8_t)(1u << i);
  }
  return mask;
}

uint8_t HostGame::activeMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i)) mask |= (uint8_t)(1u << i);
  }
  return mask;
}

uint8_t HostGame::joinedStickMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] >= ID_STICK1 && slotStick_[i] <= ID_STICK4) {
      mask |= (uint8_t)(1u << (slotStick_[i] - ID_STICK1));
    }
  }
  return mask;
}

uint8_t HostGame::activeStickMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i) && slotStick_[i] >= ID_STICK1 && slotStick_[i] <= ID_STICK4) {
      mask |= (uint8_t)(1u << (slotStick_[i] - ID_STICK1));
    }
  }
  return mask;
}

uint8_t HostGame::onlineMask() const {
  uint8_t mask = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if ((uint32_t)(nowMs_ - lastSeenMs_[i]) <= STICK_PRESENCE_MS) {
      mask |= (uint8_t)(1u << i);
    }
  }
  return mask;
}

bool HostGame::isActiveSlot(uint8_t slot) const {
  if (slot >= RTD_MAX_PLAYERS) return false;
  if (slotStick_[slot] == 0) return false;
  if (deuce_) {
    for (uint8_t i = 0; i < deuceCount_; i++) {
      if (deuceSlots_[i] == slot) return true;
    }
    return false;
  }
  return true;
}

bool HostGame::allActiveDone() const {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i) && status_[i] == PS_PLAYING) return false;
  }
  return true;
}

bool HostGame::isBonusRound() const {
  if (deuce_) return false;
  if (bonusCfg_ == BONUS_LAST) {
    return totalRounds_ >= 2 && round_ == totalRounds_;
  }
  if (bonusCfg_ == BONUS_LAST_TWO) {
    if (totalRounds_ >= 3) return (uint8_t)(round_ + 1) >= totalRounds_;
    if (totalRounds_ == 2) return round_ == 2;
  }
  return false;
}

uint8_t HostGame::findRoundWinner() const {
  uint8_t best = 0;
  uint16_t bestTime = TIME_NONE;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i) && status_[i] == PS_DONE && timeMs_[i] < bestTime) {
      bestTime = timeMs_[i];
      best = (uint8_t)(i + 1);
    }
  }
  return best;
}

uint8_t HostGame::findFinalWinner() const {
  uint8_t best = 0;
  uint8_t bestScore = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0 && score_[i] > bestScore) {
      bestScore = score_[i];
      best = (uint8_t)(i + 1);
    }
  }
  return best;
}

uint8_t HostGame::topScore() const {
  uint8_t top = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0 && score_[i] > top) top = score_[i];
  }
  return top;
}

uint8_t HostGame::topScoreCount() const {
  uint8_t top = topScore();
  uint8_t n = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0 && score_[i] == top) n++;
  }
  return n;
}

void HostGame::ringForSlot(uint8_t slot, uint32_t color, bool blink) {
  uint8_t ring = playerToRing(slot);
  leds_.ringColor[ring] = color;
  leds_.ringBlink[ring] = blink;
}

uint32_t HostGame::msUntilGo() const {
  uint32_t elapsed = (uint32_t)(nowMs_ - phaseStart_);
  if (phase_ == PH_REACT_WAIT) {
    if (elapsed >= reactDelayMs_) return 0;
    return reactDelayMs_ - elapsed;
  }
  if (phase_ == PH_COUNTDOWN) {
    uint32_t total = 3000u;
    if (elapsed >= total) return 0;
    return total - elapsed;
  }
  return 0;
}

/* ============================ sending ============================ */

void HostGame::sendCmd(uint8_t destId, uint8_t cmd, uint16_t data) {
  GamePacket pkt;
  buildPacket(&pkt, destId, ID_HOST, cmd, data);
  ports_.send(destId, (const uint8_t*)&pkt, PACKET_SIZE);
}

void HostGame::sendWithRetry(uint8_t destId, uint8_t cmd, uint16_t data) {
  if (destId < ID_STICK1 || destId > ID_STICK4) {
    sendCmd(destId, cmd, data);
    return;
  }
  uint8_t idx = (uint8_t)(destId - ID_STICK1);
  PendingCmd& pa = pending_[idx];
  pa.active = true;
  pa.cmd = cmd;
  pa.data = data;
  pa.retries = ACK_MAX_RETRIES;
  pa.lastTx = nowMs_;
  sendCmd(destId, cmd, data);
}

void HostGame::sendGoFrames(uint32_t now) {
  (void)now;
  sendCmd(ID_BROADCAST, CMD_GO, 0);
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i)) sendWithRetry(slotStick_[i], CMD_GO, 0);
  }
}

void HostGame::broadcastIdle() {
  sendCmd(ID_BROADCAST, CMD_IDLE, 0);
}

void HostGame::updateRetries(uint32_t now) {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    PendingCmd& pa = pending_[i];
    if (!pa.active) continue;
    if ((uint32_t)(now - pa.lastTx) < ACK_RETRY_INTERVAL) continue;
    if (pa.retries == 0) {
      pa.active = false;
      continue;
    }
    pa.retries--;
    pa.lastTx = now;
    uint16_t data = pa.data;
    if (pa.cmd == CMD_GO) {
      uint32_t age = (uint32_t)(ports_.micros() - goMicros_) / 1000u;
      data = (uint16_t)(age > 0xFFFFu ? 0xFFFFu : age);
    }
    sendCmd((uint8_t)(ID_STICK1 + i), pa.cmd, data);
  }
}

/* ============================ phases ============================ */

void HostGame::enterPhase(uint8_t phase) {
  phase_ = phase;
  phaseStart_ = nowMs_;

  switch (phase) {
    case PH_IDLE:
      if (idleRepeatLeft_ == 0) broadcastIdle();
      resetGame();
      leds_.neoMode = NEO_IDLE_RAINBOW;
      for (uint8_t r = 0; r < 5; r++) {
        leds_.ringColor[r] = RTD_RGB_OFF;
        leds_.ringBlink[r] = false;
      }
      if (cfg_[CFG_VOICE_GUIDE]) ports_.audioPlay(SND_PRESS_TO_JOIN);
      idlePromptAt_ = nowMs_ + IDLE_PROMPT_INTERVAL;
      break;

    case PH_JOIN:
      promptSlot_ = 0;
      promptTimerStart_ = nowMs_;
      soloTimeouts_ = 0;
      starting_ = false;
      ports_.audioStop();
      leds_.neoMode = NEO_BLINK_SLOT;
      break;

    case PH_FINAL:
      bonusRound_ = false;
      winner_ = findFinalWinner();
      gamesTotal_++;
      ports_.saveStats(bestAlltimeMs_, gamesTotal_);
      leds_.neoMode = NEO_IDLE_RAINBOW;
      for (uint8_t r = 0; r < 5; r++) {
        leds_.ringColor[r] = RTD_RGB_OFF;
        leds_.ringBlink[r] = false;
      }
      if (winner_ != 0) {
        ports_.audioPlay(playerSound(winner_));
        ports_.audioPlay(SND_WINS);
        ports_.audioPlay(SND_VICTORY_FANFARE);
      }
      ports_.audioPlay(SND_GAME_OVER);
      break;

    case PH_MAINT:
      otaState_ = OTA_READY;
      otaTarget_ = 0;
      otaProgress_ = 0;
      break;

    case PH_RESULTS:
      /* results show the times first, then the winner and the scores (DESIGN 7.4) */
      scoresAt_ = nowMs_ + RESULTS_SCORES_MS;
      break;

    default:
      break;
  }
}

void HostGame::resetGame() {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    slotStick_[i] = 0;
    score_[i] = 0;
    status_[i] = PS_NONE;
    timeMs_[i] = TIME_NONE;
    progress_[i] = 0;
    leds_.ringColor[playerToRing(i)] = RTD_RGB_OFF;
    leds_.ringBlink[playerToRing(i)] = false;
  }
  deuce_ = false;
  deuceCount_ = 0;
  memset(deuceSlots_, 0, sizeof(deuceSlots_));
  consecutiveTimeouts_ = 0;
  round_ = 0;
  totalRounds_ = cfg_[CFG_ROUNDS];
  gameModeCfg_ = cfg_[CFG_GAME_MODE];
  scoringCfg_ = cfg_[CFG_SCORING];
  bonusCfg_ = cfg_[CFG_BONUS];
  bonusRound_ = false;
  mode_ = MODE_NONE;
  winner_ = 0;
  flags_ = 0;
  promptSlot_ = 0;
  modeBagIdx_ = 0;
  lastDelayIdx_ = 0xFF;
  lastTargetIdx_ = 0xFF;
  reactionInstructPlayed_ = false;
  shakeInstructPlayed_ = false;
  scoresShown_ = false;
  newRecord_ = false;
  countdown_ = 0;
  shakeTarget_ = 0;
  otaState_ = OTA_NONE;
  otaTarget_ = 0;
  otaProgress_ = 0;
  memset(pending_, 0, sizeof(pending_));
}

/* ============================ join ============================ */

void HostGame::claimSlot(uint8_t stickId) {
  if (stickId < ID_STICK1 || stickId > ID_STICK4) return;
  if (phase_ == PH_MAINT) return;

  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] == stickId) {
      sendCmd(stickId, CMD_OK, (uint16_t)(i + 1));
      return;
    }
  }

  if (phase_ != PH_IDLE && phase_ != PH_JOIN) {
    sendCmd(stickId, CMD_IDLE, 0);
    return;
  }

  uint8_t slot = lowestFreeSlot();
  if (slot == 0xFF) return;
  uint8_t idx = (uint8_t)(slot - 1);

  slotStick_[idx] = stickId;
  score_[idx] = 0;
  status_[idx] = PS_NONE;
  timeMs_[idx] = TIME_NONE;
  progress_[idx] = 0;
  ringForSlot(idx, stickColor(stickId), false);

  sendCmd(stickId, CMD_OK, (uint16_t)slot);
  promptTimerStart_ = nowMs_;

  if (phase_ == PH_IDLE) {
    enterPhase(PH_JOIN);
  }
}

void HostGame::startGame() {
  starting_ = true;
  startShownAt_ = nowMs_;
  totalRounds_ = cfg_[CFG_ROUNDS];
  gameModeCfg_ = cfg_[CFG_GAME_MODE];
  scoringCfg_ = cfg_[CFG_SCORING];
  bonusCfg_ = cfg_[CFG_BONUS];
  modeBagIdx_ = 0;
  reactionInstructPlayed_ = false;
  shakeInstructPlayed_ = false;
  ports_.audioStop();
  leds_.neoMode = NEO_STATUS;
}

void HostGame::startRound() {
  round_++;
  bonusRound_ = isBonusRound();
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    status_[i] = isActiveSlot(i) ? PS_PLAYING : PS_NONE;
    timeMs_[i] = TIME_NONE;
    progress_[i] = 0;
    ringForSlot(i, slotStick_[i] ? stickColor(slotStick_[i]) : RTD_RGB_OFF, false);
  }
  winner_ = 0;
  scoresShown_ = false;
  newRecord_ = false;
  flags_ &= (uint8_t)~(STF_SCORES | STF_NEW_RECORD);

  if (gameModeCfg_ == 1) {
    mode_ = MODE_REACTION;
  } else if (gameModeCfg_ == 2) {
    mode_ = MODE_SHAKE;
  } else {
    mode_ = (modeBagIdx_ % 2 == 0) ? MODE_REACTION : MODE_SHAKE;
    modeBagIdx_++;
  }

  bool voice = cfg_[CFG_VOICE_GUIDE] != 0;
  uint8_t param = 0;

  if (mode_ == MODE_REACTION) {
    do {
      delayIdx_ = (uint8_t)ports_.random(NUM_REACT_DELAYS);
    } while (delayIdx_ == lastDelayIdx_ && NUM_REACT_DELAYS > 1);
    lastDelayIdx_ = delayIdx_;
    reactDelayMs_ = REACT_DELAYS[delayIdx_];
    shakeTarget_ = 0;

    announceFirst_ = voice && !reactionInstructPlayed_;
    ports_.audioPlay(SND_REACTION_MODE);
    if (announceFirst_) {
      ports_.audioPlay(SND_REACTION_INSTRUCT);
      reactionInstructPlayed_ = true;
    }
    announceBaseMs_ = announceFirst_ ? REACT_ANNOUNCE_DELAY_FIRST : REACT_ANNOUNCE_DELAY;
    leds_.neoMode = NEO_RANDOM_FAST;
  } else {
    do {
      targetIdx_ = (uint8_t)ports_.random(NUM_SHAKE_TARGETS);
    } while (targetIdx_ == lastTargetIdx_ && NUM_SHAKE_TARGETS > 1);
    lastTargetIdx_ = targetIdx_;
    shakeTarget_ = SHAKE_TARGETS[targetIdx_];
    param = shakeTarget_;

    announceFirst_ = voice && !shakeInstructPlayed_;
    ports_.audioPlay(SND_SHAKE_IT);
    if (announceFirst_) {
      ports_.audioPlay(SND_YOU_WILL_SHAKE);
      shakeInstructPlayed_ = true;
    }
    ports_.audioPlay(numberSound(shakeTarget_));
    announceBaseMs_ = announceFirst_ ? SHAKE_ANNOUNCE_DELAY_FIRST : SHAKE_ANNOUNCE_DELAY;
    leds_.neoMode = NEO_COUNTDOWN;
  }

  uint16_t gameStart = (uint16_t)(((uint16_t)mode_ << 8) | param);
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i)) sendWithRetry(slotStick_[i], CMD_GAME_START, gameStart);
  }

  enterPhase(PH_ANNOUNCE);
}

/* ============================ GO and collect ============================ */

/* Random stage lengths of the reaction wait: random weights scaled to the free time on top of
 * the minimum, the rounding rest goes to the last stage. Own xorshift32, mixed with micros(). */
void HostGame::planReactWait() {
  waitRng_ ^= ports_.micros() * 2654435761u;
  if (waitRng_ == 0) waitRng_ = 0x9E3779B9u;
  uint32_t w[REACT_WAIT_STAGES];
  uint32_t sum = 0;
  for (uint8_t k = 0; k < REACT_WAIT_STAGES; k++) {
    waitRng_ ^= waitRng_ << 13;
    waitRng_ ^= waitRng_ >> 17;
    waitRng_ ^= waitRng_ << 5;
    w[k] = 1 + (waitRng_ >> 8) % 100;
    w[k] *= w[k];   /* squared: uneven stages, a rhythm cannot be counted */
    sum += w[k];
  }
  uint32_t free = reactDelayMs_ - (uint32_t)REACT_WAIT_STAGES * REACT_WAIT_STAGE_MIN_MS;
  uint32_t end = 0;
  for (uint8_t k = 0; k < REACT_WAIT_STAGES; k++) {
    end += REACT_WAIT_STAGE_MIN_MS + free * w[k] / sum;
    waitStageEndMs_[k] = (uint16_t)end;
  }
  waitStageEndMs_[REACT_WAIT_STAGES - 1] = reactDelayMs_;
}

void HostGame::fireGo() {
  goMicros_ = ports_.micros();
  goMs_ = nowMs_;
  sendGoFrames(nowMs_);

  if (mode_ == MODE_REACTION) {
    for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
      if (isActiveSlot(i)) ringForSlot(i, RTD_RGB_YELLOW, false);
    }
    leds_.neoMode = NEO_FIXED_COLOR;
    ports_.audioStop();
    ports_.audioPlay(SND_BEEP);
    enterPhase(PH_GO);
  } else {
    shakeStartMs_ = nowMs_;
    leds_.neoMode = NEO_SHAKE_COUNTDOWN;
    ports_.audioPlay(SND_BEEP);
    enterPhase(PH_SHAKE);
  }
}

void HostGame::finishCollect() {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i) && status_[i] == PS_PLAYING) {
      status_[i] = PS_PENALTY;
      timeMs_[i] = TIME_PENALTY;
      ringForSlot(i, RTD_RGB_RED, true);
    }
  }
  leds_.neoMode = NEO_STATUS;
  enterPhase(PH_RESULTS);
}

void HostGame::finishShake() {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (isActiveSlot(i) && status_[i] == PS_PLAYING) {
      status_[i] = PS_PENALTY;
      timeMs_[i] = TIME_PENALTY;
      ringForSlot(i, RTD_RGB_RED, true);
    }
  }
  leds_.neoMode = NEO_STATUS;
  enterPhase(PH_RESULTS);
}

void HostGame::finalizeRoundRecords(uint8_t winnerSlot) {
  if (mode_ != MODE_REACTION) return;
  if (status_[winnerSlot] != PS_DONE) return;
  uint16_t t = timeMs_[winnerSlot];
  if (t == TIME_NONE) return;

  if (bestSessionMs_ == TIME_NONE || t < bestSessionMs_) bestSessionMs_ = t;

  bool alltime = (bestAlltimeMs_ == TIME_NONE || t < bestAlltimeMs_);
  if (alltime) {
    bestAlltimeMs_ = t;
    newRecord_ = true;
    ports_.saveStats(bestAlltimeMs_, gamesTotal_);
  }
}

void HostGame::showScores() {
  scoresShown_ = true;
  uint8_t winner = findRoundWinner();
  winner_ = winner;

  /* a deuce round is never a bonus round and always uses the classic rule */
  bool classic = deuce_ || scoringCfg_ == SCORING_CLASSIC;
  uint8_t factor = (bonusRound_ && !deuce_) ? 2 : 1;

  if (classic) {
    if (winner != 0) {
      uint16_t points = (uint16_t)score_[winner - 1] + factor;
      score_[winner - 1] = (points > 255) ? 255 : (uint8_t)points;
    }
  } else {
    for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
      if (!isActiveSlot(i) || status_[i] != PS_DONE) continue;
      uint8_t beaten = 0;
      for (uint8_t j = 0; j < RTD_MAX_PLAYERS; j++) {
        if (j == i || !isActiveSlot(j)) continue;
        if (status_[j] != PS_DONE || timeMs_[j] > timeMs_[i]) beaten++;
      }
      uint16_t points = (uint16_t)score_[i] + (uint16_t)beaten * factor;
      score_[i] = (points > 255) ? 255 : (uint8_t)points;
    }
  }

  if (winner != 0) {
    ports_.audioPlay(playerSound(winner));
    ports_.audioPlay(SND_FASTEST);
    finalizeRoundRecords((uint8_t)(winner - 1));
  }
  flags_ |= STF_SCORES;
}

void HostGame::enterDeuce(bool suddenDeath) {
  deuce_ = true;
  bonusRound_ = false;
  deuceCount_ = 0;
  uint8_t top = topScore();
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0 && score_[i] == top && deuceCount_ < RTD_MAX_PLAYERS) {
      deuceSlots_[deuceCount_++] = i;
    }
  }
  (void)suddenDeath;
  flags_ |= STF_DEUCE;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (slotStick_[i] != 0 && !isActiveSlot(i)) {
      sendWithRetry(slotStick_[i], CMD_IDLE, 0);
      ringForSlot(i, RTD_RGB_OFF, false);
    }
  }
}

void HostGame::afterResults() {
  bool anyWinner = (winner_ != 0);

  if (!anyWinner) {
    if (consecutiveTimeouts_ < 0xFF) consecutiveTimeouts_++;
    if (consecutiveTimeouts_ >= 2) {
      idleRepeatLeft_ = 0;
      enterPhase(PH_IDLE);
      return;
    }
  } else {
    consecutiveTimeouts_ = 0;
  }

  if (deuce_) {
    if (deuceCount_ == 2) {
      uint8_t a = deuceSlots_[0];
      uint8_t b = deuceSlots_[1];
      int diff = (int)score_[a] - (int)score_[b];
      if (diff < 0) diff = -diff;
      if (diff >= (int)cfg_[CFG_DEUCE_LEAD]) {
        enterPhase(PH_FINAL);
      } else {
        startRound();
      }
    } else {
      if (anyWinner) {
        enterPhase(PH_FINAL);
      } else {
        startRound();
      }
    }
    return;
  }

  if (round_ >= totalRounds_) {
    uint8_t count = topScoreCount();
    if (count >= 2) {
      enterDeuce(count > 2);
      startRound();
    } else {
      enterPhase(PH_FINAL);
    }
    return;
  }

  startRound();
}

/* ============================ actions ============================ */

void HostGame::actAbort() {
  ports_.audioStop();
  if (phase_ == PH_MAINT) {
    exitCause_ = 1;
    exitSrc_ = rxSrc_;
    ports_.maintExit();
  }
  sendCmd(ID_BROADCAST, CMD_IDLE, 0);
  idleRepeatLeft_ = (IDLE_REPEAT_COUNT > 1) ? (uint8_t)(IDLE_REPEAT_COUNT - 1) : 0;
  idleRepeatAt_ = nowMs_ + 30;
  enterPhase(PH_IDLE);
}

void HostGame::actStartNow() {
  if (phase_ != PH_JOIN || starting_) return;
  if (joinedCount() >= 2) startGame();
}

void HostGame::actIdentify(uint8_t stickId) {
  if (phase_ != PH_IDLE) return;
  if (stickId < ID_STICK1 || stickId > ID_STICK4) return;
  sendCmd(stickId, CMD_VIBRATE, 50);
}

void HostGame::actMaintenance(bool enter) {
  if (enter) {
    if (phase_ != PH_IDLE) return;
    ports_.maintEnter();
    enterPhase(PH_MAINT);
  } else {
    if (phase_ != PH_MAINT) return;
    /* A touch on the display must not end the update of the sticks half way. */
    if (otaState_ == OTA_STICKS) return;
    exitCause_ = 2;
    exitSrc_ = rxSrc_;
    ports_.maintExit();
    enterPhase(PH_IDLE);
  }
}

void HostGame::actOtaSticks() {
  if (!imageValid_) {
    if (ports_.stickImage(&imageVer_, &imagePatch_)) imageValid_ = true;
  }
  if (!imageValid_) return;

  /* staying in maintenance keeps maintEnter() alive; maintExit() would reboot the host */
  if (phase_ != PH_IDLE && phase_ != PH_MAINT) actAbort();
  actMaintenance(true);

  otaListCount_ = 0;
  otaFailed_ = false;
  memset(otaStatus_, 0, sizeof(otaStatus_));
  uint8_t online = onlineMask();
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (!(online & (uint8_t)(1u << i))) continue;
    if (stickVer_[i] == imageVer_ && stickPatch_[i] == imagePatch_) continue;
    otaList_[otaListCount_++] = (uint8_t)(ID_STICK1 + i);
  }

  if (otaListCount_ == 0) {
    otaState_ = OTA_DONE;
    otaTarget_ = 0;
    otaProgress_ = 100;
    return;
  }

  otaIndex_ = 0;
  otaTarget_ = otaList_[0];
  otaStartMs_ = nowMs_;
  otaProgress_ = 0;
  otaState_ = OTA_STICKS;
  sendWithRetry(otaTarget_, CMD_OTA_BEGIN,
                (uint16_t)(((uint16_t)imageVer_ << 8) | imagePatch_));
}

void HostGame::otaNext() {
  otaIndex_++;
  if (otaIndex_ >= otaListCount_) {
    otaState_ = otaFailed_ ? OTA_ERROR : OTA_DONE;
    otaTarget_ = 0;
    otaProgress_ = 100;
    return;
  }
  otaTarget_ = otaList_[otaIndex_];
  otaStartMs_ = nowMs_;
  otaProgress_ = (uint8_t)((uint16_t)otaIndex_ * 100u / otaListCount_);
  sendWithRetry(otaTarget_, CMD_OTA_BEGIN,
                (uint16_t)(((uint16_t)imageVer_ << 8) | imagePatch_));
}

void HostGame::otaTick(uint32_t now) {
  if (otaState_ != OTA_STICKS || otaTarget_ == 0) return;

  uint8_t idx = (uint8_t)(otaTarget_ - ID_STICK1);
  bool done = (stickVer_[idx] == imageVer_ && stickPatch_[idx] == imagePatch_ &&
               (onlineMask() & (uint8_t)(1u << idx)) != 0);
  if (done) {
    otaNext();
    return;
  }
  if (otaStatus_[idx] == OTA_ST_FAILED ||
      (uint32_t)(now - otaStartMs_) >= OTA_STICK_TIMEOUT_MS) {
    otaFailed_ = true;
    otaNext();
  }
}

void HostGame::applyCfg(uint8_t key, uint8_t value, bool persist) {
  if (key < 1 || key > CFG_COUNT) return;
  uint8_t v = cfgClamp(key, value);
  cfg_[key] = v;
  if (persist) ports_.saveSettings(cfg_);
  ports_.applySettings(cfg_);
}

/* ============================ packets ============================ */

void HostGame::onPacket(const uint8_t* data, int len, uint32_t rxMicros) {
  (void)rxMicros;
  if (len != PACKET_SIZE) return;
  if (!validatePacket(data, len)) return;

  GamePacket pkt;
  memcpy(&pkt, data, sizeof(pkt));
  uint16_t val = packetData(&pkt);
  uint32_t now = ports_.millis();
  nowMs_ = now;
  rxSrc_ = pkt.src_id;

  if (pkt.src_id >= ID_STICK1 && pkt.src_id <= ID_STICK4) {
    uint8_t idx = (uint8_t)(pkt.src_id - ID_STICK1);
    lastSeenMs_[idx] = now;
    if (pkt.cmd == CMD_REQ_ID || pkt.cmd == CMD_HEARTBEAT) {
      stickVer_[idx] = pkt.data_high;
      stickPatch_[idx] = pkt.data_low;
    }
  }

  switch (pkt.cmd) {
    case CMD_ACK: {
      if (pkt.src_id >= ID_STICK1 && pkt.src_id <= ID_STICK4) {
        PendingCmd& pa = pending_[pkt.src_id - ID_STICK1];
        if (pa.active && pa.cmd == pkt.data_low) pa.active = false;
      }
      break;
    }

    case CMD_REQ_ID:
      claimSlot(pkt.src_id);
      break;

    case CMD_HEARTBEAT:
      break;

    case CMD_REACTION_DONE:
    case CMD_SHAKE_DONE: {
      if (pkt.src_id < ID_STICK1 || pkt.src_id > ID_STICK4) break;
      sendCmd(pkt.src_id, CMD_ACK, pkt.cmd);

      if (phase_ != PH_GO && phase_ != PH_SHAKE) break;
      uint8_t slot = 0xFF;
      for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
        if (slotStick_[i] == pkt.src_id) {
          slot = i;
          break;
        }
      }
      if (slot == 0xFF) break;
      if (!isActiveSlot(slot)) break;
      if (status_[slot] != PS_PLAYING) break;

      if (val == TIME_PENALTY) {
        status_[slot] = PS_PENALTY;
        timeMs_[slot] = TIME_PENALTY;
        ringForSlot(slot, RTD_RGB_RED, true);
      } else {
        status_[slot] = PS_DONE;
        timeMs_[slot] = val;
        ringForSlot(slot, RTD_RGB_GREEN, false);
      }
      /* The shake animation shows results itself and keeps the progress and
       * the countdown of the players that still shake. */
      if (phase_ == PH_GO) leds_.neoMode = NEO_STATUS;
      break;
    }

    case CMD_SHAKE_PROGRESS: {
      if (phase_ != PH_SHAKE) break;
      if (pkt.src_id < ID_STICK1 || pkt.src_id > ID_STICK4) break;
      uint8_t slot = 0xFF;
      for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
        if (slotStick_[i] == pkt.src_id) {
          slot = i;
          break;
        }
      }
      if (slot == 0xFF || !isActiveSlot(slot)) break;
      uint8_t target = pkt.data_low;
      if (target == 0) break;
      uint16_t pct = (uint16_t)((uint16_t)pkt.data_high * 100u / target);
      if (pct > 100) pct = 100;
      progress_[slot] = (uint8_t)pct;
      break;
    }

    case CMD_CFG_SET:
      applyCfg(pkt.data_high, pkt.data_low, true);
      break;

    case CMD_ACTION:
      switch (pkt.data_high) {
        case ACT_ABORT_GAME:
          if (!(phase_ == PH_MAINT && otaState_ == OTA_STICKS)) actAbort();
          break;
        case ACT_TEST_SOUND:
          if (ports_.audioIdle()) ports_.audioPlay(SND_BEEP);
          break;
        case ACT_RESET_BEST:
          bestSessionMs_ = TIME_NONE;
          bestAlltimeMs_ = TIME_NONE;
          ports_.saveStats(bestAlltimeMs_, gamesTotal_);
          break;
        case ACT_FACTORY_RESET:
          for (uint8_t k = 1; k <= CFG_COUNT; k++) cfg_[k] = RTD_CFG_DEF[k];
          ports_.saveSettings(cfg_);
          ports_.applySettings(cfg_);
          break;
        case ACT_START_NOW: actStartNow(); break;
        case ACT_IDENTIFY: actIdentify(pkt.data_low); break;
        case ACT_MAINTENANCE: actMaintenance(pkt.data_low != 0); break;
        case ACT_OTA_STICKS: actOtaSticks(); break;
        default: break;
      }
      break;

    case CMD_OTA_STATUS:
      if (pkt.src_id >= ID_STICK1 && pkt.src_id <= ID_STICK4) {
        uint8_t idx = (uint8_t)(pkt.src_id - ID_STICK1);
        otaStatus_[idx] = pkt.data_high;
        if (otaState_ == OTA_STICKS && otaTarget_ == pkt.src_id) {
          uint8_t st = pkt.data_high;
          if (st == OTA_ST_FAILED) {
            otaFailed_ = true;
            otaNext();
          } else if (st == OTA_ST_UPTODATE) {
            otaNext();
          } else if (st == OTA_ST_STARTING) {
            if (otaProgress_ < 60) otaProgress_ = (uint8_t)(otaProgress_ + 20);
          }
        }
      }
      break;

    case CMD_DISPLAY_HELLO:
    case CMD_DISPLAY_INFO:
      break;

    default:
      (void)val;
      break;
  }
}

/* ============================ tick ============================ */

void HostGame::updateBeacons(uint32_t now) {
  if ((uint32_t)(now - beaconAt_) < BEACON_INTERVAL && beaconAt_ != 0) return;
  beaconAt_ = now;
  uint16_t data = (uint16_t)(((uint16_t)phase_ << 8) |
                             ((uint16_t)(activeStickMask() & 0x0F) << 4) |
                             (uint16_t)(joinedStickMask() & 0x0F));
  sendCmd(ID_BROADCAST, CMD_BEACON, data);
}

void HostGame::updateFlags() {
  flags_ &= (uint8_t)~(STF_DEUCE | STF_CAN_START | STF_UPDATE_AVAILABLE |
                       STF_NEW_RECORD | STF_SCORES | STF_BONUS);
  if (deuce_) flags_ |= STF_DEUCE;
  if (scoresShown_) flags_ |= STF_SCORES;
  if (bonusRound_) flags_ |= STF_BONUS;
  if (newRecord_) flags_ |= STF_NEW_RECORD;
  if (phase_ == PH_JOIN && !starting_ && joinedCount() >= 2) flags_ |= STF_CAN_START;
  if (imageValid_) {
    uint8_t online = onlineMask();
    for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
      if (!(online & (uint8_t)(1u << i))) continue;
      if (stickVer_[i] != imageVer_ || stickPatch_[i] != imagePatch_) {
        flags_ |= STF_UPDATE_AVAILABLE;
        break;
      }
    }
  }
}

void HostGame::buildSnapshot() {
  snap_.phase = phase_;
  snap_.flags = flags_;
  snap_.mode = mode_;
  snap_.round = round_;
  snap_.total_rounds = totalRounds_;
  snap_.countdown = (phase_ == PH_COUNTDOWN) ? countdown_ : 0;
  snap_.shake_target = (mode_ == MODE_SHAKE) ? shakeTarget_ : 0;
  snap_.prompt_slot = (phase_ == PH_JOIN) ? promptSlot_ : 0;
  snap_.winner = winner_;
  snap_.joined_mask = joinedMask();
  snap_.active_mask = activeMask();
  snap_.sticks_online = onlineMask();
  snap_.secs_left = 0;
  if (phase_ == PH_SHAKE) {
    uint32_t elapsed = (uint32_t)(nowMs_ - shakeStartMs_);
    uint32_t left = (elapsed >= TIMEOUT_SHAKE) ? 0 : (TIMEOUT_SHAKE - elapsed) / 1000u;
    snap_.secs_left = (uint8_t)left;
  }
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    snap_.players[i].stick_id = slotStick_[i];
    snap_.players[i].score = score_[i];
    snap_.players[i].status = status_[i];
    snap_.players[i].progress = progress_[i];
    snap_.players[i].time_ms = timeMs_[i];
  }
  for (uint8_t k = 0; k < CFG_SLOTS; k++) snap_.cfg[k] = cfg_[k];
  snap_.best_session_ms = bestSessionMs_;
  snap_.best_alltime_ms = bestAlltimeMs_;
  snap_.games_total = gamesTotal_;
  snap_.host_fw = FW_VERSION_BYTE;
  snap_.host_fw_patch = FW_VERSION_PATCH;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    snap_.stick_fw[i] = stickVer_[i];
    snap_.stick_fw_patch[i] = stickPatch_[i];
  }
  snap_.ota_state = otaState_;
  snap_.ota_target = otaTarget_;
  snap_.ota_progress = otaProgress_;
}

const StatePacket& HostGame::buildState() {
  buildSnapshot();
  seq_++;
  snap_.start = STATE_START;
  snap_.version = PROTO_VERSION;
  snap_.seq = seq_;
  snap_.crc = calcCRC8((const uint8_t*)&snap_, STATE_SIZE - 1);
  return snap_;
}

void HostGame::updateStateSend(uint32_t now) {
  buildSnapshot();
  const uint8_t* a = (const uint8_t*)&snap_;
  const uint8_t* b = (const uint8_t*)&lastSnap_;
  bool changed = memcmp(a + 3, b + 3, STATE_SIZE - 4) != 0;
  bool phaseChanged = snap_.phase != lastSnap_.phase;

  bool send = false;
  if (phaseChanged) {
    send = true;
    phaseSnapPending_ = true;
    phaseSnapAt_ = now + STATE_PHASE_RESEND_MS;
  } else if (changed) {
    if ((uint32_t)(now - lastSnapAt_) >= STATE_MIN_INTERVAL) send = true;
  } else if (phaseSnapPending_) {
    if ((int32_t)(now - phaseSnapAt_) >= 0) {
      send = true;
      phaseSnapPending_ = false;
    }
  }
  if (!send && (uint32_t)(now - lastSnapAt_) >= STATE_MAX_INTERVAL) send = true;

  if (!send) return;

  const StatePacket& sealed = buildState();
  ports_.send(ID_DISPLAY, (const uint8_t*)&sealed, STATE_SIZE);
  memcpy(&lastSnap_, &sealed, sizeof(lastSnap_));
  lastSnapAt_ = now;
  stateChanged_ = stateChanged_ || changed || phaseChanged;
}

void HostGame::updateLeds() {
  leds_.joinedMask = joinedMask();
  leds_.activeMask = activeMask();
  leds_.blinkSlot = (phase_ == PH_JOIN && promptSlot_ >= 1 && promptSlot_ <= RTD_MAX_PLAYERS)
                        ? (uint8_t)(promptSlot_ - 1)
                        : 0;
  leds_.flashUntilMs = flashUntilMs_;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) leds_.progress[i] = progress_[i];
  leds_.shakeStartMs = shakeStartMs_;
  leds_.waitLit = 0;
  if (phase_ == PH_REACT_WAIT) {
    uint32_t elapsed = (uint32_t)(nowMs_ - phaseStart_);
    uint8_t lit = REACT_WAIT_LEDS;
    for (uint8_t k = 0; k < REACT_WAIT_STAGES; k++) {
      if (elapsed >= waitStageEndMs_[k]) lit = (uint8_t)(lit - REACT_WAIT_LEDS / REACT_WAIT_STAGES);
    }
    leds_.waitLit = lit;
  }
  if (phase_ == PH_GO) {
    leds_.limitStartMs = goMs_;
    leds_.limitMs = TIMEOUT_REACTION;
  } else if (phase_ == PH_SHAKE) {
    leds_.limitStartMs = shakeStartMs_;
    leds_.limitMs = TIMEOUT_SHAKE;
  } else {
    leds_.limitStartMs = 0;
    leds_.limitMs = 0;
  }
}

void HostGame::tick() {
  uint32_t now = ports_.millis();
  nowMs_ = now;

  if ((uint32_t)(now - imageCheckAt_) >= 1000) {
    imageCheckAt_ = now;
    uint8_t v = 0, p = 0;
    imageValid_ = ports_.stickImage(&v, &p);
    if (imageValid_) {
      imageVer_ = v;
      imagePatch_ = p;
    }
  }

  updateRetries(now);

  if (idleRepeatLeft_ > 0) {
    if ((int32_t)(now - idleRepeatAt_) >= 0) {
      sendCmd(ID_BROADCAST, CMD_IDLE, 0);
      idleRepeatLeft_--;
      idleRepeatAt_ = now + 30;
    }
  }

  switch (phase_) {
    case PH_IDLE:
      if (cfg_[CFG_VOICE_GUIDE] && (int32_t)(now - idlePromptAt_) >= 0) {
        ports_.audioPlay(SND_PRESS_TO_JOIN);
        idlePromptAt_ = now + IDLE_PROMPT_INTERVAL;
      }
      break;

    case PH_JOIN: {
      if (starting_) {
        if ((uint32_t)(now - startShownAt_) >= JOIN_COLOURS_MS) {
          starting_ = false;
          round_ = 0;
          ports_.audioPlay(SND_GET_READY);
          startRound();
        }
        break;
      }
      if (joinedCount() >= RTD_MAX_PLAYERS) {
        startGame();
        break;
      }
      uint8_t slot = lowestFreeSlot();
      if (slot == 0xFF) {
        startGame();
        break;
      }
      if (slot != promptSlot_) {
        promptSlot_ = slot;
        ports_.audioStop();
        ports_.audioPlay(playerSound(slot));
        promptTimerStart_ = now;
      }
      uint32_t joinMs = (uint32_t)cfg_[CFG_JOIN_TIME] * 1000u;
      if ((uint32_t)(now - promptTimerStart_) >= joinMs) {
        if (joinedCount() >= 2) {
          startGame();
        } else if (joinedCount() == 1) {
          soloTimeouts_++;
          if (soloTimeouts_ >= JOIN_SOLO_TIMEOUTS) {
            idleRepeatLeft_ = 0;
            enterPhase(PH_IDLE);
          } else {
            promptTimerStart_ = now;
            ports_.audioStop();
            ports_.audioPlay(playerSound(promptSlot_));
          }
        } else {
          enterPhase(PH_IDLE);
        }
      }
      break;
    }

    case PH_ANNOUNCE: {
      uint32_t elapsed = (uint32_t)(now - phaseStart_);
      bool audioDone = ports_.audioIdle() && elapsed >= ANNOUNCE_AUDIO_MS;
      bool timeDone = elapsed >= (uint32_t)announceBaseMs_ + ANNOUNCE_SLACK_MS;
      if (!audioDone && !timeDone) break;
      if (mode_ == MODE_REACTION) {
        leds_.neoMode = NEO_REACT_WAIT;
        planReactWait();
        enterPhase(PH_REACT_WAIT);
      } else {
        countdown_ = 3;
        for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
          if (isActiveSlot(i)) sendWithRetry(slotStick_[i], CMD_COUNTDOWN, 3);
        }
        flashUntilMs_ = now + COUNTDOWN_FLASH_DURATION;
        ports_.audioPlay(SND_NUM_3);
        leds_.neoMode = NEO_COUNTDOWN;
        enterPhase(PH_COUNTDOWN);
      }
      break;
    }

    case PH_REACT_WAIT:
      if ((uint32_t)(now - phaseStart_) >= reactDelayMs_) fireGo();
      break;

    case PH_COUNTDOWN: {
      uint32_t elapsed = (uint32_t)(now - phaseStart_);
      uint8_t tickIdx = (uint8_t)(elapsed / 1000u);
      if (tickIdx <= 2) {
        uint8_t num = (uint8_t)(3 - tickIdx);
        if (num != countdown_) {
          countdown_ = num;
          for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
            if (isActiveSlot(i)) sendWithRetry(slotStick_[i], CMD_COUNTDOWN, num);
          }
          flashUntilMs_ = now + COUNTDOWN_FLASH_DURATION;
          ports_.audioPlay(numberSound(num));
          leds_.neoMode = NEO_COUNTDOWN;
        }
      } else {
        fireGo();
      }
      break;
    }

    case PH_GO: {
      if (allActiveDone()) {
        finishCollect();
        break;
      }
      /* Hard limit: a stick with no report by now gets a penalty. */
      if ((uint32_t)(now - phaseStart_) > TIMEOUT_REACTION + ROUND_LIMIT_GRACE_MS) {
        finishCollect();
      }
      break;
    }

    case PH_SHAKE:
      if (allActiveDone()) {
        finishShake();
        break;
      }
      if ((uint32_t)(now - phaseStart_) > TIMEOUT_SHAKE + ROUND_LIMIT_GRACE_MS) finishShake();
      break;

    case PH_RESULTS:
      if (!scoresShown_ && (int32_t)(now - scoresAt_) >= 0) showScores();
      if ((uint32_t)(now - phaseStart_) >= RESULTS_TOTAL_MS) afterResults();
      break;

    case PH_FINAL:
      if ((uint32_t)(now - phaseStart_) >= DURATION_FINAL) enterPhase(PH_IDLE);
      break;

    case PH_MAINT:
      otaTick(now);
      break;

    default:
      break;
  }

  updateBeacons(now);
  updateFlags();
  updateLeds();
  updateStateSend(now);
}
