#ifndef RTD_TEST_SIM_H
#define RTD_TEST_SIM_H

/*
 * Sim - native simulator: a virtual clock, a lossy radio, one HostGame, four
 * StickLogic instances with scripted players, one DisplayLink (DESIGN.md 10).
 */

#include <stdint.h>
#include "RtdProtocol.h"
#include "HostGame.h"
#include "StickLogic.h"
#include "DisplayLink.h"

#define SIM_STICKS      4
#define SIM_MAX_FRAMES  64
#define SIM_MAX_BUTTONS 64

/* scripted player behaviour */
enum {
  PL_NONE = 0,   /* never presses */
  PL_NORMAL,     /* presses reactMs after GO, releases after 100 ms */
  PL_EARLY,      /* presses earlyMs before GO; optional second press secondMs after GO */
  PL_BLANK,      /* presses 20 ms after GO and holds */
  PL_HOLD        /* presses earlyMs before GO and holds */
};

struct PlayerScript {
  uint8_t  kind;
  uint32_t reactMs;
  uint32_t earlyMs;
  uint32_t secondMs;
  uint32_t shakeUs;   /* period of one full shake cycle in us, 0 = no shaking */
  uint8_t  shakeAxis; /* SHAKE_AXIS_* */
  uint32_t shakeLeadUs; /* the player starts shaking this long after the real GO */
  uint8_t  gravAxis;  /* axis that carries 1 g: 0 X, 1 Y, 2 Z */
  uint16_t noiseLsb;  /* amplitude of the sensor noise in LSB */
};

#define SHAKE_AXIS_X     0
#define SHAKE_AXIS_Y     1
#define SHAKE_AXIS_Z     2
#define SHAKE_AXIS_DIAG  3   /* mixed signs: +x -y +z */

class Sim {
public:
  /* Build a fresh simulation; startUs is the value micros() has at t = 0. */
  explicit Sim(uint32_t seed = 1, uint64_t startUs = 0);
  ~Sim();

  /* HostGame::begin with the given settings (index 0 unused) and records. */
  void beginWith(const uint8_t* cfg, uint16_t bestAlltimeMs, uint16_t gamesTotal);

  /* ---- clock ---- */
  void step();                     /* one 1 ms step */
  void run(uint32_t ms);
  bool runUntilPhase(uint8_t phase, uint32_t maxMs);
  bool runUntilIdle(uint32_t maxMs, uint32_t stableMs = 2000);
  uint64_t nowUs() const { return nowUs_; }
  uint32_t nowMs() const { return (uint32_t)(nowUs_ / 1000u); }

  /* ---- nodes ---- */
  HostGame& host() { return *host_; }
  StickLogic& stick(uint8_t id) { return *sticks_[id - 1]; }
  DisplayLink& display() { return *display_; }

  /* ---- player scripts ---- */
  PlayerScript player[SIM_STICKS + 1];
  void setPlayer(uint8_t id, uint8_t kind, uint32_t reactMs, uint32_t shakeUs, uint8_t shakeAxis = SHAKE_AXIS_X);

  /* ---- button input ---- */
  void press(uint8_t id, uint32_t delayMs, uint32_t holdMs = 100);
  void pressAtUs(uint8_t id, uint64_t atUs, uint32_t holdMs);
  void holdButton(uint8_t id, bool down);

  /* ---- radio ---- */
  void setLoss(uint8_t percent);
  void setHostToStickLoss(uint8_t id, uint8_t percent);
  void setStickToHostLoss(uint8_t id, uint8_t percent);
  void setDisplayLoss(uint8_t percent);
  void setDuplicatePercent(uint8_t percent);
  void setJitterMs(uint32_t maxJitterMs);
  void dropNextBroadcastGo();
  void dropNextBroadcastIdle();
  void dropGoFrames(uint8_t id, uint8_t count);
  bool broadcastGoDropped() const { return broadcastGoDropped_; }

  /* ---- stick hardware ---- */
  void rebootStick(uint8_t id);

  /* ---- host hardware ---- */
  void rebootHost();   /* replace the HostGame object, as a host power cycle does */
  void setStickAlive(uint8_t id, bool alive);
  void setStickFw(uint8_t id, uint8_t ver, uint8_t patch);
  void setStickImage(bool present, uint8_t ver, uint8_t patch);
  void setOtaComesBack(uint8_t id, bool comesBack);

  /* ---- observations ---- */
  uint64_t hostGoT0Us() const { return hostGoT0Us_; }
  uint64_t stickGoUs(uint8_t id) const { return stickGoUs_[id - 1]; }
  uint16_t hostSent(uint8_t cmd) const { return hostSent_[cmd]; }
  uint16_t hostRecv(uint8_t cmd) const { return hostRecv_[cmd]; }
  uint16_t displaySent(uint8_t cmd) const { return displaySent_[cmd]; }
  uint8_t otaBeginCount() const { return otaBeginCount_; }
  uint8_t otaBeginOrder(uint8_t i) const { return otaBeginOrder_[i]; }
  uint16_t soundCount(RtdSound s) const { return sounds_[s]; }

  /* Every live stick is in JS_IDLE and holds no slot (released). */
  bool allSticksIdle() const;

  /* persistence and maintenance counters, filled by the fake ports */
  uint16_t saveSettingsCalls;
  uint16_t saveStatsCalls;
  uint16_t maintEnterCalls;
  uint16_t maintExitCalls;

private:
  struct Frame {
    uint64_t atUs;
    uint8_t  to;      /* ID_STICK1..4 or ID_DISPLAY */
    uint8_t  len;
    uint8_t  data[STATE_SIZE];
  };

  struct Button {
    uint64_t atUs;
    uint8_t  id;
    bool     down;
  };

  struct HostImpl;
  struct StickImpl;
  struct DispImpl;

  void hostSend(uint8_t dest, const uint8_t* data, uint8_t len);
  void stickSend(uint8_t id, const uint8_t* data, uint8_t len);
  void displaySend(const uint8_t* data, uint8_t len);

  void audioPlay(RtdSound s);
  void audioStop();
  bool audioIdle() const;
  bool readAccel(uint8_t id, int16_t& ax, int16_t& ay, int16_t& az);
  void otaStart(uint8_t id, uint8_t ver, uint8_t patch);

  void pushFrame(uint8_t to, const uint8_t* data, uint8_t len, uint64_t atUs);
  void deliverDue();
  void deliverFrame(const Frame& f);
  void processButtons();
  void updateScripts();
  void updateAudio();
  void schedulePress(uint8_t id, uint64_t atUs, uint32_t holdMs);
  void scheduleRelease(uint8_t id, uint64_t atUs);
  bool maybeLose(uint8_t percent);

  uint32_t rnd();
  uint32_t randn(uint32_t n);

  uint64_t nowUs_;
  uint32_t seed_;
  uint32_t rng_;

  uint8_t  hostToStickLoss_[SIM_STICKS];
  uint8_t  stickToHostLoss_[SIM_STICKS];
  uint8_t  displayLoss_;
  uint8_t  dupPercent_;
  uint32_t jitterMs_;
  bool     dropBroadcastGo_;
  bool     broadcastGoDropped_;
  bool     dropBroadcastIdle_;
  uint8_t  goDrop_[SIM_STICKS + 1];

  uint8_t  cfg_[CFG_SLOTS];   /* last settings handed to HostGame::begin */
  uint16_t bestAlltimeMs_;
  uint16_t gamesTotal_;

  Frame    frames_[SIM_MAX_FRAMES];
  uint8_t  frameCount_;
  Button   buttons_[SIM_MAX_BUTTONS];
  uint8_t  buttonCount_;

  bool     buttonDown_[SIM_STICKS + 1];

  bool     alive_[SIM_STICKS + 1];
  uint8_t  fwVer_[SIM_STICKS + 1];
  uint8_t  fwPatch_[SIM_STICKS + 1];
  bool     otaComesBack_[SIM_STICKS + 1];
  uint64_t otaRebootAt_[SIM_STICKS + 1];
  bool     otaRebootPending_[SIM_STICKS + 1];
  uint8_t  otaNewVer_[SIM_STICKS + 1];
  uint8_t  otaNewPatch_[SIM_STICKS + 1];

  bool     imagePresent_;
  uint8_t  imageVer_;
  uint8_t  imagePatch_;

  uint64_t hostGoT0Us_;
  uint64_t stickGoUs_[SIM_STICKS];
  bool     goRecorded_[SIM_STICKS];
  bool     reactScheduled_[SIM_STICKS];
  bool     earlyScheduled_[SIM_STICKS];
  uint64_t shakeStartUs_[SIM_STICKS];
  uint8_t  lastPhase_;

  uint16_t hostSent_[256];
  uint16_t hostRecv_[256];
  uint16_t displaySent_[256];
  uint16_t sounds_[SND_COUNT];
  uint8_t  otaBeginOrder_[SIM_STICKS];
  uint8_t  otaBeginCount_;

  /* audio queue */
  RtdSound audioQ_[16];
  uint8_t  audioHead_;
  uint8_t  audioTail_;
  bool     audioPlaying_;
  uint64_t audioEndUs_;
  uint64_t audioLastEndUs_;
  bool     audioHasPlayed_;

  HostImpl*    hostPorts_;
  StickImpl*   stickPorts_[SIM_STICKS];
  DispImpl*    dispPorts_;
  HostGame*    host_;
  StickLogic*  sticks_[SIM_STICKS];
  DisplayLink* display_;
};

#endif /* RTD_TEST_SIM_H */
