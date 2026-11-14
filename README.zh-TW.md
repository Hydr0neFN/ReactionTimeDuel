[English](README.md) · **繁體中文**

# 反應時間對決 (Reaction Time Duel)

本專案獲選在 Hanze Open Day 展示。

<img width="2048" height="1536" alt="IMG_2032" src="https://github.com/user-attachments/assets/0c61f458-73cb-4014-977f-d88b95b5550a" />

這是一款用 ESP32 和 ESP8266 做的多人反應時間競賽，板子之間靠 ESP-NOW 無線通訊。最多 4 位玩家、5 個回合、兩種模式，每人拿一支自製的無線搖桿。

## 專案一覽

| | |
|---|---|
| 內容 | 用 ESP32 + ESP8266 做的多人反應時間競賽，走 ESP-NOW；最多 4 位玩家、5 回合、2 種模式 |
| 硬體 | 主控端 1 台（ESP32 DevKit-C）、搖桿最多 4 支（ESP8266 ESP-12F）、顯示器 1 台（選用） |
| 技術 | Arduino／PlatformIO 韌體、ESP-NOW、NeoPixelBus、ESP8266Audio、MPU-6050 |
| 狀態 | 已封存 |
| 我的角色 | 團隊專案（未標註個人分工） |

## 運作方式

```
                         ┌──────────────────────┐
                         │     ESP32 Host        │
                         │    (Game Logic)       │
                         │                      │
                         │  NeoPixel Rings ×5   │
                         │  WS2812B Strip (89)  │
                         │  I2S Audio + Amp     │
                         │  SPIFFS (24 MP3s)    │
                         └──────────┬───────────┘
                                    │ ESP-NOW (ch 6)
              ┌─────────────────────┼─────────────────────┐
              │                     │                     │
        ┌─────┴──────┐       ┌─────┴──────┐        ┌─────┴──────┐
        │ Joystick 1 │       │ Joystick 2 │  ...   │ Joystick 4 │
        │  ESP8266   │       │  ESP8266   │        │  ESP8266   │
        │            │       │            │        │            │
        │  Button    │       │  Button    │        │  Button    │
        │  MPU-6050  │       │  MPU-6050  │        │  MPU-6050  │
        │  Vibration │       │  Vibration │        │  Vibration │
        └────────────┘       └────────────┘        └────────────┘
                                    │
                              ┌─────┴──────┐
                              │  Display   │
                              │ (optional) │
                              └────────────┘
```

- ESP32 主控端負責遊戲邏輯，驅動 5 個 NeoPixel 燈環、89 顆 LED 的 WS2812B 燈條和 I2S 音訊。
- 最多 4 支 ESP8266 搖桿透過 ESP-NOW（channel 6）加入，把按鈕或搖晃的結果送回主控端。
- 選用的顯示器也收同一份 ESP-NOW 狀態，顯示目前的遊戲狀態。
- 模式由 shuffle bag 抽，主控端負責跑回合、公布贏家。

## 遊戲模式

### 反應模式 (Reaction mode)

NeoPixel 燈環會隨機換色。燈光一凍結成黃色，就按下去。這一回合比誰反應最快；太早按會被罰。

### 搖晃模式 (Shake mode)

3-2-1 倒數完，開始搖搖桿，目標是 10、15 或 20 下。中間的燈環會顯示剩餘時間，先搖完的人贏。

> 模式用 **shuffle bag** 抽：兩種模式都出現過一輪，才會開始重複。

## 遊戲流程

```
IDLE ──> JOIN ──> COUNTDOWN ──> REACTION / SHAKE ──> COLLECT ──> RESULTS ──┐
  ^                                                                        │
  │  (after 5 rounds)    FINAL_WINNER <────────────────────────────────────┘
  └──────────────────────────┘
```

1. **待機** — 播彩虹動畫和「press to join」語音提示。
2. **加入** — 依序點名 P1 → P2 → P3 → P4。輪到的位置會在自己的 NeoPixel 燈環上閃 5 秒，還沒認領的搖桿按一下就能認領。至少要 2 位玩家。
3. **選模式** — shuffle bag 從反應模式或搖晃模式抽一個。每種模式在一場遊戲裡只播一次說明。
4. **倒數** — 搖晃模式先跑 3-2-1 倒數，音效、NeoPixel 閃爍和震動同步。反應模式不倒數，GO 前隨機等 3 秒、5 秒或 7 秒。
5. **開玩** — 反應模式：燈變黃就按。搖晃模式：拼命搖到目標次數。
6. **收結果** — 結果透過 ESP-NOW 回來。慢的玩家先看到黃色警告，再被淘汰。
7. **回合結果** — 時間顯示 3 秒，接著贏家和分數顯示 3 秒。
8. **總冠軍** — 5 回合跑完，公布總冠軍，播勝利音樂。

## 硬體

| 元件 | MCU | 數量 | 角色 |
|-----------|-----|-----|------|
| 主控制器 (Host Controller) | ESP32 (DevKit-C) | 1 | 遊戲邏輯、NeoPixel 燈環、環境氛圍 LED 燈條、I2S 音訊 |
| 搖桿 (Joystick) | ESP8266 (ESP-12F) | 最多 4 | 按鈕輸入、加速度計、震動馬達 |
| 顯示器 (Display) | ESP32/ESP8266 | 1 | 遊戲狀態顯示（選用，接收 ESP-NOW） |

腳位配置和每一台的硬體細節在 [docs/hardware.zh-TW.md](docs/hardware.zh-TW.md) · [English](docs/hardware.md)。

## 建置與執行

### 前置需求

- [PlatformIO](https://platformio.org/) IDE 或 CLI
- ESP32 DevKit-C 和 ESP-12F 開發板
- 燒錄用的 USB 線

### 主控端（ESP32）

```bash
cd ReactionTimerHost
pio run -t upload          # Flash firmware
pio run -t uploadfs        # Upload MP3 audio files to SPIFFS
```

### 搖桿（ESP8266）

每支搖桿有自己的建置環境和 `MY_ID` 旗標：

```bash
cd ReactionTimerSlave
pio run -e stick1 -t upload    # Joystick 1 (MY_ID=0x01)
pio run -e stick2 -t upload    # Joystick 2 (MY_ID=0x02)
pio run -e stick3 -t upload    # Joystick 3 (MY_ID=0x03)
pio run -e stick4 -t upload    # Joystick 4 (MY_ID=0x04)
```

## 專案結構

```
ReactionTimeDuel/
├── README.md
├── .gitignore
│
├── ReactionTimerHost/              # ESP32 Host Controller
│   ├── platformio.ini
│   ├── enable_ccache.py            # Build speed optimization
│   ├── src/
│   │   └── main.cpp                # Game state machine, NeoPixel, strip animations
│   ├── include/
│   │   ├── Protocol.h              # Packet format, CRC8, device IDs, commands
│   │   ├── GameTypes.h             # Constants, timing, player struct, NeoPixel config
│   │   └── AudioManager.h          # Non-blocking MP3 queue, I2S output, PWM volume, sound defs
│   └── data/                       # 24 MP3 files uploaded to SPIFFS
│       ├── beep.mp3, click.mp3, error.mp3
│       ├── get_ready.mp3, react_inst.mp3, will_shake.mp3
│       ├── player1.mp3 ... player4.mp3
│       ├── one.mp3, two.mp3, three.mp3, ten.mp3, fifteen.mp3, twenty.mp3
│       ├── reaction.mp3, shake.mp3, fastest.mp3, wins.mp3
│       └── victory.mp3, gameover.mp3, press_join.mp3, ready.mp3
│
└── ReactionTimerSlave/             # ESP8266 Joystick Controllers (×4)
    ├── platformio.ini              # Multi-env: stick1, stick2, stick3, stick4
    ├── enable_ccache.py
    ├── src/
    │   └── main.cpp                # Joystick state machine, shake detection, timing
    └── include/
        ├── Protocol.h              # Shared protocol (core commands, no display cmds)
        └── GameTypes.h             # Joystick-only constants (timeouts)
```

## 設計重點

- **微秒級反應計時** — 按鈕由 `IRAM_ATTR` 下降沿中斷捕捉，時間是從 GO 信號起算的 `micros()` 差值。
- **非阻塞架構** — NeoPixelBus 配 ESP32 RMT DMA，LED 輸出不會閃；音效排隊播放，間隔可調；遊戲迴圈裡沒有 `delay()`。
- **高通濾波搖晃偵測** — EMA 低通濾波器（alpha=1/64）先濾掉重力，再用遲滯狀態機在 X+Z 軸上數完整的推回週期。
- **即時搖晃進度** — 搖桿每搖 5 下就用 `CMD_SHAKE_PROGRESS` 回報一次，主控端燈環上的進度條就能跟著跑。
- **Shuffle bag 選模式** — 反應和搖晃都出現過，才會重複，不會連續抽到同一種。
- **氛圍燈條** — 89 顆 WS2812B 燈條走第二個 RMT 通道，輪播 6 種程序化動畫（rainbow、sparkle、meteor rain、color chase、breathing、fire）。
- **PWM 音量控制** — 擴大機 GAIN 腳位由 25kHz LEDC PWM 驅動，音量調起來平順。
- **韌體版本** — 加入封包會帶韌體版本（V3.0.0），用來檢查相容性。
- **可靠傳輸** — 重要命令走 ACK 表，最多重試 3 次，間隔 50ms。
- **無障礙** — 24 個 MP3 的完整語音導覽，涵蓋所有遊戲狀態、玩家播報和操作說明。

## 通訊協定與韌體

主控端和從屬端搖桿共用 `Protocol.h`：ESP-NOW、channel 6、7 位元組封包，CRC8（多項式 0x8C）涵蓋位元組 0-5。兩邊各有一份自己的 `GameTypes.h`，依角色分工。

- [通訊協定](docs/protocol.zh-TW.md) · [English](docs/protocol.md) — 封包欄位、主要命令和顯示命令。
- [主控端與搖桿韌體](docs/firmware.zh-TW.md) · [English](docs/firmware.md) — 主從端比較和搖桿狀態機。

## 相依套件

### 主控端（ESP32）

| 函式庫 | 版本 | 用途 |
|---------|---------|---------|
| [NeoPixelBus](https://github.com/Makuna/NeoPixelBus) | ≥2.8.3 | LED 燈環 + 燈條控制（RMT DMA） |
| [ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) | 1.9.7 | MP3 解碼 + I2S 輸出 |

### 搖桿（ESP8266）

| 函式庫 | 來源 | 用途 |
|---------|--------|---------|
| ESP8266WiFi | 內建 | ESP-NOW 傳輸 |
| Wire | 內建 | 用於 MPU-6050 的 I2C |

## 授權條款

MIT License

## 貢獻

歡迎貢獻！有問題就開 issue，或送 pull request。
