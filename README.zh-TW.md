[English](README.md) · **繁體中文**

# 反應時間對決 (Reaction Time Duel)
## 本專案入選 Hanze Open Day 展出！耶！
<img width="2048" height="1536" alt="IMG_2032" src="https://github.com/user-attachments/assets/0c61f458-73cb-4014-977f-d88b95b5550a" />

Reaction Time Duel 2.0 是一款多人對戰的反應時間遊戲，用 ESP32 和 ESP8266 做成，裝置之間靠 ESP-NOW 無線通訊。最多 4 位玩家各拿一支無線搖桿，在兩種模式裡比快；另外有一塊 ESP32-S3 觸控螢幕顯示比賽畫面，也能直接在上面調設定。

## 2.0 新增了什麼

- 整個專案收進同一個 repo：主機、搖桿、顯示器、共用函式庫和測試都在這裡。
- 遊戲邏輯集中在 `shared/rtd_core`，而且有測試。它是純 C++11，不依賴 Arduino 和 ESP-IDF，所以同一份程式碼既燒進韌體，也能在 PC 上跑測試。
- 顯示器可以用觸控調設定，包含音量、燈環亮度和燈條亮度。
- 待機畫面有 Hanze 標誌、最佳紀錄，以及每支搖桿一顆的狀態燈。
- GO 訊號的計時很公平：就算有人漏收 GO 封包，也不會有人佔便宜。
- 修好加入順序的問題：座位一定是 1..n 連號，按下去不會被漏掉。
- 三種裝置都能 OTA 更新，由主機當中繼站，主機自己帶一個網頁。

## 遊戲模式

### 反應模式 (Reaction)
NeoPixel 燈環會不停換顏色。燈一定格成黃色就按按鈕，最快的人贏這一回合。太早按算犯規。

### 搖晃模式 (Shake)
3-2-1 倒數完就開始搖搖桿，搖到目標次數（10、15 或 20 下）。中央燈環會倒數剩餘時間，最先搖完的人贏。

預設是 Mixed 模式：主機讓反應和搖晃輪流出現，第一回合是反應。設定裡也可以改成只玩反應或只玩搖晃。

## 遊戲流程

主機有十個階段 (phase)，每個狀態封包都會帶目前的階段（DESIGN 5.1）。

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

1. **待機 (Idle)。** 主機等玩家加入。語音導覽開著的時候，一進待機就播一次「按下加入」，之後每 45 秒再播一次。
2. **加入 (Join)。** 主機提示的座位永遠是編號最小的空位。還沒有座位的搖桿一按，就立刻拿到這個座位，所以兩個人在同一毫秒按下也會各拿到一個座位。每次提示維持 `CFG_JOIN_TIME` 秒。有 2 位以上玩家就開始；滿 4 位馬上開始；只有 1 位玩家的話，逾時 3 次後回到待機。
3. **宣布 (Announce)。** 主機播報這回合的模式和搖晃目標。語音導覽開著時，每場比賽第一次遇到某個模式會播完整說明。
4. **倒數 (Countdown)。** 搖晃回合會倒數 3-2-1，同時有語音、燈環閃爍和搖桿震動。反應回合沒有倒數，隨機等 3、5 或 7 秒。
5. **比賽 (Play)。** 反應：GO 的時候燈環定格成黃色。搖晃：比誰先搖到目標次數。搖桿會回報進度，所以燈環上看得到即時進度。
6. **結果 (Results)。** 中央燈環會倒數這一回合的時限：反應回合是 GO 之後 3000 ms，搖晃回合是 15000 ms。時限一到還沒有成績的玩家算犯規。畫面先顯示時間，再顯示贏家和分數。
7. **總結 (Final)。** 最後一回合結束後，主機宣布總冠軍。
8. **平手。** 兩人同分領先：只有這兩人打 deuce，領先 `CFG_DEUCE_LEAD` 分的人贏。三人或四人同分領先：驟死賽，第一個分出勝負的回合就結束比賽。被淘汰的搖桿會收到 `CMD_IDLE`。
9. **計分。** CLASSIC：每回合只有贏家拿 1 分。PLACES：每贏過一位玩家就拿 1 分。對手沒有有效成績，或成績比你慢，都算你贏過他；兩人時間一樣，就都不算贏過對方。加分局的得分全部加倍：LAST 是最後一回合，LAST 2 是最後兩回合。LAST 至少要 2 回合，LAST 2 至少要 3 回合。deuce 和驟死賽一律用 CLASSIC 計分，也不會是加分局。

## 硬體

三種裝置，一個 repo。

| 裝置 | 板子 | 數量 | 角色 |
|------|------|------|------|
| 主機 (Host) | ESP32 DevKit-C | 1 | 遊戲、LED、音效、設定、OTA 中繼站 |
| 搖桿 (Joystick) | ESP8266 ESP-12F | 最多 4 | 按鈕、MPU-6050、震動馬達 |
| 顯示器 (Display) | ESP32-S3 Waveshare 800x480 | 1 | 比賽畫面與設定 |

主機接了 5 個 12 顆 LED 的 NeoPixel 燈環（共 60 顆）給玩家用，另外有一條 89 顆 LED 的 WS2812B 氣氛燈條。

### 搖桿顏色

每支搖桿的 ID 和顏色都是固定的。第一次用 USB 燒錄時，ID 會寫進那支搖桿的 EEPROM。

| ID | 顏色 | 預設 MAC | USB 環境 |
|----|------|----------|----------|
| 1 | 白 (white) | BC:FF:4D:F9:F3:91 | `stick1` |
| 2 | 藍 (blue) | BC:FF:4D:F9:AE:29 | `stick2` |
| 3 | 紅 (red) | BC:FF:4D:F9:AC:42 | `stick3` |
| 4 | 黃 (yellow) | BC:FF:4D:F9:BE:62 | `stick4` |

### 主機腳位 (ESP32)

| 腳位 | 功能 |
|------|------|
| GPIO4 | NeoPixel 燈環資料線（5 個燈環，60 顆 LED） |
| GPIO16 | WS2812B 氣氛燈條資料線（89 顆 LED） |
| GPIO25 | I2S DOUT（音訊資料） |
| GPIO26 | I2S BCLK（位元時脈） |
| GPIO27 | I2S LRC（左右聲道時脈） |
| GPIO33 | 擴大機 GAIN，拉到 LOW 取最大增益 |
| GPIO0 | BOOT 鍵，按住 3 秒切換維護模式 |

音量是軟體調的，改的是 MP3 解碼器的增益（DESIGN 8）。

### 搖桿腳位 (ESP8266)

| 腳位 | 功能 |
|------|------|
| GPIO14 | 按鈕輸入，低電位觸發，用內建上拉 |
| GPIO12 | 震動馬達輸出 |
| GPIO4 | MPU-6050 SDA (I2C) |
| GPIO5 | MPU-6050 SCL (I2C, 400 kHz) |

### 顯示器腳位

RGB 面板的腳位照 Waveshare 板子的定義。觸控晶片是 GT911，接在 I2C0：

| 腳位 | 功能 |
|------|------|
| GPIO8 | GT911 SDA |
| GPIO9 | GT911 SCL (400 kHz) |
| GPIO4 | GT911 INT（reset 線接在 CH422G 擴充晶片上） |

## 架構

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

### Repo 目錄

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
  tests/                   native simulator and the T1..T22 tests
  tools/rtd.py             build, flash, test, audio and dist wrapper
  tools/run_tests.py       builds and runs the tests with a native g++
  tools/png2lvgl.py        PNG to LVGL 8 C array (RGB565 + alpha)
```

## 編譯與燒錄

### 事前準備

- PlatformIO Core，`pio` 要在 PATH 裡。
- Python 3。
- USB 線：每個裝置第一次燒錄都要用。
- 跑測試需要 C++ 編譯器。`tools/run_tests.py` 會依序找 `CXX`、PATH 裡的 `g++`、`C:/msys64/mingw64/bin/g++.exe`，最後才找 `clang++`。

### Windows：ESP32 要在 PowerShell 編譯

在 Windows 上編 ESP32 的目標請用 PowerShell，別用 Git Bash，因為 PlatformIO 的 ESP32 工具鏈不支援 MSYS。`tools/rtd.py` 呼叫 `pio` 之前會先清掉 `MSYSTEM`，所以從 Git Bash 執行這支腳本也沒問題。

### 第一次一定要用 USB 燒

2.0 改了每個裝置的分割表，所以每個裝置第一次燒 2.0 都必須走 USB：

- 主機：兩個 1.25 MB 的 OTA 區，加一個 1.375 MB 的 SPIFFS（`firmware/host/partitions.csv`）；
- 顯示器：8 MB flash，兩個 3.625 MB 的 OTA 區（`firmware/display/partitions.csv`）；
- 搖桿：`eagle.flash.4m2m.ld` 提供的兩個 1 MB OTA 區。

用 USB 燒過一次之後，每個裝置都能無線更新，做法見 [OTA 更新](#ota-更新)。

### 用 VS Code 加 PlatformIO

Repo 根目錄沒有 `platformio.ini`，三個裝置各自在 `firmware/` 底下有自己的專案，所以直接開根目錄，PlatformIO 會認不出來。請開 `ReactionTimeDuel.code-workspace`（File > Open Workspace from File），PlatformIO 就會列出主機、搖桿、顯示器三個專案。編譯或燒錄前，先在下方狀態列選好要用哪個專案。

### 用 tools/rtd.py

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

`--port` 可以不給，PlatformIO 會自己挑序列埠。

### 直接用 PlatformIO

```
pio run -d firmware/host                  # build the host
pio run -d firmware/host -t upload        # USB flash the host
pio run -d firmware/host -t uploadfs      # USB upload the MP3 file system
pio run -d firmware/display -t upload     # USB flash the display
pio run -d firmware/joystick -e stick1    # build one stick image
pio run -d firmware/joystick -e stick1 -t upload   # USB flash stick 1
pio run -d firmware/joystick -e stick     # the one OTA image
```

### 搖桿

`stick1` 到 `stick4` 是 USB 用的映像檔，燒完第一次開機時會把自己的 ID 寫進 EEPROM。`stick` 是四支搖桿共用的 OTA 映像檔，本身不帶 ID，開機時從 EEPROM 讀。

### 主機音效

`python tools/rtd.py audio` 會把 MP3 檔案系統（24 個檔案）上傳到主機。主機存的搖桿映像檔放在沒在執行的那個 OTA 區，主機一更新就會被蓋掉，所以更新順序是主機、顯示器、搖桿。

## 設定

所有設定都存在主機的 NVS，namespace 是 `rtd`。顯示器每改一項就送給主機，所以設定值以主機為準。

長按畫面右下角的齒輪 1000 ms 會開啟設定畫面。短按只會跳出提示「HOLD 1 S FOR SETTINGS」，手指按著時齒輪會變橘色。按返回鍵會關閉設定畫面，30 秒沒碰螢幕也會自動關閉。

| 設定 | 畫面標籤 | 範圍 | 預設 | 分頁 |
|------|----------|------|------|------|
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

- 音量 0 是靜音，10 是 1.0 版原本的音量，增益是 `(v/10)^2`。
- 燈環亮度是 `16 x v`。燈條亮度也是 `16 x v`，設 0 會關掉燈條。
- 燈條效果：0 AUTO、1 RAINBOW、2 SPARKLE、3 METEOR、4 CHASE、5 BREATHING、6 FIRE、7 HANZE。
- 模式：0 MIXED、1 只玩 REACTION、2 只玩 SHAKE。
- 計分：0 CLASSIC、1 PLACES。
- 加分局：0 OFF、1 LAST、2 LAST 2。
- 回合數、模式、計分和加分局在比賽開始時就固定下來。音量和亮度一改就生效。
- 顯示器上的回合數按鈕只有 3、5、7、10 四種，主機本身接受 1 到 15。

設定畫面有三個分頁：

- **SOUND & LIGHT**：音量、燈環亮度、燈條亮度、燈條效果。放開音量滑桿時會播一聲測試音。
- **GAME**：回合數、模式、計分、加分局、語音導覽、加入時間、deuce 領先分數。
- **STATUS**：每支搖桿一列，有顏色點、ONLINE 或 OFFLINE、版本和 BUZZ 按鈕。這一頁還有主機與顯示器的版本、今日最佳、歷史紀錄和 RESET RECORD、總場數，以及 START NOW、ABORT GAME、RESTORE DEFAULTS、SERVICE MENU。按 SERVICE MENU 會打開服務選單（見「除錯」），START / LEAVE UPDATE MODE 和 UPDATE JOYSTICKS 都在它的 UPDATE 頁。

| 控制項 | 動作 | 效果 |
|--------|------|------|
| BUZZ | `ACT_IDENTIFY` | 震動 500 ms，只在 `PH_IDLE` 有效 |
| START NOW | `ACT_START_NOW` | 有 2 位以上玩家時立刻開始 |
| ABORT GAME | `ACT_ABORT_GAME` | 停止音效，所有搖桿回待機，主機回待機 |
| RESET RECORD | `ACT_RESET_BEST` | 清掉今日最佳和歷史紀錄 |
| RESTORE DEFAULTS | `ACT_FACTORY_RESET` | 所有設定回到預設值 |
| START / LEAVE UPDATE MODE（服務選單） | `ACT_MAINTENANCE` | 進入或離開維護模式 |
| UPDATE JOYSTICKS（服務選單） | `ACT_OTA_STICKS` | 無線更新搖桿 |
| 放開音量滑桿 | `ACT_TEST_SOUND` | 音效閒置時播一聲測試音 |

START NOW 要有 `STF_CAN_START` 才能按。UPDATE JOYSTICKS 要有 `STF_UPDATE_AVAILABLE`，意思是主機裡存有搖桿映像檔，而且至少有一支在線的搖桿版本不同。ABORT GAME、RESTORE DEFAULTS、START / LEAVE UPDATE MODE 和 UPDATE JOYSTICKS 會先跳確認。控制項平常跟著主機送來的狀態走；你碰過的控制項在 1000 ms 內不會被蓋掉。

## OTA 更新

用 USB 燒過第一次之後，每個裝置都能無線更新，主機是中繼站：它開一個自己的 Wi-Fi 基地台，提供映像檔和更新網頁。

### 維護模式

1. 先確認遊戲在待機。主機只能從 `PH_IDLE` 進入維護模式。
2. 按住主機的 BOOT 鍵 (GPIO0) 3 秒，或在顯示器的服務選單按 START UPDATE MODE（設定畫面 → STATUS 分頁 → SERVICE MENU）。
3. 用手機或筆電連上 Wi-Fi 基地台 `RTD-Host`。
4. 用瀏覽器打開 `http://192.168.4.1`。

基地台密碼是 `RTD_AP_PASS`。repo 裡只放了範例值；自己的密碼寫在 `shared/rtd_core/src/RtdSecrets.h`（git 會忽略這個檔），內容是 `#define RTD_AP_PASS "your-password"`，至少 8 個字元，三種裝置都要用同一個密碼來 build。維護模式下 ESP-NOW 照樣在 channel 6 運作，所以搖桿和顯示器都還連得到。比賽進行中按 UPDATE JOYSTICKS 的話，主機會先中止比賽再進維護模式。要離開維護模式，可以按網頁上的 "Exit update mode"、在顯示器上按 LEAVE UPDATE MODE，或按住 BOOT 3 秒；主機會重新開機。

### 搖桿（經由主機）

1. 在 PC 上執行 `python tools/rtd.py dist`，它會產生 `dist/host.bin`、`dist/joystick.bin` 和 `dist/display.bin`。
2. 進入維護模式，打開網頁。
3. 在 "Joystick firmware" 上傳 `dist/joystick.bin`。主機會換掉舊的映像檔，並顯示目前存的版本。
4. 按網頁上的 "Update joysticks now"，或顯示器上的 UPDATE JOYSTICKS。
5. 等它跑。主機會照 ID 順序，對每一支在線而且版本不同的搖桿送 `CMD_OTA_BEGIN`。
6. 在網頁或顯示器上看進度。搖桿會連上基地台，下載 `http://192.168.4.1/fw/joystick.bin`，然後重新開機。
7. 確認結果。搖桿 120 秒內沒有回報就算失敗，主機接著更新下一支。

搖桿的 ID 不會變，因為 OTA 映像檔是從 EEPROM 讀 ID。版本已經相同的搖桿會回 `OTA_ST_UPTODATE`，主機就跳過它。

### 主機

1. 進入維護模式，打開網頁。
2. 在 "Host firmware" 上傳 `dist/host.bin`。
3. 等它跑完。主機把映像檔寫進沒在用的 OTA 區，檢查裡面有沒有 `RTDFW|host|` 標記，然後重新開機。

### 顯示器

1. 讓主機留在維護模式。顯示器會用 station 模式連上基地台，畫面切到服務選單，上面寫著 "Update mode: ON"。
2. 在網頁或服務選單的 UPDATE 頁確認顯示器的位址（`192.168.4.x`）。
3. 在 "Display firmware" 上傳 `dist/display.bin`。瀏覽器會把檔案直接送到顯示器的 `POST /update`。
4. 等到出現 "ok"。顯示器會檢查 `RTDFW|display|` 標記，把開機分割區切到新的映像檔，然後重新開機。

新韌體的 UI 起來 10 秒後，顯示器才把它標成有效。沒有標到的話，bootloader 會退回上一版。

### 用指令列更新

PC 不用手動設 IP：連上 Wi-Fi `RTD-Host` 後，主機會自動發一個位址給 PC（`192.168.4.x`）。主機要先進維護模式，顯示器的位址由工具自己向主機查。

```bash
python tools/rtd.py ota display     # dist/display.bin -> 顯示器，顯示器會重開機
python tools/rtd.py ota sticks      # dist/joystick.bin -> 主機，主機再逐一更新在線的搖桿
python tools/rtd.py ota host        # dist/host.bin -> 主機，主機重開機後自動回到維護模式
```

主機更新完重開機後會自動回到維護模式，不過主機要排在搖桿前面更新，因為主機更新會蓋掉存好的搖桿映像檔。搖桿映像檔存進主機後，主機會整份讀回來比對大小和 MD5，不符就拒收。PlatformIO 裡也有同樣的步驟，target 叫 `ota`，會先建置再上傳（VS Code：Project Tasks、Custom、「OTA upload」）：

```bash
pio run -d firmware/display -t ota
pio run -d firmware/joystick -e stick -t ota
pio run -d firmware/host -t ota
```

### 搖桿救援

搖桿的韌體壞了，或一直加入不了的時候用：

1. 關掉搖桿電源。
2. 按住搖桿按鈕，再打開電源。
3. 繼續按住 5 秒。搖桿會直接進入 OTA 下載，並在 60 秒內持續嘗試連基地台。

### 公開活動前

請在 `shared/rtd_core/src/RtdSecrets.h` 設定自己的 `RTD_AP_PASS`，repo 裡的只是範例值。改完之後主機、搖桿和顯示器都要重燒。

另外把 `debug_wifi` 關掉（見「除錯」），這樣基地台只會在維護模式開著。

## 除錯

不用每台裝置都接 USB。搖桿和顯示器會用無線把自己的狀況回報給主機，主機再把全部資料放在一個網頁上。

### 主機除錯頁

1. 用手機或筆電連上 Wi-Fi `RTD-Host`。
2. 打開 `http://192.168.4.1/debug`。

頁面每秒更新一次，內容有三塊：

- 主機計數器：收到的封包數、丟掉的封包數、壞封包數、各裝置的傳送失敗次數、最近 10 秒內最久的一次 loop、剩餘 heap、上次重開機的原因。
- 裝置列表（四支搖桿加顯示器）：每台多久沒出聲，以及它回報的數值。
- 事件紀錄：phase 切換、玩家加入、每個遊戲封包的收發。把 **TRACE** 打開，beacon、heartbeat 和診斷回報也會列出來。

| 回報的數值 | 意思 |
|---|---|
| `fw_version`、`reset_reason`、`uptime_min`、`heap_kb` | 韌體版本、上次重開機的原因、開機後幾分鐘、剩餘 heap |
| `rx_drop`、`tx_fail`、`bad_frame` | 開機以來的無線錯誤次數（最多記到 255） |
| `loop_max_ms`、`link_gap_ds` | 上次回報之後最久的一次 loop，以及最久有多久沒收到主機封包（單位 0.1 秒） |
| `btn_edges`、`btn_drop`、`stick_state`、`mpu_ok` | 搖桿才有：按鈕按下次數、漏掉的次數、目前狀態、加速度計有沒有回應 |
| `snap_per_10s`、`touch_ok`、`ota_state`、`screen` | 顯示器才有：10 秒內收到幾個狀態封包、觸控晶片有沒有回應、OTA 狀態、畫面上的 phase |

每台裝置一秒只送一個數值，整列要 13 秒左右才會填滿。搖桿只在待機時回報，比賽進行中不送。

### 除錯 Wi-Fi

`debug_wifi` 這個設定開著的時候，基地台和除錯頁在每個 phase 都能用。2.0 預設是開的。不在維護模式時，網頁只能看：上傳韌體、啟動搖桿 OTA 和離開維護模式都會回 `409`。

要切換，按除錯頁上的按鈕，或在序列埠輸入 `w`。下次開機才生效。`debug_wifi` 關掉之後，基地台跟以前一樣，只在維護模式才開。

### 主機序列埠

只要把主機接上 USB，用 115200 baud 開序列埠就看得到輸出，例如 `python tools/rtd.py monitor`。每個事件會印一行 `[EVT]`。按鍵：`?` 說明、`s` 狀態、`l` 印出全部事件紀錄、`t` 開關 trace、`w` 開關除錯 Wi-Fi。

### 顯示器上的診斷面板

按住顯示器上的連線圖示 3 秒，角落會跳出一個小面板，列出韌體版本、連線狀態、10 秒內的狀態封包數、無線錯誤次數、UI 更新最久的一次、剩餘 heap 和重開機原因。再按住 3 秒就收起來。

### 顯示器上的服務選單

按住齒輪 1 秒進設定畫面，切到 STATUS 分頁，按 **SERVICE MENU** 就會打開服務選單。主機進維護模式時，顯示器會自動切到這個畫面。裡面有四頁：

- **UPDATE**：基地台名稱、密碼和網址、顯示器的位址、OTA 狀態和進度，還有 START / LEAVE UPDATE MODE 和 UPDATE JOYSTICKS 兩顆按鈕。
- **STICKS**：每支搖桿一列，列出顏色、連線、多久沒收到它的封包、韌體版本、封包數、各種錯誤次數、loop 時間、最久多久沒收到主機封包、目前狀態和感測器。
- **HOST**：主機的計數器、基地台有沒有開、連了幾台裝置，還有除錯 Wi-Fi 的開關。
- **DISPLAY**：顯示器自己的計數器，以及主機這邊看到的顯示器狀況。

除錯 Wi-Fi 的開關要等主機下次開機才生效。

### 從 PC 看

```
python tools/rtd.py debug              # 抓一次狀態和事件紀錄，存到 logs/
python tools/rtd.py debug --watch 2    # 每 2 秒抓一次
python tools/rtd.py monitor            # 主機序列埠
python tools/rtd.py ports              # 列出序列埠
```

執行 `debug` 之前，PC 要先連上 `RTD-Host`。

## 通訊協定

韌體版本 V4.0.2，協定版本 4。所有裝置都用 ESP-NOW，固定在 Wi-Fi channel 6，省電模式關閉。封包的第一個 byte 決定封包種類。

`GamePacket`，7 bytes，第一個 byte 是 `0x0A`：

| Byte | 欄位 | 意義 |
|------|------|------|
| 0 | `start` | 固定 `0x0A` |
| 1 | `dest_id` | `0x00` 主機、`0x01-04` 搖桿、`0x10` 顯示器、`0xFF` 全部 |
| 2 | `src_id` | 傳送端 ID |
| 3 | `cmd` | 指令 |
| 4 | `data_high` | 資料高位元組 |
| 5 | `data_low` | 資料低位元組 |
| 6 | `crc` | byte 0..5 的 CRC8 |

CRC8 的多項式是 `0x8C`（reflected），初始值 0。`StatePacket` 有 72 bytes，第一個 byte 是 `0x5A`，主機用它把完整的遊戲狀態送給顯示器。

| 指令 | Hex | 方向 | 資料 |
|------|-----|------|------|
| `CMD_OK` | 0x0B | host -> stick | 0 / 座位 1..4 |
| `CMD_REQ_ID` | 0x0D | stick -> host | version / patch |
| `CMD_ACK` | 0x0E | 雙向 | 0 / 被確認的指令 |
| `CMD_GAME_START` | 0x21 | host -> stick | 模式 / 搖晃目標 |
| `CMD_GO` | 0x22 | host -> stick | 16-bit age，單位 ms |
| `CMD_VIBRATE` | 0x23 | host -> stick | 0 / 時間 x 10 ms |
| `CMD_IDLE` | 0x24 | host -> stick | 0 |
| `CMD_COUNTDOWN` | 0x25 | host -> stick | 0 / 3、2、1 |
| `CMD_REACTION_DONE` | 0x26 | stick -> host | ms，`0xFFFF` = 犯規 |
| `CMD_SHAKE_DONE` | 0x27 | stick -> host | ms，`0xFFFF` = 逾時 |
| `CMD_SHAKE_PROGRESS` | 0x28 | stick -> host | 次數 / 目標 |
| `CMD_BEACON` | 0x29 | host -> all | 階段 / 搖桿遮罩 |
| `CMD_HEARTBEAT` | 0x2A | stick -> host | version / patch |
| `CMD_CFG_SET` | 0x40 | display -> host | key / value |
| `CMD_ACTION` | 0x41 | display -> host | 動作 / 參數 |
| `CMD_DISPLAY_HELLO` | 0x42 | display -> host | version / patch |
| `CMD_OTA_BEGIN` | 0x43 | host -> stick | version / patch |
| `CMD_OTA_STATUS` | 0x44 | stick -> host | 狀態 / 細節 |
| `CMD_DISPLAY_INFO` | 0x45 | display -> host | IP 最後一碼 / 狀態 |

### 公平計時

主機先記下 GO 的瞬間 `t0 = micros()`，然後用兩種方式送 `CMD_GO`。第一個封包是廣播，age 填 0，所有搖桿會在同一瞬間收到。接著再對每支參賽的搖桿各送一次單播，這次會檢查有沒有收到 ACK，每次重送都帶上 `age = (micros() - t0) / 1000`。

搖桿在等待 GO 的時候，只採用第一個有效的 `CMD_GO`，並算出 `go_time = rx_timestamp - age * 1000`。之後收到的重複封包只回 ACK；廣播封包則一律不回 ACK。這樣一來，漏收封包的人不會吃虧，也不會佔便宜。

GO 之前 1000 ms 內按下算搶跑，GO 當下按鈕已經按著也算搶跑。GO 之後前 60 ms 內按下不計。過了這段時間後，第一次有效按下就是成績，單位 ms，最小是 1。

重送規則、位址學習和協定的其他細節，請看 `docs/DESIGN.md` 第 3、4 節。

## 測試

`python tools/rtd.py test` 會編譯並執行 PC 上的測試。`tools/run_tests.py` 用 PC 的 g++ 加上 `-std=c++11 -Wall -Wextra -Werror` 編譯 `shared/rtd_core/src` 和 `tests/`，再執行 `.pio_native` 裡的執行檔。

模擬器把真正的遊戲核心接上假的硬體介面：虛擬時鐘精準到微秒；無線電模型能對每條連線設定封包遺失率，會有 0..3 ms 的延遲抖動，還會送出重複封包；另外有四個 `StickLogic`，各配一個照腳本操作的玩家，再加一個 `HostGame` 和一個 `DisplayLink`。

測試一共 27 個。T1 到 T15 測協定和 CRC、正常流程、加入順序和加入時的各種特殊情況、GO 公不公平、搶跑、成績封包遺失、搖桿故障、平手、設定、動作、32-bit 時鐘溢位、OTA 流程和顯示器連線，最後連跑 2000 場做 fuzz 測試。

T16 到 T22 是 code review 抓到問題之後補的迴歸測試：

- 待機指令遺失或主機重開後，搖桿還留著舊座位；
- 開機很久之後的計分；
- 在維護模式裡啟動 OTA；
- 待機提示的間隔；
- 搖晃的起算時間；
- 早於 GO 的按鈕訊號。

T23 測服務選單的封包。T24 到 T27 測 PLACES 計分、加分局、靠加分局逆轉，以及計分設定在比賽開始時固定下來。

韌體目前只確認編譯會過。2.0 commit 當時，觸控、OTA、LED 和音效都還沒在實機上跑過。

## 疑難排解

- **搖桿加入不了。** 先確認主機在 `PH_IDLE`，而且搖桿有 ID。用 USB 燒一次 `stick1`..`stick4`，讓 ID 寫進 EEPROM。
- **馬達每 2 秒震一下，搖桿沒有其他反應。** 這支搖桿沒有有效的 ID：EEPROM 裡沒有紀錄，MAC 對照表也找不到。用四個 `stickN` 環境的其中一個重燒。
- **顯示器顯示 "CONNECTING TO HOST..."。** 顯示器已經 2000 ms 沒收到有效的狀態封包。檢查主機電源、兩邊的韌體版本，並確認兩邊都在 channel 6、省電模式關閉。
- **OTA 失敗。** 映像檔種類要對：主機檢查 `RTDFW|joystick|` 或 `RTDFW|host|`，顯示器檢查 `RTDFW|display|`，種類不對會被擋下來。搖桿連不上基地台的話，120 秒後逾時；這時改用開機按住 5 秒的救援方式。
- **在 Git Bash 編譯失敗。** PlatformIO 的 ESP32 工具鏈不支援 MSYS。改在 PowerShell 執行，或用會清掉 `MSYSTEM` 的 `python tools/rtd.py`。
- **找不到 `lv_conf.h`。** 顯示器專案把 `lv_conf.h` 放在 `firmware/display/lib`，並加了 `-Ilib` 和 `LV_CONF_INCLUDE_SIMPLE`。請用 `firmware/display/platformio.ini` 編譯，不要搬動這個檔案。
- **找不到 `pio`。** 安裝 PlatformIO Core，並把 `pio` 加進 PATH。
- **測試停在 "no C++ compiler found"。** 設定 `CXX`，或安裝 g++ 或 clang++。腳本也會找 `C:/msys64/mingw64/bin/g++.exe`。

## 致謝與授權

MIT License

Hanze 標誌的版權屬於 Hanze University of Applied Sciences。檔案取自 Wikimedia Commons，授權為 CC BY-SA 2.5，這裡只用在開放日展示。

顯示器字型是 Montserrat，授權為 SIL Open Font License 1.1（見 `firmware/display/assets/fonts/OFL.txt`）。

函式庫：LVGL 8.3.11、ESP8266Audio 1.9.7、NeoPixelBus ^2.8.3。
