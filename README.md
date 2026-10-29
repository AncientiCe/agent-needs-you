# Agent Needs You

A desk status light for AI coding agents: [Claude Code](https://claude.com/claude-code), Cursor and Codex. A LilyGO T-Display S3 AMOLED shows whether any session is working, finished, or blocked waiting on you, so a permission prompt never sits unnoticed in a background window.

<p align="center"><img src="docs/states.gif" alt="The board cycling through its states: asleep, working, needs you and your turn" width="480"></p>

The screen is a pair of eyes with the details beside them. The eyes show how each session is doing:

| Face | Meaning |
|---|---|
| Blue, **focused**: narrowed eyes reading back and forth, the odd bead of sweat | **Working**: the agent is busy |
| Amber, **curious**: big eyes looking around, head tilting, a bobbing "?" | **Needs you**: permission prompt or idle input wait; the message is shown |
| Green, **happy**: ^ ^ eyes bouncing, blushing, sparkles | **Your turn**: the agent finished its reply (calm and grey-green once seen) |
| Lilac, **drowsy**: half-lidded, slow blinks | An idle session |
| Lilac, **asleep**: eyes shut, Zzz, the odd peek | No sessions; shows the board's IP |

Beside the eyes: the state, the project, how long it has been in that state, the message when it needs you, and a count of sessions per state.

Cursor and Codex sessions are labelled with the agent name, e.g. `aice (cursor)`.

The screen rotates through the sessions, most urgent first (needs you > working > your turn), and keeps the urgent ones up longer: 15 seconds for *needs you*, 8 for *working*, 4 for *your turn* (2 once seen). Idle sessions are only shown when nothing else is going on. When a session finishes or starts waiting on you, it is shown straight away (8 seconds for *your turn*) before the rotation carries on. Working sessions go stale after 30 minutes; finished ones clear after 2 hours.

**Buttons:** BOOT marks every *your turn* session as seen (grey); one left unseen turns grey by itself after 15 minutes. The other button cycles which session is shown and holds it for 30 seconds before the rotation resumes.

## Hardware

LilyGO T-Display S3 AMOLED (ESP32-S3R8, 16 MB flash, 8 MB PSRAM, RM67162 240x536 over QSPI). Powered over USB.

## Setting up a board

A board without Wi-Fi settings starts in setup and shows how to connect:

1. On a phone or laptop, join the open Wi-Fi network shown on the screen (`AgentNeedsYou-XXXX`).
2. The setup page usually opens by itself; if not, go to `http://192.168.4.1`.
3. Pick your network, enter its password, and optionally a static IP (with gateway and subnet mask) and an update password. Save.

The board restarts, joins your network and shows its IP on the idle screen. Use that IP for the hooks below. A static IP, or an address reserved for the board in your router, keeps the hooks pointing at the right place.

To change networks later, hold the right-hand button for five seconds (or send `setup` over serial). If the saved network can't be reached within a minute of boot, setup opens by itself. When nobody uses it for ten minutes, the board restarts and tries the saved network again.

## Build and flash

Needs [PlatformIO](https://platformio.org/).

```bash
pio run -t upload
```

The upload port in `platformio.ini` is `COM4`; change it for your machine. A board flashed this way starts in setup. To build your settings in instead, copy `include/secrets.h.example` to `include/secrets.h` (gitignored) and fill it in. Settings saved in setup take priority over `secrets.h`.

After boot the board prints `wifi up: http://<ip>/` over serial and shows the IP on its idle screen.

Each [release](https://github.com/AncientiCe/agent-needs-you/releases) has two firmware images: `*-full.bin` for a new board (flash it at offset `0x0`, e.g. `esptool.py --chip esp32s3 write_flash 0x0 agent-needs-you-<version>-full.bin`) and `*-update.bin` for updating over Wi-Fi.

## Updating over Wi-Fi

Once a board runs firmware with update support and has an update password (set in setup, or `UPDATE_PASSWORD` in `secrets.h`), it no longer needs USB for updates. Without an update password, the board refuses updates over Wi-Fi.

From a clone:

```bash
ANU_HOST=<device-ip> ANU_UPDATE_PASSWORD=<password> pio run -e ota -t upload
```

Or with a release image and plain curl:

```bash
curl -fsS -H "X-Update-Password: <password>" --data-binary @agent-needs-you-<version>-update.bin http://<device-ip>/update
```

The board writes the image to its spare firmware slot, checks it and restarts into it. A broken or cut-short upload leaves the running firmware in place. The session list starts empty after the restart. The password travels unencrypted over your LAN, so treat it as a guard against accidents and neighbours, not as strong protection.

## Agent hooks

Install hooks for every agent found on the machine. Works on Windows, macOS and Linux; needs Python 3.8+, standard library only. No clone needed:

```bash
# macOS / Linux
curl -fsSL https://raw.githubusercontent.com/AncientiCe/agent-needs-you/main/hooks/install.py | python3 - --ip <device-ip>
```

```powershell
# Windows (PowerShell)
irm https://raw.githubusercontent.com/AncientiCe/agent-needs-you/main/hooks/install.py | python - --ip <device-ip>
```

From a clone, run `python hooks/install.py --ip <device-ip>` instead (`python3` on macOS/Linux). Options go after the `-` in the one-liners too, e.g. `... | python3 - --uninstall`.

Hooks call the Python that ran the installer (or the system Python, if run from a virtualenv), so install with the interpreter you want the hooks to use.

Options: `--agents claude,cursor,codex` to pick agents, `--dry-run` to see what would change, `--uninstall` to remove the hooks. Run it on each computer whose agents should report to the board.

On Windows, Codex runs hooks in its session shell. The installer generates PowerShell commands by default; use `--codex-shell cmd` if your Codex session uses Command Prompt. Reinstalling replaces older commands that fail to launch in PowerShell; review and trust the changed Codex hooks afterward.

The installer copies [`hooks/relay.py`](hooks/relay.py) to `~/.claude-light/` and adds one hook per event to `~/.claude/settings.json`, `~/.cursor/hooks.json` and `~/.codex/hooks.json`. Existing hooks are kept, earlier Agent Needs You hooks are replaced, and each changed file is backed up next to itself (`*.bak-claude-light-<timestamp>`).

The relay converts each agent's hook payload to the board's format, posts it with a one-second timeout, prints whatever the agent expects from a hook, and always exits 0. If the board is off, hooks carry on normally.

| Board state | Claude Code | Codex | Cursor |
|---|---|---|---|
| Working | `UserPromptSubmit`, `PostToolUse` | `UserPromptSubmit`, `PostToolUse` | `beforeSubmitPrompt`, `postToolUse` |
| Needs you | `Notification` | `PermissionRequest` | — |
| Your turn | `Stop` | `Stop`, `Interrupt` | `stop` |
| Added / removed | `SessionStart` / `SessionEnd` | `SessionStart` / `SessionEnd` | `sessionStart` / `sessionEnd` |

Notes:
- **Codex** skips new hooks until you trust them: run `/hooks` in Codex once after installing.
- **Codex questions:** unanswered desktop questions (`request_user_input_async`) currently leave the board showing working, even though permission approval prompts show needs you. The relay only maps `PermissionRequest` to needs you. Adding `PreToolUse` alone would not reliably track a pending asynchronous question: subsequent `PostToolUse` events can report working while the question is still unanswered. The [documented hook lifecycle](https://learn.chatgpt.com/docs/hooks) does not specify question-open or question-answered events; reliable support needs a verified signal for both. See [issue #1](https://github.com/AncientiCe/agent-needs-you/issues/1).
- **Cursor** has no hook that fires when it waits for your approval without also answering it, so Cursor sessions show working and your turn, never needs you.
- Already-running sessions may need a restart to pick up new hooks.

### Anything else

Any script can drive the board directly:

```bash
curl -s -m 2 -H "Content-Type: application/json" \
  -d '{"hook_event_name":"Notification","session_id":"ci-build","cwd":"my-project","message":"CI failed","agent":"ci"}' \
  http://<device-ip>/hook
```

`hook_event_name` is one of `UserPromptSubmit` (working), `Notification` (needs you), `Stop` (your turn) or `SessionEnd` (remove). The last folder of `cwd` is the label; `agent` and `message` are optional. The board tracks up to 8 sessions.

## Debugging

- `GET /` returns the session table as JSON. `POST /update` takes new firmware (see above).
- The same hook JSON can be sent as one line over USB serial (115200 baud). Other serial commands: `state` prints the table, `setup` opens setup, `test` shows colour bars for 20 seconds, `inv0`/`inv1` toggle colour inversion.
- Opening the serial port with DTR/RTS asserted resets the board and clears the session list.

The firmware streams each request body through an ArduinoJson filter rather than buffering it, so large `PostToolUse` payloads (hundreds of KB of tool output) don't exhaust RAM.

## License

MIT. See [LICENSE](LICENSE).
