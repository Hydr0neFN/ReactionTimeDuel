[English](protocol.md) · **繁體中文**

# 通訊協定

所有裝置都走 **ESP-NOW、channel 6**，封包固定 7 位元組。

## 封包格式

| 位元組 | 欄位 | 說明 |
|------|-------|-------------|
| 0 | `START` | 固定為 `0x0A` |
| 1 | `DEST_ID` | 目的地（0x00=Host, 0x01-04=Sticks, 0x10=Display, 0xFF=Broadcast） |
| 2 | `SRC_ID` | 發送端 ID |
| 3 | `CMD` | 命令位元組 |
| 4 | `DATA_HIGH` | 資料高位元組 |
| 5 | `DATA_LOW` | 資料低位元組 |
| 6 | `CRC` | 針對位元組 0-5 計算的 CRC8（多項式 0x8C） |

## 主要命令

| 命令 | 十六進位 | 方向 | 說明 |
|---------|-----|-----------|-------------|
| `CMD_REQ_ID` | `0x0D` | Stick → Host | 請求加入（資料編碼包含韌體版本） |
| `CMD_OK` | `0x0B` | Host → Stick | 確認加入（資料 = slot） |
| `CMD_ACK` | `0x0E` | 雙向 | 確認前一條命令 |
| `CMD_GAME_START` | `0x21` | Host → Stick | 回合開始（高位元組=模式，低位元組=參數） |
| `CMD_GO` | `0x22` | Host → Stick | GO 信號 — 開始計時 |
| `CMD_VIBRATE` | `0x23` | Host → Stick | 震動（`0xFF`=GO，其餘為持續時間 x 10ms） |
| `CMD_IDLE` | `0x24` | Host → Stick | 返回待機狀態 |
| `CMD_COUNTDOWN` | `0x25` | Host → Stick | 倒數讀秒（3, 2, 1） |
| `CMD_REACTION_DONE` | `0x26` | Stick → Host | 反應時間（毫秒，`0xFFFF` = 懲罰） |
| `CMD_SHAKE_DONE` | `0x27` | Stick → Host | 搖晃時間（毫秒，`0xFFFF` = 逾時） |
| `CMD_SHAKE_PROGRESS` | `0x28` | Stick → Host | 搖晃進度回報（每 5 次搖晃） |

主控端也會送顯示命令（`0x30`-`0x3E`）給選用的顯示器，顯示即時遊戲狀態。
