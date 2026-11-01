"""Codex question tracking: needs you until the question is answered. Posting
to several boards."""

import json
import socket
import tempfile
import threading
import time
import unittest
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from unittest.mock import patch

import relay

CALL = "call_9a5fad6153c1413590e1c514d066809a"
REPLY = {
    "type": "response_item",
    "payload": {"type": "message", "role": "user", "content": [{
        "type": "input_text",
        "text": "<send_user_message_question_reply>\n" + json.dumps([{
            "questionItemId": json.dumps(["request_user_input_async", CALL, 0]),
            "answer": "Working",
        }]),
    }]},
}


class CodexQuestionTests(unittest.TestCase):
    def setUp(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        self.dir = Path(folder.name)
        self.transcript = self.dir / "rollout.jsonl"
        self.transcript.write_text('{"type":"session_meta"}\n', encoding="utf-8")
        patcher = patch.object(relay, "QUESTIONS", str(self.dir / "codex-questions.json"))
        patcher.start()
        self.addCleanup(patcher.stop)

    def event(self, name, **fields):
        data = {"hook_event_name": name, "session_id": "s1", "cwd": "/w/aice",
                "transcript_path": str(self.transcript), **fields}
        body = relay.normalise("codex", data)
        return body["hook_event_name"], body.get("message", "")

    def ask(self):
        return self.event("PostToolUse", tool_name="request_user_input_async", tool_use_id=CALL,
                          tool_input={"questions": [{"title": "Which board?"}]},
                          tool_response={"accepted": True})

    def append(self, record):
        with self.transcript.open("a", encoding="utf-8") as f:
            f.write(json.dumps(record) + "\n")

    def test_async_question_needs_you_until_answered(self):
        self.assertEqual(self.ask(), ("Notification", "Codex asks: Which board?"))
        # Codex keeps working and ends the turn while the question is open.
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "Notification")
        self.assertEqual(self.event("Stop"), ("Notification", "Codex asks: Which board?"))
        self.append(REPLY)
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "PostToolUse")
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "PostToolUse")

    def test_reply_to_another_question_does_not_count(self):
        self.ask()
        self.append(json.loads(json.dumps(REPLY).replace(CALL, "call_other")))
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "Notification")

    def test_new_prompt_or_interrupt_clears_the_question(self):
        self.ask()
        self.assertEqual(self.event("UserPromptSubmit")[0], "UserPromptSubmit")
        self.assertEqual(self.event("Stop")[0], "Stop")
        self.ask()
        self.assertEqual(self.event("Interrupt")[0], "Stop")
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "PostToolUse")

    def test_question_without_transcript_is_reported_once(self):
        self.transcript.unlink()
        self.assertEqual(self.ask()[0], "Notification")
        self.assertEqual(self.event("PostToolUse", tool_name="Bash")[0], "PostToolUse")

    def test_blocking_question_waits_from_pre_to_post_tool_use(self):
        tool_input = json.dumps({"questions": [{"header": "Scope", "question": "Ship it?"}]})
        self.assertEqual(self.event("PreToolUse", tool_name="request_user_input", tool_input=tool_input),
                         ("Notification", "Codex asks: Ship it?"))
        self.assertEqual(self.event("PostToolUse", tool_name="request_user_input")[0], "PostToolUse")

    def test_other_sessions_are_not_held(self):
        self.ask()
        body = relay.normalise("codex", {"hook_event_name": "PostToolUse", "session_id": "s2",
                                         "tool_name": "Bash"})
        self.assertEqual(body["hook_event_name"], "PostToolUse")


class SeveralBoardsTests(unittest.TestCase):
    def test_board_that_hangs_holds_up_no_other(self):
        received = []

        class Board(BaseHTTPRequestHandler):
            def do_POST(self):
                received.append(json.loads(self.rfile.read(int(self.headers["Content-Length"]))))
                self.send_response(204)
                self.end_headers()

            def log_message(self, *args):
                pass

        live = HTTPServer(("127.0.0.1", 0), Board)
        threading.Thread(target=live.handle_request, daemon=True).start()
        self.addCleanup(live.server_close)
        # Listens but never answers, like a board that has hung.
        hung = socket.socket()
        hung.bind(("127.0.0.1", 0))
        hung.listen(1)
        self.addCleanup(hung.close)

        urls = [f"http://127.0.0.1:{hung.getsockname()[1]}/hook", f"http://127.0.0.1:{live.server_port}/hook"]
        body = {"hook_event_name": "Stop", "session_id": "s1", "cwd": "/w/aice"}
        start = time.monotonic()
        relay.post_all(urls, body, 0.5)
        self.assertLess(time.monotonic() - start, 1.5)
        self.assertEqual(received, [body])


if __name__ == "__main__":
    unittest.main()
