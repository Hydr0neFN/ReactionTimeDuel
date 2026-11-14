# Reaction Time Duel 2.0 — Design (normative)

This document is the contract for the 2.0 rework. Firmware version `V4.0.3`, protocol version 4.
The baseline (the code that worked at the Hanze open day) is git commit `cc638ad`.
Rule for every job: keep the baseline game rules, timings, LED and audio behaviour unless a section here changes it.

## 1. System

```
              +---------------------------+
              |  HOST  ESP32 DevKit-C     |  game logic, 5 NeoPixel rings, 89-LED strip,
              |  Arduino, firmware/host   |  I2S MP3 audio, settings (NVS), OTA hub (AP + web)
              +-------------+-------------+
                            | ESP-NOW, channel 6
        +----------+--------+--------+----------+------------------+
        |          |                 |          |                  |
   +----+---+ +----+---+        +----+---+ +----+---+      +-------+---------------+
   | STICK1 | | STICK2 |        | STICK3 | | STICK4 |      | DISPLAY ESP32-S3      |
   | white  | | blue   |        | red    | | yellow |      | Waveshare 800x480 RGB |
   +--------+ +--------+        +--------+ +--------+      | GT911 touch, LVGL 8.3 |
   ESP8266 ESP-12F, Arduino, firmware/joystick             | ESP-IDF, firmware/display
   button GPIO14, motor GPIO12, MPU-6050 (SDA4/SCL5)       +-----------------------+
```

- The joystick ID (1..4) is fixed per physical stick because each stick has a colour: 1 white, 2 blue, 3 red, 4 yellow.
- Three devices, one repo, one shared library (`shared/rtd_core`) that holds the wire protocol and all game logic.
- The game logic is plain C++11 with no Arduino or ESP-IDF include. The same sources run in the firmware and in the PC tests.

## 2. Repository layout

```
ReactionTimeDuel/
  docs/DESIGN.md                 this file
  shared/rtd_core/               PlatformIO library used by all three firmwares and by the tests
    library.json
    src/RtdProtocol.h            wire format (Appendix A, copy verbatim)
    src/RtdNetConfig.h           default MAC addresses, AP SSID and password (Appendix B)
    src/HostGame.h/.cpp          host game state machine (section 5)
    src/StickLogic.h/.cpp        joystick state machine (section 6)
    src/DisplayLink.h/.cpp       display side link logic (section 7.1)
  firmware/host/                 ESP32 Arduino project
  firmware/joystick/             ESP8266 Arduino project
  firmware/display/              ESP32-S3 ESP-IDF project (LVGL)
    assets/                      PNG sources of the images (assets/original = decoded baseline art)
  tests/                         native tests (g++), simulator of host + 4 sticks + lossy radio
  tools/rtd.py                   build / flash / test wrapper
  tools/run_tests.py             builds and runs tests/ with a native g++
  tools/png2lvgl.py              PNG -> LVGL 8 C array (RGB565 + alpha)
```

Toolchain pins: host and display `platform = espressif32@6.9.0` (Arduino core 2.0.17 / ESP-IDF 5.3). Joystick
`platform = espressif8266@4.2.1`. LVGL `lvgl/lvgl@8.3.11`. Host libraries: `earlephilhower/ESP8266Audio@1.9.7`,
`makuna/NeoPixelBus@^2.8.3`. Every project adds `lib_extra_dirs = ../../shared`.

## 3. Wire protocol v4

All devices: Wi-Fi STA interface, channel 6, power save off. Transport: ESP-NOW.

Three frame types. The first byte tells them apart.

| frame | first byte | size | direction | use |
|---|---|---|---|---|
| `GamePacket` | `0x0A` | 7 | any | commands, results, ACK (same layout as the baseline) |
| `StatePacket` | `0x5A` | 72 | host -> display | full game state snapshot |
| `ServicePacket` | `0x5B` | 92 | host -> display | service report (section 12) |

CRC8: polynomial `0x8C` (reflected), init 0, over all bytes before the CRC byte. Same function as the baseline.

### 3.1 Commands (GamePacket)

| cmd | hex | direction | data_high / data_low | notes |
|---|---|---|---|---|
| `CMD_OK` | 0x0B | host -> stick | 0 / slot 1..4 | join confirmed. Idempotent. |
| `CMD_REQ_ID` | 0x0D | stick -> host | version byte / patch | join request |
| `CMD_ACK` | 0x0E | both | 0 / acked cmd | |
| `CMD_GAME_START` | 0x21 | host -> stick | mode / shake target | ACK required |
| `CMD_GO` | 0x22 | host -> stick | 16 bit age in ms | section 4.1. ACK required for unicast. |
| `CMD_VIBRATE` | 0x23 | host -> stick | 0 / duration x 10 ms | |
| `CMD_IDLE` | 0x24 | host -> stick | 0 | ACK required for unicast |
| `CMD_COUNTDOWN` | 0x25 | host -> stick | 0 / 3,2,1 | ACK required |
| `CMD_REACTION_DONE` | 0x26 | stick -> host | 16 bit ms, 0xFFFF = penalty | host answers `CMD_ACK` |
| `CMD_SHAKE_DONE` | 0x27 | stick -> host | 16 bit ms, 0xFFFF = timeout | host answers `CMD_ACK` |
| `CMD_SHAKE_PROGRESS` | 0x28 | stick -> host | count / target (stick internal units) | no ACK |
| `CMD_BEACON` | 0x29 | host -> broadcast | phase / masks | every 500 ms. low nibble = joined sticks (bit0 = stick 1), high nibble = sticks active in this round |
| `CMD_HEARTBEAT` | 0x2A | stick -> host | version byte / patch | every 1000 ms, first one 50 ms x ID after boot |
| `CMD_CFG_SET` | 0x40 | display -> host | key / value | section 8 |
| `CMD_ACTION` | 0x41 | display -> host | action / argument | section 8.2 |
| `CMD_DISPLAY_HELLO` | 0x42 | display -> host | version byte / patch | at boot and every 2000 ms |
| `CMD_OTA_BEGIN` | 0x43 | host -> stick | target version byte / patch | section 9. ACK required. |
| `CMD_OTA_STATUS` | 0x44 | stick -> host | status / detail | `OTA_ST_*` |
| `CMD_DISPLAY_INFO` | 0x45 | display -> host | last IP octet / display ota state | sent in maintenance mode every 2000 ms |
| `CMD_SVC_REQ` | 0x47 | display -> host | service op / argument | section 12. No ACK. |

Version byte = `(major << 4) | minor`.

### 3.2 Address learning

Default MAC addresses are compiled in (`RtdNetConfig.h`). Each device also learns:

- Host: a valid packet with `src_id` = stick N or display from a MAC that differs from the stored one replaces the peer for that ID.
- Stick and display: a valid packet with `src_id == ID_HOST` from a MAC that differs from the stored host MAC replaces the host peer. The beacon is a broadcast, so a new host board is found in 0.5 s.

A replaced board therefore needs no reflash of the other devices.

### 3.3 Reliability rules

- Host -> stick commands marked "ACK required": retry every 40 ms, at most 4 retries, one pending command per stick (a new command replaces the pending one).
- Stick -> host results: resend every 30 ms until the host `CMD_ACK` arrives, at most 12 sends. The host always ACKs a result, also a duplicate or a late one.
- Stick -> host `CMD_REQ_ID`: resend every 80 ms until `CMD_OK` arrives, at most 6 sends.
- Host -> display: the snapshot is idempotent. Send at once when its content changed (rate limit 50 ms, except a phase change which is sent at once and again 20 ms later), and at least every 250 ms.
- Stick resync: when the beacon says this stick is not joined and the stick is not idle, the stick goes idle. When no host packet arrived for 10 s and the stick is not idle, the stick goes idle.
- No `Serial` output and no blocking call inside any ESP-NOW receive callback. The callback takes the receive timestamp, copies the frame into a queue and returns.

## 4. Fair timing

### 4.1 GO

The host fixes the GO instant `t0 = micros()` and then:

1. sends `CMD_GO` as a broadcast with age 0 (one frame, all sticks receive it at the same instant),
2. sends `CMD_GO` to each active stick as a unicast with ACK tracking. Every (re)transmission carries `age = (micros() - t0) / 1000`, saturated at 0xFFFF.

A stick takes the first valid `CMD_GO` while it waits for GO: `go_time = rx_timestamp - age * 1000`. Later copies only get an ACK (unicast only; a broadcast is never ACKed). A lost frame therefore gives no advantage.

### 4.2 Button

- The button ISR pushes the `micros()` of every falling edge into a small ring buffer. The main loop passes them to `StickLogic` in order.
- False start (reaction mode): penalty when the button is down at GO, or a falling edge happened in the last `FALSE_START_WINDOW_MS` = 1000 ms before GO.
- Blanking: falling edges in the first `GO_BLANK_MS` = 60 ms after GO are ignored (the motor start can glitch the pin; nobody reacts that fast). When the button is down at the end of the blanking window (two samples 2 ms apart), that is a false start too.
- A valid press is the first falling edge after the blanking window. Result = `(edge - go_time) / 1000` ms, minimum 1.
- The GO vibration starts after the button level was sampled, as in the baseline.

## 5. Host game logic (`HostGame`)

Port from `firmware/host/src/main.cpp` of the baseline. Same states, same timings, same audio order, same LED modes, except for the changes H1..H12.

### 5.1 Phases (also the `phase` field of the snapshot)

| value | name | baseline state |
|---|---|---|
| 0 | `PH_IDLE` | STATE_IDLE (now waits for a player, see H1) |
| 1 | `PH_JOIN` | STATE_JOIN |
| 2 | `PH_ANNOUNCE` | mode announcement (first part of STATE_COUNTDOWN / STATE_REACTION) |
| 3 | `PH_COUNTDOWN` | shake 3-2-1 |
| 4 | `PH_REACT_WAIT` | random delay before GO; the centre ring counts down in 4 stages (H15) |
| 5 | `PH_GO` | STATE_COLLECT (reaction GO fired) |
| 6 | `PH_SHAKE` | STATE_SHAKE |
| 7 | `PH_RESULTS` | STATE_SHOW_RESULTS |
| 8 | `PH_FINAL` | STATE_FINAL_WINNER |
| 9 | `PH_MAINT` | new: maintenance / OTA (section 9) |

### 5.2 Changes against the baseline

- **H1 Idle is an attract mode.** The host stays in `PH_IDLE` until a stick sends `CMD_REQ_ID`. That stick gets slot 1 and the host enters `PH_JOIN`. Audio in idle: `press_join` on entry and then every 45 s, only when `CFG_VOICE_GUIDE` = 1.
- **H2 Join without gaps (fixes "press order skipped").** The prompted slot is always the lowest free slot; it is derived, never stored on its own. A join request from a stick without a slot always claims the prompted slot at once, so two presses in the same millisecond get two consecutive slots and no press is dropped. A request from a stick that already has a slot gets `CMD_OK` with its slot again. When the prompted slot changes: stop audio, play "Player N", restart the prompt timer. Prompt timer = `CFG_JOIN_TIME` seconds since the last claim. On timeout: 2 or more players -> start; 1 player -> restart the timer and replay the prompt, and after `JOIN_SOLO_TIMEOUTS` = 3 timeouts go back to idle (send `CMD_IDLE`). 4 players -> start at once. `ACT_START_NOW` with 2 or more players -> start at once. Start = show colours 1 s, then "get ready", as in the baseline.
- **H3 Single thread.** All packets are handled in `HostGame::onPacket()`, called from the main loop that drains the receive queue. No game state is touched in a callback.
- **H4 GO** as in section 4.1.
- **H5 Announcements end when the audio ends.** An announcement is over when (audio idle and 1500 ms passed) or the baseline fixed delay + 2000 ms passed. With `CFG_VOICE_GUIDE` = 0 the long instructions (`react_inst`, `will_shake`) are never queued.
- **H6 Settings** (section 8): rounds, game mode, scoring and bonus are latched when a game starts (`PH_JOIN` -> first round). A change during a game has an effect only on the next game. Volume and brightness apply at once.
- **H7 Tie at the end.** Exactly 2 players share the top score: baseline deuce (only those two play; a lead of `CFG_DEUCE_LEAD` points wins). 3 or 4 players share the top score: sudden death among them; the first round with a winner ends the game. Sticks that are out get `CMD_IDLE`. `STF_DEUCE` is set in both cases.
- **H8 Records.** Reaction mode only: a round winner time below `best_session_ms` / `best_alltime_ms` updates them. The all-time value and `games_total` (incremented on entering `PH_FINAL`) are persisted. `STF_NEW_RECORD` is set during the results of a round that beat the all-time value.
- **H9 Results are always ACKed** (section 3.3). A result is accepted only in `PH_GO` / `PH_SHAKE` from an active player that has no result yet.
- **H10 Beacon** every 500 ms in every phase.
- **H11 Stick presence.** A stick is online when any valid packet from it arrived in the last 3000 ms. The version from `CMD_HEARTBEAT` / `CMD_REQ_ID` is kept per stick.
- **H12 `randomSeed`** from the hardware RNG, not from an ADC pin.
- **H15 Reaction wait ring.** In `PH_REACT_WAIT` (`NEO_REACT_WAIT`) every active player ring shows the player colour and the centre ring is amber (`RGB_HANZE`), a real countdown with random stage lengths. 12 LEDs are lit at the start; 3 LEDs go off at the end of each of the 4 stages (12, 9, 6, 3, 0 lit; `REACT_WAIT_STAGES`). The 4 stage lengths are random weights (squared, so the stages are uneven) scaled to the free time above a minimum of 300 ms per stage; they sum to exactly `reactDelayMs_`. The last stage ends in the tick in which `fireGo()` runs, so the ring is empty exactly at GO and at least 3 LEDs are lit before. The ring shows that GO comes soon but not when: the last stage can be short or long. The stage lengths come from a private xorshift32 (mixed with `ports_.micros()` at each entry), so `ports_.random()` and the delay choice are untouched. `LedView.waitLit` is the lit count (0 outside the phase). After GO `NEO_FIXED_COLOR` and `paintLimit` take the centre ring over as before.
- **H13 Scoring.** `CFG_SCORING` selects the rule of a regular round. `SCORING_CLASSIC`: the round winner gets 1 point. `SCORING_PLACES`: a player with a valid time gets 1 point for each other active player that it beat. A player beats another player when the other player has no valid time (`PS_PENALTY`, or still no result) or has a time that is strictly longer. Two players with the same time do not beat each other. A player without a valid time gets 0 points. Four active players with four different valid times get 3 / 2 / 1 / 0 points. Two active players get 1 / 0, the same as classic. A score saturates at 255. The round winner, the winner sounds and the records stay as they are. The end of the game (H7) works on the scores whatever the rule.
- **H14 Bonus round.** `CFG_BONUS` selects the bonus rounds. `BONUS_LAST`: the last regular round is a bonus round, and only when the game has 2 or more rounds. `BONUS_LAST_TWO`: the last two regular rounds are bonus rounds when the game has 3 or more rounds; with 2 rounds only the last round; with 1 round none. In a bonus round every point of H13 counts double. `STF_BONUS` is set from the start of the bonus round (the announcement) until the round after it starts, or until the game goes to `PH_FINAL`, `PH_IDLE` or a deuce. It stays set in the results of the bonus round. A deuce round and a sudden-death round always use `SCORING_CLASSIC` and are never a bonus round, so that `CFG_DEUCE_LEAD` keeps its meaning.
- **State flags.** `STF_DEUCE` 0x01, `STF_SCORES` 0x02, `STF_NEW_RECORD` 0x04, `STF_CAN_START` 0x08, `STF_UPDATE_AVAILABLE` 0x10, `STF_BONUS` 0x20.

### 5.3 Ports

`HostGame` talks to the outside only through an abstract `HostPorts` class (virtual functions), implemented by the firmware and by the simulator:

| function | meaning |
|---|---|
| `uint32_t millis()`, `uint32_t micros()` | monotonic, wrap allowed (all comparisons use unsigned subtraction) |
| `uint32_t random(uint32_t n)` | 0..n-1 |
| `void send(uint8_t destId, const uint8_t* data, uint8_t len)` | destId = `ID_STICK1..4`, `ID_DISPLAY`, `ID_BROADCAST`. The firmware maps the ID to a MAC. |
| `void audioPlay(RtdSound s)`, `void audioStop()`, `bool audioIdle()` | queue a sound / stop and clear the queue / nothing playing and queue empty |
| `void applySettings(const uint8_t* cfg)` | volume, brightness, strip mode changed |
| `void saveSettings(const uint8_t* cfg)`, `void saveStats(uint16_t bestAlltimeMs, uint16_t gamesTotal)` | persistence. The firmware debounces flash writes by 2 s. |
| `bool stickImage(uint8_t* verByte, uint8_t* patch)` | a joystick firmware image is stored on the host |
| `void maintEnter()`, `void maintExit()` | start AP + web server / reboot into normal mode |

`HostGame` API: `begin(cfg, bestAlltime, gamesTotal)`, `onPacket(data, len, rxMicros)`, `tick()`, `const StatePacket& state()` (CRC and seq are set by `buildState()` when the firmware sends it), `const LedView& leds()`, `stateChanged()` / `clearStateChanged()`.

`LedView` carries what the LED code of the baseline read from globals: neo mode, the 5 ring override colours (0 = none, else 0xRRGGBB), the 5 ring blink flags, the blinking slot, "flash until" time, joined and active masks, per player progress in percent, shake start time, the lit count `waitLit` of the reaction wait ring. The LED animations stay in the firmware.

`RtdSound` is an enum with one value per MP3 file of `firmware/host/data`; the firmware maps it to the file name.

## 6. Joystick logic (`StickLogic`)

Port from `firmware/joystick/src/main.cpp` of the baseline. States `JS_IDLE`, `JS_WAITING_GO`, `JS_REACTION_TIMING`, `JS_SHAKE_COUNTING`, `JS_DONE`, plus `JS_OTA`.

Ports (`StickPorts`): `millis()`, `micros()`, `send(data, len)` (to the host), `motor(bool on)`, `bool buttonDown()`, `bool readAccel(int16_t& ax, int16_t& ay, int16_t& az)`, `void otaStart(uint8_t verByte, uint8_t patch)`.

API: `StickLogic(ports, stickId)`, `onPacket(data, len, rxMicros)`, `onButtonFall(us)`, `tick()`.

Changes against the baseline:

- **J1** Timing and false start as in section 4.
- **J2** Reliability and resync as in section 3.3. No `delay()` anywhere; the baseline `sendToHostRetry` (3 sends with `delay(20)`) is replaced by the resend rule.
- **J3** Heartbeat every 1000 ms in every state except `JS_OTA`.
- **J4** Shake detection works in every direction (V4.0.3). The old code summed the three axes (`|ax + ay + az| > 67152`) at the default +-2 g range: one axis saturates at 32767, so a shake along one axis never counted and axes with opposite signs cancelled. Now:
  - `mpuInit()` sets the MPU-6050 to +-8 g (`ACCEL_CONFIG` 0x10, 4096 LSB per g) and the low-pass filter to 44 Hz (`CONFIG` 3), which removes the buzz of the vibration motor but passes shakes.
  - The sampling stays 200 Hz (every 5 ms). The stick keeps a gravity estimate per axis (integer low-pass, 1/64 per sample, time constant about 320 ms, started from the first sample after GO). The dynamic vector is the sample minus the gravity. Its squared length (each axis clamped to +-32767, sum in `uint32_t`, no float) is compared with the squared thresholds.
  - One count on the rising crossing of `SHAKE_HIGH_LSB` (1.5 g). The detector re-arms when the length falls below `SHAKE_LOW_LSB` (0.75 g) and only counts again after `SHAKE_MIN_GAP_MS` (40 ms).
  - A back-and-forth shake makes two counts (one per reversal). `SHAKE_FACTOR` is 4: the host targets 10 / 15 / 20 mean 40 / 60 / 80 counts, that is 20 / 30 / 40 full cycles. The progress frame `(milestone << 8) | target` stays below 256 (largest 80), and the host percentage `milestone x 100 / target` is unchanged.
  - Calibration values of the last shake round stay in `StickLogic` until the next shake GO and go to the host as `CMD_DIAG` keys (section 11.1). A "peak" is the highest dynamic length of one counted shake.
- **J5** Stick ID at run time (one binary for all four sticks, needed for OTA). Order: EEPROM record (magic + ID) -> MAC table in `RtdNetConfig.h` -> none. A build with `-DMY_ID=n` (env `stick1..stick4`) writes the EEPROM record on boot when it differs. Env `stick` (no `MY_ID`) is the OTA image. A stick without an ID does not join; it pulses the motor 3 times at boot.
- **J6** Radio: `WIFI_NONE_SLEEP`, maximum TX power.
- **J7** Motor pulses are timed with unsigned subtraction (`millis() - start >= duration`), not with an absolute end time.

## 7. Display

### 7.1 `DisplayLink` (in `rtd_core`)

Validates a `StatePacket` (start, version, length, CRC), keeps the last good one, reports `linkUp()` (a good snapshot in the last 2000 ms), builds `CMD_DISPLAY_HELLO`, `CMD_CFG_SET`, `CMD_ACTION`, `CMD_DISPLAY_INFO`. Setting sync: `setCfg(key, value)` sends at once, then resends every 400 ms while the snapshot shows another value, at most 5 sends. `cfg(key)` returns the pending value while a send is pending, else the snapshot value.

### 7.2 Hardware

Waveshare ESP32-S3 Touch LCD 800x480 (the 4.3", 5" and 7" boards share these pins). RGB panel code of the baseline stays. Touch: GT911 on I2C0, SDA GPIO8, SCL GPIO9, 400 kHz, INT GPIO4. The touch reset line is on the CH422G I/O expander. Reset sequence from the Waveshare demo:

1. Configure GPIO4 as output.
2. I2C write to address `0x24`: byte `0x01` (expander outputs enabled).
3. I2C write to address `0x38`: byte `0x2C` (touch reset low, backlight on).
4. Wait 100 ms. Set GPIO4 low. Wait 100 ms.
5. I2C write to address `0x38`: byte `0x2E` (touch reset high). Wait 200 ms.
6. Configure GPIO4 as input.

GT911: probe address `0x5D`, then `0x14`. Read status register `0x814E`; when bit 7 is set and the low nibble is 1..5, read the first point from `0x8150` (x low, x high, y low, y high) and write 0 to `0x814E`. Scale by the resolution in `0x8048..0x804B` when it differs from 800x480. Write a small driver in the project (`touch_gt911.c/.h`); do not add a managed component. A touch init failure is not fatal: log it and run without touch.

LVGL input device: pointer, read from the GT911 driver, long press time 1000 ms.

### 7.3 Theme (keep the original look)

| token | value | source |
|---|---|---|
| background | `0x101418` | baseline panels |
| text | `0xFFFFFF`, Montserrat Bold | baseline images and fonts |
| accent orange | `0xFF9430` | the GO! image; also the Hanze orange |
| accent green | `0x2AAC00` (ring), `0x40CC00` (winner box) | baseline images |
| danger / free slot red | `0xE81818` | baseline player rings |
| secondary text | `0x8A9099` | new |
| panel fill | `0x1A2026`, radius 12 | new |
| stick colours | 1 `0xFFFFFF`, 2 `0x0000FF`, 3 `0xFF0000`, 4 `0xFFFF00` | baseline |

Fonts: `ui_font_MontserratBold20/28/40` (new, ASCII 0x20..0x7F, 4 bpp), `ui_font_MonserratBold64V2` (baseline), `ui_font_MontserratBold120` (digits and space only). Use `lv_font_montserrat_24/32` only for `LV_SYMBOL_*` icons.
New images (PNG in `firmware/display/assets`, C arrays made by `tools/png2lvgl.py`): `hanze_logo` 440x121, `icon_link_on` 40x40, `icon_link_off` 40x40, `icon_gear` 44x44.
The SquareLine files in `lib/ui` stay as they are; new widgets are created in code.

### 7.4 Screens

The display renders the snapshot. Animations start on a change between two snapshots.

**Idle screen** (`PH_IDLE`): Hanze logo top centre (slow vertical float, 6 px, 4 s). Title "REACTION TIME DUEL" (Bold 40). A pill with a green 3 px outline and the orange text "PRESS YOUR BUTTON TO JOIN" (Bold 28), breathing opacity 255 <-> 120 in 1.2 s. Stats row (Bold 20, labels in secondary text): "BEST TODAY n ms", "RECORD n ms", "GAMES n" ("--" when none). Four dots in the stick colours: filled = stick online, outline = offline. 12 soft circles in the stick colours drift upwards behind the content (6..14 px, opacity 30..60 %, 6..12 s loops). Bottom right: link icon and gear icon. A badge "UPDATE AVAILABLE" next to the gear when `STF_UPDATE_AVAILABLE` is set.

**Game screen** (all other game phases) = the baseline main screen plus:

| phase | centre | players |
|---|---|---|
| `PH_JOIN` | `ui_imgPressJoyn` in the centre circle | joined: ring in the stick colour (short pop: border width 12 -> 4). prompted slot: green ring, smooth pulse (opacity, 600 ms each way) |
| `PH_ANNOUNCE` | mode image (and shake target number). With `STF_DEUCE` on the first announcement after the flag appeared: the DEUCE image for 1500 ms first | |
| `PH_REACT_WAIT` | mode image stays; the centre circle pulses slowly. Nothing may hint at the GO time | |
| `PH_COUNTDOWN` | the number, each tick fades in and moves up 12 px in 200 ms | |
| `PH_GO` | GO! image; full screen orange flash, opacity 180 -> 0 in 250 ms | a finished player shows "n ms"; a penalty shows "FOUL" in red |
| `PH_SHAKE` | shake image + target number; the flash as for GO | an arc (166 px, width 6, stick colour) around each active ring shows `progress`; finished players show "n ms" |
| `PH_RESULTS` | circle and GO hidden. With `STF_SCORES`: winner image + number when `winner` != 0; "NEW RECORD!" (Bold 28, orange, blinking) with `STF_NEW_RECORD` | the round time as smaller text, the score as a large number and the rank tag; the winner ring pulses |
| `PH_FINAL` | winner image + number, "FINAL WINNER" (Bold 28), confetti: 40 small rectangles in the stick colours, orange and green fall for the whole phase | scores |

**Scores and ranks.** Every joined player shows its score as a large number (Bold 64) inside its ring in all phases of a game; the "PLAYER n" image is small and at the top of the ring then. The colour of a joystick always means that player: the number is on a badge in the stick colour, with dark digits on white and yellow and white digits on blue and red. From the first scored round on, a rank tag 1ST, 2ND, 3RD or 4TH shows below the number; the tag of the leader is dark text on an orange pill. Players with the same score share the rank. At the results every gain shows as "+N" next to the number; the "+N" moves up and fades out. The round time stays as smaller text. The baseline "W: s" line is removed.

**Bonus round.** With `STF_BONUS` the banner "BONUS ROUND x2" (orange pill, breathing) shows at the left of the centre circle, and the round label reads "ROUND r/t  x2".

Always: "ROUND r/t" (or "DEUCE") bottom left (Bold 20, secondary text), link icon and gear (opacity 40 %) bottom right.
Screen changes: fade, 300 ms.
Link lost (`!linkUp()`): top layer overlay with `icon_link_off` and "CONNECTING TO HOST..." (Bold 28).

**Settings screen**: opened by a long press (1000 ms) on the gear, so a visitor's tap does not open it. A tap shows the hint "HOLD 1 S FOR SETTINGS" for 1.5 s, and the gear is orange while it is pressed, so that the operator sees that the touch works. Closed by the back button or after 30 s without a touch. Header 64 px with a back button and "SETTINGS". Tab view with three tabs. Every control is at least 56 px high. Slider indicator and checked buttons in accent orange.

| tab | controls |
|---|---|
| SOUND & LIGHT | Volume slider 0..10 (on release also `ACT_TEST_SOUND`), Ring brightness slider 1..10, Strip brightness slider 0..10, Strip effect drop-down (AUTO, RAINBOW, SPARKLE, METEOR, CHASE, BREATHING, FIRE, HANZE) |
| GAME | Rounds buttons 3 / 5 / 7 / 10, Mode buttons MIXED / REACTION / SHAKE, Scoring buttons CLASSIC / PLACES, Bonus buttons OFF / LAST / LAST 2, Voice guide switch, Join time slider 3..10 s, Deuce lead buttons 1 / 2 / 3 |
| STATUS | one row per joystick: colour dot, ONLINE / OFFLINE, version, BUZZ button (`ACT_IDENTIFY`, only in `PH_IDLE`). Host and display version. Best today / record with RESET RECORD. Games total. START NOW (enabled with `STF_CAN_START`). ABORT GAME, RESTORE DEFAULTS; these two ask for confirmation in a message box. SERVICE MENU opens the service screen (section 12) |

A control follows the snapshot value unless the user touched it in the last 1000 ms. A slider sends while it is dragged (at most one send per 150 ms) and on release.

**Service screen** (`PH_MAINT`): the maintenance screen is replaced by the service screen of section 12. It is always shown in `PH_MAINT` and it opens from the settings screen (button SERVICE MENU) in `PH_IDLE`. The UPDATE page shows "UPDATE MODE", the AP name and password, "http://192.168.4.1", the display's own address when it is connected, the OTA state text and a progress bar.

### 7.5 Structure and performance

- ESP-NOW receive callback -> FreeRTOS queue (16 frames) -> an LVGL timer (20 ms) drains it and updates the UI. The baseline "one pending packet" slot and its fast paths are removed.
- The LVGL task stack is 8 kB.
- The anti-tearing mode is 3 (2 frame buffers, LVGL direct mode): LVGL draws only the changed areas and each frame is shown on a panel vsync. The panel refresh rate is 39 Hz (16 MHz pixel clock), thus the frame rate is 39 fps when a frame is drawn in less than 25 ms, and 19.5 fps for a frame that needs longer (a screen change). The refresh period (`-DLV_DISP_DEF_REFR_PERIOD=10`) is below the vsync period for this. Mode 2 (3 frame buffers, full refresh) drew the full screen for each frame; it is a build flag (`-DCONFIG_EXAMPLE_LVGL_PORT_AVOID_TEAR_MODE=2`) for the case that mode 3 shows artifacts. The LVGL blend functions run from IRAM. Do not set `CONFIG_ESP32S3_DATA_CACHE_64KB`, `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` or `CONFIG_LCD_RGB_RESTART_IN_VSYNC` for an OTA image: an image with these three options did not start (reset by the RTC watchdog) and the bootloader went back to the old image. Which of the three options causes this is not known.
- Remove the unused LovyanGFX dependency and `lgfx_conf.*`, `display.*`.
- Display-local state: none. All settings live on the host.

## 8. Settings (owned by the host, stored in NVS namespace `rtd`)

| key | name | range | default | effect |
|---|---|---|---|---|
| 1 | `CFG_VOLUME` | 0..10 | 10 | audio gain = (v/10)^2. 10 = baseline loudness, 0 = mute |
| 2 | `CFG_RING_BRIGHT` | 1..10 | 3 | NeoPixel ring brightness = 16 x v (baseline 50) |
| 3 | `CFG_STRIP_BRIGHT` | 0..10 | 5 | strip brightness = 16 x v (baseline 80), 0 = strip off |
| 4 | `CFG_ROUNDS` | 1..15 | 5 | rounds per game |
| 5 | `CFG_GAME_MODE` | 0..2 | 0 | 0 mixed (reaction, shake, reaction, ...), 1 reaction only, 2 shake only |
| 6 | `CFG_VOICE_GUIDE` | 0..1 | 1 | 1 = full (long instructions the first time, idle prompt), 0 = short |
| 7 | `CFG_JOIN_TIME` | 3..10 | 5 | seconds per join prompt |
| 8 | `CFG_STRIP_MODE` | 0..7 | 0 | 0 auto cycle, 1 rainbow, 2 sparkle, 3 meteor, 4 chase, 5 breathing, 6 fire, 7 Hanze (orange breathing) |
| 9 | `CFG_DEUCE_LEAD` | 1..3 | 3 | lead needed in a 2 player deuce |
| 10 | `CFG_SCORING` | 0..1 | 1 | 0 `SCORING_CLASSIC` (the round winner gets 1 point), 1 `SCORING_PLACES` (1 point per player that it beat) |
| 11 | `CFG_BONUS` | 0..2 | 1 | 0 `BONUS_OFF`, 1 `BONUS_LAST` (the last round counts double), 2 `BONUS_LAST_TWO` (the last two rounds count double) |

`CMD_CFG_SET`: an unknown key is ignored; a value outside the range is clamped. The snapshot carries all values in `cfg[key]`.

### 8.2 Actions (`CMD_ACTION`)

| id | name | argument | behaviour |
|---|---|---|---|
| 1 | `ACT_ABORT_GAME` | - | any game phase: stop audio, broadcast `CMD_IDLE` three times (0, 30, 60 ms), go to `PH_IDLE` |
| 2 | `ACT_TEST_SOUND` | - | play the beep when the audio is idle |
| 3 | `ACT_RESET_BEST` | - | clear both records, persist |
| 4 | `ACT_FACTORY_RESET` | - | all settings to default, persist, apply |
| 5 | `ACT_START_NOW` | - | section 5.2 H2 |
| 6 | `ACT_IDENTIFY` | stick ID | `PH_IDLE` only: `CMD_VIBRATE` 500 ms to that stick |
| 7 | `ACT_MAINTENANCE` | 1 enter / 0 exit | section 9 |
| 8 | `ACT_OTA_STICKS` | - | section 9.3 |

## 9. OTA

Goal: after the first USB flash of 2.0, every device is updated over the air, with the host as the hub.

```
 laptop / phone                 HOST (AP "RTD-Host", 192.168.4.1, channel 6)
 browser  ---- Wi-Fi ---->  web page:  [host.bin]      -> host updates itself, reboots
                                        [joystick.bin]  -> stored in the host file system
                                        [display.bin]   -> sent by the browser to the display
                                             |
                 ESP-NOW CMD_OTA_BEGIN       |  HTTP GET /fw/joystick.bin
                 +---------------------------+---------------+
                 v                                           v
        STICK n joins the AP, downloads, flashes, reboots (one stick after the other)
        DISPLAY joins the AP in maintenance mode and accepts POST /update
```

### 9.1 Maintenance mode (`PH_MAINT`)

Entered by `ACT_MAINTENANCE` 1, by `ACT_OTA_STICKS`, or by holding the host BOOT button (GPIO0) for 3 s. Only from `PH_IDLE` (the display action first aborts a running game). The host calls `maintEnter()`: Wi-Fi mode AP+STA, soft AP `RTD_AP_SSID` / `RTD_AP_PASS` on channel 6, HTTP server on port 80. ESP-NOW keeps running. Exit (`ACT_MAINTENANCE` 0 from the display, or the web page): `maintExit()` = reboot. BOOT only enters: GPIO0 is also on the USB-serial auto-reset circuit, and a serial tool that holds DTR keeps it LOW. The BOOT button is armed after GPIO0 reads HIGH for 1 s. Local actions use source `ID_HOST`, so `/api/debug` `last_exit_src` 16 means the display sent the exit. The AP is off in normal mode, so game timing is not disturbed.

### 9.2 Image marker

Every firmware contains the string `RTDFW|<kind>|<major>.<minor>.<patch>|` with kind `host`, `joystick` or `display` (macro `RTD_FW_MARKER(kind)` in `RtdProtocol.h`; the firmware prints it at boot so the linker keeps it). An uploaded image is accepted only when it holds the marker of the right kind. This stops a display image from being flashed into a stick.

### 9.3 Joysticks

- The web page stores an uploaded joystick image raw in the host OTA slot that does not run, 4096 bytes in; the first sector stays erased. NVS namespace `rtdimg` holds version, size and MD5, and the host checks the MD5 at boot. `stickImage()` reports it. A host update overwrites that slot, so store the joystick image again after a host update. (Up to V4.0.1 the image was a SPIFFS file; SPIFFS with the sounds was too full and lost a part of the image.)
- `STF_UPDATE_AVAILABLE` = an image is stored and at least one online stick has another version.
- `ACT_OTA_STICKS` (or the web page button): enter maintenance mode, then for each online stick with another version, in ID order: send `CMD_OTA_BEGIN`; wait until that stick reports the image version in a heartbeat (success), or `CMD_OTA_STATUS` failed, or 120 s passed (failure); then the next stick. `ota_state`, `ota_target`, `ota_progress` in the snapshot follow this. When all are done the host stays in maintenance mode and shows the result.
- Stick: on `CMD_OTA_BEGIN` with a version other than its own -> ACK, send `CMD_OTA_STATUS(OTA_ST_STARTING)`, call `otaStart()`. The firmware stops ESP-NOW, joins the AP (timeout 20 s), runs `ESPhttpUpdate` on `http://192.168.4.1/fw/joystick.bin` and reboots; on any failure it reboots too (back to normal operation with the old firmware). Same version -> `CMD_OTA_STATUS(OTA_ST_UPTODATE)`.
- Recovery: a stick whose button is held for 5 s at power-on goes straight to the OTA download (it retries the AP for 60 s).
- The host serves the image with `Content-Length` and the `x-MD5` header.

### 9.4 Host

Web page upload -> Arduino `Update` -> reboot. Before this reboot the host stores the NVS flag `resmaint`; the new firmware reads and clears it at boot and enters `PH_MAINT` again, so the service stays active. After a joystick image upload the host reads the stored file back and compares size and MD5 with the received bytes (a stored image once gave back 24576 of 317760 bytes); `/api/status` reports the SPIFFS use. Partition table: two OTA app slots of 0x140000 and SPIFFS 0x160000 (the Arduino default table for 4 MB, named explicitly). The host application must stay below 1.25 MB. The 24 MP3 files (753 kB) and the joystick image share the SPIFFS.

### 9.5 Display

- New partition table for 8 MB flash: nvs 0x6000, otadata 0x2000, phy_init 0x1000, ota_0 0x3A0000, ota_1 0x3A0000. The first 2.0 flash is by USB. The application must stay below 3.6 MB.
- In `PH_MAINT` the display joins the AP as a station (ESP-NOW keeps running on channel 6), starts an HTTP server and sends `CMD_DISPLAY_INFO`.
- `POST /update` with the raw image as body: stream into the inactive OTA slot, scan for the marker `RTDFW|display|`, on success set the boot partition and reboot; else answer 400. `OPTIONS /update` and all answers carry `Access-Control-Allow-Origin: *` so the host page can send the file from the browser.
- Rollback: `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`; the application marks itself valid 10 s after the UI is up.

### 9.6 Host web page

One page, no external resources, same dark theme. Shows host / stick / display versions and online state, the stored joystick image version, the three uploads, "Update joysticks now", "Exit update mode". `GET /api/status` returns the same data as JSON; the page polls it every second.

## 10. Tests (`tests/`, native, `python tools/run_tests.py`)

A simulator links `rtd_core` with fake ports: a virtual clock (1 ms steps, microsecond resolution for events), a radio with per-link loss, delay jitter 0..3 ms and duplicates, 4 `StickLogic` instances with scripted players, one `HostGame`, one `DisplayLink`. No test framework dependency: a small `CHECK` macro, exit code 0 only when every check passes. Compile flags `-std=c++11 -Wall -Wextra -Werror`.

| id | test | must hold |
|---|---|---|
| T1 | protocol | CRC8 against an independent bitwise implementation; `sizeof(GamePacket)` = 7, `sizeof(StatePacket)` = 72 and the field offsets of Appendix A; build / validate round trip; a corrupted byte is rejected |
| T2 | happy path | 4 players, 5 rounds, mixed modes: ends in `PH_FINAL` with a winner, then `PH_IDLE`; every stick idle; sum of scores = rounds with a winner |
| T3 | join order | 4 sticks press in the same millisecond, and in every permutation with gaps: slots are 1..n without a gap, every stick has exactly one slot, nothing is dropped. Same with 30 % loss |
| T4 | join edge cases | 2 players start after the join time; 1 player returns to idle after 3 timeouts and the stick is released; a second press of a joined stick changes nothing; `ACT_START_NOW` |
| T5 | GO fairness | 30 % loss and jitter: for every stick abs(reported - true reaction time) <= 2 ms, also when the broadcast GO was lost |
| T6 | false start | press 500 ms before GO, button held at GO, press inside the blanking window and held: penalty. Press 1500 ms before GO and again 250 ms after GO: valid 250 ms |
| T7 | lost results | 50 % loss stick -> host: every result arrives; a duplicate result does not change the score |
| T8 | stick failures | a stick reboots in the middle of a round: it resyncs by the beacon and the game finishes. A stick disappears: the round ends by timeout and the game finishes |
| T9 | ties | 2-way deuce ends only with the configured lead; 3-way tie ends with the first won round; two rounds in a row without a winner return to idle |
| T10 | settings | clamp, unknown key, persistence call, rounds and mode latched at game start, reaction only, shake only, short voice guide queues no instruction sound |
| T11 | actions | abort in every phase returns to idle and idles the sticks; identify; reset record; factory reset |
| T12 | clock wrap | the whole happy path with `millis()` and `micros()` starting 5 s before the 32 bit wrap |
| T13 | OTA sequence | 2 of 4 sticks outdated: updated one after the other in ID order; a stick that never comes back times out after 120 s and the next one starts |
| T14 | display link | bad CRC / wrong version / wrong length rejected; link lost after 2000 ms; `setCfg` resends until the snapshot matches, at most 5 sends |
| T15 | fuzz | 2000 games with random seeds: random player count, random rounds 1..5, both scoring rules and all three bonus values, random behaviour (no press, early press, slow, stick dropout), loss 0..40 %. Invariants on every tick: phase valid; slots without a gap when a game starts; in a round a player gets at most (active - 1) x 2 points and all players together at most active x (active - 1) / 2 x 2 points, so that the bound is correct for classic and places, with and without a bonus round; every game is back in `PH_IDLE` within a bounded simulated time; 2 s after the host is idle no reachable stick is in another state |
| T24 | places scoring | 4 players with 4 different times get 3 / 2 / 1 / 0; a false start gets 0 and counts as beaten; two equal times do not beat each other; 3 and 2 active players; nobody finishes, nobody scores |
| T25 | bonus round | `STF_BONUS` clear in the rounds before, set from the start of the bonus round through its results, clear in `PH_FINAL`; the points double under classic and under places; `BONUS_LAST_TWO` with 5, 3, 2 and 1 rounds; never in a deuce or a sudden death; `BONUS_OFF` |
| T26 | catch-up | a player that is behind before the last round wins the game through the bonus round |
| T27 | scoring settings | the defaults are 1 / 1; both keys clamp; a change in the middle of a game does not change the running game, in both directions |
| T28 | shake directions | a shake along X, Y, Z (on top of gravity) and a mixed-sign diagonal each reach the target, 2 counts per cycle (time to target), the calibration values are plausible; sticks at rest with gravity on X, Y or Z and noise count 0 |
| T29 | reaction wait ring | `waitLit` is 12 at entry, only 12 / 9 / 6 / 3 before GO, never rises, 0 at the GO tick and outside `PH_REACT_WAIT`; the 4 stages are at least 300 ms, sum to the delay, are uneven (longest >= 2 x shortest in most rounds), differ between rounds with the same delay and the last stage has a wide spread |

The three firmware builds (`pio run`) are checks too.

## 11. Debug and diagnostics

Goal: find a fault on real hardware with one USB cable (host) or with no cable (host web page). Sticks and the display need no cable.

### 11.1 Reports from sticks and display (`RtdDiag.h`)

- `CMD_DIAG` (0x46), `dest_id = ID_HOST`, `data_high` = `DIAG_*` key, `data_low` = value (saturates at 255). `HostGame`, `StickLogic` and `DisplayLink` ignore the command; the firmware glue sends and reads it.
- Each device sends one key every `DIAG_INTERVAL_MS` (1 s) and rotates through its keys. A stick sends `DIAG_FW_VERSION`, `DIAG_MPU_OK` and `DIAG_STICK_STATE` first.
- Shake calibration keys of a stick (acceleration in 0.1 g, `peak LSB x 10 / 4096`, saturates at 255): `DIAG_SHAKE_COUNT` `shake_count`, `DIAG_SHAKE_PEAK_MAX` `shake_peak_max_dg`, `DIAG_SHAKE_PEAK_MIN` `shake_peak_min_dg`, `DIAG_SHAKE_PEAK_AVG` `shake_peak_avg_dg`, `DIAG_SHAKE_SOFT_MAX` `shake_soft_max_dg` (the highest length that stayed below the high threshold: how close a too-soft shake came). They describe the last shake round and arrive after it, while the stick is idle. A full rotation of 18 keys takes 18 s (3.6 s in the service menu).
- While the beacon shows `PH_MAINT` (service menu open), a stick sends one key every `DIAG_INTERVAL_FAST_MS` (200 ms). A full rotation of 18 keys then takes 3.6 s.
- A stick sends `CMD_DIAG` only while `StickLogic::state() == JS_IDLE`, so that a round has no extra frame on the air.

### 11.2 Host debug hub

- Counters: frames received, ring drops, bad frames, frames sent, send failures per destination, longest loop pass, free heap, reset reason, uptime.
- Per device (4 sticks, display): time since the last frame, frame count, the last value of each `DIAG_*` key with its age.
- Event log: a ring of the last 200 events with a sequence number and `millis()`. Events: boot, phase change, joined-mask change, every frame received and sent except `CMD_BEACON`, `CMD_HEARTBEAT`, `CMD_DIAG`, `CMD_SHAKE_PROGRESS` and the snapshot. A received frame records its `micros()` stamp. Trace mode also logs the excluded frames.
- Serial (115200): each event prints one line `[EVT] ...` as it happens. Commands: `?` help, `s` status, `l` whole log, `t` trace on / off, `w` debug Wi-Fi on / off.
- Web: `GET /debug` (page, polls every second), `GET /api/debug` (JSON), `GET /api/log?since=<seq>` (text, one event per line).

### 11.3 Debug Wi-Fi

- Setting `debug_wifi` (NVS, default on in 2.0 test builds): the access point and the web server run in every phase, not only in `PH_MAINT`.
- Outside `PH_MAINT` only `GET` routes answer. Uploads, `/ota/sticks` and `/maint/exit` answer 409.
- Switch: serial `w`, or `POST /api/debug/wifi?on=0|1` (takes effect after a reboot when it switches off).
- Switch it off for a public event (the access-point password is shared with maintenance mode).

### 11.4 Display

- Sends `CMD_DIAG` as above. A hidden overlay (long press on the link icon, 3 s) shows its own counters on screen.

### 11.5 PC tool

- `python tools/rtd.py debug [--watch SEC] [--out FILE]` reads `/api/debug` and `/api/log` from `192.168.4.1` and writes them to `logs/`.
- `python tools/rtd.py monitor [--port COMx] [--seconds N]` records the host serial port to `logs/`.

## 12. Service menu

Goal: show the health of all devices on the display, with no phone and no cable. The numbers live in the host debug hub (section 11). The display asks for them and shows them on its service screen.

### 12.1 Frames (`RtdService.h`)

Two frames. The first byte tells them apart. The transport is the same as in section 3.

| frame | first byte | size | direction | use |
|---|---|---|---|---|
| `GamePacket` | `0x0A` | 7 | display -> host | request, `cmd = CMD_SVC_REQ` (0x47) |
| `ServicePacket` | `0x5B` | 92 | host -> display | counters of the host and of all devices |

Request: `dest_id = ID_HOST`, `src_id = ID_DISPLAY`, `data_high` = op, `data_low` = argument.

| op | value | argument | behaviour |
|---|---|---|---|
| `SVC_OP_POLL` | 0 | none | the host sends reports for the next `SVC_HOLD_MS` |
| `SVC_OP_DEBUG_WIFI` | 1 | 0 / 1 | store the debug Wi-Fi setting, effective at the next boot. Also a poll. |

An unknown op is ignored. A request from any other source or to any other destination is ignored.

`ServicePacket`, 92 bytes, CRC8 over bytes 0..90:

| offset | field | use |
|---|---|---|
| 0 | `start` | `SVC_START` = `0x5B` |
| 1 | `version` | `PROTO_VERSION` |
| 2 | `seq` | +1 per transmission |
| 3 | `host_flags` | `SVH_DEBUG_WIFI` 0x01, `SVH_AP_ON` 0x02, `SVH_TRACE` 0x04 |
| 4 | `uptime_min` | host uptime, minutes |
| 6 | `rx_frames` | frames received by the host |
| 8 | `rx_drop` | frames lost because the host receive ring was full |
| 9 | `bad_frame` | frames with a wrong length, start byte or CRC |
| 10 | `tx_fail_bcast` | failed broadcast sends |
| 11 | `heap_kb` | free heap, KB |
| 12 | `heap_min_kb` | lowest free heap since boot, KB |
| 13 | `loop_max_ms` | longest loop pass of the last 10 s, ms |
| 14 | `reset_reason` | reset reason of the host |
| 15 | `ap_clients` | stations joined to the access point |
| 16..90 | `dev[0..4]` | five `ServiceDevice` rows of 15 bytes |
| 91 | `crc` | CRC8 over bytes 0..90 |

`ServiceDevice`, 15 bytes. Index 0..3 = stick ID 1..4, index 4 = display:

| offset | field | use |
|---|---|---|
| 0 | `flags` | `SVD_SEEN` 0x01, `SVD_DIAG` 0x02, `SVD_SENSOR_OK` 0x04 |
| 1 | `age_ds` | time since the last frame, 0.1 s units. 254 = 25.4 s or more. `SVC_AGE_NEVER` = 255: no frame since the host booted |
| 2 | `frames` | frames received from the device |
| 4 | `host_tx_fail` | failed host sends to the device |
| 5 | `fw` | `DIAG_FW_VERSION`, 0 = unknown |
| 6 | `reset_reason` | `DIAG_RESET_REASON` |
| 7 | `uptime_min` | `DIAG_UPTIME_MIN` |
| 8 | `heap_kb` | `DIAG_HEAP_KB` |
| 9 | `rx_drop` | `DIAG_RX_DROP` |
| 10 | `tx_fail` | `DIAG_TX_FAIL` |
| 11 | `bad_frame` | `DIAG_BAD_FRAME` |
| 12 | `loop_max_ms` | `DIAG_LOOP_MAX_MS` |
| 13 | `link_gap_ds` | `DIAG_LINK_GAP_DS` |
| 14 | `extra` | stick: `DIAG_STICK_STATE`, display: `DIAG_SNAP_PER_10S` |

Every field holds the last value of that `DIAG_*` key. A field is 0 when the device never sent the key. Every counter saturates: `rtdSvcClamp8` at 255, `rtdSvcClamp16` at 65535. `rtdSvcAge` never returns 255.

### 12.2 Timing

| constant | value | meaning |
|---|---|---|
| `SVC_POLL_MS` | 1000 | the display repeats the request while the service screen is open |
| `SVC_SEND_MS` | 500 | period of one report while a request is fresh |
| `SVC_HOLD_MS` | 3000 | the host stops the reports this long after the last request |

The host sends nothing before the first request. A lost request costs nothing: the display repeats it after one second.

### 12.3 Who reads the frames

- `HostGame`, `StickLogic` and `DisplayLink` ignore `CMD_SVC_REQ` and the `ServicePacket`.
- The host firmware handles the request in the main loop, never in a callback. `Debug::noteRx` counts every request.
- The event log shows `CMD_SVC_REQ` and the `ServicePacket` only in trace mode, as it does for `CMD_DIAG` and the snapshot.
- The host sends the report on the path of the snapshot to the display. That path accepts 92 bytes.

### 12.4 Host behaviour

- Accept a request only with `src_id = ID_DISPLAY` and `dest_id = ID_HOST`.
- `SVC_OP_POLL` stores `millis()` as the last request time.
- `SVC_OP_DEBUG_WIFI` calls `writeDebugWifi(data_low != 0)` only when the value differs from `storage.debugWifi()`. It also stores the last request time.
- While the last request is younger than `SVC_HOLD_MS`, send one `ServicePacket` to `ID_DISPLAY` every `SVC_SEND_MS`. This runs from the loop task, also between the chunks of an upload.
- `Debug::fillService(sp, nowMs)` zeroes the frame and fills the host counters and the five device rows. The caller sets `seq`, `SVH_DEBUG_WIFI`, `SVH_AP_ON`, `ap_clients`, then calls `sealService`.
- `SVH_TRACE` comes from the trace mode. `SVH_AP_ON` is set when the access point runs (maintenance mode or debug Wi-Fi). `ap_clients` = `WiFi.softAPgetStationNum()` when the access point runs, else 0.
- `SVD_DIAG` is set when at least one `DIAG_*` value of the device is valid. `SVD_SENSOR_OK` is set when `DIAG_MPU_OK` (stick) or `DIAG_TOUCH_OK` (display) is valid and 1.

### 12.5 Display behaviour

- The service screen replaces the maintenance screen. It opens from the settings screen (button SERVICE MENU) in `PH_IDLE` and it is always shown in `PH_MAINT`.
- Page UPDATE: AP name, password, URL, display address, OTA state and progress, START / LEAVE UPDATE MODE, UPDATE JOYSTICKS.
- Page STICKS: one row per joystick: colour, link, age, firmware, frames, faults, loop, gap, state, sensor.
- Page HOST: host counters, AP state, clients, debug Wi-Fi switch.
- Page DISPLAY: the display's own counters and the host's view of the display.
- The debug Wi-Fi switch sends `SVC_OP_DEBUG_WIFI`. It takes effect at the next boot.

## Appendix A — `shared/rtd_core/src/RtdProtocol.h` (copy verbatim)

```c
#ifndef RTD_PROTOCOL_H
#define RTD_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

#define FW_VERSION_MAJOR  4
#define FW_VERSION_MINOR  0
#define FW_VERSION_PATCH  1
#define FW_VERSION_STRING "V4.0.3"
#define FW_VERSION_BYTE   ((uint8_t)((FW_VERSION_MAJOR << 4) | FW_VERSION_MINOR))
#define PROTO_VERSION     4

#define RTD_STR2(x) #x
#define RTD_STR(x)  RTD_STR2(x)
/* Marker searched in uploaded images. kind = host | joystick | display (no quotes). */
#define RTD_FW_MARKER(kind) "RTDFW|" #kind "|" RTD_STR(FW_VERSION_MAJOR) "." RTD_STR(FW_VERSION_MINOR) "." RTD_STR(FW_VERSION_PATCH) "|"

#define ESPNOW_CHANNEL    6

/* ---- device IDs ---- */
#define ID_HOST           0x00
#define ID_STICK1         0x01
#define ID_STICK2         0x02
#define ID_STICK3         0x03
#define ID_STICK4         0x04
#define ID_DISPLAY        0x10
#define ID_BROADCAST      0xFF
#define RTD_MAX_PLAYERS   4

/* ---- GamePacket ---- */
#define PACKET_START      0x0A
#define PACKET_SIZE       7

typedef struct __attribute__((packed)) {
  uint8_t start;
  uint8_t dest_id;
  uint8_t src_id;
  uint8_t cmd;
  uint8_t data_high;
  uint8_t data_low;
  uint8_t crc;
} GamePacket;

#define CMD_OK             0x0B
#define CMD_REQ_ID         0x0D
#define CMD_ACK            0x0E
#define CMD_GAME_START     0x21
#define CMD_GO             0x22
#define CMD_VIBRATE        0x23
#define CMD_IDLE           0x24
#define CMD_COUNTDOWN      0x25
#define CMD_REACTION_DONE  0x26
#define CMD_SHAKE_DONE     0x27
#define CMD_SHAKE_PROGRESS 0x28
#define CMD_BEACON         0x29
#define CMD_HEARTBEAT      0x2A
#define CMD_CFG_SET        0x40
#define CMD_ACTION         0x41
#define CMD_DISPLAY_HELLO  0x42
#define CMD_OTA_BEGIN      0x43
#define CMD_OTA_STATUS     0x44
#define CMD_DISPLAY_INFO   0x45

#define MODE_NONE         0x00
#define MODE_REACTION     0x01
#define MODE_SHAKE        0x02

#define TIME_PENALTY      0xFFFF

/* ---- phases ---- */
#define PH_IDLE        0
#define PH_JOIN        1
#define PH_ANNOUNCE    2
#define PH_COUNTDOWN   3
#define PH_REACT_WAIT  4
#define PH_GO          5
#define PH_SHAKE       6
#define PH_RESULTS     7
#define PH_FINAL       8
#define PH_MAINT       9
#define PH_COUNT       10

/* ---- player status ---- */
#define PS_NONE     0   /* not in this round */
#define PS_PLAYING  1   /* in this round, no result yet */
#define PS_DONE     2   /* valid time in time_ms */
#define PS_PENALTY  3   /* false start or timeout */

/* ---- state flags ---- */
#define STF_DEUCE            0x01
#define STF_SCORES           0x02   /* results: winner and scores are visible */
#define STF_NEW_RECORD       0x04
#define STF_CAN_START        0x08   /* join phase with 2 or more players */
#define STF_UPDATE_AVAILABLE 0x10
#define STF_BONUS            0x20   /* bonus round: the points of this round count double */

/* ---- settings ---- */
#define CFG_VOLUME        1
#define CFG_RING_BRIGHT   2
#define CFG_STRIP_BRIGHT  3
#define CFG_ROUNDS        4
#define CFG_GAME_MODE     5
#define CFG_VOICE_GUIDE   6
#define CFG_JOIN_TIME     7
#define CFG_STRIP_MODE    8
#define CFG_DEUCE_LEAD    9
#define CFG_SCORING       10   /* SCORING_* */
#define CFG_BONUS         11   /* BONUS_* */
#define CFG_COUNT         11   /* highest key */
#define CFG_SLOTS         12   /* array size, index = key, index 0 unused */

static const uint8_t RTD_CFG_MIN[CFG_SLOTS] = {0, 0, 1, 0, 1, 0, 0, 3, 0, 1, 0, 0};
static const uint8_t RTD_CFG_MAX[CFG_SLOTS] = {0, 10, 10, 10, 15, 2, 1, 10, 7, 3, 1, 2};
static const uint8_t RTD_CFG_DEF[CFG_SLOTS] = {0, 10, 3, 5, 5, 0, 1, 5, 0, 3, 1, 1};

#define SCORING_CLASSIC   0    /* the round winner gets 1 point */
#define SCORING_PLACES    1    /* a player with a valid time gets 1 point per active player that it beat */
#define BONUS_OFF         0
#define BONUS_LAST        1    /* the last regular round is a bonus round */
#define BONUS_LAST_TWO    2    /* the last two regular rounds are bonus rounds */

/* ---- actions ---- */
#define ACT_ABORT_GAME    1
#define ACT_TEST_SOUND    2
#define ACT_RESET_BEST    3
#define ACT_FACTORY_RESET 4
#define ACT_START_NOW     5
#define ACT_IDENTIFY      6
#define ACT_MAINTENANCE   7
#define ACT_OTA_STICKS    8

/* ---- OTA ---- */
#define OTA_ST_STARTING   1
#define OTA_ST_UPTODATE   2
#define OTA_ST_FAILED     3

#define OTA_NONE          0   /* snapshot ota_state */
#define OTA_READY         1   /* maintenance mode, nothing running */
#define OTA_STICKS        2   /* updating stick ota_target */
#define OTA_DONE          3
#define OTA_ERROR         4   /* at least one stick failed */

/* ---- StatePacket (host -> display) ---- */
#define STATE_START       0x5A
#define STATE_SIZE        72
#define TIME_NONE         0xFFFF

typedef struct __attribute__((packed)) {
  uint8_t  stick_id;     /* 0 = slot empty, else ID_STICK1..4 */
  uint8_t  score;
  uint8_t  status;       /* PS_* */
  uint8_t  progress;     /* shake progress 0..100 */
  uint16_t time_ms;      /* valid when status == PS_DONE, else TIME_NONE */
} PlayerState;           /* 6 bytes */

typedef struct __attribute__((packed)) {
  uint8_t  start;            /*  0 STATE_START */
  uint8_t  version;          /*  1 PROTO_VERSION */
  uint8_t  seq;              /*  2 +1 per transmission */
  uint8_t  phase;            /*  3 PH_* */
  uint8_t  mode;             /*  4 MODE_* */
  uint8_t  round;            /*  5 1-based, 0 before the first round */
  uint8_t  total_rounds;     /*  6 */
  uint8_t  flags;            /*  7 STF_* */
  uint8_t  countdown;        /*  8 3,2,1 in PH_COUNTDOWN, else 0 */
  uint8_t  shake_target;     /*  9 10/15/20 in a shake round, else 0 */
  uint8_t  prompt_slot;      /* 10 1..4 in PH_JOIN, else 0 */
  uint8_t  winner;           /* 11 1..4 round winner (with STF_SCORES) or final winner, else 0 */
  uint8_t  joined_mask;      /* 12 bit0 = player slot 1 */
  uint8_t  active_mask;      /* 13 players in the current round */
  uint8_t  sticks_online;    /* 14 bit0 = stick ID 1 */
  uint8_t  secs_left;        /* 15 PH_SHAKE: seconds left, else 0 */
  PlayerState players[RTD_MAX_PLAYERS]; /* 16..39 */
  uint8_t  cfg[CFG_SLOTS];   /* 40..51 */
  uint16_t best_session_ms;  /* 52 TIME_NONE = none */
  uint16_t best_alltime_ms;  /* 54 */
  uint16_t games_total;      /* 56 */
  uint8_t  host_fw;          /* 58 version byte */
  uint8_t  host_fw_patch;    /* 59 */
  uint8_t  stick_fw[RTD_MAX_PLAYERS];        /* 60 version byte per stick ID, 0 = unknown */
  uint8_t  stick_fw_patch[RTD_MAX_PLAYERS];  /* 64 */
  uint8_t  ota_state;        /* 68 OTA_* */
  uint8_t  ota_target;       /* 69 stick ID being updated, else 0 */
  uint8_t  ota_progress;     /* 70 0..100 */
  uint8_t  crc;              /* 71 CRC8 over bytes 0..70 */
} StatePacket;

/* ---- helpers ---- */
static inline uint8_t calcCRC8(const uint8_t* data, uint8_t len) {
  uint8_t crc = 0x00;
  while (len--) {
    uint8_t extract = *data++;
    for (uint8_t i = 8; i; i--) {
      uint8_t sum = (uint8_t)((crc ^ extract) & 0x01);
      crc >>= 1;
      if (sum) crc ^= 0x8C;
      extract >>= 1;
    }
  }
  return crc;
}

static inline uint16_t packetData(const GamePacket* pkt) {
  return (uint16_t)(((uint16_t)pkt->data_high << 8) | pkt->data_low);
}

static inline void buildPacket(GamePacket* pkt, uint8_t dest, uint8_t src, uint8_t cmd, uint16_t data) {
  pkt->start = PACKET_START;
  pkt->dest_id = dest;
  pkt->src_id = src;
  pkt->cmd = cmd;
  pkt->data_high = (uint8_t)((data >> 8) & 0xFF);
  pkt->data_low = (uint8_t)(data & 0xFF);
  pkt->crc = calcCRC8((const uint8_t*)pkt, PACKET_SIZE - 1);
}

static inline int validatePacket(const uint8_t* data, int len) {
  if (len != PACKET_SIZE || data[0] != PACKET_START) return 0;
  return calcCRC8(data, PACKET_SIZE - 1) == data[PACKET_SIZE - 1];
}

static inline void sealState(StatePacket* st) {
  st->start = STATE_START;
  st->version = PROTO_VERSION;
  st->crc = calcCRC8((const uint8_t*)st, STATE_SIZE - 1);
}

static inline int validateState(const uint8_t* data, int len) {
  if (len != STATE_SIZE || data[0] != STATE_START || data[1] != PROTO_VERSION) return 0;
  return calcCRC8(data, STATE_SIZE - 1) == data[STATE_SIZE - 1];
}

static inline uint8_t cfgClamp(uint8_t key, uint8_t value) {
  if (key < 1 || key > CFG_COUNT) return value;
  if (value < RTD_CFG_MIN[key]) return RTD_CFG_MIN[key];
  if (value > RTD_CFG_MAX[key]) return RTD_CFG_MAX[key];
  return value;
}

#endif /* RTD_PROTOCOL_H */
```

## Appendix B — `shared/rtd_core/src/RtdNetConfig.h` (copy verbatim)

```c
#ifndef RTD_NET_CONFIG_H
#define RTD_NET_CONFIG_H

/* Default addresses. Devices also learn addresses at run time (DESIGN.md 3.2). */
#define RTD_MAC_HOST    {0x88, 0x57, 0x21, 0xB3, 0x05, 0xAC}
#define RTD_MAC_DISPLAY {0xD0, 0xCF, 0x13, 0x01, 0xD1, 0xA4}
#define RTD_MAC_STICK1  {0xBC, 0xFF, 0x4D, 0xF9, 0xF3, 0x91}   /* white  */
#define RTD_MAC_STICK2  {0xBC, 0xFF, 0x4D, 0xF9, 0xAE, 0x29}   /* blue   */
#define RTD_MAC_STICK3  {0xBC, 0xFF, 0x4D, 0xF9, 0xAC, 0x42}   /* red    */
#define RTD_MAC_STICK4  {0xBC, 0xFF, 0x4D, 0xF9, 0xBE, 0x62}   /* yellow */

/* Maintenance access point of the host (OTA).
 * Set your own password in RtdSecrets.h next to this file (not in git):
 *   #define RTD_AP_PASS "your-password"   (8 characters or more)
 * All devices must be built with the same password. */
#if defined(__has_include)
#if __has_include("RtdSecrets.h")
#include "RtdSecrets.h"
#endif
#endif
#define RTD_AP_SSID     "RTD-Host"
#ifndef RTD_AP_PASS
#define RTD_AP_PASS     "change-me-1234"   /* placeholder */
#endif
#define RTD_HOST_IP     "192.168.4.1"

#endif /* RTD_NET_CONFIG_H */
```
