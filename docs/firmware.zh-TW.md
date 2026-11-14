[English](firmware.md) · **繁體中文**

# 主控端與搖桿韌體

主控端 (ESP32) 和從屬端搖桿 (ESP8266) 共用 `Protocol.h` 的封包格式和命令定義。兩邊各有一份照自己角色寫的 `GameTypes.h`。

## 主從端比較

| | 主控端 (ESP32) | 從屬端 (ESP8266) |
|---|---|---|
| **角色** | 遊戲統籌、LED/音訊輸出 | 輸入裝置、送結果 |
| **時脈** | 240 MHz | 160 MHz |
| **Protocol.h** | 包含顯示命令 (`DISP_*`) | 只有核心命令 |
| **NeoPixel 模式** | 8 種（rainbow、status、blink slot、shake countdown 等） | 6 種（沒有 blink slot / shake countdown） |
| **燈環對應** | 反過來：P1=Ring4, P2=Ring3, P3=Ring1, P4=Ring0 | 照順序：P1=Ring0, P2=Ring1, center=Ring2, P3=Ring3, P4=Ring4 |
| **狀態機** | 8 個遊戲狀態，管理完整回合 | 4 個搖桿狀態（idle、waiting GO、timing/shaking、done） |
| **音訊** | 非阻塞 MP3 佇列，I2S + PWM 音量 | 沒有音訊硬體 |
| **週邊** | NeoPixel 燈環、WS2812B 燈條、I2S DAC、擴大機 | 按鈕、MPU-6050、震動馬達 |

## 搖桿狀態機

每支搖桿每個回合都照這個順序跑：

```
JS_IDLE ──> JS_WAITING_GO ──> JS_REACTION_TIMING  ──> JS_DONE
                          └──> JS_SHAKE_COUNTING   ──> JS_DONE
```

- **JS_IDLE** — 按鈕輪詢有防彈跳；按下就送帶韌體版本的 `CMD_REQ_ID`
- **JS_WAITING_GO** — 收到 `CMD_GAME_START`，等 `CMD_GO`
- **JS_REACTION_TIMING** — `micros()` 計時中，按鈕中斷會記下時間戳
- **JS_SHAKE_COUNTING** — MPU-6050 高通濾波器數完整的推回週期，每 5 下回報一次進度
- **JS_DONE** — 結果已送出，等下一回合或待機命令
