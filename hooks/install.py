"""Install (or remove) Agent Needs You hooks for Claude Code, Cursor and Codex.

    python hooks/install.py --ip 192.168.2.141            # every agent found
    python hooks/install.py --ip 192.168.2.141 --agents claude,cursor
    python hooks/install.py --uninstall
    python hooks/install.py --ip 192.168.2.141 --dry-run  # show, change nothing

Without a clone (relay.py is then downloaded from GitHub):

    curl -fsSL https://raw.githubusercontent.com/AncientiCe/agent-needs-you/main/hooks/install.py | python3 - --ip 192.168.2.141
    irm https://raw.githubusercontent.com/AncientiCe/agent-needs-you/main/hooks/install.py | python - --ip 192.168.2.141

Copies relay.py to ~/.claude-light/ and merges one hook per event into each
agent's user config. Existing hooks are left alone, earlier Agent Needs You
hooks are replaced, and every changed file is backed up first.

Standard library only.
"""

import argparse
import json
import shutil
import sys
import time
import urllib.request
from pathlib import Path

MARKER = ".claude-light/relay.py"
RELAY_URL = "https://raw.githubusercontent.com/AncientiCe/agent-needs-you/main/hooks/relay.py"

# event -> matcher (None = no matcher field)
CLAUDE_EVENTS = {
    "SessionStart": None,
    "UserPromptSubmit": None,
    "PostToolUse": "*",
    "Notification": None,
    "Stop": None,
    "SessionEnd": None,
}
CODEX_EVENTS = {
    "SessionStart": None,
    "UserPromptSubmit": None,
    "PreToolUse": "request_user_input",
    "PostToolUse": "*",
    "PermissionRequest": "*",
    "Stop": None,
    "Interrupt": None,
    "SessionEnd": None,
}
CURSOR_EVENTS = ["sessionStart", "beforeSubmitPrompt", "postToolUse", "stop", "sessionEnd"]


def is_ours(command):
    c = command.replace("\\", "/")
    # The relay, or the hand-written curl hook from the first setup.
    return MARKER in c or ("curl" in c and "--data-binary @-" in c and "/hook" in c)


def python_path():
    """The interpreter hooks should run: never a virtualenv that may be deleted later."""
    exe = sys.executable
    if sys.prefix != sys.base_prefix:
        base = getattr(sys, "_base_executable", "") or shutil.which("python3") or shutil.which("python")
        if base and Path(base).exists():
            exe = base
    return Path(exe).as_posix()


def hook_command(relay, agent, url, codex_shell="powershell"):
    py = python_path()
    if sys.platform == "win32" and agent == "codex" and codex_shell == "powershell":
        # Codex runs hooks in the session's shell. A quoted path alone is a
        # string in PowerShell; the call operator is required to execute it.
        def quote(value):
            return "'" + value.replace("'", "''") + "'"

        return f"& {quote(py)} -S {quote(relay.as_posix())} --agent {agent} --url {quote(url)}"
    return f'"{py}" -S "{relay.as_posix()}" --agent {agent} --url {url}'


# ---------------------------------------------------------------- configs

def strip_grouped(hooks):
    """Remove our entries from Claude/Codex-style {event: [{matcher, hooks: [...]}]}."""
    for event in list(hooks):
        groups = []
        for g in hooks[event]:
            inner = [h for h in g.get("hooks", []) if not is_ours(h.get("command", ""))]
            if inner:
                groups.append({**g, "hooks": inner})
        if groups:
            hooks[event] = groups
        else:
            del hooks[event]


def add_grouped(hooks, events, command, timeout):
    for event, matcher in events.items():
        group = {"hooks": [{"type": "command", "command": command, "timeout": timeout}]}
        if matcher:
            group = {"matcher": matcher, **group}
        hooks.setdefault(event, []).append(group)


def strip_flat(hooks):
    """Remove our entries from Cursor-style {event: [{command}]}."""
    for event in list(hooks):
        entries = [e for e in hooks[event] if not is_ours(e.get("command", ""))]
        if entries:
            hooks[event] = entries
        else:
            del hooks[event]


def update_claude(doc, command):
    hooks = doc.setdefault("hooks", {})
    strip_grouped(hooks)
    if command:
        add_grouped(hooks, CLAUDE_EVENTS, command, 5)
    if not hooks:
        del doc["hooks"]


def update_codex(doc, command):
    hooks = doc.setdefault("hooks", {})
    strip_grouped(hooks)
    if command:
        add_grouped(hooks, CODEX_EVENTS, command, 3)  # SessionEnd allows at most 3s


def update_cursor(doc, command):
    doc.setdefault("version", 1)
    hooks = doc.setdefault("hooks", {})
    strip_flat(hooks)
    if command:
        for event in CURSOR_EVENTS:
            hooks.setdefault(event, []).append({"command": command, "timeout": 5})


AGENTS = {
    # name: (config dir, config file, updater)
    "claude": (".claude", "settings.json", update_claude),
    "cursor": (".cursor", "hooks.json", update_cursor),
    "codex": (".codex", "hooks.json", update_codex),
}


# ---------------------------------------------------------------- main

def apply(path, updater, command, dry_run):
    before = json.loads(path.read_text(encoding="utf-8")) if path.exists() else {}
    after = json.loads(json.dumps(before))
    updater(after, command)
    if after == before:
        return "unchanged"
    if dry_run:
        return "would update"
    if path.exists():
        backup = path.with_name(path.name + time.strftime(".bak-claude-light-%Y%m%d-%H%M%S"))
        shutil.copy2(path, backup)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(after, indent=2) + "\n", encoding="utf-8")
    return "updated"


def relay_source():
    """relay.py from next to this script, or from GitHub when run without a clone."""
    here = globals().get("__file__", "")
    local = Path(here).resolve().parent / "relay.py" if here and here != "<stdin>" else None
    if local and local.exists():
        return local.read_bytes()
    try:
        with urllib.request.urlopen(RELAY_URL, timeout=15) as r:
            src = r.read()
    except Exception as e:
        raise SystemExit(f"couldn't download {RELAY_URL} ({e}); nothing changed")
    if b"def normalise" not in src:
        raise SystemExit(f"download from {RELAY_URL} doesn't look like relay.py; nothing changed")
    return src


def check_board(url):
    status_url = url.rsplit("/hook", 1)[0] + "/"
    try:
        with urllib.request.urlopen(status_url, timeout=2) as r:
            sessions = json.load(r).get("sessions", [])
        return f"board reachable at {status_url} ({len(sessions)} sessions)"
    except Exception as e:
        return f"WARNING: board not reachable at {status_url} ({e}); hooks are installed anyway"


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--ip", help="board IP address, e.g. 192.168.2.141")
    p.add_argument("--url", help="full hook URL (instead of --ip)")
    p.add_argument("--agents", help="comma-separated: claude,cursor,codex (default: every one found)")
    p.add_argument("--codex-shell", choices=("powershell", "cmd"), default="powershell",
                   help="Codex session shell on Windows (default: powershell)")
    p.add_argument("--uninstall", action="store_true", help="remove Agent Needs You hooks")
    p.add_argument("--dry-run", action="store_true", help="report what would change, write nothing")
    p.add_argument("--home", type=Path, default=Path.home(), help=argparse.SUPPRESS)
    args = p.parse_args()

    url = args.url or (f"http://{args.ip}/hook" if args.ip else None)
    if not url and not args.uninstall:
        p.error("--ip or --url is required (the board shows its IP on the idle screen)")

    if args.agents:
        names = [a.strip() for a in args.agents.split(",") if a.strip()]
        unknown = set(names) - set(AGENTS)
        if unknown:
            p.error(f"unknown agent(s): {', '.join(sorted(unknown))}")
    else:
        names = [n for n, (d, _, _) in AGENTS.items() if (args.home / d).is_dir()]
    if not names:
        p.error("no agents found; pass --agents")

    relay = args.home / ".claude-light" / "relay.py"
    if args.uninstall:
        if relay.exists() and not args.dry_run:
            relay.unlink()
    else:
        src = relay_source()  # fetch before touching any config
        if not args.dry_run:
            relay.parent.mkdir(exist_ok=True)
            relay.write_bytes(src)

    for name in names:
        dirname, filename, updater = AGENTS[name]
        path = args.home / dirname / filename
        command = None if args.uninstall else hook_command(relay, name, url, args.codex_shell)
        print(f"{name:7} {path}: {apply(path, updater, command, args.dry_run)}")

    if not args.uninstall:
        print(check_board(url))
        if "codex" in names:
            print("codex: new hooks must be trusted once; run /hooks in Codex and trust them")
        print("Restart running sessions if they don't pick up the new hooks.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
