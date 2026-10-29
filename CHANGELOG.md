# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Documentation

- Documented the Codex desktop pending-question limitation ([#1](https://github.com/AncientiCe/agent-needs-you/issues/1)): permission prompts show needs you, but unanswered asynchronous questions are not tracked reliably by the current relay.

### Changed

- **Renamed to Agent Needs You** (repository `AncientiCe/agent-needs-you`), since the board serves Claude Code, Cursor and Codex. The old GitHub URLs redirect, and hooks installed under the old name keep working and are replaced on the next install.

## [0.3.0] - 2026-10-24

### Added

- **The screen has eyes.** Each state gets its own animated face: *working* is focused (narrowed eyes reading back and forth, the odd bead of sweat), *needs you* is curious (big eyes looking around, head tilts, a bobbing "?"), *your turn* is happy (^ ^ eyes bouncing, blush and sparkles, calm once seen), an idle session is drowsy, and with no sessions the face sleeps with Zzz and the occasional peek. The eyes blink, ease from one mood into the next, and boing when a state changes.
- **New layout and type.** Eyes on the left; on the right the state, the project, how long it has been in that state ("for 3 min", "finished 2 min ago"), the message when it needs you, and session counts. Text uses proper proportional fonts on a black background instead of the scaled pixel font on full-colour backgrounds.

### Changed

- **Finished sessions fade on their own.** A *your turn* session that nobody marks as seen turns grey after 15 minutes, as if BOOT had been pressed, so an old result no longer looks as urgent as a fresh one or takes screen time from working sessions. It still clears after 2 hours.

## [0.2.0] - 2026-10-24

### Changed

- **Screen rotates through sessions** instead of sticking to the single most urgent one. Priority is now needs you > working > your turn, and higher priority stays up longer (15 s / 8 s / 4 s, 2 s once seen). A session that finishes or starts waiting is shown immediately, then the rotation moves on, so a finished session no longer hides one that is still working. The bottom row now lists counts in the same order.

## [0.1.0] - 2026-10-24

First release: a desk status light that shows when an AI coding agent needs you.

### Added

- **Firmware** for the LilyGO T-Display S3 AMOLED. It shows the most urgent session: blue **WORKING**, blinking amber **NEEDS YOU** with the agent's message, green **YOUR TURN**, or an idle screen with the board's IP. A bottom row counts sessions per state. Up to 8 sessions are tracked; working sessions go stale after 30 minutes and finished ones clear after 2 hours.
- **Buttons:** BOOT marks every *your turn* session as seen; the other button cycles the shown session and returns to the most urgent one after 30 seconds.
- **HTTP API:** `POST /hook` takes Claude Code hook JSON (or any script's), `GET /` returns the session table. Request bodies are streamed through a JSON filter, so large `PostToolUse` payloads don't exhaust RAM. The same JSON works over USB serial, along with `state`, `test` and `inv0`/`inv1` commands.
- **Hook installer** (`hooks/install.py`) for Claude Code, Cursor and Codex on Windows, macOS and Linux, Python 3.8+ with the standard library only. It runs without a clone (`curl … | python3 -` or `irm … | python -`), keeps existing hooks, replaces earlier Claude Needs You hooks, backs up each changed file, and supports `--agents`, `--dry-run` and `--uninstall`. Run from a virtualenv, it points the hooks at the system Python.
- **Relay** (`hooks/relay.py`) that converts each agent's hook payload to the board's format with a one-second timeout and always exits 0, so agents carry on when the board is off.

[Unreleased]: https://github.com/AncientiCe/agent-needs-you/compare/v0.3.0...HEAD
[0.3.0]: https://github.com/AncientiCe/agent-needs-you/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/AncientiCe/agent-needs-you/compare/v0.1.0...v0.2.0
[0.1.0]: https://github.com/AncientiCe/agent-needs-you/releases/tag/v0.1.0
