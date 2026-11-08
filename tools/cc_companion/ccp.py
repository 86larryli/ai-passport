"""CCP/1 encoder and decoder for the Claude Code Companion (bridge side).

Wire format: UTF-8 lines ending in "\\n", fields separated by "\\t". Text fields
never contain tabs, newlines, or other control characters; a line break inside
a multi-line field (the attention body) is encoded as 0x1F. The authoritative
description is docs/claude-code-companion/protocol.md.

The byte limits mirror main/cc_types.h (they include the C terminating NUL, so
the longest text the bridge sends is limit - 1 bytes). tests/test_cc_companion.py
parses cc_types.h and fails when the two drift apart.
"""

from __future__ import annotations

from dataclasses import dataclass, field

PROTO_VERSION = 1
SERVICE_UUID = "9aa10001-aa98-4d97-a56d-233f9c782706"
RX_UUID = "9aa10002-aa98-4d97-a56d-233f9c782706"   # bridge -> device (write)
TX_UUID = "9aa10003-aa98-4d97-a56d-233f9c782706"   # device -> bridge (notify)
DEVICE_NAME_PREFIX = "CC-Companion"

MAX_SESSIONS = 6
MAX_AGENTS = 5
LINE_MAX = 640

# Mirrors the CC_*_MAX macros in main/cc_types.h (bytes including NUL).
LIMITS = {
    "SID": 12,
    "PROJECT": 48,
    "ACTIVITY": 128,
    "NOTE": 128,
    "AGENT_TYPE": 24,
    "AGENT_ACTIVITY": 96,
    "Q_TITLE": 64,
    "Q_BODY": 512,
    "EVT_TEXT": 96,
    "VERSION": 24,
}

SESSION_STATES = ("work", "perm", "ask", "done", "idle", "err", "bg")
AGENT_STATES = ("run", "done", "err")
EVENT_KINDS = ("perm", "ask", "done", "err")

LINE_BREAK = "\x1f"
ELLIPSIS = "..."


def clean(text: object, limit: str, multiline: bool = False) -> str:
    """Sanitize text for one field and truncate it to LIMITS[limit] - 1 bytes."""
    max_bytes = LIMITS[limit] - 1
    value = "" if text is None else str(text)
    value = value.replace("\r\n", "\n").replace("\r", "\n")
    out = []
    for ch in value:
        code = ord(ch)
        if ch == "\n":
            out.append(LINE_BREAK if multiline else " ")
        elif code < 0x20 or code == 0x7F or 0x80 <= code < 0xA0:
            out.append(" ")
        elif 0xD800 <= code <= 0xDFFF:
            out.append("?")
        else:
            out.append(ch)
    value = "".join(out)
    if not multiline:
        value = " ".join(value.split())
    else:
        value = value.strip(" " + LINE_BREAK)
    return truncate_utf8(value, max_bytes)


def truncate_utf8(value: str, max_bytes: int) -> str:
    data = value.encode("utf-8")
    if len(data) <= max_bytes:
        return value
    budget = max_bytes - len(ELLIPSIS)
    cut = data[:budget]
    # Drop a partial trailing character.
    return cut.decode("utf-8", errors="ignore").rstrip() + ELLIPSIS


@dataclass
class AgentView:
    aid: str
    state: str
    age_s: int
    type: str
    activity: str


@dataclass
class SessionView:
    sid: str
    state: str
    age_s: int
    project: str
    activity: str
    note: str = ""
    agents_total: int = 0
    agents_running: int = 0
    agents: list[AgentView] = field(default_factory=list)
    q_kind: str = ""          # "", "perm" or "ask"
    q_title: str = ""
    q_body: str = ""


def _line(*fields: object) -> str:
    text = "\t".join(str(f) for f in fields) + "\n"
    if len(text.encode("utf-8")) > LINE_MAX:
        raise ValueError(f"line exceeds {LINE_MAX} bytes: {fields[0]}")
    return text


def hello(bridge_version: str) -> str:
    return _line("HELLO", PROTO_VERSION, clean(bridge_version, "VERSION"))


def clock(unix_s: float, utc_offset_min: int) -> str:
    return _line("CLOCK", int(unix_s), int(utc_offset_min))


def ping() -> str:
    return _line("PING")


def event(kind: str, sid: str, project: str, text: str) -> str:
    if kind not in EVENT_KINDS:
        raise ValueError(kind)
    return _line("EVT", kind, clean(sid, "SID"), clean(project, "PROJECT"), clean(text, "EVT_TEXT"))


def snapshot(seq: int, sessions: list[SessionView], total: int) -> list[str]:
    lines = [_line("SNAP", seq, total)]
    for s in sessions[:MAX_SESSIONS]:
        if s.state not in SESSION_STATES:
            raise ValueError(s.state)
        lines.append(_line("S", clean(s.sid, "SID"), s.state, max(0, int(s.age_s)),
                           clean(s.project, "PROJECT"), clean(s.activity, "ACTIVITY"),
                           s.agents_total, s.agents_running))
        if s.note:
            lines.append(_line("M", clean(s.note, "NOTE")))
        if s.q_kind:
            lines.append(_line("Q", s.q_kind, clean(s.q_title, "Q_TITLE"),
                               clean(s.q_body, "Q_BODY", multiline=True)))
        for a in s.agents[:MAX_AGENTS]:
            if a.state not in AGENT_STATES:
                raise ValueError(a.state)
            lines.append(_line("A", clean(a.aid, "SID"), a.state, max(0, int(a.age_s)),
                               clean(a.type, "AGENT_TYPE"), clean(a.activity, "AGENT_ACTIVITY")))
    lines.append(_line("END", seq))
    return lines


@dataclass
class DeviceMessage:
    kind: str                 # "HI", "ACK", "ERR", or "UNKNOWN"
    fields: list[str]


def parse_device_line(line: str) -> DeviceMessage:
    parts = line.rstrip("\r\n").split("\t")
    kind = parts[0] if parts[0] in ("HI", "ACK", "ERR") else "UNKNOWN"
    return DeviceMessage(kind, parts[1:])
