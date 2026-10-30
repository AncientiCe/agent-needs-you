"""Forward an agent hook event to the Agent Needs You board.

Each agent runs this as a hook command with its event JSON on stdin:

    python -S relay.py --agent claude|cursor|codex --url http://<board-ip>/hook

The event is normalised to the board's fields (hook_event_name, session_id,
cwd, message, agent) and POSTed with a short timeout. The relay prints only
what the agent expects from a hook and always exits 0, so a board that is
off or unreachable never blocks or fails the agent.

Hooks run on every tool call, so startup cost matters: standard library
only, run with -S, and a raw socket instead of urllib (whose import and
Windows proxy lookup cost ~150 ms per call).
"""

import json
import os
import socket
import sys

# Agent event name -> board event name. Events not listed are ignored.
EVENTS = {
    "claude": {
        "SessionStart": "SessionStart",
        "UserPromptSubmit": "UserPromptSubmit",
        "PostToolUse": "PostToolUse",
        "Notification": "Notification",
        "Stop": "Stop",
        "SessionEnd": "SessionEnd",
    },
    "codex": {
        "SessionStart": "SessionStart",
        "UserPromptSubmit": "UserPromptSubmit",
        "PreToolUse": "PreToolUse",
        "PostToolUse": "PostToolUse",
        "PermissionRequest": "Notification",
        "Stop": "Stop",
        "Interrupt": "Stop",
        "SessionEnd": "SessionEnd",
    },
    # Cursor has no event for "waiting on your approval" that can be observed
    # without answering it, so Cursor sessions show working / your turn only.
    "cursor": {
        "sessionStart": "SessionStart",
        "beforeSubmitPrompt": "UserPromptSubmit",
        "postToolUse": "PostToolUse",
        "stop": "Stop",
        "sessionEnd": "SessionEnd",
    },
}

# Codex questions asked with request_user_input_async that have no answer yet,
# by session: {"call", "path", "offset", "message"}. The tool returns at once
# and the answer arrives later as a user message in the session transcript.
QUESTIONS = os.path.join(os.path.dirname(os.path.abspath(__file__)), "codex-questions.json")
REPLY_MARK = b"send_user_message_question_reply"


def load_questions():
    try:
        with open(QUESTIONS, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_questions(questions):
    tmp = f"{QUESTIONS}.{os.getpid()}"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(questions, f)
    os.replace(tmp, QUESTIONS)


def question_text(data):
    tool_input = data.get("tool_input") or {}
    if isinstance(tool_input, str):
        try:
            tool_input = json.loads(tool_input)
        except ValueError:
            tool_input = {}
    for q in tool_input.get("questions") or []:
        text = q.get("title") or q.get("question") or q.get("header")
        if text:
            return "Codex asks: " + text
    return "Codex has a question for you"


def answered(asked):
    """True once the transcript holds the reply to the asked question."""
    try:
        with open(asked["path"], "rb") as f:
            f.seek(asked["offset"])
            tail = f.read()
    except OSError:
        return True  # transcript gone: stop holding the session
    call = asked["call"].encode("utf-8")
    return any(REPLY_MARK in line and call in line for line in tail.splitlines())


def codex_question(data, event, message):
    """Report a Codex session as needs you while a question waits for an answer.

    request_user_input blocks, so it waits from its PreToolUse until its
    PostToolUse. request_user_input_async does not, so later events of the
    session are reported as needs you until its answer reaches the transcript.
    """
    tool = data.get("tool_name")
    if event == "PreToolUse" and tool == "request_user_input":
        return "Notification", question_text(data)

    session = str(data["session_id"])
    questions = load_questions()
    if event == "PostToolUse" and tool == "request_user_input_async":
        path = data.get("transcript_path") or ""
        try:
            offset = os.path.getsize(path)
        except OSError:
            return "Notification", question_text(data)  # no transcript to watch
        asked = questions[session] = {
            "call": str(data.get("tool_use_id") or ""),
            "path": path,
            "offset": offset,
            "message": question_text(data),
        }
        save_questions(questions)
        return "Notification", asked["message"]

    asked = questions.get(session)
    if not asked:
        return event, message
    if data["hook_event_name"] in ("UserPromptSubmit", "Interrupt", "SessionEnd") or answered(asked):
        del questions[session]
        save_questions(questions)
        return event, message
    if event in ("PreToolUse", "PostToolUse", "Stop"):
        return "Notification", asked["message"]
    return event, message


def normalise(agent, data):
    event = EVENTS[agent].get(data.get("hook_event_name", ""))
    if not event:
        return None
    if agent == "cursor":
        session = data.get("conversation_id") or data.get("session_id")
        roots = data.get("workspace_roots") or []
        cwd = data.get("cwd") or (roots[0] if roots else "")
    else:
        session = data.get("session_id")
        cwd = data.get("cwd", "")
    if not session:
        return None

    message = data.get("message", "")
    if agent == "codex" and event == "Notification":
        tool = data.get("tool_name") or "a tool"
        message = "Codex needs your permission to use " + tool
    if agent == "codex":
        event, message = codex_question(data, event, message)

    out = {"hook_event_name": event, "session_id": str(session), "cwd": cwd}
    if message:
        out["message"] = message[:200]
    if agent != "claude":
        out["agent"] = agent
    return out


def reply(agent, event):
    """What the agent expects on stdout for a hook that changes nothing."""
    if agent == "cursor":
        return '{"continue": true}' if event == "beforeSubmitPrompt" else "{}"
    if agent == "codex" and event in ("Stop", "SubagentStop"):
        return "{}"
    return ""  # Claude Code: plain stdout from some events becomes context


def post(url, body, timeout):
    """POST JSON to http://host[:port]/path and wait for the status line."""
    hostport, _, path = url.split("://", 1)[-1].partition("/")
    host, _, port = hostport.partition(":")
    data = json.dumps(body).encode("utf-8")
    head = (
        f"POST /{path} HTTP/1.1\r\nHost: {hostport}\r\n"
        f"Content-Type: application/json\r\nContent-Length: {len(data)}\r\n"
        "Connection: close\r\n\r\n"
    ).encode("ascii")
    with socket.create_connection((host, int(port or 80)), timeout=timeout) as s:
        s.sendall(head + data)
        s.recv(64)


def main():
    args = dict(zip(sys.argv[1::2], sys.argv[2::2]))
    agent = args.get("--agent", "")
    url = args.get("--url", "")
    if agent not in EVENTS or not url:
        sys.stderr.write("usage: relay.py --agent claude|cursor|codex --url http://<ip>/hook\n")
        return 0

    event = ""
    try:
        data = json.loads(sys.stdin.buffer.read().decode("utf-8", "replace"))
        event = data.get("hook_event_name", "")
        body = normalise(agent, data)
        if body:
            post(url, body, float(args.get("--timeout", 1.0)))
    except Exception:
        pass  # never fail the agent's hook
    out = reply(agent, event)
    if out:
        sys.stdout.write(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
