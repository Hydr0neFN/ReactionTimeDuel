#ifndef RTD_HOST_GAME_H
#define RTD_HOST_GAME_H

/*
 * HostGame - host game state machine (DESIGN.md section 5).
 *
 * Ported from the baseline firmware/host/src/main.cpp. Plain C++11, no Arduino
 * or ESP-IDF include, no heap, no exceptions, no RTTI, no STL. One thread: every
 * frame goes through onPacket() and the main loop calls tick().
 */

#include <stdint.h>
#include "RtdProtocol.h"

/* ---- game constants (baseline names, baseline values unless DESIGN.md changed them) ---- */
#define TOTAL_ROUNDS       5       /* baseline default, CFG_ROUNDS overrides it */
#define DEUCE_LEAD         3       /* baseline default, CFG_DEUCE_LEAD overrides it */
#define TIMEOUT_JOIN       30000   /* max join phase */
#define JOIN_IDLE_TIME     5000    /* baseline join prompt length */

#define DURATION_IDLE      3000
#define DURATION_COUNTDOWN 4000
#define DURATION_RESULTS   7000
#define DURATION_FINAL     15000   /* player + wins + victory + game over + gaps */

#define NUM_REACT_DELAYS   3
static const uint16_t REACT_DELAYS[NUM_REACT_DELAYS] = {3000, 5000, 7000};

#define REACT_ANNOUNCE_DELAY        3000   /* ms, subsequent rounds */
#define REACT_ANNOUNCE_DELAY_FIRST  8000   /* ms, round with instructions */

#define NUM_SHAKE_TARGETS  3
static const uint8_t SHAKE_TARGETS[NUM_SHAKE_TARGETS] = {10, 15, 20};

#define SHAKE_ANNOUNCE_DELAY        4000   /* ms, subsequent rounds */
#define SHAKE_ANNOUNCE_DELAY_FIRST  8500   /* ms, round with instructions */

#define COUNTDOWN_FLASH_DURATION  200      /* ms white flash on the rings */

/* ---- DESIGN.md additions ---- */
#define JOIN_SOLO_TIMEOUTS     3       /* 1 player timeouts before back to idle */
#define IDLE_PROMPT_INTERVAL   45000   /* attract sound repeat in PH_IDLE */
#define GO_BLANK_MS            60      /* stick blanking window, mirrored for the host */
#define FALSE_START_WINDOW_MS  1000    /* stick false start window */
#define ACK_RETRY_INTERVAL     40      /* host -> stick retry interval */
#define ACK_MAX_RETRIES        4       /* host -> stick retries */
#define BEACON_INTERVAL        500     /* CMD_BEACON period */
#define STICK_PRESENCE_MS      3000    /* a stick is online this long after its last packet */
#define STATE_MIN_INTERVAL     50      /* snapshot rate limit */
#define STATE_PHASE_RESEND_MS  20      /* extra snapshot after a phase change */
#define STATE_MAX_INTERVAL     250     /* snapshot keep-alive */
#define ANNOUNCE_AUDIO_MS      1500    /* announcement end: audio idle + this */
#define ANNOUNCE_SLACK_MS      2000    /* announcement end: baseline delay + this */
#define RESULTS_SCORES_MS      3000    /* results: times first, then winner/scores */
#define RESULTS_TOTAL_MS       6000    /* results phase length */
#define JOIN_COLOURS_MS        1000    /* show the joined colours before the first round */
#define OTA_STICK_TIMEOUT_MS   120000  /* per stick OTA timeout */
#define IDLE_REPEAT_COUNT      3       /* CMD_IDLE broadcasts on abort / idle */

/* ---- LED view (the animations stay in the firmware) ---- */
/* Reaction wait: the centre ring counts down in this many stages of equal LED count,
 * at random intervals that sum to the wait; the last stage ends at GO. */
#define REACT_WAIT_STAGES     4
#define REACT_WAIT_LEDS       12   /* LEDs of the centre ring */
#define REACT_WAIT_STAGE_MIN_MS 300

#define NEO_OFF             0
#define NEO_IDLE_RAINBOW    1
#define NEO_STATUS          2
#define NEO_RANDOM_FAST     3
#define NEO_FIXED_COLOR     4
#define NEO_COUNTDOWN       5
#define NEO_BLINK_SLOT      6
#define NEO_SHAKE_COUNTDOWN 7
#define NEO_REACT_WAIT     8   /* reaction wait: player colours, centre ring counts down in stages */

#define RTD_RGB_OFF    0x000000u
#define RTD_RGB_RED    0xFF0000u
#define RTD_RGB_GREEN  0x00FF00u
#define RTD_RGB_BLUE   0x0000FFu
#define RTD_RGB_YELLOW 0xFFFF00u
#define RTD_RGB_WHITE  0xFFFFFFu

/* ---- sounds: one value per MP3 file in firmware/host/data ---- */
enum RtdSound {
  SND_NONE = 0,        /* no sound */
  SND_BUTTON_CLICK,    /* click.mp3 */
  SND_GET_READY,       /* get_ready.mp3 */
  SND_PRESS_TO_JOIN,   /* press_join.mp3 */
  SND_READY,           /* ready.mp3 */
  SND_REACTION_MODE,   /* reaction.mp3 */
  SND_REACTION_INSTRUCT, /* react_inst.mp3 */
  SND_SHAKE_IT,        /* shake.mp3 */
  SND_YOU_WILL_SHAKE,  /* will_shake.mp3 */
  SND_NUM_1,           /* one.mp3 */
  SND_NUM_2,           /* two.mp3 */
  SND_NUM_3,           /* three.mp3 */
  SND_NUM_10,          /* ten.mp3 */
  SND_NUM_15,          /* fifteen.mp3 */
  SND_NUM_20,          /* twenty.mp3 */
  SND_BEEP,            /* beep.mp3 */
  SND_PLAYER_1,        /* player1.mp3 */
  SND_PLAYER_2,        /* player2.mp3 */
  SND_PLAYER_3,        /* player3.mp3 */
  SND_PLAYER_4,        /* player4.mp3 */
  SND_FASTEST,         /* fastest.mp3 */
  SND_WINS,            /* wins.mp3 */
  SND_VICTORY_FANFARE, /* victory.mp3 */
  SND_GAME_OVER,       /* gameover.mp3 */
  SND_ERROR_TONE,      /* error.mp3 */
  SND_COUNT            /* number of sounds */
};

/*
 * LedView - what the baseline LED code read from globals. The animations stay
 * in the firmware; HostGame only publishes the parameters.
 */
struct LedView {
  uint8_t  neoMode;             /* NEO_* mode */
  uint32_t ringColor[5];        /* ring override, 0 = none, else 0xRRGGBB */
  bool     ringBlink[5];        /* blink this ring on and off */
  uint8_t  blinkSlot;           /* player slot 0..3 blinked in NEO_BLINK_SLOT */
  uint32_t flashUntilMs;        /* countdown white flash until this millis() value */
  uint8_t  joinedMask;          /* bit0 = player slot 1 */
  uint8_t  activeMask;          /* players of the current round */
  uint8_t  progress[RTD_MAX_PLAYERS]; /* shake progress 0..100 */
  uint32_t shakeStartMs;        /* millis() at GO of a shake round */
  uint32_t limitStartMs;        /* millis() at GO, start of the centre ring countdown */
  uint32_t limitMs;             /* round limit in ms, 0 = no countdown */
  uint8_t  waitLit;             /* PH_REACT_WAIT: lit LEDs of the centre ring (12, 9, 6, 3), 0 at GO and outside */
};

/*
 * HostPorts - everything HostGame needs from the device.
 * The firmware implements it with Arduino / ESP-IDF calls.
 */
class HostPorts {
public:
  virtual ~HostPorts() {}

  /* Monotonic milliseconds since boot, may wrap (compare with unsigned subtraction). */
  virtual uint32_t millis() = 0;

  /* Monotonic microseconds since boot, may wrap (compare with unsigned subtraction). */
  virtual uint32_t micros() = 0;

  /* Uniform random value 0..n-1 (seed comes from the hardware RNG). */
  virtual uint32_t random(uint32_t n) = 0;

  /* Send one frame to a device ID or to ID_BROADCAST; the firmware maps the ID to a MAC. */
  virtual void send(uint8_t destId, const uint8_t* data, uint8_t len) = 0;

  /* Queue a sound; plays after the sounds already queued. */
  virtual void audioPlay(RtdSound s) = 0;

  /* Stop the current sound and clear the queue. */
  virtual void audioStop() = 0;

  /* True when nothing plays and the queue is empty. */
  virtual bool audioIdle() = 0;

  /* Volume, brightness or strip mode changed; apply at once. */
  virtual void applySettings(const uint8_t* cfg) = 0;

  /* Persist the settings (the firmware debounces flash writes by 2 s). */
  virtual void saveSettings(const uint8_t* cfg) = 0;

  /* Persist the all-time record and the games counter. */
  virtual void saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal) = 0;

  /* A joystick firmware image is stored on the host: report its version. */
  virtual bool stickImage(uint8_t* verByte, uint8_t* patch) = 0;

  /* Start the maintenance access point and the web server. */
  virtual void maintEnter() = 0;

  /* Leave the maintenance mode (the firmware reboots). */
  virtual void maintExit() = 0;
};

class HostGame {
public:
  /* Bind the game to its ports. */
  explicit HostGame(HostPorts& ports);

  /* Load the settings and the persisted records and start in PH_IDLE. */
  void begin(const uint8_t* cfg, uint16_t bestAlltimeMs, uint16_t gamesTotal);

  /* Handle one received frame (already copied out of the receive callback). */
  void onPacket(const uint8_t* data, int len, uint32_t rxMicros);

  /* Run all timers, retries, beacons and snapshot sends; call from the main loop. */
  void tick();

  /* Current snapshot (CRC and seq are only updated by buildState()). */
  const StatePacket& state() const { return snap_; }

  /* Seal the snapshot (start, version, seq, crc) and return it, ready to send. */
  const StatePacket& buildState();

  /* Current LED parameters. */
  const LedView& leds() const { return leds_; }

  /* True when the snapshot content changed since the last clearStateChanged(). */
  bool stateChanged() const { return stateChanged_; }

  /* Clear the changed flag after the firmware sent the snapshot itself. */
  void clearStateChanged() { stateChanged_ = false; }

  /* Current PH_* phase. */
  uint8_t phase() const { return phase_; }
  /* Why the host left maintenance last: 1 abort action, 2 leave action, plus the
   * packet source. Diagnostic only; Maint stores it before a restart. */
  uint8_t exitCause() const { return exitCause_; }
  uint8_t exitSrc() const { return exitSrc_; }

  /* Milliseconds until the next GO, 0 when no GO is scheduled (diagnostics / tests). */
  uint32_t msUntilGo() const;

private:
  /* --- helpers --- */
  void enterPhase(uint8_t phase);
  void resetGame();
  void buildSnapshot();
  void updateLeds();
  void planReactWait();
  void updateRetries(uint32_t now);
  void updateBeacons(uint32_t now);
  void updateStateSend(uint32_t now);
  void updateFlags();

  void sendCmd(uint8_t destId, uint8_t cmd, uint16_t data);
  void sendWithRetry(uint8_t destId, uint8_t cmd, uint16_t data);
  void sendGoFrames(uint32_t now);
  void broadcastIdle();

  uint8_t lowestFreeSlot() const;
  uint8_t joinedCount() const;
  uint8_t joinedMask() const;
  uint8_t activeMask() const;
  uint8_t joinedStickMask() const;
  uint8_t activeStickMask() const;
  uint8_t onlineMask() const;
  bool isActiveSlot(uint8_t slot) const;
  bool allActiveDone() const;
  bool isBonusRound() const;
  uint8_t findRoundWinner() const;
  uint8_t findFinalWinner() const;
  uint8_t topScore() const;
  uint8_t topScoreCount() const;
  void ringForSlot(uint8_t slot, uint32_t color, bool blink);

  void claimSlot(uint8_t stickId);
  void startGame();
  void startRound();
  void fireGo();
  void finishCollect();
  void finishShake();
  void showScores();
  void afterResults();
  void enterDeuce(bool suddenDeath);
  void finalizeRoundRecords(uint8_t winner);
  void actAbort();
  void actStartNow();
  void actIdentify(uint8_t stickId);
  void actMaintenance(bool enter);
  void actOtaSticks();
  void otaTick(uint32_t now);
  void otaNext();
  void applyCfg(uint8_t key, uint8_t value, bool persist);
  void updateIdleAvailability();

  HostPorts& ports_;

  /* --- settings and records --- */
  uint8_t  cfg_[CFG_SLOTS];
  uint16_t bestSessionMs_;
  uint16_t bestAlltimeMs_;
  uint16_t gamesTotal_;
  bool     newRecord_;

  /* --- slots --- */
  uint8_t  slotStick_[RTD_MAX_PLAYERS];   /* ID_STICK1..4 or 0 */
  uint8_t  score_[RTD_MAX_PLAYERS];
  uint8_t  status_[RTD_MAX_PLAYERS];      /* PS_* */
  uint16_t timeMs_[RTD_MAX_PLAYERS];
  uint8_t  progress_[RTD_MAX_PLAYERS];    /* 0..100 */

  /* --- presence and versions --- */
  uint32_t lastSeenMs_[RTD_MAX_PLAYERS];
  uint8_t  stickVer_[RTD_MAX_PLAYERS];
  uint8_t  stickPatch_[RTD_MAX_PLAYERS];

  /* --- phase --- */
  uint8_t  phase_;
  uint32_t phaseStart_;
  uint32_t nowMs_;

  /* --- round --- */
  uint8_t  round_;
  uint8_t  totalRounds_;
  uint8_t  gameModeCfg_;      /* latched CFG_GAME_MODE */
  uint8_t  scoringCfg_;       /* latched CFG_SCORING */
  uint8_t  bonusCfg_;         /* latched CFG_BONUS */
  uint8_t  mode_;
  uint8_t  countdown_;
  uint8_t  shakeTarget_;
  uint8_t  promptSlot_;
  uint8_t  winner_;
  uint8_t  flags_;
  bool     bonusRound_;       /* the current round is a bonus round */

  /* --- announcement --- */
  uint16_t announceBaseMs_;
  bool     announceFirst_;
  bool     reactionInstructPlayed_;
  bool     shakeInstructPlayed_;
  uint8_t  delayIdx_;
  uint8_t  targetIdx_;
  uint8_t  lastDelayIdx_;
  uint8_t  lastTargetIdx_;
  uint8_t  modeBagIdx_;
  uint16_t reactDelayMs_;
  uint16_t waitStageEndMs_[REACT_WAIT_STAGES];  /* end of each wait stage, after PH_REACT_WAIT entry */
  uint32_t waitRng_;                            /* private xorshift32, keeps ports_.random() untouched */

  /* --- join --- */
  uint32_t promptTimerStart_;
  uint8_t  soloTimeouts_;
  bool     starting_;
  uint32_t startShownAt_;

  /* --- GO and collect --- */
  uint32_t goMicros_;
  uint32_t goMs_;
  uint32_t shakeStartMs_;
  uint8_t  exitCause_;
  uint8_t  exitSrc_;
  uint8_t  rxSrc_;
  uint8_t  idleRepeatLeft_;
  uint32_t idleRepeatAt_;

  /* --- deuce / sudden death --- */
  bool     deuce_;
  uint8_t  deuceSlots_[RTD_MAX_PLAYERS];
  uint8_t  deuceCount_;
  uint8_t  consecutiveTimeouts_;

  /* --- results --- */
  bool     scoresShown_;
  uint32_t scoresAt_;

  /* --- idle --- */
  uint32_t idlePromptAt_;

  /* --- retries --- */
  struct PendingCmd {
    bool     active;
    uint8_t  cmd;
    uint16_t data;
    uint8_t  retries;
    uint32_t lastTx;
  };
  PendingCmd pending_[RTD_MAX_PLAYERS];

  /* --- beacon and snapshot --- */
  uint32_t beaconAt_;
  uint32_t lastSnapAt_;
  uint32_t phaseSnapAt_;
  bool     phaseSnapPending_;
  uint8_t  seq_;
  StatePacket snap_;
  StatePacket lastSnap_;
  LedView  leds_;
  bool     stateChanged_;
  uint32_t flashUntilMs_;

  /* --- OTA --- */
  uint8_t  otaState_;
  uint8_t  otaTarget_;
  uint8_t  otaProgress_;
  uint8_t  otaList_[RTD_MAX_PLAYERS];
  uint8_t  otaListCount_;
  uint8_t  otaIndex_;
  uint32_t otaStartMs_;
  bool     otaFailed_;
  uint8_t  otaStatus_[RTD_MAX_PLAYERS];
  bool     imageValid_;
  uint8_t  imageVer_;
  uint8_t  imagePatch_;
  uint32_t imageCheckAt_;
};

#endif /* RTD_HOST_GAME_H */
