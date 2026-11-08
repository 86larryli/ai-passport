#!/usr/bin/env python3
"""Host tests for the Claude Code Companion bridge (tools/cc_companion).

Standard library only. Also guards the protocol contract with the firmware:
byte limits (main/cc_types.h), GATT UUIDs (main/cc_ble.c), and a golden CCP
transcript (tests/fixtures/ccp_scenario.txt) that tests/test_ccp.c parses.

Regenerate the golden transcript after an intentional protocol change:
  python3 tests/test_cc_companion.py --write-golden
"""

from __future__ import annotations

import asyncio
import io
import json
import os
import stat
import threading
import re
import sys
import tempfile
import unittest
import uuid
from contextlib import redirect_stdout
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools" / "cc_companion"))

import auth  # noqa: E402
import bridge  # noqa: E402
import ccp  # noqa: E402
import hook  # noqa: E402
import install_hooks  # noqa: E402
from state import SessionTracker, permission_detail, question_detail, summarize  # noqa: E402

GOLDEN = ROOT / "tests" / "fixtures" / "ccp_scenario.txt"
TOKEN = "t" * 43
A = {"session_id": "aaaaaaaa-1111-2222-3333-444444444444", "cwd": "/w/ai-passport"}
B = {"session_id": "bbbbbbbb-1111-2222-3333-444444444444", "cwd": "/w/生成实验室"}
C = {"session_id": "cccccccc-1111-2222-3333-444444444444", "cwd": "/w/ai-passport"}
D = {"session_id": "dddddddd-1111-2222-3333-444444444444", "cwd": "/w/react-native-backend"}


def ev(session: dict, name: str, ts: float, **fields) -> dict:
    return {**session, "hook_event_name": name, "ts": ts, **fields}


def scenario() -> tuple[SessionTracker, list]:
    """A deterministic multi-session scenario used by several tests and the golden file."""
    t = SessionTracker(done_alert_min_s=10, clock=lambda: 1000.0)
    alerts = []
    for e in [
        ev(A, "SessionStart", 100, source="startup"),
        ev(B, "SessionStart", 101, source="startup"),
        ev(C, "SessionStart", 102, source="startup"),
        ev(A, "UserPromptSubmit", 110, prompt="Build the firmware\nand fix warnings"),
        ev(A, "PreToolUse", 111, tool_name="Bash", tool_use_id="t1",
           tool_input={"command": "idf.py build\t--ccache", "description": "Build"}),
        ev(A, "PermissionRequest", 112, tool_name="Bash",
           tool_input={"command": "rm -rf build/\nidf.py fullclean", "description": "Clean"}),
        ev(B, "UserPromptSubmit", 120, prompt="整理数据"),
        ev(B, "PreToolUse", 121, tool_name="Task",
           tool_input={"subagent_type": "Explore", "description": "查找记录"}),
        ev(B, "SubagentStart", 122, agent_id="agent-explore-0001", agent_type="Explore"),
        ev(B, "PreToolUse", 123, agent_id="agent-explore-0001", agent_type="Explore",
           tool_name="Grep", tool_input={"pattern": "assay", "path": "/w/生成实验室/data"}),
        ev(B, "SubagentStart", 124, agent_id="agent-plan-0002", agent_type="Plan"),
        ev(B, "SubagentStop", 130, agent_id="agent-plan-0002", agent_type="Plan",
           last_assistant_message="Plan ready"),
        ev(C, "UserPromptSubmit", 140, prompt="Ship it"),
        ev(C, "PreToolUse", 141, tool_name="AskUserQuestion", tool_input={"questions": [{
            "header": "Target", "question": "Which channel?",
            "options": [{"label": "Beta"}, {"label": "Stable"}]}]}),
        ev(C, "Stop", 149, last_assistant_message="unused"),   # turn took 9 s: no done alert
        ev(D, "UserPromptSubmit", 160, prompt="Start the API"),
        ev(D, "Stop", 165, last_assistant_message="API is running.", background_tasks=[
            {"id": "task-001", "type": "shell", "status": "running", "command": "npm run dev"}]),
    ]:
        alerts.extend(t.apply(e))
    return t, alerts


def golden_text() -> str:
    t, alerts = scenario()
    views, total = t.views(now=1000.0)
    lines = [ccp.hello("0.1.0"), ccp.clock(1794095280, -480)]
    lines += [ccp.event(a.kind, a.sid, a.project, a.text) for a in alerts]
    lines += ccp.snapshot(7, views, total)
    lines.append(ccp.ping())
    return "".join(lines)


class ContractTests(unittest.TestCase):
    def test_limits_match_cc_types_h(self):
        header = (ROOT / "main" / "cc_types.h").read_text(encoding="utf-8")
        macros = {m.group(1): int(m.group(2))
                  for m in re.finditer(r"#define CC_(\w+)_MAX (\d+)", header)}
        for key, value in ccp.LIMITS.items():
            self.assertEqual(macros.get(key), value, f"CC_{key}_MAX drifted")
        self.assertEqual(macros["LINE"], ccp.LINE_MAX)
        self.assertIn(f"#define CC_MAX_SESSIONS {ccp.MAX_SESSIONS}", header)
        self.assertIn(f"#define CC_MAX_AGENTS {ccp.MAX_AGENTS}", header)
        self.assertIn(f"#define CC_PROTO_VERSION {ccp.PROTO_VERSION}", header)

    def test_uuids_match_firmware(self):
        source = (ROOT / "main" / "cc_ble.c").read_text(encoding="utf-8")
        found = {}
        for name, body in re.findall(r"static const ble_uuid128_t (\w+) =\s*BLE_UUID128_INIT\(([^)]*)\)", source):
            data = bytes(int(b, 16) for b in re.findall(r"0x([0-9a-fA-F]{2})", body))
            found[name] = str(uuid.UUID(bytes=data[::-1]))   # NimBLE stores little-endian
        self.assertEqual(found.get("SVC_UUID"), ccp.SERVICE_UUID)
        self.assertEqual(found.get("RX_UUID"), ccp.RX_UUID)
        self.assertEqual(found.get("TX_UUID"), ccp.TX_UUID)
        self.assertIn(f'"{ccp.DEVICE_NAME_PREFIX}-', source)

    def test_golden_transcript_is_current(self):
        self.assertEqual(GOLDEN.read_text(encoding="utf-8"), golden_text(),
                         "run: python3 tests/test_cc_companion.py --write-golden")


class EncoderTests(unittest.TestCase):
    def test_clean_strips_controls_and_tabs(self):
        self.assertEqual(ccp.clean("a\tb\nc\x00d", "ACTIVITY"), "a b c d")
        self.assertEqual(ccp.clean("line1\nline2\r\nline3", "Q_BODY", multiline=True),
                         "line1\x1fline2\x1fline3")
        self.assertEqual(ccp.clean(None, "NOTE"), "")

    def test_truncation_respects_utf8_and_limit(self):
        text = "中" * 100   # 300 bytes
        out = ccp.clean(text, "PROJECT")
        self.assertLessEqual(len(out.encode("utf-8")), ccp.LIMITS["PROJECT"] - 1)
        self.assertTrue(out.endswith("..."))
        out.encode("utf-8").decode("utf-8")
        exact = "x" * (ccp.LIMITS["PROJECT"] - 1)
        self.assertEqual(ccp.clean(exact, "PROJECT"), exact)

    def test_snapshot_limits_and_line_length(self):
        views = [ccp.SessionView(f"s{i}", "work", 5, "p" * 300, "a" * 500, note="n" * 500,
                                 agents_total=9, agents_running=9,
                                 agents=[ccp.AgentView(f"a{j}", "run", 1, "t" * 99, "x" * 300)
                                         for j in range(9)],
                                 q_kind="perm", q_title="T" * 200, q_body=("b" * 50 + "\n") * 20)
                 for i in range(9)]
        lines = ccp.snapshot(3, views, 9)
        self.assertEqual(lines[0], "SNAP\t3\t9\n")
        self.assertEqual(lines[-1], "END\t3\n")
        self.assertEqual(sum(1 for line in lines if line.startswith("S\t")), ccp.MAX_SESSIONS)
        self.assertEqual(sum(1 for line in lines if line.startswith("A\t")), ccp.MAX_SESSIONS * ccp.MAX_AGENTS)
        for line in lines:
            self.assertLessEqual(len(line.encode("utf-8")), ccp.LINE_MAX)
            self.assertEqual(line.count("\n"), 1)

    def test_device_lines(self):
        self.assertEqual(ccp.parse_device_line("HI\t1\t0.1.0\t87\n").fields, ["1", "0.1.0", "87"])
        self.assertEqual(ccp.parse_device_line("ACK\t5").kind, "ACK")
        self.assertEqual(ccp.parse_device_line("WHAT").kind, "UNKNOWN")


class TrackerTests(unittest.TestCase):
    def test_scenario_states_and_order(self):
        t, alerts = scenario()
        views, total = t.views(now=1000.0)
        self.assertEqual(total, 4)
        by_sid = {v.sid: v for v in views}
        self.assertEqual(by_sid["dddddddd"].state, "bg")
        self.assertEqual(by_sid["dddddddd"].activity, "1 shell running")
        a, b, c = by_sid["aaaaaaaa"], by_sid["bbbbbbbb"], by_sid["cccccccc"]
        self.assertEqual(a.state, "perm")
        self.assertEqual(a.q_kind, "perm")
        self.assertIn("rm -rf build/", a.q_body)
        self.assertEqual(a.note, "> Build the firmware")
        self.assertEqual(b.state, "work")
        self.assertEqual(b.activity, "Agent: 查找记录")
        self.assertEqual((b.agents_total, b.agents_running), (2, 1))
        self.assertEqual(b.agents[0].type, "Explore")          # running first
        self.assertEqual(b.agents[0].activity, 'Grep "assay" in data')
        self.assertEqual(b.agents[1].state, "done")
        self.assertEqual(c.state, "done")                       # Stop after the question
        self.assertEqual(c.q_kind, "")
        # Two sessions share "ai-passport": numbered by start time.
        self.assertEqual(a.project, "ai-passport #1")
        self.assertEqual(c.project, "ai-passport #2")
        self.assertEqual(views[0].sid, "aaaaaaaa")              # needs the user: first
        self.assertEqual([x.kind for x in alerts], ["perm", "ask"])   # C's turn took < 10 s

    def test_permission_cleared_by_post_tool_use(self):
        t = SessionTracker(clock=lambda: 50.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        t.apply(ev(A, "PermissionRequest", 2, tool_name="Bash", tool_input={"command": "ls"}))
        self.assertEqual(t.views(50)[0][0].state, "perm")
        t.apply(ev(A, "PostToolUse", 3, tool_name="Bash"))
        view = t.views(50)[0][0]
        self.assertEqual(view.state, "work")
        self.assertEqual(view.q_kind, "")

    def test_out_of_order_events_do_not_regress(self):
        t = SessionTracker(clock=lambda: 50.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        # The permission request is delivered before the PreToolUse that preceded it.
        t.apply(ev(A, "PermissionRequest", 3, tool_name="Bash", tool_input={"command": "ls"}))
        t.apply(ev(A, "PreToolUse", 2, tool_name="Bash", tool_input={"command": "ls"}))
        self.assertEqual(t.views(50)[0][0].state, "perm")
        # A late PreToolUse of an earlier tool does not replace newer activity.
        t.apply(ev(A, "PreToolUse", 5, tool_name="Read", tool_input={"file_path": "/w/ai-passport/b.c"}))
        t.apply(ev(A, "PreToolUse", 4, tool_name="Read", tool_input={"file_path": "/w/ai-passport/a.c"}))
        self.assertEqual(t.views(50)[0][0].activity, "Read b.c")

    def test_done_alert_threshold_and_error(self):
        t = SessionTracker(done_alert_min_s=10, clock=lambda: 100.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        alerts = t.apply(ev(A, "Stop", 30, last_assistant_message="All good.\nMore text"))
        self.assertEqual([(x.kind, x.text) for x in alerts], [("done", "All good.")])
        alerts = t.apply(ev(A, "StopFailure", 40, error_type="rate_limit"))
        self.assertEqual([(x.kind, x.text) for x in alerts], [("err", "rate_limit")])
        self.assertEqual(t.views(100)[0][0].state, "err")

    def test_background_agent_keeps_session_in_background(self):
        t = SessionTracker(done_alert_min_s=10, clock=lambda: 100.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        t.apply(ev(A, "SubagentStart", 2, agent_id="bg1", agent_type="general"))
        self.assertEqual(t.apply(ev(A, "Stop", 3)), [])            # no finished alert yet
        t.apply(ev(A, "PreToolUse", 4, agent_id="bg1", tool_name="Read", tool_input={"file_path": "/w/x"}))
        view = t.views(100)[0][0]
        self.assertEqual(view.state, "bg")                          # the subagent does not reopen "work"
        self.assertEqual(view.activity, "1 subagent running")
        self.assertEqual(view.agents[0].activity, "Read /w/x")
        alerts = t.apply(ev(A, "SubagentStop", 30, agent_id="bg1", agent_type="general"))
        self.assertEqual(t.views(100)[0][0].state, "done")
        self.assertEqual([x.kind for x in alerts], ["done"])       # 1 -> 30 s: whole job measured

    def test_background_shells_and_monitors_from_stop(self):
        t = SessionTracker(done_alert_min_s=10, clock=lambda: 200.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="start the dev servers"))
        t.apply(ev(A, "PreToolUse", 2, tool_name="Bash", tool_use_id="tu1",
                   tool_input={"command": "npm run dev --prefix /w/ai-passport/api", "run_in_background": True}))
        rows = t.views(200)[0][0].agents
        self.assertEqual([(r.type, r.activity) for r in rows], [("shell", "npm run dev --prefix api")])
        alerts = t.apply(ev(A, "Stop", 5, last_assistant_message="Servers are up.", background_tasks=[
            {"id": "b1", "type": "shell", "status": "running", "command": "npm run dev --prefix /w/ai-passport/api"},
            {"id": "b2", "type": "shell", "status": "running", "command": "tail -f logs/app.log"},
            {"id": "m1", "type": "monitor", "status": "running", "description": "watch build output"},
            {"id": "s1", "type": "subagent", "status": "running", "agent_type": "Explore"},
            {"id": "old", "type": "shell", "status": "completed", "command": "npm ci"},
        ]))
        self.assertEqual(alerts, [])
        view = t.views(200)[0][0]
        self.assertEqual(view.state, "bg")
        self.assertEqual(view.activity, "1 subagent, 2 shells, 1 monitor running")
        self.assertEqual(view.note, "Servers are up.")
        self.assertEqual(sorted(r.activity for r in view.agents),
                         ["npm run dev --prefix api", "tail -f logs/app.log", "watch build output"])
        self.assertEqual((view.agents_running, view.agents_total), (3, 3))
        # Claude is re-invoked when tasks finish; the last Stop has nothing in flight.
        t.apply(ev(A, "UserPromptSubmit", 40, prompt="task finished"))
        alerts = t.apply(ev(A, "Stop", 41, background_tasks=[]))
        view = t.views(200)[0][0]
        self.assertEqual(view.state, "done")
        self.assertEqual(view.agents, [])
        self.assertEqual([(x.kind, x.text) for x in alerts], [("done", "Servers are up.")])

    def test_real_subagent_stop_payload_updates_background(self):
        # Shape captured from Claude Code 2.1.319 (paths shortened).
        t = SessionTracker(clock=lambda: 300.0)
        t.apply(ev(D, "UserPromptSubmit", 1, prompt="review PR 1628"))
        t.apply(ev(D, "SubagentStart", 2, agent_id="ac0759ad07b1b22c8", agent_type="review-claude"))
        t.apply(ev(D, "SubagentStart", 3, agent_id="afinished000000000", agent_type="Explore"))
        t.apply(ev(D, "Stop", 4))
        t.apply(ev(D, "SubagentStop", 9, agent_id="afinished000000000", agent_type="Explore",
                   background_tasks=[
                       {"id": "ac0759ad07b1b22c8", "type": "subagent", "status": "running",
                        "description": "Claude lane review of PR 1628", "agent_type": "review-claude"},
                       {"id": "b3gd50gla", "type": "shell", "status": "running",
                        "description": "Run Gemini reviewer lane for PR 1628",
                        "command": "node /w/claude-config/scripts/run-opencode-lane.mjs --agent gemini"}]))
        view = t.views(300)[0][0]
        self.assertEqual(view.state, "bg")
        self.assertEqual(view.activity, "1 subagent, 1 shell running")
        self.assertIn("Run Gemini reviewer lane for PR 1628", [r.activity for r in view.agents])
        self.assertEqual((view.agents_running, view.agents_total), (2, 3))

    def test_monitor_tasks_reported_as_shell_are_labeled_monitor(self):
        t = SessionTracker(clock=lambda: 100.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="watch the bridge"))
        t.apply(ev(A, "PreToolUse", 2, tool_name="Monitor", tool_use_id="m1",
                   tool_input={"description": "bridge errors or link drops", "command": "tail -f x | grep E"}))
        t.apply(ev(A, "Stop", 3, background_tasks=[
            {"id": "bl26", "type": "shell", "status": "running", "description": "Run the bridge",
             "command": "uv run bridge.py"},
            {"id": "b96m", "type": "shell", "status": "running", "description": "bridge errors or link drops",
             "command": "tail -f x | grep E"}]))
        view = t.views(100)[0][0]
        self.assertEqual(view.activity, "1 shell, 1 monitor running")
        self.assertEqual(sorted(r.type for r in view.agents), ["monitor", "shell"])

    def test_session_first_seen_mid_turn_still_alerts(self):
        t = SessionTracker(done_alert_min_s=10, clock=lambda: 100.0)
        t.apply(ev(A, "PreToolUse", 1, tool_name="Bash", tool_input={"command": "npm run build"}))
        alerts = t.apply(ev(A, "Stop", 30, last_assistant_message="Build finished."))
        self.assertEqual([(x.kind, x.text) for x in alerts], [("done", "Build finished.")])

    def test_notification_prompt_does_not_replace_note(self):
        t = SessionTracker(clock=lambda: 100.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="Build the app"))
        t.apply(ev(A, "Stop", 2, background_tasks=[{"id": "b", "type": "shell", "status": "running"}]))
        t.apply(ev(A, "UserPromptSubmit", 3, prompt="<task-notification><task-id>b</task-id>..."))
        self.assertEqual(t.views(100)[0][0].note, "> Build the app")

    def test_question_is_not_replaced_by_its_permission_events(self):
        t = SessionTracker(clock=lambda: 100.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        questions = {"questions": [{"header": "Save work", "question": "How?", "options": [{"label": "Fork"}]}]}
        alerts = t.apply(ev(A, "PreToolUse", 2, tool_name="AskUserQuestion", tool_input=questions))
        alerts += t.apply(ev(A, "PermissionRequest", 3, tool_name="AskUserQuestion", tool_input=questions))
        alerts += t.apply(ev(A, "Notification", 4, notification_type="permission_prompt",
                             message="Claude needs your permission to use AskUserQuestion"))
        self.assertEqual([x.kind for x in alerts], ["ask"])
        view = t.views(100)[0][0]
        self.assertEqual((view.state, view.q_kind, view.q_title), ("ask", "ask", "Save work"))

    def test_ordering_puts_background_with_working(self):
        t = SessionTracker(clock=lambda: 50.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        t.apply(ev(A, "Stop", 2, background_tasks=[{"id": "b", "type": "shell", "status": "running"}]))
        t.apply(ev(B, "UserPromptSubmit", 3, prompt="y"))
        t.apply(ev(B, "Stop", 4))
        self.assertEqual([v.state for v in t.views(50)[0]], ["bg", "done"])

    def test_session_end_and_prune(self):
        t = SessionTracker(stale_s=100, clock=lambda: 0.0)
        t.apply(ev(A, "SessionStart", 1, source="startup"))
        t.apply(ev(B, "SessionStart", 1, source="startup"))
        t.apply(ev(A, "SessionEnd", 2, reason="other"))
        self.assertEqual(t.views(2)[1], 1)
        t.prune(now=500)
        self.assertEqual(t.views(500)[1], 0)

    def test_notifications(self):
        t = SessionTracker(clock=lambda: 0.0)
        t.apply(ev(A, "UserPromptSubmit", 1, prompt="x"))
        alerts = t.apply(ev(A, "Notification", 2, notification_type="permission_prompt",
                            message="Claude needs your permission to use Bash"))
        self.assertEqual([x.kind for x in alerts], ["perm"])
        self.assertEqual(t.apply(ev(A, "Notification", 3, notification_type="permission_prompt",
                                    message="again")), [])
        self.assertEqual(t.apply(ev(A, "Notification", 4, notification_type="idle_prompt",
                                    message="waiting")), [])

    def test_long_question_keeps_every_option(self):
        long_q = "\u6211\u4eec\u9700\u8981\u51b3\u5b9a\u5bfc\u51fa\u65b9\u6848" * 20   # ~600 bytes
        opts = [{"label": "\u9009\u9879" * 30 + str(i)} for i in range(4)]
        title, body = question_detail({"questions": [{"header": "H", "question": long_q, "options": opts}]})
        sent = ccp.clean(body, "Q_BODY", multiline=True)
        self.assertEqual(sent, ccp.clean(body, "Q_BODY", multiline=True))
        self.assertLessEqual(len(body.encode("utf-8")), ccp.LIMITS["Q_BODY"] - 1)
        lines = body.split("\n")
        self.assertEqual([line[:3] for line in lines[1:]], ["1. ", "2. ", "3. ", "4. "])
        self.assertTrue(lines[0].endswith("...") and lines[1].endswith("..."))
        # Encoding does not cut it further: the last option survives on the wire.
        self.assertIn("4. ", sent)

    def test_many_options_degrade_with_a_count(self):
        opts = [{"label": "x" * 200} for _ in range(12)]
        _title, body = question_detail({"questions": [{"question": "Q?", "options": opts}, {"question": "2"}]})
        self.assertLessEqual(len(body.encode("utf-8")), ccp.LIMITS["Q_BODY"] - 1)
        self.assertIn("options)", body)
        self.assertTrue(body.endswith("(+1 more)"))

    def test_summaries(self):
        cwd = "/w/proj"
        self.assertEqual(summarize("Bash", {"command": "make\nmake test"}, cwd), "$ make")
        self.assertEqual(summarize("Edit", {"file_path": "/w/proj/src/a.c"}, cwd), "Edit src/a.c")
        self.assertEqual(summarize("WebFetch", {"url": "https://docs.lvgl.io/9.5/x?y=1"}, cwd),
                         "Fetch docs.lvgl.io/9.5/x")
        self.assertEqual(summarize("mcp__github__create_issue", {}, cwd), "github: create_issue")
        self.assertEqual(summarize("Weird", {}, cwd), "Weird")
        self.assertEqual(summarize("Bash", {"command": 'grep -n X /w/proj/src/a.c && cd /w/proj'}, cwd),
                         "$ grep -n X src/a.c && cd .")
        home = os.path.expanduser("~")
        self.assertEqual(summarize("Bash", {"command": f"ls {home}/Code"}, cwd), "$ ls ~/Code")
        self.assertEqual(summarize("SubagentHandback", {}, cwd), "Reporting back")
        self.assertEqual(summarize("Bash", {"command": "cp /w/proj /w/projector/x"}, cwd), "$ cp . /w/projector/x")
        self.assertEqual(permission_detail("Bash", {"command": "ls", "description": "List"}, cwd),
                         "ls\n# List")


class HookTests(unittest.TestCase):
    def test_reduce_drops_output_and_caps_strings(self):
        payload = {
            "session_id": "s", "hook_event_name": "PostToolUse", "cwd": "/w",
            "tool_name": "Read", "tool_response": {"content": "x" * 100000},
            "tool_input": {"file_path": "/w/a", "content": "y" * 100000, "command": "z" * 5000},
            "transcript_path": "/secret/path.jsonl", "prompt": "p" * 5000,
        }
        out = hook.reduce(payload, now=1.5)
        self.assertNotIn("tool_response", out)
        self.assertNotIn("transcript_path", out)
        self.assertNotIn("content", out["tool_input"])
        self.assertEqual(len(out["tool_input"]["command"]), hook.TOOL_INPUT["command"])
        self.assertEqual(len(out["prompt"]), hook.TOP_LEVEL["prompt"])
        self.assertEqual(out["ts"], 1.5)

    def test_reduce_keeps_questions(self):
        out = hook.reduce({"tool_input": {"questions": [
            {"question": "Q?", "header": "H", "options": [{"label": "A", "description": "d"}, "B"]}]}})
        self.assertEqual(out["tool_input"]["questions"][0]["options"], [{"label": "A"}, {"label": "B"}])

    def test_main_never_fails_without_a_bridge(self):
        stdin = sys.stdin
        old = os.environ.get("CC_COMPANION_TOKEN_FILE")
        try:
            os.environ["CC_COMPANION_TOKEN_FILE"] = "/nonexistent/cc-companion/token"
            sys.stdin = io.StringIO("not json")
            self.assertEqual(hook.main(), 0)
            self.assertEqual(sys.stdin.read(), "")      # stdin was drained
        finally:
            sys.stdin = stdin
            if old is None:
                os.environ.pop("CC_COMPANION_TOKEN_FILE", None)
            else:
                os.environ["CC_COMPANION_TOKEN_FILE"] = old


class InstallTests(unittest.TestCase):
    def test_install_is_idempotent_and_preserves_other_hooks(self):
        foreign = {"type": "command", "command": "~/bin/notify.sh"}
        settings = {"model": "opus", "hooks": {"Stop": [{"hooks": [foreign]}],
                                               "PreToolUse": [{"matcher": "Bash", "hooks": [foreign]}]}}
        once = install_hooks.install(json.loads(json.dumps(settings)), "python3", None)
        twice = install_hooks.install(json.loads(json.dumps(once)), "python3", None)
        self.assertEqual(once, twice)
        self.assertEqual(once["model"], "opus")
        self.assertIn({"hooks": [foreign]}, once["hooks"]["Stop"])
        ours = [h for groups in once["hooks"].values() for g in groups for h in g["hooks"]
                if install_hooks.MARKER in h.get("command", "")]
        self.assertEqual(len(ours), len(install_hooks.EVENTS))
        by_event = {event: g["hooks"][-1] for event, groups in once["hooks"].items() for g in groups
                    if install_hooks.MARKER in g["hooks"][-1].get("command", "")}
        self.assertFalse(by_event["SessionEnd"]["async"])
        self.assertTrue(all(h["async"] for e, h in by_event.items() if e != "SessionEnd"))
        removed = install_hooks.strip(json.loads(json.dumps(once)))
        self.assertEqual(removed, settings)

    def test_cli_backup_and_uninstall(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "settings.json"
            path.write_text('{"theme": "dark"}\n')
            with redirect_stdout(io.StringIO()):
                self.assertEqual(install_hooks.main(["--settings", str(path)]), 0)
                self.assertTrue(list(Path(tmp).glob("settings.json.bak-*")))
                self.assertIn("SessionStart", json.loads(path.read_text())["hooks"])
                self.assertEqual(install_hooks.main(["--settings", str(path), "--uninstall"]), 0)
            self.assertEqual(json.loads(path.read_text()), {"theme": "dark"})


class FakeLink:
    def __init__(self, on_ready, _on_line):
        self.ready = True
        self.sent: list[str] = []
        self.on_ready = on_ready

    async def run(self):
        await asyncio.Event().wait()

    async def send(self, text: str) -> bool:
        self.sent.append(text)
        return True


class BridgeTests(unittest.TestCase):
    def test_ingest_sends_events_then_snapshot(self):
        async def scenario_run():
            b = bridge.Bridge(SessionTracker(), FakeLink, TOKEN)
            task = asyncio.create_task(b.sender())
            b.ingest(ev(A, "UserPromptSubmit", 1.0, prompt="x"))
            b.ingest(ev(A, "PermissionRequest", 1.1, tool_name="Bash", tool_input={"command": "ls"}))
            await asyncio.sleep(0.4)
            task.cancel()
            return b.link.sent

        sent = asyncio.run(scenario_run())
        joined = "".join(sent)
        self.assertTrue(joined.startswith("EVT\tperm\taaaaaaaa\tai-passport\tBash: $ ls\n"))
        self.assertIn("SNAP\t1\t1\n", joined)
        self.assertIn("Q\tperm\tBash\tls\n", joined)

    def test_http_ingest_and_status(self):
        async def http_run():
            b = bridge.Bridge(SessionTracker(), FakeLink, TOKEN)
            server = await asyncio.start_server(b.handle_http, "127.0.0.1", 0)
            port = server.sockets[0].getsockname()[1]
            body = json.dumps(ev(A, "SessionStart", 1.0, source="startup")).encode()

            async def request(raw: bytes) -> bytes:
                reader, writer = await asyncio.open_connection("127.0.0.1", port)
                writer.write(raw)
                await writer.drain()
                reply = await reader.read()
                writer.close()
                await writer.wait_closed()
                return reply

            auth_header = b"Authorization: Bearer " + TOKEN.encode() + b"\r\n"
            post = b"POST /event HTTP/1.1\r\nContent-Length: %d\r\n" % len(body)
            denied = [
                await request(post + b"\r\n" + body),                                   # no token
                await request(post + b"Authorization: Bearer wrong\r\n\r\n" + body),     # wrong token
                await request(b"GET /status HTTP/1.1\r\n\r\n"),                         # no token
            ]
            sessions_after_denied = len(b.tracker.sessions)
            first = await request(post + auth_header + b"\r\n" + body)
            second = await request(b"GET /status HTTP/1.1\r\n" + auth_header + b"\r\n")
            server.close()
            await server.wait_closed()
            return denied, sessions_after_denied, first, second

        denied, sessions_after_denied, first, second = asyncio.run(http_run())
        for reply in denied:
            self.assertTrue(reply.startswith(b"HTTP/1.1 401"), reply[:40])
        self.assertNotIn(b"ai-passport", b"".join(denied))
        self.assertEqual(sessions_after_denied, 0)
        self.assertTrue(first.startswith(b"HTTP/1.1 204"))
        status = json.loads(second.split(b"\r\n\r\n", 1)[1])
        self.assertEqual(status["sessions"][0]["project"], "ai-passport")

    def test_hook_end_to_end_with_token(self):
        loop = asyncio.new_event_loop()
        b = bridge.Bridge(SessionTracker(), FakeLink, TOKEN)
        server = loop.run_until_complete(asyncio.start_server(b.handle_http, "127.0.0.1", 0))
        port = server.sockets[0].getsockname()[1]
        thread = threading.Thread(target=loop.run_forever, daemon=True)
        thread.start()
        with tempfile.TemporaryDirectory() as tmp:
            token_file = Path(tmp) / "token"
            token_file.write_text(TOKEN + "\n")
            env = {"CC_COMPANION_TOKEN_FILE": str(token_file), "CC_COMPANION_PORT": str(port)}
            old_env = {k: os.environ.get(k) for k in env}
            stdin = sys.stdin
            try:
                os.environ.update(env)
                sys.stdin = io.StringIO(json.dumps(ev(A, "UserPromptSubmit", 1.0, prompt="hi")))
                self.assertEqual(hook.main(), 0)
            finally:
                sys.stdin = stdin
                for k, v in old_env.items():
                    if v is None:
                        os.environ.pop(k, None)
                    else:
                        os.environ[k] = v
        loop.call_soon_threadsafe(loop.stop)
        thread.join(2)
        server.close()
        loop.run_until_complete(server.wait_closed())
        loop.close()
        self.assertIn(A["session_id"], b.tracker.sessions)


class AuthTests(unittest.TestCase):
    def test_token_file_is_private_and_reused(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "cc" / "token"
            token = auth.load_or_create_token(path)
            self.assertGreaterEqual(len(token), 32)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)
            self.assertEqual(auth.load_or_create_token(path), token)
            self.assertEqual(auth.read_token(path), token)
            os.chmod(path, 0o644)
            self.assertEqual(auth.load_or_create_token(path), token)
            self.assertEqual(stat.S_IMODE(path.stat().st_mode), 0o600)
            path.write_text("short")
            self.assertNotEqual(auth.load_or_create_token(path), "short")
            os.chmod(path.parent, 0o755)            # e.g. created earlier by another writer
            auth.load_or_create_token(path)
            self.assertEqual(stat.S_IMODE(path.parent.stat().st_mode), 0o700)

    def test_authorized_header(self):
        self.assertTrue(auth.authorized("Bearer " + TOKEN, TOKEN))
        self.assertFalse(auth.authorized("Bearer x" + TOKEN, TOKEN))
        self.assertFalse(auth.authorized(TOKEN, TOKEN))
        self.assertFalse(auth.authorized(None, TOKEN))

    def test_hook_and_bridge_share_the_default_path(self):
        self.assertEqual(hook.DEFAULT_TOKEN_FILE, str(auth.DEFAULT_TOKEN_FILE))


if __name__ == "__main__":
    if "--write-golden" in sys.argv:
        GOLDEN.parent.mkdir(parents=True, exist_ok=True)
        GOLDEN.write_text(golden_text(), encoding="utf-8")
        print(f"wrote {GOLDEN}")
        sys.exit(0)
    unittest.main()
