#!/usr/bin/env python3
"""Claude Code hook that forwards a trimmed event to the local companion bridge.

Install it as an asynchronous command hook (tools/cc_companion/install_hooks.py
does this). It reads the hook JSON from stdin, keeps only the fields the bridge
uses, drops tool output entirely, caps every string, adds the time the hook
started, and POSTs the result to http://127.0.0.1:<port>/event.

It sends the bridge's shared-secret token (~/.config/cc-companion/token, see
auth.py) as `Authorization: Bearer`; without a token it sends nothing.

It never blocks Claude Code and never fails visibly: every error is swallowed
and the script exits 0. Standard library only, Python 3.8+.
"""

from __future__ import annotations

import json
import os
import sys
import time
import urllib.request

DEFAULT_PORT = 47820
MAX_STDIN = 1 << 20
DEFAULT_TOKEN_FILE = "~/.config/cc-companion/token"   # must match auth.py

TOP_LEVEL = {
    "session_id": 128, "hook_event_name": 64, "cwd": 512, "tool_name": 128,
    "tool_use_id": 128, "agent_id": 128, "agent_type": 128, "notification_type": 64,
    "message": 600, "title": 200, "source": 64, "reason": 64, "prompt": 400,
    "last_assistant_message": 600, "error": 300, "error_type": 120, "error_details": 300,
    "permission_mode": 64,
}
TOOL_INPUT = {
    "command": 600, "description": 300, "file_path": 512, "notebook_path": 512,
    "path": 512, "pattern": 200, "url": 512, "query": 300, "subagent_type": 128,
    "skill": 128, "name": 128, "run_in_background": 8,
}


def _cap(value, limit):
    if isinstance(value, str):
        return value[:limit]
    if isinstance(value, (int, float, bool)) or value is None:
        return value
    return str(value)[:limit]


def _questions(raw):
    if not isinstance(raw, list):
        return None
    out = []
    for q in raw[:4]:
        if not isinstance(q, dict):
            continue
        options = []
        for option in (q.get("options") or [])[:6]:
            label = option.get("label") if isinstance(option, dict) else option
            options.append({"label": _cap(label, 120)})
        out.append({"question": _cap(q.get("question"), 400), "header": _cap(q.get("header"), 60),
                    "options": options, "multiSelect": bool(q.get("multiSelect"))})
    return out


def reduce(payload: dict, now: float | None = None) -> dict:
    out: dict = {k: _cap(payload[k], limit) for k, limit in TOP_LEVEL.items() if k in payload}
    if "stop_hook_active" in payload:
        out["stop_hook_active"] = bool(payload["stop_hook_active"])
    tool_input = payload.get("tool_input")
    if isinstance(tool_input, dict):
        reduced: dict = {k: _cap(tool_input[k], limit) for k, limit in TOOL_INPUT.items() if k in tool_input}
        questions = _questions(tool_input.get("questions"))
        if questions is not None:
            reduced["questions"] = questions
        out["tool_input"] = reduced
    for key in ("background_tasks", "session_crons"):
        items = payload.get(key)
        if isinstance(items, list):
            out[key] = [_flat(item) for item in items[:20]]
    out["ts"] = time.time() if now is None else now
    return out


def _flat(item):
    """Keep only the scalar fields of a list entry (shape varies by Claude Code version)."""
    if isinstance(item, dict):
        return {str(k)[:40]: _cap(v, 200) for k, v in list(item.items())[:16]
                if isinstance(v, (str, int, float, bool)) or v is None}
    return _cap(item, 200)


def read_token():
    path = os.path.expanduser(os.environ.get("CC_COMPANION_TOKEN_FILE", DEFAULT_TOKEN_FILE))
    try:
        with open(path, encoding="utf-8") as handle:
            return handle.read().strip() or None
    except OSError:
        return None


def main() -> int:
    started = time.time()
    try:
        raw = sys.stdin.read(MAX_STDIN)   # always drain stdin so Claude Code never sees EPIPE
        token = read_token()
        if not token:
            return 0   # bridge never ran on this account: nothing to talk to
        payload = json.loads(raw)
        if not isinstance(payload, dict):
            return 0
        body = json.dumps(reduce(payload, started), ensure_ascii=False).encode("utf-8")
        port = int(os.environ.get("CC_COMPANION_PORT", DEFAULT_PORT))
        request = urllib.request.Request(
            f"http://127.0.0.1:{port}/event", data=body, method="POST",
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {token}"})
        urllib.request.urlopen(request, timeout=0.5).close()
    except Exception:  # noqa: BLE001 - a status display must never disturb Claude Code
        pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
