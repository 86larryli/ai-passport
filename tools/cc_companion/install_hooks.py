#!/usr/bin/env python3
"""Add (or remove) the companion hooks in a Claude Code settings file.

Default target: ~/.claude/settings.json (user settings, every project).
Every hook is asynchronous except SessionEnd (see SYNC_EVENTS).
The script is idempotent: it recognizes its own entries by the hook.py path,
replaces them in place, and never touches other hooks. It writes a timestamped
backup next to the settings file before changing it. Use --dry-run first.

  python3 tools/cc_companion/install_hooks.py --dry-run
  python3 tools/cc_companion/install_hooks.py
  python3 tools/cc_companion/install_hooks.py --uninstall
"""

from __future__ import annotations

import argparse
import json
import shlex
import shutil
import sys
import time
from pathlib import Path

HOOK_SCRIPT = Path(__file__).resolve().parent / "hook.py"
MARKER = "cc_companion/hook.py"
EVENTS = (
    "SessionStart", "SessionEnd", "UserPromptSubmit", "PreToolUse", "PostToolUse",
    "PostToolUseFailure", "PermissionRequest", "PermissionDenied", "Notification",
    "SubagentStart", "SubagentStop", "Stop", "StopFailure",
)


# Claude Code may exit without waiting for asynchronous SessionEnd hooks, which
# would leave a closed session on the device. That one hook runs synchronously;
# hook.py's HTTP timeout (0.5 s) bounds the delay at exit.
SYNC_EVENTS = ("SessionEnd",)


def hook_entry(python: str, port: int | None, run_async: bool = True) -> dict:
    command = f"{shlex.quote(python)} {shlex.quote(str(HOOK_SCRIPT))}"
    if port:
        command = f"CC_COMPANION_PORT={int(port)} {command}"
    return {"type": "command", "command": command, "async": run_async, "timeout": 5}


def _is_ours(handler: object) -> bool:
    return isinstance(handler, dict) and MARKER in str(handler.get("command", ""))


def strip(settings: dict) -> dict:
    """Remove every companion handler, dropping groups and events left empty."""
    hooks = settings.get("hooks")
    if not isinstance(hooks, dict):
        return settings
    for event in list(hooks):
        groups = hooks[event]
        if not isinstance(groups, list):
            continue
        kept = []
        for group in groups:
            if not isinstance(group, dict) or not isinstance(group.get("hooks"), list):
                kept.append(group)
                continue
            handlers = [h for h in group["hooks"] if not _is_ours(h)]
            if handlers:
                kept.append({**group, "hooks": handlers})
        if kept:
            hooks[event] = kept
        else:
            del hooks[event]
    if not hooks:
        settings.pop("hooks", None)
    return settings


def install(settings: dict, python: str, port: int | None) -> dict:
    settings = strip(settings)
    hooks = settings.setdefault("hooks", {})
    for event in EVENTS:
        entry = hook_entry(python, port, run_async=event not in SYNC_EVENTS)
        hooks.setdefault(event, []).append({"hooks": [entry]})
    return settings


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--settings", type=Path, default=Path("~/.claude/settings.json").expanduser())
    parser.add_argument("--python", default="python3", help="interpreter used to run hook.py")
    parser.add_argument("--port", type=int, help="bridge port if not the default 47820")
    parser.add_argument("--uninstall", action="store_true")
    parser.add_argument("--dry-run", action="store_true", help="print the result without writing")
    args = parser.parse_args(argv)

    try:
        original = args.settings.read_text(encoding="utf-8")
        settings = json.loads(original) if original.strip() else {}
    except FileNotFoundError:
        original, settings = "", {}
    if not isinstance(settings, dict):
        print(f"{args.settings} is not a JSON object; refusing to edit it", file=sys.stderr)
        return 1

    updated = strip(settings) if args.uninstall else install(settings, args.python, args.port)
    text = json.dumps(updated, indent=2, ensure_ascii=False) + "\n"
    if args.dry_run:
        print(text, end="")
        return 0
    if text == original:
        print(f"{args.settings}: already up to date")
        return 0
    args.settings.parent.mkdir(parents=True, exist_ok=True)
    if original:
        backup = args.settings.with_name(f"{args.settings.name}.bak-{time.strftime('%Y%m%d-%H%M%S')}")
        shutil.copy2(args.settings, backup)
        print(f"backup: {backup}")
    args.settings.write_text(text, encoding="utf-8")
    print(f"{'removed companion hooks from' if args.uninstall else 'installed companion hooks in'} {args.settings}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
