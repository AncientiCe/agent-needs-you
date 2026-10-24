# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.0] - 2026-10-24

First release: a desk status light that shows when an AI coding agent needs you.

### Added

- **Firmware** for the LilyGO T-Display S3 AMOLED. It shows the most urgent session: blue **WORKING**, blinking amber **NEEDS YOU** with the agent's message, green **YOUR TURN**, or an idle screen with the board's IP. A bottom row counts sessions per state. Up to 8 sessions are tracked; working sessions go stale after 30 minutes and finished ones clear after 2 hours.
- **Buttons:** BOOT marks every *your turn* session as seen; the other button cycles the shown session and returns to the most urgent one after 30 seconds.
- **HTTP API:** `POST /hook` takes Claude Code hook JSON (or any script's), `GET /` returns the session table. Request bodies are streamed through a JSON filter, so large `PostToolUse` payloads don't exhaust RAM. The same JSON works over USB serial, along with `state`, `test` and `inv0`/`inv1` commands.
- **Hook installer** (`hooks/install.py`) for Claude Code, Cursor and Codex on Windows, macOS and Linux, Python 3.8+ with the standard library only. It runs without a clone (`curl … | python3 -` or `irm … | python -`), keeps existing hooks, replaces earlier Claude Needs You hooks, backs up each changed file, and supports `--agents`, `--dry-run` and `--uninstall`. Run from a virtualenv, it points the hooks at the system Python.
- **Relay** (`hooks/relay.py`) that converts each agent's hook payload to the board's format with a one-second timeout and always exits 0, so agents carry on when the board is off.

[Unreleased]: https://github.com/AncientiCe/claude_needs_you/compare/v0.1.0...HEAD
[0.1.0]: https://github.com/AncientiCe/claude_needs_you/releases/tag/v0.1.0
