**English** · [繁體中文](protocol.zh-TW.md)

# Communication protocol

All devices communicate over **ESP-NOW on channel 6** with a 7-byte packet.

## Packet format

| Byte | Field | Description |
|------|-------|-------------|
| 0 | `START` | Always `0x0A` |
| 1 | `DEST_ID` | Destination (0x00=Host, 0x01-04=Sticks, 0x10=Display, 0xFF=Broadcast) |
| 2 | `SRC_ID` | Sender ID |
| 3 | `CMD` | Command byte |
| 4 | `DATA_HIGH` | Data high byte |
| 5 | `DATA_LOW` | Data low byte |
| 6 | `CRC` | CRC8 (polynomial 0x8C) over bytes 0-5 |

## Key commands

| Command | Hex | Direction | Description |
|---------|-----|-----------|-------------|
| `CMD_REQ_ID` | `0x0D` | Stick → Host | Request to join (data encodes firmware version) |
| `CMD_OK` | `0x0B` | Host → Stick | Join confirmed (data = slot) |
| `CMD_ACK` | `0x0E` | Both | Acknowledge previous command |
| `CMD_GAME_START` | `0x21` | Host → Stick | Round start (high=mode, low=param) |
| `CMD_GO` | `0x22` | Host → Stick | GO signal — start timing |
| `CMD_VIBRATE` | `0x23` | Host → Stick | Vibrate (`0xFF`=GO, else duration x 10ms) |
| `CMD_IDLE` | `0x24` | Host → Stick | Return to idle state |
| `CMD_COUNTDOWN` | `0x25` | Host → Stick | Countdown tick (3, 2, 1) |
| `CMD_REACTION_DONE` | `0x26` | Stick → Host | Reaction time in ms (`0xFFFF` = penalty) |
| `CMD_SHAKE_DONE` | `0x27` | Stick → Host | Shake time in ms (`0xFFFF` = timeout) |
| `CMD_SHAKE_PROGRESS` | `0x28` | Stick → Host | Shake milestone (every 5 shakes) |

The host also sends display commands (`0x30`-`0x3E`) to an optional display unit for real-time game status.
