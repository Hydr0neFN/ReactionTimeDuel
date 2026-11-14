#ifndef RTD_STICK_LOGIC_H
#define RTD_STICK_LOGIC_H

/*
 * StickLogic - joystick state machine (DESIGN.md section 6).
 *
 * Plain C++11, no Arduino or ESP-IDF include, no heap. The firmware feeds it
 * every frame from the receive queue, every button falling edge from the ISR
 * ring buffer and calls tick() from the main loop.
 */

#include <stdint.h>
#include "RtdProtocol.h"

/* ---- joystick states ---- */
#define JS_IDLE             0   /* waiting for a host command */
#define JS_WAITING_GO       1   /* CMD_GAME_START received */
#define JS_REACTION_TIMING  2   /* GO received, waiting for the button */
#define JS_SHAKE_COUNTING   3   /* GO received, counting shakes */
#define JS_DONE             4   /* result sent, waiting for the next round */
#define JS_OTA              5   /* downloading a new firmware image */

/* ---- timing, baseline values ---- */
#define FALSE_START_WINDOW_MS   1000    /* a press this long before GO is a false start */
#define GO_BLANK_MS             60      /* presses this long after GO are ignored */
#define GO_BLANK_SAMPLE_MS      2       /* gap of the two samples at the end of the blanking */
#define GO_VIBRATE_MS           500     /* GO haptic pulse */
#define COUNTDOWN_VIBRATE_MS    200     /* countdown haptic pulse */
#define JOIN_VIBRATE_MS         200     /* haptic feedback after CMD_OK */
#define RESULT_VIBRATE_MS       100     /* reaction result haptic pulse */
#define SHAKE_DONE_VIBRATE_MS   200     /* shake result haptic pulse */
#define RESULT_RESEND_MS        30      /* result resend interval */
#define RESULT_MAX_SENDS        12      /* total number of result transmissions */
#define REQID_RESEND_MS         80      /* join request resend interval */
#define REQID_MAX_SENDS         6       /* total number of join requests */
#define HEARTBEAT_INTERVAL_MS   1000    /* heartbeat period */
#define HEARTBEAT_FIRST_MS      50      /* first heartbeat is 50 ms x stick ID after boot */
#define STICK_SILENCE_MS        10000   /* no host packet for this long -> idle */
#define DONE_RESET_MS           60000   /* safety reset while in JS_DONE */
#define BEACON_MISS_LIMIT       2       /* beacons without this stick before a stale slot is dropped */

/* ---- shake detection, tuned on the real hardware (do not change) ---- */
#define SHAKE_BASE_G            16384
#define SHAKE_THRESHOLD         ((SHAKE_BASE_G + 6000) * 3)   /* ~67152 raw magnitude */
#define SHAKE_SAMPLE_MS         5       /* ~200 Hz */
#define SHAKE_FACTOR            3       /* internal counts per host target unit */
#define SHAKE_PROGRESS_STEPS    12      /* progress messages per ring */

/*
 * StickPorts - everything StickLogic needs from the device.
 * The firmware implements it with Arduino / ESP-IDF calls.
 */
class StickPorts {
public:
  virtual ~StickPorts() {}

  /* Monotonic milliseconds since boot, may wrap (compare with unsigned subtraction). */
  virtual uint32_t millis() = 0;

  /* Monotonic microseconds since boot, may wrap (compare with unsigned subtraction). */
  virtual uint32_t micros() = 0;

  /* Send one frame to the host (the firmware maps the host to a MAC). */
  virtual void send(const uint8_t* data, uint8_t len) = 0;

  /* Switch the vibration motor on or off. */
  virtual void motor(bool on) = 0;

  /* True while the button is pressed (active low is already handled by the firmware). */
  virtual bool buttonDown() = 0;

  /* Read one accelerometer sample; false when the sensor did not answer. */
  virtual bool readAccel(int16_t& ax, int16_t& ay, int16_t& az) = 0;

  /* Start the OTA download of the given firmware version (the firmware reboots). */
  virtual void otaStart(uint8_t verByte, uint8_t patch) = 0;
};

class StickLogic {
public:
  /* Bind the logic to its ports and its fixed stick ID (ID_STICK1..ID_STICK4). */
  StickLogic(StickPorts& ports, uint8_t stickId);

  /* Handle one received frame (already copied out of the receive callback). */
  void onPacket(const uint8_t* data, int len, uint32_t rxMicros);

  /* Handle one button falling edge, timestamped by the ISR. */
  void onButtonFall(uint32_t us);

  /* Run the timers and the state machine; call from the main loop. */
  void tick();

  /* Current JS_* state. */
  uint8_t state() const { return state_; }

  /* PH_* phase of the host from the last CMD_BEACON (PH_IDLE before the first beacon). */
  uint8_t hostPhase() const { return hostPhase_; }

  /* Player slot 1..4 assigned by the host, 0 = not joined. */
  uint8_t assignedSlot() const { return assignedSlot_; }

  /* Fixed stick ID of this stick. */
  uint8_t stickId() const { return id_; }

  /* Last result value sent to the host (ms, or TIME_PENALTY). */
  uint16_t lastResult() const { return lastResult_; }

private:
  void sendCmd(uint8_t cmd, uint16_t data);
  uint16_t versionData() const;
  void goIdle();
  void vibStart(uint16_t durationMs);
  void startJoinRequest(uint32_t now);
  void finish(uint8_t cmd, uint16_t data, uint32_t now);
  void handleGo(const GamePacket& pkt, uint16_t data, uint32_t rxMicros);
  void shakeTick(uint32_t now);
  void reactionTick(uint32_t now);

  StickPorts& ports_;
  uint8_t id_;
  uint8_t state_;

  uint8_t assignedSlot_;
  uint8_t beaconMissCount_;  /* consecutive beacons without this stick */
  uint8_t hostPhase_;        /* PH_* from the last beacon */
  uint8_t mode_;
  uint8_t shakeParam_;      /* host target 10/15/20 */
  uint16_t shakeTarget_;    /* internal target, host target x SHAKE_FACTOR */

  uint32_t bootMs_;
  uint32_t lastHostMs_;
  uint32_t hbMs_;
  bool hbStarted_;

  /* join request */
  bool joinActive_;
  uint8_t reqSends_;
  uint32_t reqNextMs_;

  /* result */
  bool resultActive_;
  uint8_t resultCmd_;
  uint16_t resultData_;
  uint16_t lastResult_;
  uint8_t resultSends_;
  uint32_t resultNextMs_;
  uint32_t doneMs_;

  /* timing */
  uint32_t goUs_;
  uint32_t goMs_;
  bool sawFall_;
  uint32_t lastFallUs_;
  bool blankSampled_;
  bool blankDone_;
  bool blankS1_;
  uint32_t blankSampleMs_;

  /* shake */
  uint16_t shakeCount_;
  bool shakeWasAbove_;
  uint32_t shakeStartMs_;
  uint32_t lastAccelMs_;
  uint16_t shakeLastReported_;

  /* motor */
  bool motorOn_;
  uint32_t motorStartMs_;
  uint16_t motorDurMs_;
};

#endif /* RTD_STICK_LOGIC_H */
