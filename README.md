**English** · [繁體中文](README.zh-TW.md)

# Reaction Time Duel
## This project was selected for the Hanze Open Day showcase! Yipee!
<img width="600" height="800" alt="IMG_5491" src="https://github.com/user-attachments/assets/1e02c7fe-5259-4cc4-b624-4e4bc2f220d6" />


Reaction Time Duel 2.0 is a competitive multiplayer reaction-time game built with ESP32 and ESP8266 microcontrollers, communicating wirelessly over ESP-NOW. Up to 4 players compete in two game modes, each with a custom wireless joystick, while an ESP32-S3 touch display shows the game and its settings.

## What is New in 2.0

- One repository: host, joysticks, display, the shared library and the tests.
- One shared, tested game core in `shared/rtd_core`. It is plain C++11 with no Arduino or ESP-IDF include, so the same sources run in the firmware and in the PC tests.
- Touch settings on the display, including volume, ring brightness and strip brightness.
- An idle screen with the Hanze logo, the records and one dot per stick.
- Fair GO timing: a lost GO frame gives no player an advantage.
- The join-order fix: slots are always 1..n without a gap and no press is dropped.
- OTA for all three devices, with the host as the hub and its own web page.

## Game Modes

### Reaction Mode
Every player ring shows its player colour. The centre ring counts down in 4 stages (12, 9, 6, 3 lit LEDs) at uneven random intervals and is empty exactly at GO, so it shows that GO is coming but not when. When the rings turn yellow, press your button. The fastest reaction time wins the round. Press too early and you get a penalty.

### Shake Mode
After a 3-2-1 countdown, shake your joystick in any direction to reach the target count (10, 15 or 20 shakes; one shake is one back-and-forth). A center ring countdown shows the remaining time. The first to finish wins.

In Mixed mode (the default) the host alternates Reaction and Shake, starting with Reaction. Reaction only and Shake only are also available in the settings.

## Game Flow

The host runs ten phases. The phase is part of every state packet (DESIGN 5.1).

```
PH_IDLE --(stick press)--> PH_JOIN --(2+ players)--> PH_ANNOUNCE
                                                          |
              +-------------------------------------------+
              |                                           |
      (shake) v                                  (reaction) v
PH_COUNTDOWN -> PH_SHAKE                 PH_REACT_WAIT -> PH_GO
              |                                           |
              +---------------------+---------------------+
                                    v
                              PH_RESULTS --(more rounds)--> PH_ANNOUNCE
                                    |
                                    +--(last round)--> PH_FINAL --> PH_IDLE

PH_IDLE --(BOOT 3 s or display)--> PH_MAINT --(reboot)--> PH_IDLE
```

1. **Idle.** The host waits for a player. It plays "press to join" on entry and then every 45 s, only when the voice guide is on.
2. **Join.** The prompted slot is always the lowest free slot. Any stick without a slot that presses claims the prompted slot at once, so two presses in the same millisecond get two slots. Each prompt lasts `CFG_JOIN_TIME` seconds. With 2 or more players the game starts; 4 players start at once; a single player goes back to idle after 3 timeouts.
3. **Announce.** The host plays the mode, the shake target and, the first time in a game, the long instructions when the voice guide is on.
4. **Countdown.** Shake rounds count 3-2-1 with audio, ring flash and a vibration tick. Reaction rounds skip the countdown and wait a random 3, 5 or 7 s; the centre ring counts down in 4 stages of random length and empties at GO.
5. **Play.** Reaction: the rings freeze to yellow at GO. Shake: race to the target count. A stick reports progress milestones, so the rings show live progress.
6. **Results.** The centre ring counts down the hard limit of the round: 3000 ms after GO in a reaction round, 15000 ms in a shake round. A player with no result at the limit gets a penalty. Times show first, then the winner and the scores.
7. **Final.** After the last round the host announces the final winner.
8. **Ties.** Two players share the top score: only those two play a deuce, and a lead of `CFG_DEUCE_LEAD` wins. Three or four players share the top score: sudden death, and the first round with a winner ends the game. Sticks that are out get `CMD_IDLE`.
9. **Scoring.** CLASSIC gives the round winner 1 point. PLACES: you get one point for every player you beat. You beat a player when that player has no valid time or a slower time; two players with the same time do not beat each other. The bonus round doubles every point: LAST makes the last round a bonus round, LAST 2 makes the last two rounds bonus rounds. LAST needs at least 2 rounds and LAST 2 needs at least 3 rounds. A deuce or sudden-death round always scores the classic way.

## Hardware

Three device types, one repository.

| Device | Board | Qty | Role |
|--------|-------|-----|------|
| Host | ESP32 DevKit-C | 1 | Game, LEDs, audio, settings, OTA hub |
| Joystick | ESP8266 ESP-12F | up to 4 | Button, MPU-6050, vibration motor |
| Display | ESP32-S3 Waveshare 800x480 | 1 | Game screen and settings |

The host has 5 NeoPixel rings of 12 LEDs (60 in total) for the players and one 89-LED WS2812B ambient strip.

### Joystick Colors

Each physical stick has a fixed ID and a fixed color. The ID is written to EEPROM by the first USB flash of that stick.

| ID | Color | Default MAC | USB environment |
|----|-------|-------------|-----------------|
| 1 | white | BC:FF:4D:F9:F3:91 | `stick1` |
| 2 | blue | BC:FF:4D:F9:AE:29 | `stick2` |
| 3 | red | BC:FF:4D:F9:AC:42 | `stick3` |
| 4 | yellow | BC:FF:4D:F9:BE:62 | `stick4` |

### Host Pins (ESP32)

| Pin | Function |
|-----|----------|
| GPIO4 | NeoPixel ring data (5 rings, 60 LEDs) |
| GPIO16 | WS2812B ambient strip data (89 LEDs) |
| GPIO25 | I2S DOUT (audio data) |
| GPIO26 | I2S BCLK (bit clock) |
| GPIO27 | I2S LRC (word select) |
| GPIO33 | Amplifier GAIN, driven LOW for maximum gain |
| GPIO0 | BOOT button, held 3 s enters maintenance mode |

Volume is set in software by the MP3 decoder gain (DESIGN 8).

### Joystick Pins (ESP8266)

| Pin | Function |
|-----|----------|
| GPIO14 | Button input, active LOW, internal pull-up |
| GPIO12 | Vibration motor output |
| GPIO4 | MPU-6050 SDA (I2C) |
| GPIO5 | MPU-6050 SCL (I2C, 400 kHz) |

### Display Pins

The RGB panel pins come from the Waveshare board. The touch controller is a GT911 on I2C0:

| Pin | Function |
|-----|----------|
| GPIO8 | GT911 SDA |
| GPIO9 | GT911 SCL (400 kHz) |
| GPIO4 | GT911 INT (the reset line is on the CH422G expander) |

## Architecture

```
   +-----------------------------+
   |  HOST  ESP32 DevKit-C       |  game logic, 5 NeoPixel rings,
   |  Arduino, firmware/host     |  89-LED strip, I2S MP3 audio,
   |                             |  settings (NVS), OTA hub (AP + web)
   +--------------+--------------+
                 |
                 | ESP-NOW, channel 6
    +--------+---+----+--------+---------+
 +------+ +------+ +------+ +------+ +---------+
 |STICK1| |STICK2| |STICK3| |STICK4| | DISPLAY |
 |white | |blue  | |red   | |yellow| | ESP32-S3|
 +------+ +------+ +------+ +------+ | 800x480 |
                                     | GT911   |
                                     +---------+

 phone or laptop browser
   |  Wi-Fi AP "RTD-Host", 192.168.4.1 (maintenance mode only)
   +--> HOST web page: host, joystick and display update
```

### Repository Layout

```
ReactionTimeDuel/
  docs/DESIGN.md           normative design (sections 1 to 10)
  shared/rtd_core/         library shared by all three firmwares and tests
    src/RtdProtocol.h      wire format, commands, settings, state packet
    src/RtdNetConfig.h     default MACs, AP name and password
    src/HostGame.h/.cpp    host game state machine
    src/StickLogic.h/.cpp  joystick state machine
    src/DisplayLink.h/.cpp display link logic
  firmware/host/           ESP32 Arduino project (game, LEDs, audio, OTA)
  firmware/joystick/       ESP8266 Arduino project (button, motor, IMU)
  firmware/display/        ESP32-S3 ESP-IDF project (LVGL 8.3)
  tests/                   native simulator and the T1..T29 tests
  tools/rtd.py             build, flash, test, audio and dist wrapper
  tools/run_tests.py       builds and runs the tests with a native g++
  tools/png2lvgl.py        PNG to LVGL 8 C array (RGB565 + alpha)
```

## Build and Flash

### Prerequisites

- PlatformIO Core, with `pio` on PATH.
- Python 3.
- USB cables for the first flash of every device.
- For the tests: a C++ compiler. `tools/run_tests.py` tries `CXX`, `g++` on PATH, `C:/msys64/mingw64/bin/g++.exe` and then `clang++`.

### Windows: Build ESP32 Targets from PowerShell

On Windows, build the ESP32 targets from PowerShell, not from Git Bash. The PlatformIO ESP32 tools refuse MSYS. `tools/rtd.py` clears `MSYSTEM` before it calls `pio`, so the wrapper also works when you start it from Git Bash.

### The First Flash Is Always USB

The 2.0 partition tables changed on every device. The first 2.0 flash of each device must go over USB:

- host: two 1.25 MB OTA slots and a 1.375 MB SPIFFS (`firmware/host/partitions.csv`);
- display: 8 MB flash with two 3.625 MB OTA slots (`firmware/display/partitions.csv`);
- joystick: two 1 MB OTA slots from `eagle.flash.4m2m.ld`.

After that first USB flash, every device updates over the air. See [OTA Update](#ota-update).

### VS Code with PlatformIO

The repository root has no `platformio.ini`; each device has its own project under `firmware/`. Open `ReactionTimeDuel.code-workspace` (File > Open Workspace from File). PlatformIO then lists the host, the joystick and the display as three projects. Select the project in the PlatformIO status bar before you build or upload.

### Commands with tools/rtd.py

```
python tools/rtd.py build                    # host, display and stick image
python tools/rtd.py build host               # one target
python tools/rtd.py flash host --port COM5
python tools/rtd.py flash display --port COM5
python tools/rtd.py flash stick1 --port COM5  # stick 1, writes the ID
python tools/rtd.py flash stick2 --port COM5
python tools/rtd.py flash stick3 --port COM5
python tools/rtd.py flash stick4 --port COM5
python tools/rtd.py audio --port COM5         # host MP3 file system
python tools/rtd.py test                      # native core tests
python tools/rtd.py dist                      # build all, copy to dist/
python tools/rtd.py ota display|sticks|host   # send an image over the air (see OTA)
```

`--port` is optional. Without it PlatformIO picks the port.

### Plain PlatformIO Commands

```
pio run -d firmware/host                  # build the host
pio run -d firmware/host -t upload        # USB flash the host
pio run -d firmware/host -t uploadfs      # USB upload the MP3 file system
pio run -d firmware/display -t upload     # USB flash the display
pio run -d firmware/joystick -e stick1    # build one stick image
pio run -d firmware/joystick -e stick1 -t upload   # USB flash stick 1
pio run -d firmware/joystick -e stick     # the one OTA image
```

### Joysticks

`stick1` .. `stick4` are the USB images. Each one writes its stick ID to EEPROM on the first boot after the flash. `stick` is the single OTA image for all four sticks; it has no ID and reads the ID from EEPROM.

### Host Audio

`python tools/rtd.py audio` uploads the host MP3 file system (24 files). The stored joystick image lives in the host OTA slot that does not run. A host update overwrites it, so update in this order: host, display, joysticks.

## Settings

All settings live on the host in NVS and are stored under the namespace `rtd`. The display sends every change, so the host owns the values.

A long press of 1000 ms on the gear, at the bottom right of the screen, opens the settings. A tap only shows the hint "HOLD 1 S FOR SETTINGS". The gear is orange while a finger is on it. The back button closes the screen, and so does 30 s without a touch.

| Setting | Label | Range | Default | Tab |
|---------|-------|-------|---------|-----|
| `CFG_VOLUME` | Volume | 0..10 | 10 | SOUND & LIGHT |
| `CFG_RING_BRIGHT` | Ring brightness | 1..10 | 3 | SOUND & LIGHT |
| `CFG_STRIP_BRIGHT` | Strip brightness | 0..10 | 5 | SOUND & LIGHT |
| `CFG_STRIP_MODE` | Strip effect | 0..7 | 0 | SOUND & LIGHT |
| `CFG_ROUNDS` | Rounds | 1..15 | 5 | GAME |
| `CFG_GAME_MODE` | Mode | 0..2 | 0 | GAME |
| `CFG_VOICE_GUIDE` | Voice guide | 0..1 | 1 | GAME |
| `CFG_JOIN_TIME` | Join time (s) | 3..10 | 5 | GAME |
| `CFG_DEUCE_LEAD` | Deuce lead | 1..3 | 3 | GAME |
| `CFG_SCORING` | Scoring | 0..1 | 1 | GAME |
| `CFG_BONUS` | Bonus | 0..2 | 1 | GAME |

- Volume 0 is mute. Volume 10 is the baseline loudness, and the gain is `(v/10)^2`.
- Ring brightness is `16 x v`. Strip brightness is `16 x v`, and 0 turns the strip off.
- Strip effect: 0 AUTO, 1 RAINBOW, 2 SPARKLE, 3 METEOR, 4 CHASE, 5 BREATHING, 6 FIRE, 7 HANZE.
- Mode: 0 MIXED, 1 REACTION only, 2 SHAKE only.
- Scoring: 0 CLASSIC, 1 PLACES.
- Bonus: 0 OFF, 1 LAST, 2 LAST 2.
- Rounds, mode, scoring and bonus are latched when a game starts. Volume and brightness apply at once.
- The display rounds buttons offer 3, 5, 7 and 10. The host accepts any value from 1 to 15.

The settings screen has three tabs:

- **SOUND & LIGHT**: volume, ring brightness, strip brightness and strip effect. Releasing the volume slider plays the test beep.
- **GAME**: rounds, mode, scoring, bonus, voice guide, join time and deuce lead.
- **STATUS**: one row per stick with a color dot, ONLINE or OFFLINE, version and a BUZZ button. It also shows the host and display versions, the best today, the record with RESET RECORD, the games total, START NOW, ABORT GAME, RESTORE DEFAULTS and SERVICE MENU. SERVICE MENU opens the service menu (see Debug); its UPDATE page holds START / LEAVE UPDATE MODE and UPDATE JOYSTICKS.

| Control | Action | Effect |
|---------|--------|--------|
| BUZZ | `ACT_IDENTIFY` | 500 ms vibration, `PH_IDLE` only |
| START NOW | `ACT_START_NOW` | start at once with 2 or more players |
| ABORT GAME | `ACT_ABORT_GAME` | stop audio, idle all sticks, go to idle |
| RESET RECORD | `ACT_RESET_BEST` | clear the best today and the record |
| RESTORE DEFAULTS | `ACT_FACTORY_RESET` | all settings back to default |
| START / LEAVE UPDATE MODE (service menu) | `ACT_MAINTENANCE` | enter or leave maintenance mode |
| UPDATE JOYSTICKS (service menu) | `ACT_OTA_STICKS` | update the sticks over the air |
| Volume release | `ACT_TEST_SOUND` | play the beep when audio is idle |

START NOW needs `STF_CAN_START`. UPDATE JOYSTICKS needs `STF_UPDATE_AVAILABLE`, which means the host holds a joystick image and at least one online stick has another version. ABORT GAME, RESTORE DEFAULTS, START / LEAVE UPDATE MODE and UPDATE JOYSTICKS ask for confirmation first. A control follows the snapshot unless you touched it in the last 1000 ms.

## OTA Update

After the first USB flash, every device updates over the air with the host as the hub. The host serves the images and the web page from its own access point.

### Maintenance Mode

1. Make sure the game is idle. The host enters maintenance mode only from `PH_IDLE`.
2. Hold the host BOOT button (GPIO0) for 3 s, or press START UPDATE MODE in the service menu of the display (settings, STATUS tab, SERVICE MENU).
3. Join the Wi-Fi access point `RTD-Host`.
4. Open `http://192.168.4.1` in a browser.

The AP password is `RTD_AP_PASS`. The repository holds only a placeholder: put `#define RTD_AP_PASS "your-password"` (8 characters or more) in `shared/rtd_core/src/RtdSecrets.h`, which git ignores, and build all devices with it. ESP-NOW keeps running on channel 6, so the sticks and the display stay reachable. UPDATE JOYSTICKS aborts a running game and then enters maintenance mode. To leave, press "Exit update mode" on the page or press LEAVE UPDATE MODE on the display. The host reboots. BOOT does not leave update mode: GPIO0 also sits on the USB-serial reset circuit, and a serial tool can hold it LOW.

### Joysticks (through the Host)

1. Run `python tools/rtd.py dist` on the PC. It writes `dist/host.bin`, `dist/joystick.bin` and `dist/display.bin`.
2. Enter maintenance mode and open the page.
3. Upload `dist/joystick.bin` under "Joystick firmware". The host replaces the old image and shows the stored version.
4. Press "Update joysticks now" on the page, or UPDATE JOYSTICKS on the display.
5. Wait. The host sends `CMD_OTA_BEGIN` to each online stick with another version, in ID order.
6. Watch the progress on the page or on the display. The stick joins the AP, downloads `http://192.168.4.1/fw/joystick.bin` and reboots.
7. Check the result. A stick that does not report back within 120 s counts as failed and the next stick starts.

Each stick keeps its ID: the OTA image reads the ID from EEPROM. A stick that already has the image version answers `OTA_ST_UPTODATE` and is skipped.

### Host

1. Enter maintenance mode and open the page.
2. Upload `dist/host.bin` under "Host firmware".
3. Wait. The host writes the inactive OTA slot, scans for the `RTDFW|host|` marker and reboots.

### Display

1. Keep the host in maintenance mode. The display joins the AP as a station and shows the service menu with "Update mode: ON".
2. Check the display address on the page or on the UPDATE page of the service menu (`192.168.4.x`).
3. Upload `dist/display.bin` under "Display firmware". The browser sends the file to the display at `POST /update`.
4. Wait for "ok". The display scans for the `RTDFW|display|` marker, sets the new boot partition and reboots.

The display marks the new application valid 10 s after the UI is up. Without that mark the bootloader rolls back.

### From the Command Line

The PC needs no manual address: join the Wi-Fi `RTD-Host` and the host gives the PC an address (`192.168.4.x`). The host must be in maintenance mode. The tool reads the display address from the host.

```bash
python tools/rtd.py ota display     # dist/display.bin -> display, the display restarts
python tools/rtd.py ota sticks      # dist/joystick.bin -> host, then the host updates each online stick
python tools/rtd.py ota host        # dist/host.bin -> host, the host restarts and enters maintenance mode again
```

After a host update the host restarts and enters maintenance mode again by itself. Update the host before the joysticks: a host update erases the stored joystick image. The host reads a stored joystick image back and rejects it when size or MD5 differ. PlatformIO has the same step as the target `ota`, which builds the image first (in VS Code: Project Tasks, Custom, "OTA upload"):

```bash
pio run -d firmware/display -t ota
pio run -d firmware/joystick -e stick -t ota
pio run -d firmware/host -t ota
```

### Joystick Recovery

Use this when a stick has no working firmware or cannot join:

1. Power the stick off.
2. Hold the stick button and power it on.
3. Keep the button held for 5 s. The stick goes straight to the OTA download and retries the AP for 60 s.

### Before a Public Event

Set `RTD_AP_PASS` in `shared/rtd_core/src/RtdSecrets.h` (git ignores this file). The repository value is a placeholder. Reflash the host, the joysticks and the display after the change.

Switch `debug_wifi` off (see Debug). Then the access point is on only in maintenance mode.

## Debug

You do not need a USB cable on every device. The joysticks and the display report their health to the host by radio, and the host shows everything on one web page.

### Host Debug Page

1. Join the Wi-Fi network `RTD-Host` with a phone or a laptop.
2. Open `http://192.168.4.1/debug`.

The page refreshes every second. It shows:

- Host counters: received frames, dropped frames, bad frames, failed sends per device, the longest loop pass of the last 10 s, free heap and the reset reason.
- One row per device (four joysticks and the display): the time since its last frame and its reported values.
- The event log: phase changes, joins and each game packet. Switch **TRACE** on to see beacons, heartbeats and diagnostics reports too.

| Reported value | Meaning |
|---|---|
| `fw_version`, `reset_reason`, `uptime_min`, `heap_kb` | Firmware, the cause of the last reset, minutes since boot, free heap |
| `rx_drop`, `tx_fail`, `bad_frame` | Radio faults since boot (each stops at 255) |
| `loop_max_ms`, `link_gap_ds` | Longest loop pass and longest gap between host frames (0.1 s units) since the last report |
| `btn_edges`, `btn_drop`, `stick_state`, `mpu_ok` | Joystick only: button presses, lost presses, state, accelerometer answer |
| `shake_count`, `shake_peak_max_dg`, `shake_peak_min_dg`, `shake_peak_avg_dg`, `shake_soft_max_dg` | Joystick only, last shake round, for calibration: counted shakes; highest, lowest and average peak of a counted shake; highest magnitude that stayed below the count threshold. Peaks are in 0.1 g (15 = the 1.5 g threshold) |
| `snap_per_10s`, `touch_ok`, `ota_state`, `screen` | Display only: snapshots in 10 s, touch controller answer, OTA state, phase on screen |

Each device sends one value per second, so a full row needs about 18 s. A joystick reports only while it is idle, not during a round.

### Debug Wi-Fi

The setting `debug_wifi` keeps the access point and the debug page on in every phase. It is on by default in 2.0. Outside maintenance mode the pages are read-only: firmware uploads, the joystick OTA start and the exit command answer `409`.

Switch it with the button on the debug page or with `w` on the serial console. The change takes effect at the next boot. With `debug_wifi` off, the access point starts only in maintenance mode, as before.

### Host Serial Console

Connect only the host by USB at 115200 baud, for example with `python tools/rtd.py monitor`. Each event prints one `[EVT]` line. Keys: `?` help, `s` status, `l` whole event log, `t` trace on/off, `w` debug Wi-Fi on/off.

### Display Panel

Press the link icon on the display for 3 s. A small panel shows the firmware version, the link state, snapshots per 10 s, radio faults, the longest UI pass, free heap and the reset reason. Press 3 s again to hide it.

### Service Menu on the Display

Open the service menu from the settings screen: long press the gear (1 s), open the STATUS tab and press **SERVICE MENU**. In maintenance mode the service screen shows at once. It has four pages:

- **UPDATE**: the access point name, password and URL, the display address, the OTA state and progress, START / LEAVE UPDATE MODE and UPDATE JOYSTICKS.
- **STICKS**: one row per joystick with its colour, link, age, firmware, frames, faults, loop time, link gap, state and sensor.
- **HOST**: the host counters, the access point state, the number of clients and the debug Wi-Fi switch.
- **DISPLAY**: the display's own counters and the host's view of the display.

The debug Wi-Fi switch takes effect at the next boot.

### From a PC

```
python tools/rtd.py debug              # one status report and the event log, saved under logs/
python tools/rtd.py debug --watch 2    # repeat every 2 s
python tools/rtd.py monitor            # host serial console
python tools/rtd.py ports              # list the serial ports
```

The PC must be on the `RTD-Host` network for `debug`.

## Protocol

Firmware V4.0.3, protocol version 4. All devices use ESP-NOW on Wi-Fi channel 6 with power save off. The first byte selects the frame type.

`GamePacket`, 7 bytes, first byte `0x0A`:

| Byte | Field | Meaning |
|------|-------|---------|
| 0 | `start` | always `0x0A` |
| 1 | `dest_id` | `0x00` host, `0x01-04` stick, `0x10` display, `0xFF` all |
| 2 | `src_id` | sender ID |
| 3 | `cmd` | command byte |
| 4 | `data_high` | data high byte |
| 5 | `data_low` | data low byte |
| 6 | `crc` | CRC8 over bytes 0..5 |

CRC8 is polynomial `0x8C` (reflected), init 0. `StatePacket` is 72 bytes, first byte `0x5A`, and carries the full game snapshot from the host to the display.

| Command | Hex | Direction | Data |
|---------|-----|-----------|------|
| `CMD_OK` | 0x0B | host -> stick | 0 / slot 1..4 |
| `CMD_REQ_ID` | 0x0D | stick -> host | version / patch |
| `CMD_ACK` | 0x0E | both | 0 / acked command |
| `CMD_GAME_START` | 0x21 | host -> stick | mode / shake target |
| `CMD_GO` | 0x22 | host -> stick | 16-bit age in ms |
| `CMD_VIBRATE` | 0x23 | host -> stick | 0 / duration x 10 ms |
| `CMD_IDLE` | 0x24 | host -> stick | 0 |
| `CMD_COUNTDOWN` | 0x25 | host -> stick | 0 / 3, 2, 1 |
| `CMD_REACTION_DONE` | 0x26 | stick -> host | ms, `0xFFFF` = penalty |
| `CMD_SHAKE_DONE` | 0x27 | stick -> host | ms, `0xFFFF` = timeout |
| `CMD_SHAKE_PROGRESS` | 0x28 | stick -> host | count / target |
| `CMD_BEACON` | 0x29 | host -> all | phase / stick masks |
| `CMD_HEARTBEAT` | 0x2A | stick -> host | version / patch |
| `CMD_CFG_SET` | 0x40 | display -> host | key / value |
| `CMD_ACTION` | 0x41 | display -> host | action / argument |
| `CMD_DISPLAY_HELLO` | 0x42 | display -> host | version / patch |
| `CMD_OTA_BEGIN` | 0x43 | host -> stick | version / patch |
| `CMD_OTA_STATUS` | 0x44 | stick -> host | status / detail |
| `CMD_DISPLAY_INFO` | 0x45 | display -> host | last IP octet / state |

### Fair Timing

The host fixes the GO instant `t0 = micros()` and then sends `CMD_GO` in two ways. The first frame is a broadcast with age 0, so all sticks receive it at the same instant. The second frame is a unicast to each active stick, with ACK tracking, and every retransmission carries `age = (micros() - t0) / 1000`.

A stick takes the first valid `CMD_GO` while it waits and computes `go_time = rx_timestamp - age * 1000`. Later copies only get an ACK, and a broadcast is never ACKed. A lost frame therefore gives no advantage.

A falling edge in the 1000 ms before GO is a false start, and so is a button that is down at GO. Falling edges in the first 60 ms after GO are ignored. The first valid edge after that window gives the result in ms, minimum 1.

Reliability rules, address learning and the rest of the protocol are in `docs/DESIGN.md` sections 3 and 4.

## Tests

`python tools/rtd.py test` builds and runs the native test suite. `tools/run_tests.py` compiles `shared/rtd_core/src` and `tests/` with a native g++ using `-std=c++11 -Wall -Wextra -Werror`, then runs the binary from `.pio_native`.

The simulator links the real game core with fake ports: a virtual clock with microsecond event resolution, a radio with per-link loss, 0..3 ms jitter and duplicates, four `StickLogic` instances with scripted players, one `HostGame` and one `DisplayLink`.

The 29 tests cover the protocol and CRC, the happy path, join order and join edge cases, GO fairness, false starts, lost results, stick failures, ties, settings, actions, the 32-bit clock wrap, the OTA sequence, the display link and a 2000-game fuzz run. T16 to T22 are regression tests for defects that a code review found: a stale stick slot after a lost idle command or a host reboot, scoring after a long uptime, OTA from maintenance mode, the idle prompt interval, the shake start time and a button edge before GO. T24 to T27 cover places scoring, the bonus round, a catch-up win through the bonus round and the scoring settings latch. T28 covers shake detection in every direction and at rest, T29 the reaction wait countdown.

The firmware builds are checked by the compiler only. Touch, OTA, LED and audio behavior were not yet run on hardware at the time of the 2.0 commit.

## Troubleshooting

- **A stick does not join.** Check that the host is in `PH_IDLE` and that the stick has an ID. Flash `stick1`..`stick4` over USB once so the EEPROM record is written.
- **The motor pulses every 2 s and the stick does nothing.** The stick has no valid ID. Its EEPROM record and the MAC table both missed. Flash the stick with one of the four `stickN` environments.
- **The display shows "CONNECTING TO HOST...".** The display has no valid snapshot for 2000 ms. Check the host power, the firmware versions and that both devices use channel 6 with power save off.
- **OTA fails.** Upload an image of the right kind: the host checks `RTDFW|joystick|` or `RTDFW|host|`, and the display checks `RTDFW|display|`. A wrong image is rejected. A stick that cannot join the AP times out after 120 s; use the 5 s recovery at power-on.
- **The build fails in Git Bash.** The PlatformIO ESP32 tools refuse MSYS. Run the command from PowerShell, or use `python tools/rtd.py`, which clears `MSYSTEM`.
- **`lv_conf.h` is not found.** The display project keeps `lv_conf.h` in `firmware/display/lib` and adds `-Ilib` plus `LV_CONF_INCLUDE_SIMPLE`. Build through `firmware/display/platformio.ini` and do not move the file.
- **`pio` is not found.** Install PlatformIO Core and put `pio` on PATH.
- **The tests stop with "no C++ compiler found".** Set `CXX`, or install g++ or clang++. The script also looks for `C:/msys64/mingw64/bin/g++.exe`.

## Credits and License

MIT License

The Hanze logo is property of Hanze University of Applied Sciences. The file comes from Wikimedia Commons under CC BY-SA 2.5 and is used for the open-day showcase.

The display font is Montserrat, under the SIL Open Font License 1.1 (see `firmware/display/assets/fonts/OFL.txt`).

Libraries: LVGL 8.3.11, ESP8266Audio 1.9.7 and NeoPixelBus ^2.8.3.
