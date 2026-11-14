[English](hardware.md) · **繁體中文**

# 硬體細節

每一台的硬體和腳位配置。元件總表在 [README](../README.zh-TW.md)。

## 主控制器（ESP32）

- **5 個 NeoPixel 燈環**（每個 12 顆 LED，共 60 顆）— 玩家狀態與遊戲動畫
- **89 顆 LED 的 WS2812B 氛圍燈條** — 輪播 6 種動畫（rainbow、sparkle、meteor、color chase、breathing、fire）
- **I2S MP3 音訊** — SPIFFS 裡放了 24 個音效檔，拿來做語音播報、倒數和音效
- **PWM 音量控制** — 用擴大機的 GAIN 腳位調硬體增益

### 主控端腳位

| 腳位 | 功能 |
|-----|----------|
| GPIO4 | NeoPixel 燈環資料（5 個燈環，60 顆 LED） |
| GPIO16 | WS2812B 氛圍燈條資料（89 顆 LED） |
| GPIO25 | I2S DOUT（音訊資料） |
| GPIO26 | I2S BCLK（位元時脈） |
| GPIO27 | I2S LRC（左右聲道選擇，word select） |
| GPIO33 | 擴大機 GAIN（PWM 音量控制） |

## 搖桿控制器（ESP8266）

- **按鈕**（GPIO14，active LOW）— 反應輸入，用 IRAM 中斷做到微秒級精度
- **MPU-6050 加速度計** — 搖晃偵測，高通濾波加遲滯判定（X+Z 軸）
- **震動馬達** — 倒數讀秒、GO 信號和完成時的觸覺回饋

### 搖桿腳位

| 腳位 | 功能 |
|-----|----------|
| GPIO14 | 按鈕輸入（active LOW，內部上拉） |
| GPIO12 | 震動馬達輸出 |
| GPIO4 | I2C SDA (MPU-6050) |
| GPIO5 | I2C SCL (MPU-6050, 400kHz) |
