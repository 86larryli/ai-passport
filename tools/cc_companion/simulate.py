#!/usr/bin/env python3
"""Replay a scripted set of Claude Code hook events into a running bridge.

Exercises every device screen without running Claude: two working sessions,
subagents, a permission request, a question with options, Chinese text, a
finished turn, and an API error. Standard library only.

  python3 tools/cc_companion/simulate.py            # real-time pacing
  python3 tools/cc_companion/simulate.py --speed 4  # four times faster
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import urllib.request
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from auth import read_token  # noqa: E402

A = {"session_id": "sim-aaaa-1111", "cwd": "/Users/me/Code/ai-passport"}
B = {"session_id": "sim-bbbb-2222", "cwd": "/Users/me/Code/生成实验室"}
C = {"session_id": "sim-cccc-3333", "cwd": "/Users/me/Code/cheapbite"}

SCRIPT = [
    (0.0, A, {"hook_event_name": "SessionStart", "source": "startup"}),
    (0.5, B, {"hook_event_name": "SessionStart", "source": "startup"}),
    (0.5, C, {"hook_event_name": "SessionStart", "source": "startup"}),
    (1.0, A, {"hook_event_name": "UserPromptSubmit", "prompt": "Build the firmware and fix any warnings"}),
    (1.5, A, {"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_use_id": "t1",
              "tool_input": {"command": "./tools/validate.sh --firmware"}}),
    (2.0, B, {"hook_event_name": "UserPromptSubmit",
              "prompt": "整理一下本周的实验数据"}),
    (2.5, B, {"hook_event_name": "PreToolUse", "tool_name": "Task", "tool_use_id": "t2",
              "tool_input": {"subagent_type": "Explore", "description": "查找实验记录"}}),
    (3.0, B, {"hook_event_name": "SubagentStart", "agent_id": "agent-explore-1", "agent_type": "Explore"}),
    (3.5, B, {"hook_event_name": "PreToolUse", "tool_name": "Grep", "agent_id": "agent-explore-1",
              "agent_type": "Explore", "tool_input": {"pattern": "assay_result", "path": "/Users/me/Code/x/data"}}),
    (4.0, B, {"hook_event_name": "SubagentStart", "agent_id": "agent-plan-2", "agent_type": "Plan"}),
    (5.0, A, {"hook_event_name": "PostToolUse", "tool_name": "Bash", "tool_use_id": "t1"}),
    (5.5, A, {"hook_event_name": "PreToolUse", "tool_name": "Bash", "tool_use_id": "t3",
              "tool_input": {"command": "rm -rf build/ && idf.py fullclean"}}),
    (6.0, A, {"hook_event_name": "PermissionRequest", "tool_name": "Bash",
              "tool_input": {"command": "rm -rf build/ && idf.py fullclean",
                             "description": "Clean stale build output"}}),
    (10.0, A, {"hook_event_name": "PostToolUse", "tool_name": "Bash", "tool_use_id": "t3"}),
    (10.5, B, {"hook_event_name": "SubagentStop", "agent_id": "agent-explore-1", "agent_type": "Explore",
               "last_assistant_message": "找到 12 条记录"}),
    (11.0, B, {"hook_event_name": "PreToolUse", "tool_name": "AskUserQuestion", "tool_use_id": "t4",
               "tool_input": {"questions": [{
                   "header": "导出格式",
                   "question": "数据要导出成什么格式？",
                   "options": [{"label": "CSV"}, {"label": "Excel"}, {"label": "两种都要"}]}]}}),
    (16.0, B, {"hook_event_name": "PostToolUse", "tool_name": "AskUserQuestion", "tool_use_id": "t4"}),
    (17.0, A, {"hook_event_name": "Stop",
               "last_assistant_message": "Firmware builds cleanly; 0 warnings left."}),
    (18.0, C, {"hook_event_name": "UserPromptSubmit", "prompt": "Bump the extension version"}),
    (19.0, C, {"hook_event_name": "StopFailure", "error_type": "rate_limit"}),
    (21.0, B, {"hook_event_name": "SubagentStop", "agent_id": "agent-plan-2", "agent_type": "Plan"}),
    (22.0, B, {"hook_event_name": "Stop", "last_assistant_message": "已导出 CSV 和 Excel。"}),
]


def post(port: int, payload: dict) -> None:
    token = read_token()
    if not token:
        raise SystemExit("no bridge token found; start bridge.py first")
    request = urllib.request.Request(f"http://127.0.0.1:{port}/event",
                                     data=json.dumps(payload).encode("utf-8"), method="POST",
                                     headers={"Content-Type": "application/json",
                                              "Authorization": f"Bearer {token}"})
    urllib.request.urlopen(request, timeout=2).close()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=47820)
    parser.add_argument("--speed", type=float, default=1.0)
    parser.add_argument("--end", action="store_true", help="end the simulated sessions afterwards")
    args = parser.parse_args(argv)
    start = time.time()
    for at, session, event in SCRIPT:
        delay = start + at / args.speed - time.time()
        if delay > 0:
            time.sleep(delay)
        payload = {**session, **event, "ts": time.time()}
        post(args.port, payload)
        print(f"{at:5.1f}s {session['session_id']} {event['hook_event_name']}")
    if args.end:
        for session in (A, B, C):
            post(args.port, {**session, "hook_event_name": "SessionEnd", "reason": "other"})
    return 0


if __name__ == "__main__":
    sys.exit(main())
