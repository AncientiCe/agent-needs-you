# Claude Needs You

A desk status light for [Claude Code](https://claude.com/claude-code) sessions. A LilyGO T-Display S3 AMOLED shows whether any session is working, finished, or blocked waiting on you, so a permission prompt never sits unnoticed in a background window.

| Screen | Meaning | Hook event |
|---|---|---|
| Blue **WORKING** | Claude is busy | `UserPromptSubmit`, `PostToolUse` |
| Amber **NEEDS YOU** (blinking) | Permission prompt or idle input wait; shows the message | `Notification` |
| Green **YOUR TURN** | Claude finished its reply | `Stop` |
| Black idle screen | No sessions; shows the board's IP | `SessionEnd` removes a session |

The screen shows the most urgent session (needs you > your turn > working > idle) and a bottom row of counts per state. Working sessions go stale after 30 minutes; finished ones clear after 2 hours.

**Buttons:** BOOT marks every *your turn* session as seen (grey). The other button cycles which session is shown; after 30 seconds the screen goes back to the most urgent one.

## Hardware

LilyGO T-Display S3 AMOLED (ESP32-S3R8, 16 MB flash, 8 MB PSRAM, RM67162 240x536 over QSPI). Powered over USB.

## Build and flash

Needs [PlatformIO](https://platformio.org/).

```bash
cp include/secrets.h.example include/secrets.h   # then fill in Wi-Fi details
pio run -t upload
```

`include/secrets.h` is gitignored. Set `STATIC_IP` there (or reserve the address in your router) so the hooks always find the board. The upload port in `platformio.ini` is `COM4`; change it for your machine.

After boot the board prints `wifi up: http://<ip>/` over serial and shows the IP on its idle screen.

## Claude Code hooks

The board accepts Claude Code's raw hook JSON at `POST /hook`, so no client script is needed. Add a command hook to `~/.claude/settings.json` for each of `SessionStart`, `UserPromptSubmit`, `PostToolUse` (matcher `*`), `Notification`, `Stop` and `SessionEnd`, appending to any hooks you already have:

```json
{
  "hooks": [
    {
      "type": "command",
      "command": "curl.exe -s -m 2 --connect-timeout 1 -o /dev/null -H \"Content-Type: application/json\" -H \"Expect:\" --data-binary @- http://<device-ip>/hook || exit 0",
      "timeout": 5
    }
  ]
}
```

`curl.exe` avoids PowerShell's `curl` alias (use plain `curl` on macOS/Linux). If the board is off, the hook fails within a second and never blocks Claude.

## Debugging

- `GET /` returns the session table as JSON.
- The same hook JSON can be sent as one line over USB serial (115200 baud). Other serial commands: `state` prints the table, `test` shows colour bars for 20 seconds, `inv0`/`inv1` toggle colour inversion.
- Opening the serial port with DTR/RTS asserted resets the board and clears the session list.

The firmware streams each request body through an ArduinoJson filter rather than buffering it, so large `PostToolUse` payloads (hundreds of KB of tool output) don't exhaust RAM.
