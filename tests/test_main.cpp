/*
 * Native tests for rtd_core, T1..T15 of DESIGN.md section 10 plus T16..T27.
 */

#include <stdio.h>
#include <string.h>

#include "sim.h"

static int g_checks = 0;
static int g_fails = 0;

static void checkFail(const char* file, int line, const char* expr) {
  g_fails++;
  printf("  check at %s:%d: %s\n", file, line, expr);
}

#define CHECK(c) do { g_checks++; if (!(c)) checkFail(__FILE__, __LINE__, #c); } while (0)

static uint32_t g_rng = 0x12345u;
static uint32_t trnd() {
  g_rng = g_rng * 1664525u + 1013904223u;
  return g_rng >> 8;
}
static uint32_t trange(uint32_t lo, uint32_t hi) {
  return lo + (hi > lo ? trnd() % (hi - lo + 1) : 0);
}

static void defaultCfg(uint8_t* cfg) {
  for (int i = 0; i < CFG_SLOTS; i++) cfg[i] = RTD_CFG_DEF[i];
}

static int idiff(int a, int b) { return a > b ? a - b : b - a; }

/* The slot of a stick in the snapshot, 0 when it has none. */
static uint8_t slotOf(const StatePacket& st, uint8_t stickId) {
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (st.players[i].stick_id == stickId) return (uint8_t)(i + 1);
  }
  return 0;
}

/* Slots are 1..n without a gap and every stick has exactly one slot. */
static bool slotsWithoutGap(const StatePacket& st) {
  uint8_t n = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    if (st.joined_mask & (uint8_t)(1u << i)) n++;
  }
  if (st.joined_mask != (uint8_t)((1u << n) - 1u)) return false;
  uint8_t seen = 0;
  for (uint8_t i = 0; i < RTD_MAX_PLAYERS; i++) {
    uint8_t s = st.players[i].stick_id;
    if (s == 0) continue;
    if (s > RTD_MAX_PLAYERS) return false;
    if (seen & (uint8_t)(1u << (s - 1))) return false;
    seen |= (uint8_t)(1u << (s - 1));
  }
  return true;
}

static void joinSticks(Sim& sim, int count, uint32_t gapMs) {
  for (int i = 1; i <= count; i++) {
    sim.press((uint8_t)i, gapMs * (uint32_t)i, 100);
  }
}

static bool waitJoined(Sim& sim, uint8_t mask, uint32_t maxMs) {
  for (uint32_t i = 0; i < maxMs; i++) {
    if (sim.host().state().joined_mask == mask) return true;
    sim.step();
  }
  return sim.host().state().joined_mask == mask;
}

/* ===================================================================== */
/* T1 protocol                                                           */
/* ===================================================================== */

static uint8_t crcRef(const uint8_t* d, int n) {
  uint8_t crc = 0;
  for (int i = 0; i < n; i++) {
    crc = (uint8_t)(crc ^ d[i]);
    for (int b = 0; b < 8; b++) {
      if (crc & 1) crc = (uint8_t)((crc >> 1) ^ 0x8C);
      else crc = (uint8_t)(crc >> 1);
    }
  }
  return crc;
}

static void testT1() {
  CHECK(sizeof(GamePacket) == 7);
  CHECK(sizeof(PlayerState) == 6);
  CHECK(sizeof(StatePacket) == STATE_SIZE);
  CHECK(STATE_SIZE == 72);
  CHECK(PACKET_SIZE == 7);
  CHECK(PACKET_START == 0x0A);
  CHECK(STATE_START == 0x5A);
  CHECK(PROTO_VERSION == 4);

  CHECK(offsetof(GamePacket, start) == 0);
  CHECK(offsetof(GamePacket, dest_id) == 1);
  CHECK(offsetof(GamePacket, src_id) == 2);
  CHECK(offsetof(GamePacket, cmd) == 3);
  CHECK(offsetof(GamePacket, data_high) == 4);
  CHECK(offsetof(GamePacket, data_low) == 5);
  CHECK(offsetof(GamePacket, crc) == 6);

  CHECK(offsetof(PlayerState, stick_id) == 0);
  CHECK(offsetof(PlayerState, score) == 1);
  CHECK(offsetof(PlayerState, status) == 2);
  CHECK(offsetof(PlayerState, progress) == 3);
  CHECK(offsetof(PlayerState, time_ms) == 4);

  CHECK(offsetof(StatePacket, start) == 0);
  CHECK(offsetof(StatePacket, version) == 1);
  CHECK(offsetof(StatePacket, seq) == 2);
  CHECK(offsetof(StatePacket, phase) == 3);
  CHECK(offsetof(StatePacket, mode) == 4);
  CHECK(offsetof(StatePacket, round) == 5);
  CHECK(offsetof(StatePacket, total_rounds) == 6);
  CHECK(offsetof(StatePacket, flags) == 7);
  CHECK(offsetof(StatePacket, countdown) == 8);
  CHECK(offsetof(StatePacket, shake_target) == 9);
  CHECK(offsetof(StatePacket, prompt_slot) == 10);
  CHECK(offsetof(StatePacket, winner) == 11);
  CHECK(offsetof(StatePacket, joined_mask) == 12);
  CHECK(offsetof(StatePacket, active_mask) == 13);
  CHECK(offsetof(StatePacket, sticks_online) == 14);
  CHECK(offsetof(StatePacket, secs_left) == 15);
  CHECK(offsetof(StatePacket, players) == 16);
  CHECK(offsetof(StatePacket, cfg) == 40);
  CHECK(offsetof(StatePacket, best_session_ms) == 52);
  CHECK(offsetof(StatePacket, best_alltime_ms) == 54);
  CHECK(offsetof(StatePacket, games_total) == 56);
  CHECK(offsetof(StatePacket, host_fw) == 58);
  CHECK(offsetof(StatePacket, host_fw_patch) == 59);
  CHECK(offsetof(StatePacket, stick_fw) == 60);
  CHECK(offsetof(StatePacket, stick_fw_patch) == 64);
  CHECK(offsetof(StatePacket, ota_state) == 68);
  CHECK(offsetof(StatePacket, ota_target) == 69);
  CHECK(offsetof(StatePacket, ota_progress) == 70);
  CHECK(offsetof(StatePacket, crc) == 71);

  uint8_t buf[256];
  for (int i = 0; i < 256; i++) buf[i] = (uint8_t)(i * 37 + 11);
  for (int n = 0; n <= 255; n++) {
    CHECK(calcCRC8(buf, (uint8_t)n) == crcRef(buf, n));
  }

  GamePacket pkt;
  buildPacket(&pkt, ID_STICK2, ID_HOST, CMD_GO, 0x1234);
  CHECK(pkt.start == PACKET_START);
  CHECK(pkt.dest_id == ID_STICK2);
  CHECK(pkt.src_id == ID_HOST);
  CHECK(pkt.cmd == CMD_GO);
  CHECK(packetData(&pkt) == 0x1234);
  CHECK(validatePacket((const uint8_t*)&pkt, PACKET_SIZE) == 1);

  uint8_t raw[PACKET_SIZE];
  memcpy(raw, &pkt, PACKET_SIZE);
  raw[3] ^= 0x01;
  CHECK(validatePacket(raw, PACKET_SIZE) == 0);
  memcpy(raw, &pkt, PACKET_SIZE);
  CHECK(validatePacket(raw, PACKET_SIZE - 1) == 0);
  CHECK(validatePacket(raw, PACKET_SIZE + 1) == 0);

  StatePacket st;
  memset(&st, 0, sizeof(st));
  st.phase = PH_SHAKE;
  st.round = 3;
  sealState(&st);
  CHECK(st.start == STATE_START);
  CHECK(st.version == PROTO_VERSION);
  CHECK(st.crc == crcRef((const uint8_t*)&st, STATE_SIZE - 1));
  CHECK(validateState((const uint8_t*)&st, STATE_SIZE) == 1);

  uint8_t sraw[STATE_SIZE];
  memcpy(sraw, &st, STATE_SIZE);
  sraw[20] ^= 0x40;
  CHECK(validateState(sraw, STATE_SIZE) == 0);
  CHECK(validateState((const uint8_t*)&st, STATE_SIZE - 1) == 0);

  StatePacket bad;
  memcpy(&bad, &st, sizeof(bad));
  bad.version = 3;
  bad.crc = crcRef((const uint8_t*)&bad, STATE_SIZE - 1);
  CHECK(validateState((const uint8_t*)&bad, STATE_SIZE) == 0);

  for (int k = 1; k <= CFG_COUNT; k++) {
    CHECK(cfgClamp((uint8_t)k, 0) == RTD_CFG_MIN[k]);
    CHECK(cfgClamp((uint8_t)k, 255) == RTD_CFG_MAX[k]);
  }
}

/* ===================================================================== */
/* shared helpers for the game tests                                     */
/* ===================================================================== */

struct GameResult {
  bool sawFinal;
  bool sawIdle;
  bool allIdle;
  bool sawReaction;
  bool sawShake;
  int  roundsWithWinner;
  int  scoreSum;
  int  roundsPlayed;
  bool slotsOkAtStart;
  bool sawResults;
  uint32_t firstResultsMs;   /* start of the first PH_RESULTS, relative to the sim start */
};

static GameResult playGame(uint64_t startUs, uint32_t seed, int players, uint8_t rounds,
                           uint8_t modeCfg) {
  GameResult r;
  memset(&r, 0, sizeof(r));

  Sim sim(seed, startUs);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = rounds;
  cfg[CFG_GAME_MODE] = modeCfg;
  cfg[CFG_SCORING] = SCORING_CLASSIC;
  cfg[CFG_BONUS] = BONUS_OFF;
  sim.beginWith(cfg, TIME_NONE, 0);

  for (int id = 1; id <= players; id++) {
    sim.setPlayer((uint8_t)id, PL_NORMAL, (uint32_t)(150 + 60 * id), (uint32_t)(50000 + 8000 * id));
  }
  joinSticks(sim, players, 20);

  bool scoresSeen = false;
  bool startChecked = false;

  for (int i = 0; i < 900000; i++) {
    sim.step();
    const StatePacket& st = sim.host().state();

    if (!startChecked && st.phase == PH_ANNOUNCE && st.round == 1) {
      startChecked = true;
      r.slotsOkAtStart = slotsWithoutGap(st) &&
                         st.joined_mask == (uint8_t)((1u << players) - 1u);
    }
    if (st.phase == PH_ANNOUNCE || st.phase == PH_COUNTDOWN || st.phase == PH_REACT_WAIT ||
        st.phase == PH_GO || st.phase == PH_SHAKE) {
      if (st.mode == MODE_REACTION) r.sawReaction = true;
      if (st.mode == MODE_SHAKE) r.sawShake = true;
    }
    if (st.phase == PH_RESULTS) {
      if (!r.sawResults) r.firstResultsMs = (uint32_t)(sim.nowMs() - (uint32_t)(startUs / 1000u));
      r.sawResults = true;
      if ((st.flags & STF_SCORES) && !scoresSeen) {
        scoresSeen = true;
        if (st.winner != 0) r.roundsWithWinner++;
      }
    } else {
      scoresSeen = false;
    }
    if (st.phase == PH_FINAL) {
      r.sawFinal = true;
      r.roundsPlayed = st.round;
      int sum = 0;
      for (int k = 0; k < RTD_MAX_PLAYERS; k++) sum += st.players[k].score;
      r.scoreSum = sum;
    }
    if (r.sawFinal && st.phase == PH_IDLE) {
      r.sawIdle = true;
      break;
    }
  }
  if (r.sawIdle) {
    sim.runUntilIdle(5000, 2000);
    r.allIdle = sim.allSticksIdle();
  }
  return r;
}

/* ===================================================================== */
/* T2 happy path                                                         */
/* ===================================================================== */

static void testT2() {
  GameResult r = playGame(0, 2024, 4, 5, 0);
  CHECK(r.sawFinal);
  CHECK(r.sawIdle);
  CHECK(r.allIdle);
  CHECK(r.slotsOkAtStart);
  CHECK(r.roundsPlayed == 5);
  CHECK(r.roundsWithWinner > 0);
  CHECK(r.scoreSum == r.roundsWithWinner);
  CHECK(r.sawReaction);
  CHECK(r.sawShake);
}

/* ===================================================================== */
/* T3 join order                                                         */
/* ===================================================================== */

static bool nextPerm(int* a, int n) {
  int i = n - 2;
  while (i >= 0 && a[i] >= a[i + 1]) i--;
  if (i < 0) return false;
  int j = n - 1;
  while (a[j] <= a[i]) j--;
  int t = a[i]; a[i] = a[j]; a[j] = t;
  for (int l = i + 1, r = n - 1; l < r; l++, r--) {
    t = a[l]; a[l] = a[r]; a[r] = t;
  }
  return true;
}

static void testT3() {
  {
    Sim sim(3001);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    sim.beginWith(cfg, TIME_NONE, 0);
    for (uint8_t id = 1; id <= 4; id++) sim.press(id, 5, 100);
    CHECK(waitJoined(sim, 0x0F, 5000));
    CHECK(slotsWithoutGap(sim.host().state()));
  }

  int perm[4] = {1, 2, 3, 4};
  int permCount = 0;
  do {
    for (int pass = 0; pass < 2; pass++) {
      uint32_t seed = (uint32_t)(3100 + permCount * 8 + pass);
      Sim sim(seed);
      uint8_t cfg[CFG_SLOTS];
      defaultCfg(cfg);
      cfg[CFG_VOICE_GUIDE] = 0;
      cfg[CFG_JOIN_TIME] = 3;
      sim.beginWith(cfg, TIME_NONE, 0);
      if (pass == 1) sim.setLoss(30);

      for (int k = 0; k < 4; k++) {
        sim.press((uint8_t)perm[k], 50 + 200 * (uint32_t)k, 100);
      }

      bool joined = false;
      for (int i = 0; i < 8000; i++) {
        if (sim.host().state().joined_mask == 0x0F) { joined = true; break; }
        if (i % 700 == 699) {
          /* a player presses again after a lost request */
          for (int k = 0; k < 4; k++) {
            if (sim.stick((uint8_t)perm[k]).assignedSlot() == 0) {
              sim.press((uint8_t)perm[k], 0, 100);
            }
          }
        }
        sim.step();
      }
      if (joined) {
        CHECK(slotsWithoutGap(sim.host().state()));
        /* every stick ended up in its own slot */
        for (int k = 0; k < 4; k++) {
          CHECK(slotOf(sim.host().state(), (uint8_t)perm[k]) != 0);
        }
      } else {
        CHECK(!"all four sticks joined");
      }
    }
    permCount++;
  } while (nextPerm(perm, 4));

  CHECK(permCount == 24);

  /* lossless radio: every first press gets the lowest free slot within one round trip */
  {
    Sim sim(3002);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 10;
    sim.beginWith(cfg, TIME_NONE, 0);

    const uint8_t order[4] = {3, 1, 4, 2};
    for (int k = 0; k < 4; k++) {
      uint8_t id = order[k];
      sim.press(id, 0, 100);
      bool got = false;
      for (int i = 0; i < 12; i++) {   /* press, request, CMD_OK: one radio round trip */
        sim.step();
        if (sim.stick(id).assignedSlot() == (uint8_t)(k + 1)) { got = true; break; }
      }
      CHECK(got);
      CHECK(slotOf(sim.host().state(), id) == (uint8_t)(k + 1));
    }
    CHECK(slotsWithoutGap(sim.host().state()));
  }

  /* 30 % loss: four presses in a fixed order end in four distinct slots within 1 s */
  for (int trial = 0; trial < 4; trial++) {
    Sim sim((uint32_t)(3200 + trial));
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 10;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setLoss(30);

    const uint8_t order[4] = {2, 4, 1, 3};
    for (int k = 0; k < 4; k++) sim.press(order[k], 30 + 40 * (uint32_t)k, 100);
    sim.run(1000);

    const StatePacket& st = sim.host().state();
    uint8_t seen = 0;
    for (int k = 0; k < 4; k++) {
      uint8_t slot = slotOf(st, order[k]);
      CHECK(slot != 0);
      CHECK(sim.stick(order[k]).assignedSlot() == slot);
      if (slot != 0) {
        CHECK(!(seen & (uint8_t)(1u << (slot - 1))));
        seen |= (uint8_t)(1u << (slot - 1));
      }
    }
    CHECK(slotsWithoutGap(st));
  }
}

/* ===================================================================== */
/* T4 join edge cases                                                    */
/* ===================================================================== */

static void testT4() {
  {
    Sim sim(401);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(sim.runUntilPhase(PH_ANNOUNCE, 8000));
    CHECK(sim.host().state().joined_mask == 0x03);
  }

  {
    Sim sim(402);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.press(1, 10, 100);
    CHECK(sim.runUntilPhase(PH_JOIN, 2000));
    bool back = false;
    for (int i = 0; i < 20000 && !back; i++) {
      sim.step();
      if (sim.host().phase() == PH_IDLE) back = true;
    }
    CHECK(back);
    CHECK(sim.host().state().joined_mask == 0);
    sim.run(500);
    CHECK(sim.stick(1).state() == JS_IDLE);
    CHECK(sim.stick(1).assignedSlot() == 0);
  }

  {
    Sim sim(403);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.press(1, 10, 100);
    sim.press(2, 200, 100);
    CHECK(waitJoined(sim, 0x03, 3000));
    uint8_t before[RTD_MAX_PLAYERS];
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) before[i] = sim.host().state().players[i].stick_id;
    sim.press(1, 10, 100);
    sim.run(300);
    for (int i = 0; i < RTD_MAX_PLAYERS; i++) {
      CHECK(sim.host().state().players[i].stick_id == before[i]);
    }
    CHECK(sim.host().state().joined_mask == 0x03);
  }

  {
    Sim sim(404);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 10;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 3000));
    sim.run(100);
    CHECK(sim.host().state().flags & STF_CAN_START);
    sim.display().sendAction(ACT_START_NOW, 0);
    sim.run(100);
    CHECK(sim.runUntilPhase(PH_ANNOUNCE, 2000));
  }
}

/* ===================================================================== */
/* T5 GO fairness                                                        */
/* ===================================================================== */

static void goFairness(uint32_t seed, bool dropFrames) {
  Sim sim(seed);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 1;
  sim.beginWith(cfg, TIME_NONE, 0);

  for (uint8_t id = 1; id <= 4; id++) sim.setPlayer(id, PL_NORMAL, 250, 0);
  joinSticks(sim, 4, 20);
  CHECK(waitJoined(sim, 0x0F, 5000));
  CHECK(sim.runUntilPhase(PH_REACT_WAIT, 10000));

  sim.setLoss(30);
  sim.setJitterMs(3);
  sim.setDuplicatePercent(10);
  if (dropFrames) {
    sim.dropNextBroadcastGo();
    for (uint8_t id = 1; id <= 4; id++) sim.dropGoFrames(id, 1);
  }

  bool gotResults = false;
  for (int i = 0; i < 60000; i++) {
    sim.step();
    if (sim.host().phase() == PH_RESULTS) { gotResults = true; break; }
  }
  CHECK(gotResults);

  const StatePacket& st = sim.host().state();
  uint64_t t0 = sim.hostGoT0Us();
  for (uint8_t id = 1; id <= 4; id++) {
    uint8_t slot = slotOf(st, id);
    CHECK(slot != 0);
    if (slot == 0) continue;
    CHECK(st.players[slot - 1].status == PS_DONE);
    int reported = st.players[slot - 1].time_ms;
    CHECK(idiff(reported, 250) <= 2);
    uint64_t go = sim.stickGoUs(id);
    CHECK(go >= t0);
    CHECK(go - t0 <= 10000);
  }
}

static void testT5() {
  goFairness(501, false);
  goFairness(502, true);
}

/* ===================================================================== */
/* T6 false start                                                        */
/* ===================================================================== */

static void falseStartCase(uint32_t seed, uint8_t kind, uint32_t earlyMs, uint32_t secondMs,
                           bool expectPenalty, int expectMs) {
  Sim sim(seed);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 1;
  sim.beginWith(cfg, TIME_NONE, 0);

  sim.setPlayer(1, kind, 250, 0);
  sim.setPlayer(2, PL_NORMAL, 400, 0);
  sim.player[1].earlyMs = earlyMs;
  sim.player[1].secondMs = secondMs;

  sim.press(1, 10, 100);
  sim.press(2, 20, 100);
  CHECK(waitJoined(sim, 0x03, 5000));

  bool reached = sim.runUntilPhase(PH_RESULTS, 40000);
  CHECK(reached);
  const StatePacket& st = sim.host().state();
  uint8_t slot = slotOf(st, 1);
  CHECK(slot != 0);
  if (slot == 0) return;

  if (expectPenalty) {
    CHECK(st.players[slot - 1].status == PS_PENALTY);
    CHECK(st.players[slot - 1].time_ms == TIME_PENALTY);
  } else {
    CHECK(st.players[slot - 1].status == PS_DONE);
    CHECK(idiff(st.players[slot - 1].time_ms, expectMs) <= 2);
  }
}

static void testT6() {
  /* press 500 ms before GO: false start */
  falseStartCase(601, PL_EARLY, 500, 0, true, 0);
  /* button held at GO: false start */
  falseStartCase(602, PL_HOLD, 100, 0, true, 0);
  /* press inside the blanking window and hold: false start */
  falseStartCase(603, PL_BLANK, 0, 0, true, 0);
  /* press 1500 ms before GO and again 250 ms after: valid 250 ms */
  falseStartCase(604, PL_EARLY, 1500, 250, false, 250);
}

/* ===================================================================== */
/* T7 lost results                                                       */
/* ===================================================================== */

static void testT7() {
  Sim sim(701);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 1;
  cfg[CFG_SCORING] = SCORING_CLASSIC;
  cfg[CFG_BONUS] = BONUS_OFF;
  sim.beginWith(cfg, TIME_NONE, 0);

  sim.setPlayer(1, PL_NORMAL, 200, 0);
  sim.setPlayer(2, PL_NORMAL, 300, 0);
  sim.press(1, 10, 100);
  sim.press(2, 20, 100);
  CHECK(waitJoined(sim, 0x03, 5000));

  sim.setStickToHostLoss(1, 50);
  sim.setDuplicatePercent(25);

  bool reached = sim.runUntilPhase(PH_RESULTS, 40000);
  CHECK(reached);
  const StatePacket& st = sim.host().state();
  uint8_t s1 = slotOf(st, 1);
  uint8_t s2 = slotOf(st, 2);
  CHECK(s1 != 0 && s2 != 0);
  CHECK(st.players[s1 - 1].status == PS_DONE);
  CHECK(st.players[s2 - 1].status == PS_DONE);

  /* the scores are shown after 3 s; then a duplicate result must change nothing */
  sim.run(3500);
  const StatePacket& after = sim.host().state();
  uint8_t scores[RTD_MAX_PLAYERS];
  for (int i = 0; i < RTD_MAX_PLAYERS; i++) scores[i] = after.players[i].score;
  int sum = 0;
  for (int i = 0; i < RTD_MAX_PLAYERS; i++) sum += scores[i];
  CHECK(sum == 1);

  GamePacket dup;
  buildPacket(&dup, ID_HOST, ID_STICK1, CMD_REACTION_DONE, 100);
  sim.host().onPacket((const uint8_t*)&dup, PACKET_SIZE, (uint32_t)sim.nowUs());
  sim.run(100);
  const StatePacket& later = sim.host().state();
  int sum2 = 0;
  for (int i = 0; i < RTD_MAX_PLAYERS; i++) sum2 += later.players[i].score;
  CHECK(sum2 == sum);
}

/* ===================================================================== */
/* T8 stick failures                                                     */
/* ===================================================================== */

static void testT8() {
  {
    Sim sim(801);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.setPlayer(3, PL_NORMAL, 300, 0);
    joinSticks(sim, 3, 20);
    CHECK(waitJoined(sim, 0x07, 5000));
    CHECK(sim.runUntilPhase(PH_GO, 20000));

    sim.rebootStick(1);
    sim.run(200);
    CHECK(sim.stick(1).state() == JS_IDLE);

    bool results = sim.runUntilPhase(PH_RESULTS, 40000);
    CHECK(results);
    const StatePacket& st = sim.host().state();
    uint8_t slot = slotOf(st, 1);
    CHECK(slot != 0);
    if (slot != 0) CHECK(st.players[slot - 1].status == PS_PENALTY);

    CHECK(sim.runUntilPhase(PH_FINAL, 20000));
    CHECK(sim.runUntilIdle(30000, 2000));
    CHECK(sim.allSticksIdle());
  }

  {
    Sim sim(802);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    CHECK(sim.runUntilPhase(PH_GO, 20000));

    sim.setStickAlive(2, false);
    bool results = sim.runUntilPhase(PH_RESULTS, 40000);
    CHECK(results);
    const StatePacket& st = sim.host().state();
    uint8_t slot = slotOf(st, 2);
    CHECK(slot != 0);
    if (slot != 0) CHECK(st.players[slot - 1].status == PS_PENALTY);
    CHECK(sim.runUntilPhase(PH_FINAL, 20000));
    CHECK(sim.runUntilIdle(30000, 2000));
  }
}

/* ===================================================================== */
/* T9 ties                                                               */
/* ===================================================================== */

static void testT9() {
  /* two rounds with no winner end the game */
  {
    Sim sim(901);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 5;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NONE, 0, 0);
    sim.setPlayer(2, PL_NONE, 0, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    bool back = false;
    bool sawRound2 = false;
    uint8_t maxRound = 0;
    for (int i = 0; i < 90000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.round > maxRound) maxRound = st.round;
      if (st.round >= 2) sawRound2 = true;
      if (sawRound2 && st.phase == PH_IDLE) { back = true; break; }
    }
    CHECK(back);
    CHECK(sawRound2);
    CHECK(maxRound == 2);
  }

  /* two way deuce: only a lead of CFG_DEUCE_LEAD ends it */
  {
    Sim sim(902);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_DEUCE_LEAD] = 2;
    cfg[CFG_SCORING] = SCORING_CLASSIC;
    cfg[CFG_BONUS] = BONUS_OFF;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NONE, 0, 0);
    sim.setPlayer(2, PL_NONE, 0, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));

    bool deuce = false;
    for (int i = 0; i < 30000; i++) {
      sim.step();
      if (sim.host().state().flags & STF_DEUCE) { deuce = true; break; }
    }
    CHECK(deuce);

    /* stick 1 wins the next deuce round */
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 400, 0);
    CHECK(sim.runUntilPhase(PH_RESULTS, 40000));
    sim.run(6500);
    CHECK(sim.host().phase() != PH_FINAL);
    CHECK(sim.host().state().players[0].score == 1);

    /* and the round after that, so the lead reaches 2 */
    bool final = false;
    for (int i = 0; i < 60000; i++) {
      sim.step();
      if (sim.host().phase() == PH_FINAL) { final = true; break; }
    }
    CHECK(final);
    CHECK(sim.host().state().players[0].score == 2);
    CHECK(sim.host().state().winner == 1);
  }

  /* three way tie: sudden death ends with the first won round */
  {
    Sim sim(903);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_SCORING] = SCORING_CLASSIC;
    cfg[CFG_BONUS] = BONUS_OFF;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NONE, 0, 0);
    sim.setPlayer(2, PL_NONE, 0, 0);
    sim.setPlayer(3, PL_NONE, 0, 0);
    joinSticks(sim, 3, 20);
    CHECK(waitJoined(sim, 0x07, 5000));

    bool deuce = false;
    for (int i = 0; i < 40000; i++) {
      sim.step();
      if ((sim.host().state().flags & STF_DEUCE) && sim.host().state().round == 2) {
        deuce = true;
        break;
      }
    }
    CHECK(deuce);
    CHECK(sim.host().phase() != PH_FINAL);

    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 400, 0);
    sim.setPlayer(3, PL_NORMAL, 600, 0);
    bool final = false;
    for (int i = 0; i < 70000; i++) {
      sim.step();
      if (sim.host().phase() == PH_FINAL) { final = true; break; }
    }
    CHECK(final);
    CHECK(sim.host().state().winner == 1);
    /* the stick that was out got CMD_IDLE */
    CHECK(sim.stick(3).state() == JS_IDLE || sim.host().state().players[2].score == 0);
  }
}

/* ===================================================================== */
/* T10 settings                                                          */
/* ===================================================================== */

static void testT10() {
  {
    Sim sim(1001);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    sim.beginWith(cfg, TIME_NONE, 0);

    sim.display().setCfg(CFG_ROUNDS, 200);
    sim.run(200);
    CHECK(sim.host().state().cfg[CFG_ROUNDS] == RTD_CFG_MAX[CFG_ROUNDS]);

    uint8_t before = sim.host().state().cfg[CFG_VOLUME];
    GamePacket p;
    buildPacket(&p, ID_HOST, ID_DISPLAY, CMD_CFG_SET, (uint16_t)((99 << 8) | 5));
    sim.host().onPacket((const uint8_t*)&p, PACKET_SIZE, (uint32_t)sim.nowUs());
    sim.run(50);
    CHECK(sim.host().state().cfg[CFG_VOLUME] == before);

    uint16_t calls = sim.saveSettingsCalls;
    sim.display().setCfg(CFG_VOLUME, 4);
    sim.run(100);
    CHECK(sim.saveSettingsCalls > calls);
    CHECK(sim.host().state().cfg[CFG_VOLUME] == 4);
  }

  {
    /* rounds and mode are latched when the game starts */
    Sim sim(1002);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 3;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    CHECK(sim.runUntilPhase(PH_ANNOUNCE, 5000));

    sim.display().setCfg(CFG_ROUNDS, 1);
    sim.display().setCfg(CFG_GAME_MODE, 2);
    sim.run(200);
    CHECK(sim.host().state().total_rounds == 3);

    bool final = false;
    for (int i = 0; i < 200000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.phase == PH_ANNOUNCE || st.phase == PH_REACT_WAIT) {
        CHECK(st.mode == MODE_REACTION);
      }
      if (st.phase == PH_FINAL) { final = true; break; }
    }
    CHECK(final);
    CHECK(sim.host().state().round == 3);
  }

  {
    /* reaction only */
    GameResult r = playGame(0, 1003, 2, 2, 1);
    CHECK(r.sawFinal);
  }

  {
    /* shake only */
    Sim sim(1004);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 2;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 0, 80000);
    sim.setPlayer(2, PL_NORMAL, 0, 100000);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    bool sawShake = false;
    for (int i = 0; i < 120000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.phase == PH_SHAKE) {
        sawShake = true;
        CHECK(st.mode == MODE_SHAKE);
      }
      if (st.phase == PH_FINAL) break;
    }
    CHECK(sawShake);
  }

  {
    /* short voice guide queues no long instruction */
    Sim sim(1005);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 0;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 60000);
    sim.setPlayer(2, PL_NORMAL, 250, 70000);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    for (int i = 0; i < 120000; i++) {
      sim.step();
      if (sim.host().phase() == PH_FINAL) break;
    }
    CHECK(sim.soundCount(SND_REACTION_INSTRUCT) == 0);
    CHECK(sim.soundCount(SND_YOU_WILL_SHAKE) == 0);
  }

  {
    /* full voice guide does queue them */
    Sim sim(1006);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 1;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 0;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 60000);
    sim.setPlayer(2, PL_NORMAL, 250, 70000);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));
    for (int i = 0; i < 120000; i++) {
      sim.step();
      if (sim.host().phase() == PH_FINAL) break;
    }
    CHECK(sim.soundCount(SND_REACTION_INSTRUCT) >= 1);
    CHECK(sim.soundCount(SND_YOU_WILL_SHAKE) >= 1);
  }
}

/* ===================================================================== */
/* T11 actions                                                           */
/* ===================================================================== */

static void abortAtPhase(uint32_t seed, uint8_t phase, uint8_t modeCfg) {
  Sim sim(seed);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = modeCfg;
  sim.beginWith(cfg, TIME_NONE, 0);
  for (uint8_t id = 1; id <= 3; id++) {
    sim.setPlayer(id, PL_NORMAL, 200 + 50 * id, 60000);
  }
  if (phase == PH_IDLE) {
    sim.run(100);
    CHECK(sim.host().phase() == PH_IDLE);
  } else {
    joinSticks(sim, 3, 20);
    CHECK(sim.runUntilPhase(phase, 90000));
  }
  sim.display().sendAction(ACT_ABORT_GAME, 0);

  bool ok = false;
  for (int i = 0; i < 40; i++) {
    sim.step();
    if (sim.host().phase() == PH_IDLE && sim.allSticksIdle()) {
      ok = true;
      break;
    }
  }
  CHECK(ok);
  CHECK(sim.host().phase() == PH_IDLE);
  CHECK(sim.allSticksIdle());
  CHECK(sim.host().state().joined_mask == 0);
}

static void testT11() {
  abortAtPhase(1101, PH_IDLE, 1);
  abortAtPhase(1102, PH_JOIN, 1);
  abortAtPhase(1103, PH_ANNOUNCE, 1);
  abortAtPhase(1104, PH_REACT_WAIT, 1);
  abortAtPhase(1105, PH_GO, 1);
  abortAtPhase(1106, PH_RESULTS, 1);
  abortAtPhase(1107, PH_FINAL, 1);
  abortAtPhase(1108, PH_COUNTDOWN, 2);
  abortAtPhase(1109, PH_SHAKE, 2);

  {
    /* identify, reset record, factory reset */
    Sim sim(1110);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, 400, 10);
    CHECK(sim.host().state().best_alltime_ms == 400);

    uint16_t vib = sim.hostSent(CMD_VIBRATE);
    sim.display().sendAction(ACT_IDENTIFY, 3);
    sim.run(50);
    CHECK(sim.hostSent(CMD_VIBRATE) == vib + 1);

    sim.display().sendAction(ACT_RESET_BEST, 0);
    sim.run(50);
    CHECK(sim.host().state().best_alltime_ms == TIME_NONE);
    CHECK(sim.host().state().best_session_ms == TIME_NONE);

    sim.display().setCfg(CFG_VOLUME, 2);
    sim.run(50);
    CHECK(sim.host().state().cfg[CFG_VOLUME] == 2);
    sim.display().sendAction(ACT_FACTORY_RESET, 0);
    sim.run(50);
    for (int k = 1; k <= CFG_COUNT; k++) {
      CHECK(sim.host().state().cfg[k] == RTD_CFG_DEF[k]);
    }
  }
}

/* ===================================================================== */
/* T12 clock wrap                                                        */
/* ===================================================================== */

static void testT12() {
  /* start 5 s before the 32 bit microsecond wrap */
  uint64_t microsWrap = 0xFFFFFFFFull - 5000000ull;
  GameResult r1 = playGame(microsWrap, 1201, 4, 5, 0);
  CHECK(r1.sawFinal);
  CHECK(r1.sawIdle);
  CHECK(r1.allIdle);
  CHECK(r1.scoreSum == r1.roundsWithWinner);

  /* start 5 s before the 32 bit millisecond wrap */
  uint64_t millisWrap = (0xFFFFFFFFull - 5ull) * 1000ull;
  GameResult r2 = playGame(millisWrap, 1202, 3, 5, 0);
  CHECK(r2.sawFinal);
  CHECK(r2.sawIdle);
  CHECK(r2.allIdle);
  CHECK(r2.scoreSum == r2.roundsWithWinner);
}

/* ===================================================================== */
/* T13 OTA sequence                                                      */
/* ===================================================================== */

static void testT13() {
  {
    /* the joystick firmware itself answers CMD_OTA_BEGIN */
    Sim sim(1303);
    sim.run(200);
    GamePacket p;
    buildPacket(&p, ID_STICK1, ID_HOST, CMD_OTA_BEGIN,
                (uint16_t)(((uint16_t)FW_VERSION_BYTE << 8) | FW_VERSION_PATCH));
    sim.stick(1).onPacket((const uint8_t*)&p, PACKET_SIZE, (uint32_t)sim.nowUs());
    sim.run(5);
    CHECK(sim.stick(1).state() == JS_IDLE);
    CHECK(sim.hostRecv(CMD_OTA_STATUS) == 1);

    uint16_t acks = sim.hostRecv(CMD_ACK);
    uint16_t status = sim.hostRecv(CMD_OTA_STATUS);
    buildPacket(&p, ID_STICK1, ID_HOST, CMD_OTA_BEGIN, (uint16_t)((0x30 << 8) | 1));
    sim.stick(1).onPacket((const uint8_t*)&p, PACKET_SIZE, (uint32_t)sim.nowUs());
    sim.run(5);
    CHECK(sim.stick(1).state() == JS_OTA);
    CHECK(sim.hostRecv(CMD_ACK) == acks + 1);
    CHECK(sim.hostRecv(CMD_OTA_STATUS) == status + 1);
  }

  {
    Sim sim(1301);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setStickFw(1, (uint8_t)((2 << 4) | 0), 0);
    sim.setStickFw(3, (uint8_t)((2 << 4) | 0), 0);
    sim.setStickImage(true, FW_VERSION_BYTE, FW_VERSION_PATCH);
    sim.run(1500);
    CHECK(sim.host().state().sticks_online == 0x0F);
    CHECK(sim.host().state().flags & STF_UPDATE_AVAILABLE);

    sim.display().sendAction(ACT_OTA_STICKS, 0);
    sim.run(50);
    CHECK(sim.host().phase() == PH_MAINT);

    bool done = false;
    for (int i = 0; i < 40000; i++) {
      sim.step();
      uint8_t s = sim.host().state().ota_state;
      if (s == OTA_DONE || s == OTA_ERROR) { done = true; break; }
    }
    CHECK(done);
    CHECK(sim.host().state().ota_state == OTA_DONE);
    CHECK(sim.otaBeginCount() == 2);
    if (sim.otaBeginCount() == 2) {
      CHECK(sim.otaBeginOrder(0) == 1);
      CHECK(sim.otaBeginOrder(1) == 3);
    }
    CHECK(sim.host().state().stick_fw[0] == FW_VERSION_BYTE);
    CHECK(sim.host().state().stick_fw[2] == FW_VERSION_BYTE);
  }

  {
    /* a stick that never comes back times out after 120 s, the next one starts */
    Sim sim(1302);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setStickFw(1, (uint8_t)((2 << 4) | 0), 0);
    sim.setStickFw(3, (uint8_t)((2 << 4) | 0), 0);
    sim.setStickImage(true, FW_VERSION_BYTE, FW_VERSION_PATCH);
    sim.setOtaComesBack(1, false);
    sim.run(1500);

    sim.display().sendAction(ACT_OTA_STICKS, 0);
    sim.run(50);
    CHECK(sim.host().phase() == PH_MAINT);

    bool done = false;
    for (int i = 0; i < 200000; i++) {
      sim.step();
      uint8_t s = sim.host().state().ota_state;
      if (s == OTA_DONE || s == OTA_ERROR) { done = true; break; }
    }
    CHECK(done);
    CHECK(sim.host().state().ota_state == OTA_ERROR);
    CHECK(sim.otaBeginCount() == 2);
    if (sim.otaBeginCount() == 2) {
      CHECK(sim.otaBeginOrder(0) == 1);
      CHECK(sim.otaBeginOrder(1) == 3);
    }
  }
}

/* ===================================================================== */
/* T14 display link                                                      */
/* ===================================================================== */

static void testT14() {
  Sim sim(1401);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  sim.beginWith(cfg, TIME_NONE, 0);
  sim.run(300);
  CHECK(sim.display().hasState());
  CHECK(sim.display().linkUp());
  CHECK(sim.display().state().phase == sim.host().state().phase);

  StatePacket st;
  memcpy(&st, &sim.host().state(), sizeof(st));
  st.crc ^= 0xFF;
  CHECK(!sim.display().onPacket((const uint8_t*)&st, STATE_SIZE, sim.nowMs()));

  memcpy(&st, &sim.host().state(), sizeof(st));
  st.version = 3;
  st.crc = calcCRC8((const uint8_t*)&st, STATE_SIZE - 1);
  CHECK(!sim.display().onPacket((const uint8_t*)&st, STATE_SIZE, sim.nowMs()));

  memcpy(&st, &sim.host().state(), sizeof(st));
  st.crc = calcCRC8((const uint8_t*)&st, STATE_SIZE - 1);
  CHECK(!sim.display().onPacket((const uint8_t*)&st, STATE_SIZE - 1, sim.nowMs()));

  sim.setDisplayLoss(100);
  sim.run(2500);
  CHECK(!sim.display().linkUp());

  /* setCfg resends while the snapshot shows another value, at most 5 sends */
  {
    Sim sim2(1402);
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    sim2.beginWith(cfg, TIME_NONE, 0);
    sim2.run(300);
    CHECK(sim2.display().linkUp());

    sim2.setDisplayLoss(100);
    sim2.run(50);
    uint16_t before = sim2.displaySent(CMD_CFG_SET);
    sim2.display().setCfg(CFG_ROUNDS, 7);
    sim2.run(3000);
    uint16_t sends = (uint16_t)(sim2.displaySent(CMD_CFG_SET) - before);
    CHECK(sends == CFG_MAX_SENDS);
    CHECK(sim2.display().cfg(CFG_ROUNDS) == 7 || sim2.display().cfg(CFG_ROUNDS) != 0);
    CHECK(!sim2.display().cfgPending(CFG_ROUNDS));
  }

  /* a matching snapshot ends the resends */
  {
    Sim sim3(1403);
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    sim3.beginWith(cfg, TIME_NONE, 0);
    sim3.run(300);
    uint16_t before = sim3.displaySent(CMD_CFG_SET);
    sim3.display().setCfg(CFG_ROUNDS, 4);
    sim3.run(1500);
    uint16_t sends = (uint16_t)(sim3.displaySent(CMD_CFG_SET) - before);
    CHECK(sends >= 1);
    CHECK(sends <= 2);
    CHECK(sim3.display().cfg(CFG_ROUNDS) == 4);
    CHECK(sim3.host().state().cfg[CFG_ROUNDS] == 4);
  }
}

/* ===================================================================== */
/* T15 fuzz                                                              */
/* ===================================================================== */

static void testT15() {
  const int games = 2000;
  int failures = 0;

  for (int g = 0; g < games; g++) {
    uint32_t seed = (uint32_t)(g * 2654435761u + 7u);
    Sim sim(seed);

    int players = (int)trange(2, 4);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = (uint8_t)trange(0, 1);
    cfg[CFG_JOIN_TIME] = (uint8_t)trange(3, 5);
    cfg[CFG_ROUNDS] = (uint8_t)trange(1, 5);
    cfg[CFG_GAME_MODE] = (uint8_t)trange(0, 2);
    cfg[CFG_DEUCE_LEAD] = 1;
    cfg[CFG_SCORING] = (uint8_t)(g % 2);   /* both scoring rules */
    cfg[CFG_BONUS] = (uint8_t)(g % 3);     /* OFF, LAST and LAST_TWO */
    sim.beginWith(cfg, (uint16_t)trange(0, 900), (uint16_t)trange(0, 40));

    sim.setLoss((uint8_t)trange(0, 40));
    sim.setDuplicatePercent((uint8_t)trange(0, 20));
    sim.setJitterMs(3);

    for (int id = 1; id <= players; id++) {
      uint32_t kindRoll = trange(0, 99);
      uint8_t kind = PL_NORMAL;
      if (kindRoll < 10) kind = PL_NONE;
      else if (kindRoll < 20) kind = PL_EARLY;
      else if (kindRoll < 24) kind = PL_BLANK;
      sim.setPlayer((uint8_t)id, kind, trange(120, 1500), trange(50, 200) * 1000u);
      sim.player[id].earlyMs = trange(100, 900);
      sim.player[id].secondMs = trange(150, 600);
    }
    for (int id = 1; id <= players; id++) {
      sim.press((uint8_t)id, trange(0, 400), 100);
    }

    int dropAt = (int)trange(0, 3);          /* 0 = no dropout */
    uint8_t dropId = (uint8_t)trange(1, (uint32_t)players);
    bool dropped = false;

    bool bad = false;
    bool idle = false;
    bool leftIdle = false;
    uint32_t stable = 0;
    /* up to five shake rounds with 30 s timeouts each need more than 200 s */
    for (int i = 0; i < 600000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.phase != PH_IDLE) leftIdle = true;

      if (st.phase >= PH_COUNT) bad = true;
      if (!slotsWithoutGap(st)) bad = true;
      /* classic: a round adds at most 1 x factor to the winner; places: at most
         (active - 1) x factor to one player and active x (active - 1) / 2 x factor
         in total, with factor 2 in a bonus round and active <= RTD_MAX_PLAYERS */
      uint32_t scoreTotal = 0;
      for (int k = 0; k < RTD_MAX_PLAYERS; k++) {
        scoreTotal += st.players[k].score;
        if ((uint32_t)st.players[k].score >
            (uint32_t)st.round * (RTD_MAX_PLAYERS - 1) * 2) bad = true;
      }
      if (scoreTotal > (uint32_t)st.round *
                       (RTD_MAX_PLAYERS * (RTD_MAX_PLAYERS - 1) / 2) * 2) bad = true;

      if (!dropped && dropAt == 1 && st.phase == PH_GO && i > 200) {
        sim.setStickAlive(dropId, false);
        dropped = true;
      }

      if (leftIdle && sim.host().phase() == PH_IDLE && sim.allSticksIdle()) {
        stable++;
        if (stable >= 2000) { idle = true; break; }
      } else {
        stable = 0;
      }
    }
    if (!idle || bad || !leftIdle) failures++;
  }
  CHECK(failures == 0);
}

/* ===================================================================== */
/* T16 stale slot after a lost CMD_IDLE                                  */
/* ===================================================================== */

static void testT16() {
  Sim sim(1601);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  sim.beginWith(cfg, TIME_NONE, 0);

  sim.press(1, 10, 100);
  CHECK(waitJoined(sim, 0x01, 3000));
  sim.run(10);                     /* let CMD_OK reach the stick */
  CHECK(sim.stick(1).assignedSlot() == 1);

  /* the CMD_IDLE broadcast of the return to idle is lost */
  sim.dropNextBroadcastIdle();
  bool idle = false;
  for (int i = 0; i < 30000; i++) {
    sim.step();
    if (sim.host().phase() == PH_IDLE) { idle = true; break; }
  }
  CHECK(idle);
  CHECK(sim.stick(1).state() == JS_IDLE);
  CHECK(sim.stick(1).assignedSlot() == 1);   /* the lost broadcast leaves a stale slot */

  /* two beacons without this stick release it, within 2 s */
  bool cleared = false;
  for (int i = 0; i < 2000; i++) {
    sim.step();
    if (sim.stick(1).assignedSlot() == 0) { cleared = true; break; }
  }
  CHECK(cleared);
  CHECK(sim.stick(1).state() == JS_IDLE);

  /* a new press joins again and the host shows the stick */
  sim.press(1, 10, 100);
  CHECK(waitJoined(sim, 0x01, 3000));
  sim.run(10);
  CHECK(sim.stick(1).assignedSlot() == 1);
}

/* ===================================================================== */
/* T17 stale slot after a host reboot                                    */
/* ===================================================================== */

static void testT17() {
  Sim sim(1701);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 10;
  sim.beginWith(cfg, TIME_NONE, 0);

  sim.press(1, 10, 100);
  CHECK(waitJoined(sim, 0x01, 3000));
  sim.run(10);
  CHECK(sim.stick(1).assignedSlot() == 1);
  sim.run(600);

  /* the host board is replaced; the radio drops the CMD_IDLE it sends at boot */
  sim.setHostToStickLoss(1, 100);
  sim.rebootHost();
  sim.run(100);
  sim.setHostToStickLoss(1, 0);
  CHECK(sim.host().phase() == PH_IDLE);
  CHECK(sim.stick(1).state() == JS_IDLE);
  CHECK(sim.stick(1).assignedSlot() == 1);

  bool cleared = false;
  for (int i = 0; i < 2000; i++) {
    sim.step();
    if (sim.stick(1).assignedSlot() == 0) { cleared = true; break; }
  }
  CHECK(cleared);

  sim.press(1, 10, 100);
  CHECK(waitJoined(sim, 0x01, 3000));
  sim.run(10);
  CHECK(sim.stick(1).assignedSlot() == 1);
}

/* ===================================================================== */
/* T18 scores after 2^31 ms and a clock wrap inside the game             */
/* ===================================================================== */

static void testT18() {
  GameResult base = playGame(0, 1801, 4, 5, 0);
  CHECK(base.sawFinal);
  CHECK(base.roundsWithWinner > 0);

  /* 0x90000000 ms of uptime: now - 0 is past 2^31 */
  GameResult late = playGame(0x90000000ull * 1000ull, 1801, 4, 5, 0);
  CHECK(late.sawFinal);
  CHECK(late.scoreSum == base.scoreSum);

  /* the millisecond counter wraps inside the game */
  GameResult wrap = playGame(0xFFFFE000ull * 1000ull, 1801, 4, 5, 0);
  CHECK(wrap.sawFinal);
  CHECK(wrap.scoreSum == base.scoreSum);

  /* and it wraps in the middle of the first results phase, where scores are due */
  CHECK(base.sawResults);
  uint64_t wrapMs = 0xFFFFFFFFull - (uint64_t)(base.firstResultsMs + 7000);
  GameResult midWrap = playGame(wrapMs * 1000ull, 1801, 4, 5, 0);
  CHECK(midWrap.sawFinal);
  CHECK(midWrap.scoreSum == base.scoreSum);

  /* times first, then the scores after RESULTS_SCORES_MS */
  {
    Sim sim(1802);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 1;
    cfg[CFG_GAME_MODE] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 200, 0);
    sim.setPlayer(2, PL_NORMAL, 250, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));

    bool sawEarly = false;
    bool sawLate = false;
    uint32_t resMs = 0;
    bool inResults = false;
    for (int i = 0; i < 60000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.phase != PH_RESULTS) { inResults = false; continue; }
      if (!inResults) {
        inResults = true;
        resMs = sim.nowMs();
        CHECK(!(st.flags & STF_SCORES));
      }
      uint32_t ageMs = (uint32_t)(sim.nowMs() - resMs);
      if (ageMs >= 100 && ageMs < RESULTS_SCORES_MS - 100 && !(st.flags & STF_SCORES)) {
        sawEarly = true;
      }
      if (ageMs >= RESULTS_SCORES_MS + 100 && (st.flags & STF_SCORES)) sawLate = true;
      if (sawLate) break;
    }
    CHECK(sawEarly);
    CHECK(sawLate);
  }
}

/* ===================================================================== */
/* T19 OTA sticks keeps the maintenance state                            */
/* ===================================================================== */

static void testT19() {
  Sim sim(1901);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  sim.beginWith(cfg, TIME_NONE, 0);
  sim.setStickFw(1, (uint8_t)((2 << 4) | 0), 0);
  sim.setStickImage(true, FW_VERSION_BYTE, FW_VERSION_PATCH);
  sim.run(1000);

  sim.display().sendAction(ACT_MAINTENANCE, 1);
  sim.run(50);
  CHECK(sim.host().phase() == PH_MAINT);
  CHECK(sim.maintEnterCalls == 1);
  CHECK(sim.maintExitCalls == 0);

  sim.display().sendAction(ACT_OTA_STICKS, 0);
  sim.run(200);
  CHECK(sim.maintExitCalls == 0);
  CHECK(sim.maintEnterCalls == 1);
  CHECK(sim.host().phase() == PH_MAINT);
  CHECK(sim.otaBeginCount() >= 1);
  CHECK(sim.otaBeginOrder(0) == 1);

  /* a leave or abort from the display while the sticks update does nothing */
  sim.display().sendAction(ACT_MAINTENANCE, 0);
  sim.run(100);
  sim.display().sendAction(ACT_ABORT_GAME, 0);
  sim.run(100);
  CHECK(sim.host().phase() == PH_MAINT);
  CHECK(sim.maintExitCalls == 0);
}

/* ===================================================================== */
/* T20 idle press-join prompt plays once, then every 45 s                */
/* ===================================================================== */

static void testT20() {
  /* default settings: the voice guide is on and no stick ever presses */
  Sim sim(2001);

  sim.run(10000);
  CHECK(sim.soundCount(SND_PRESS_TO_JOIN) == 1);

  sim.run(40000);
  CHECK(sim.soundCount(SND_PRESS_TO_JOIN) == 2);
}

/* ===================================================================== */
/* T21 the shake start uses the GO age                                   */
/* ===================================================================== */

static void testT21() {
  Sim sim(2101);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 2;
  sim.beginWith(cfg, TIME_NONE, 0);
  sim.setJitterMs(0);

  /* the same physical shake for both: it starts at the real GO instant */
  sim.setPlayer(1, PL_NONE, 0, 120000);
  sim.setPlayer(2, PL_NONE, 0, 120000);
  sim.press(1, 10, 100);
  sim.press(2, 20, 100);
  CHECK(waitJoined(sim, 0x03, 5000));

  /* stick 2 loses the broadcast GO and the first unicast (age 0), so only the retry
     with its age reaches it */
  sim.dropGoFrames(2, 2);

  bool reached = sim.runUntilPhase(PH_RESULTS, 40000);
  CHECK(reached);
  const StatePacket& st = sim.host().state();
  uint8_t s1 = slotOf(st, 1);
  uint8_t s2 = slotOf(st, 2);
  CHECK(s1 != 0 && s2 != 0);
  if (s1 == 0 || s2 == 0) return;
  CHECK(st.players[s1 - 1].status == PS_DONE);
  CHECK(st.players[s2 - 1].status == PS_DONE);
  int t1 = st.players[s1 - 1].time_ms;
  int t2 = st.players[s2 - 1].time_ms;
  CHECK(t1 > 1000);
  CHECK(idiff(t1, t2) <= 2);
}

/* ===================================================================== */
/* T22 a button edge older than the GO is a false start                  */
/* ===================================================================== */

static void testT22() {
  Sim sim(2201);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 1;
  sim.beginWith(cfg, TIME_NONE, 0);
  sim.setPlayer(1, PL_NORMAL, 600, 0);
  sim.setPlayer(2, PL_NORMAL, 400, 0);
  sim.press(1, 10, 100);
  sim.press(2, 20, 100);
  CHECK(waitJoined(sim, 0x03, 5000));
  CHECK(sim.runUntilPhase(PH_GO, 20000));

  /* let the GO frame reach the sticks */
  sim.run(10);
  CHECK(sim.stick(1).state() == JS_REACTION_TIMING);
  uint32_t goUs = (uint32_t)sim.stickGoUs(1);
  CHECK(goUs != 0);

  /* an edge timestamped 500 us before GO, delivered after the GO arrived */
  sim.stick(1).onButtonFall((uint32_t)(goUs - 500u));
  CHECK(sim.stick(1).lastResult() != 0xFFFEu);
  sim.run(50);

  const StatePacket& st = sim.host().state();
  uint8_t slot = slotOf(st, 1);
  CHECK(slot != 0);
  if (slot != 0) {
    CHECK(st.players[slot - 1].status == PS_PENALTY);
    CHECK(st.players[slot - 1].time_ms == TIME_PENALTY);
  }
}

/* ===================================================================== */
/* T23 service report frame (tests/test_service.cpp)                     */
/* ===================================================================== */

int serviceChecks();   /* number of failed checks */

static void testT23() { CHECK(serviceChecks() == 0); }

/* ===================================================================== */
/* T24 places scoring                                                    */
/* ===================================================================== */

/* One reaction round with one script per slot 1..players; returns the scores
   after the first round (PH_RESULTS with STF_SCORES). */
static bool firstRoundScores(uint32_t seed, int players, uint8_t scoring, uint8_t bonus,
                             const uint8_t* kinds, const uint32_t* reactMs, uint8_t* scores) {
  Sim sim(seed);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 1;
  cfg[CFG_GAME_MODE] = 1;
  cfg[CFG_SCORING] = scoring;
  cfg[CFG_BONUS] = bonus;
  sim.beginWith(cfg, TIME_NONE, 0);

  for (int id = 1; id <= players; id++) {
    sim.setPlayer((uint8_t)id, kinds[id - 1], reactMs[id - 1], 0);
    sim.player[id].earlyMs = 500;
    sim.player[id].secondMs = 0;
  }
  joinSticks(sim, players, 20);
  if (!waitJoined(sim, (uint8_t)((1u << players) - 1u), 8000)) return false;

  for (int i = 0; i < 200000; i++) {
    sim.step();
    const StatePacket& st = sim.host().state();
    if (st.phase == PH_RESULTS && (st.flags & STF_SCORES)) {
      for (int k = 0; k < RTD_MAX_PLAYERS; k++) scores[k] = st.players[k].score;
      return true;
    }
  }
  return false;
}

static void testT24() {
  uint8_t kinds[4] = {PL_NORMAL, PL_NORMAL, PL_NORMAL, PL_NORMAL};
  uint32_t react[4] = {150, 300, 450, 600};
  uint8_t s[RTD_MAX_PLAYERS];

  /* four active players with four different valid times: 3 / 2 / 1 / 0 */
  CHECK(firstRoundScores(2401, 4, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 3 && s[1] == 2 && s[2] == 1 && s[3] == 0);

  /* a false start gets 0 and counts as a player that the others beat */
  kinds[0] = PL_EARLY;   /* 500 ms before GO */
  CHECK(firstRoundScores(2402, 4, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 0 && s[1] == 3 && s[2] == 2 && s[3] == 1);

  /* two players with the same time do not beat each other */
  kinds[0] = PL_NORMAL;
  react[0] = 300;
  react[1] = 300;
  react[2] = 450;
  react[3] = 600;
  CHECK(firstRoundScores(2403, 4, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 2 && s[1] == 2 && s[2] == 1 && s[3] == 0);

  /* three active players: 2 / 1 / 0 */
  react[0] = 150;
  react[1] = 300;
  react[2] = 450;
  CHECK(firstRoundScores(2404, 3, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 2 && s[1] == 1 && s[2] == 0 && s[3] == 0);

  /* two active players: 1 / 0, the same as classic */
  react[0] = 150;
  react[1] = 300;
  CHECK(firstRoundScores(2405, 2, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 1 && s[1] == 0 && s[2] == 0 && s[3] == 0);

  /* nobody finishes: nobody scores */
  kinds[0] = PL_NONE;
  kinds[1] = PL_NONE;
  kinds[2] = PL_NONE;
  kinds[3] = PL_NONE;
  CHECK(firstRoundScores(2406, 4, SCORING_PLACES, BONUS_OFF, kinds, react, s));
  CHECK(s[0] == 0 && s[1] == 0 && s[2] == 0 && s[3] == 0);
}

/* ===================================================================== */
/* T25 bonus round                                                       */
/* ===================================================================== */

struct BonusTrace {
  bool announce[32];   /* STF_BONUS in the first snapshot of round r (1-based) */
  bool results[32];    /* STF_BONUS seen in PH_RESULTS of round r */
  bool everBonus;
  bool bonusInFinal;
  bool bonusInDeuce;
};

/* Play a whole game with four reaction players and record the flag and scores. */
static bool traceGame(uint32_t seed, uint8_t rounds, uint8_t scoring, uint8_t bonus,
                      const uint32_t* reactMs, uint8_t* finalScores,
                      uint8_t* finalWinner, BonusTrace* tr) {
  Sim sim(seed);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = rounds;
  cfg[CFG_GAME_MODE] = 1;
  cfg[CFG_SCORING] = scoring;
  cfg[CFG_BONUS] = bonus;
  sim.beginWith(cfg, TIME_NONE, 0);

  for (uint8_t id = 1; id <= 4; id++) sim.setPlayer(id, PL_NORMAL, reactMs[id - 1], 0);
  joinSticks(sim, 4, 20);
  if (!waitJoined(sim, 0x0F, 8000)) return false;

  memset(tr, 0, sizeof(*tr));
  uint8_t lastAnnounce = 0;
  for (int i = 0; i < 900000; i++) {
    sim.step();
    const StatePacket& st = sim.host().state();
    bool bonusFlag = (st.flags & STF_BONUS) != 0;
    if (st.round >= 1 && st.round < 32) {
      if (st.phase == PH_ANNOUNCE && lastAnnounce != st.round) {
        tr->announce[st.round] = bonusFlag;
        lastAnnounce = st.round;
      }
      if (st.phase == PH_RESULTS && bonusFlag) tr->results[st.round] = true;
    }
    if (bonusFlag) tr->everBonus = true;
    if (st.phase == PH_FINAL && bonusFlag) tr->bonusInFinal = true;
    if ((st.flags & STF_DEUCE) && bonusFlag) tr->bonusInDeuce = true;
    if (st.phase == PH_FINAL) break;
  }
  if (sim.host().phase() != PH_FINAL) return false;
  const StatePacket& st = sim.host().state();
  for (int k = 0; k < RTD_MAX_PLAYERS; k++) finalScores[k] = st.players[k].score;
  *finalWinner = st.winner;
  return true;
}

static void testT25() {
  uint32_t react[4] = {150, 300, 450, 600};
  uint8_t s[RTD_MAX_PLAYERS];
  uint8_t winner = 0;
  BonusTrace tr;

  /* three rounds, BONUS_LAST: clear before, set from the start of round 3 through
     its results, clear in PH_FINAL. Classic: 1 + 1 + 2 points. */
  CHECK(traceGame(2501, 3, SCORING_CLASSIC, BONUS_LAST, react, s, &winner, &tr));
  CHECK(!tr.announce[1] && !tr.announce[2] && tr.announce[3]);
  CHECK(!tr.results[1] && !tr.results[2] && tr.results[3]);
  CHECK(!tr.bonusInFinal);
  CHECK(winner == 1);
  CHECK(s[0] == 4 && s[1] == 0 && s[2] == 0 && s[3] == 0);

  /* the same game by places: round 1 is 3/2/1/0, the bonus round 2 is 6/4/2/0 */
  CHECK(traceGame(2502, 2, SCORING_PLACES, BONUS_LAST, react, s, &winner, &tr));
  CHECK(!tr.announce[1] && tr.announce[2]);
  CHECK(!tr.results[1] && tr.results[2]);
  CHECK(winner == 1);
  CHECK(s[0] == 9 && s[1] == 6 && s[2] == 3 && s[3] == 0);

  /* BONUS_LAST_TWO: the last two rounds of five */
  CHECK(traceGame(2503, 5, SCORING_CLASSIC, BONUS_LAST_TWO, react, s, &winner, &tr));
  CHECK(!tr.announce[1] && !tr.announce[2] && !tr.announce[3]);
  CHECK(tr.announce[4] && tr.announce[5]);
  CHECK(!tr.bonusInFinal);

  /* BONUS_LAST_TWO: the last two rounds of three */
  CHECK(traceGame(2504, 3, SCORING_CLASSIC, BONUS_LAST_TWO, react, s, &winner, &tr));
  CHECK(!tr.announce[1] && tr.announce[2] && tr.announce[3]);

  /* BONUS_LAST_TWO: with two rounds only the last one */
  CHECK(traceGame(2505, 2, SCORING_CLASSIC, BONUS_LAST_TWO, react, s, &winner, &tr));
  CHECK(!tr.announce[1] && tr.announce[2]);

  /* BONUS_LAST_TWO: with one round there is no bonus round */
  CHECK(traceGame(2506, 1, SCORING_CLASSIC, BONUS_LAST_TWO, react, s, &winner, &tr));
  CHECK(!tr.everBonus);

  /* BONUS_OFF: never a bonus round */
  CHECK(traceGame(2507, 3, SCORING_CLASSIC, BONUS_OFF, react, s, &winner, &tr));
  CHECK(!tr.everBonus);

  /* a two player deuce is never a bonus round: round 2 is a regular bonus round,
     but both players have the same time and score 0, so the game goes to a deuce.
     The deuce round uses classic scoring, so the winner gets 1 point, not 2. */
  {
    Sim sim(2508);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_SCORING] = SCORING_PLACES;
    cfg[CFG_BONUS] = BONUS_LAST;
    cfg[CFG_DEUCE_LEAD] = 1;
    sim.beginWith(cfg, TIME_NONE, 0);
    sim.setPlayer(1, PL_NORMAL, 300, 0);
    sim.setPlayer(2, PL_NORMAL, 300, 0);
    sim.press(1, 10, 100);
    sim.press(2, 20, 100);
    CHECK(waitJoined(sim, 0x03, 5000));

    bool deuce = false;
    bool bonusInDeuce = false;
    bool bonusRound2 = false;
    for (int i = 0; i < 200000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.round == 2 && st.phase == PH_ANNOUNCE && (st.flags & STF_BONUS)) bonusRound2 = true;
      if (st.flags & STF_DEUCE) {
        deuce = true;
        if (st.flags & STF_BONUS) bonusInDeuce = true;
        if (st.round >= 3 && st.players[0].status == PS_PLAYING) {
          sim.setPlayer(1, PL_NORMAL, 200, 0);
          sim.setPlayer(2, PL_NORMAL, 500, 0);
        }
      }
      if (st.phase == PH_FINAL) break;
    }
    CHECK(deuce);
    CHECK(bonusRound2);
    CHECK(!bonusInDeuce);
    CHECK(sim.host().phase() == PH_FINAL);
    CHECK(sim.host().state().players[0].score == 1);
    CHECK(sim.host().state().winner == 1);
  }

  /* a three way sudden death is never a bonus round either */
  {
    Sim sim(2509);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_SCORING] = SCORING_PLACES;
    cfg[CFG_BONUS] = BONUS_LAST;
    sim.beginWith(cfg, TIME_NONE, 0);
    for (uint8_t id = 1; id <= 3; id++) sim.setPlayer(id, PL_NORMAL, 300, 0);
    joinSticks(sim, 3, 20);
    CHECK(waitJoined(sim, 0x07, 5000));

    bool suddenDeath = false;
    bool bonusInDeuce = false;
    for (int i = 0; i < 300000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.flags & STF_DEUCE) {
        suddenDeath = true;
        if (st.flags & STF_BONUS) bonusInDeuce = true;
        if (st.round >= 3 && st.players[0].status == PS_PLAYING) {
          sim.setPlayer(1, PL_NORMAL, 200, 0);
          sim.setPlayer(2, PL_NORMAL, 500, 0);
          sim.setPlayer(3, PL_NORMAL, 700, 0);
        }
      }
      if (st.phase == PH_FINAL) break;
    }
    CHECK(suddenDeath);
    CHECK(!bonusInDeuce);
    CHECK(sim.host().phase() == PH_FINAL);
    CHECK(sim.host().state().players[0].score == 1);
    CHECK(sim.host().state().winner == 1);
  }
}

/* ===================================================================== */
/* T26 catch-up through the bonus round                                  */
/* ===================================================================== */

static void testT26() {
  Sim sim(2601);
  uint8_t cfg[CFG_SLOTS];
  defaultCfg(cfg);
  cfg[CFG_VOICE_GUIDE] = 0;
  cfg[CFG_JOIN_TIME] = 3;
  cfg[CFG_ROUNDS] = 2;
  cfg[CFG_GAME_MODE] = 1;
  cfg[CFG_SCORING] = SCORING_PLACES;
  cfg[CFG_BONUS] = BONUS_LAST;
  sim.beginWith(cfg, TIME_NONE, 0);

  /* round 1: player 4 is the slowest */
  sim.setPlayer(1, PL_NORMAL, 150, 0);
  sim.setPlayer(2, PL_NORMAL, 300, 0);
  sim.setPlayer(3, PL_NORMAL, 450, 0);
  sim.setPlayer(4, PL_NORMAL, 600, 0);
  joinSticks(sim, 4, 20);
  CHECK(waitJoined(sim, 0x0F, 5000));

  bool swapped = false;
  bool bonusRound2 = false;
  bool final = false;
  for (int i = 0; i < 300000; i++) {
    sim.step();
    const StatePacket& st = sim.host().state();
    if (st.round == 2) {
      if ((st.flags & STF_BONUS) && st.phase == PH_ANNOUNCE) bonusRound2 = true;
      if (!swapped) {
        /* round 2: the order is reversed, player 4 is now the fastest */
        sim.setPlayer(1, PL_NORMAL, 600, 0);
        sim.setPlayer(2, PL_NORMAL, 450, 0);
        sim.setPlayer(3, PL_NORMAL, 300, 0);
        sim.setPlayer(4, PL_NORMAL, 150, 0);
        swapped = true;
      }
    }
    if (st.phase == PH_FINAL) { final = true; break; }
  }
  CHECK(final);
  CHECK(bonusRound2);

  const StatePacket& st = sim.host().state();
  /* round 1: 3 / 2 / 1 / 0; round 2 doubled: 0 / 2 / 4 / 6 */
  CHECK(st.players[0].score == 3);
  CHECK(st.players[1].score == 4);
  CHECK(st.players[2].score == 5);
  CHECK(st.players[3].score == 6);
  CHECK(st.winner == 4);
}

/* ===================================================================== */
/* T27 settings latch and clamp                                          */
/* ===================================================================== */

static void testT27() {
  /* defaults: PLACES and BONUS_LAST */
  {
    Sim sim(2701);
    sim.run(100);
    CHECK(sim.host().state().cfg[CFG_SCORING] == 1);
    CHECK(sim.host().state().cfg[CFG_BONUS] == 1);
    CHECK(RTD_CFG_DEF[CFG_SCORING] == SCORING_PLACES);
    CHECK(RTD_CFG_DEF[CFG_BONUS] == BONUS_LAST);
  }

  /* both keys clamp */
  {
    Sim sim(2702);
    sim.run(100);
    sim.display().setCfg(CFG_SCORING, 200);
    sim.run(100);
    CHECK(sim.host().state().cfg[CFG_SCORING] == RTD_CFG_MAX[CFG_SCORING]);
    sim.display().setCfg(CFG_BONUS, 200);
    sim.run(100);
    CHECK(sim.host().state().cfg[CFG_BONUS] == RTD_CFG_MAX[CFG_BONUS]);
    sim.display().setCfg(CFG_SCORING, 0);
    sim.display().setCfg(CFG_BONUS, 0);
    sim.run(100);
    CHECK(sim.host().state().cfg[CFG_SCORING] == SCORING_CLASSIC);
    CHECK(sim.host().state().cfg[CFG_BONUS] == BONUS_OFF);
  }

  /* a change in the middle of a game does not change the running game:
     classic and BONUS_OFF stay in force although the settings change */
  {
    Sim sim(2703);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 3;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_SCORING] = SCORING_CLASSIC;
    cfg[CFG_BONUS] = BONUS_OFF;
    sim.beginWith(cfg, TIME_NONE, 0);
    for (uint8_t id = 1; id <= 4; id++) sim.setPlayer(id, PL_NORMAL, (uint32_t)(150 + 60 * id), 0);
    joinSticks(sim, 4, 20);
    CHECK(waitJoined(sim, 0x0F, 5000));
    CHECK(sim.runUntilPhase(PH_ANNOUNCE, 10000));

    sim.display().setCfg(CFG_SCORING, SCORING_PLACES);
    sim.display().setCfg(CFG_BONUS, BONUS_LAST);
    sim.run(200);
    CHECK(sim.host().state().cfg[CFG_SCORING] == SCORING_PLACES);

    bool bonusSeen = false;
    bool final = false;
    for (int i = 0; i < 300000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.flags & STF_BONUS) bonusSeen = true;
      if (st.phase == PH_FINAL) { final = true; break; }
    }
    CHECK(final);
    CHECK(!bonusSeen);
    const StatePacket& st = sim.host().state();
    int sum = 0;
    for (int k = 0; k < RTD_MAX_PLAYERS; k++) sum += st.players[k].score;
    CHECK(sum == 3);   /* classic: one point per round for player 1 */
  }

  /* the reverse latch: places and the bonus round stay in force */
  {
    Sim sim(2704);
    uint8_t cfg[CFG_SLOTS];
    defaultCfg(cfg);
    cfg[CFG_VOICE_GUIDE] = 0;
    cfg[CFG_JOIN_TIME] = 3;
    cfg[CFG_ROUNDS] = 2;
    cfg[CFG_GAME_MODE] = 1;
    cfg[CFG_SCORING] = SCORING_PLACES;
    cfg[CFG_BONUS] = BONUS_LAST;
    sim.beginWith(cfg, TIME_NONE, 0);
    for (uint8_t id = 1; id <= 4; id++) sim.setPlayer(id, PL_NORMAL, (uint32_t)(150 + 60 * id), 0);
    joinSticks(sim, 4, 20);
    CHECK(waitJoined(sim, 0x0F, 5000));
    CHECK(sim.runUntilPhase(PH_ANNOUNCE, 10000));

    sim.display().setCfg(CFG_SCORING, SCORING_CLASSIC);
    sim.display().setCfg(CFG_BONUS, BONUS_OFF);
    sim.run(200);
    CHECK(sim.host().state().cfg[CFG_SCORING] == SCORING_CLASSIC);

    bool bonusRound2 = false;
    bool final = false;
    for (int i = 0; i < 300000; i++) {
      sim.step();
      const StatePacket& st = sim.host().state();
      if (st.round == 2 && st.phase == PH_ANNOUNCE && (st.flags & STF_BONUS)) bonusRound2 = true;
      if (st.phase == PH_FINAL) { final = true; break; }
    }
    CHECK(final);
    CHECK(bonusRound2);
    const StatePacket& st = sim.host().state();
    CHECK(st.players[0].score == 9);
    CHECK(st.players[1].score == 6);
    CHECK(st.players[2].score == 3);
    CHECK(st.players[3].score == 0);
  }
}

/* ===================================================================== */
/* runner                                                                */
/* ===================================================================== */

typedef void (*TestFn)();

struct TestCase {
  const char* id;
  const char* name;
  TestFn fn;
};

int main() {
  static const TestCase cases[] = {
    {"T1", "protocol", testT1},
    {"T2", "happy path", testT2},
    {"T3", "join order", testT3},
    {"T4", "join edge cases", testT4},
    {"T5", "GO fairness", testT5},
    {"T6", "false start", testT6},
    {"T7", "lost results", testT7},
    {"T8", "stick failures", testT8},
    {"T9", "ties", testT9},
    {"T10", "settings", testT10},
    {"T11", "actions", testT11},
    {"T12", "clock wrap", testT12},
    {"T13", "OTA sequence", testT13},
    {"T14", "display link", testT14},
    {"T15", "fuzz", testT15},
    {"T16", "stale slot after lost idle", testT16},
    {"T17", "stale slot after host reboot", testT17},
    {"T18", "scores after high uptime", testT18},
    {"T19", "OTA in maintenance mode", testT19},
    {"T20", "idle prompt interval", testT20},
    {"T21", "shake start uses GO age", testT21},
    {"T22", "button edge before GO", testT22},
    {"T23", "service menu", testT23},
    {"T24", "places scoring", testT24},
    {"T25", "bonus round", testT25},
    {"T26", "catch-up through the bonus round", testT26},
    {"T27", "scoring settings latch", testT27},
  };

  int count = (int)(sizeof(cases) / sizeof(cases[0]));
  for (int i = 0; i < count; i++) {
    int before = g_fails;
    cases[i].fn();
    if (g_fails == before) {
      printf("PASS %s %s\n", cases[i].id, cases[i].name);
    } else {
      printf("FAIL %s %s\n", cases[i].id, cases[i].name);
    }
    fflush(stdout);
  }

  printf("SUMMARY: %d tests, %d checks, %d failures\n", count, g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
