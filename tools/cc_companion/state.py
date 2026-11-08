"""Aggregate Claude Code hook events into per-session state for the device.

Each Claude Code session reports its lifecycle through hooks (see
docs/claude-code-companion/README.md). Hooks run asynchronously, so two events
of the same session can arrive out of order; every event carries the time the
hook started (`ts`) and a state change only applies when it is not older than
the change it would replace.
"""

from __future__ import annotations

import logging
import os
import re
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable
from urllib.parse import urlsplit

from ccp import LIMITS, MAX_AGENTS, AgentView, SessionView, truncate_utf8

log = logging.getLogger("cc_companion.state")

# Sort order on the device: what needs the user first.
PRIORITY = {"perm": 0, "ask": 0, "err": 1, "work": 2, "bg": 2, "done": 3, "idle": 4}


@dataclass
class Alert:
    kind: str       # perm / ask / done / err
    sid: str
    project: str
    text: str


@dataclass
class Agent:
    agent_id: str
    type: str
    state: str = "run"
    activity: str = "Starting"
    since: float = 0.0
    ts: float = 0.0


@dataclass
class BgTask:
    """A background shell, monitor, workflow, ... that outlives the main turn."""
    task_id: str
    kind: str
    label: str
    since: float


# Statuses that mean a background task is over (Claude Code does not enumerate them).
TERMINAL_STATUSES = {"completed", "complete", "done", "finished", "failed", "error", "killed",
                     "stopped", "cancelled", "canceled", "exited", "terminated"}
KIND_NAMES = {"subagent": ("subagent", "subagents"), "shell": ("shell", "shells"),
              "monitor": ("monitor", "monitors"), "workflow": ("workflow", "workflows"),
              "teammate": ("teammate", "teammates")}
KIND_ORDER = ["subagent", "shell", "monitor", "workflow", "teammate"]


@dataclass
class Session:
    session_id: str
    cwd: str
    started: float
    state: str = "idle"
    state_since: float = 0.0
    state_ts: float = 0.0
    activity: str = "Session started"
    activity_ts: float = 0.0
    note: str = ""
    last_reply: str = ""          # Claude's latest reply (the note may be a prompt)
    attention: tuple[str, str, str] | None = None   # (kind, title, body)
    turn_started: float = 0.0
    last_seen: float = 0.0
    agents: dict[str, Agent] = field(default_factory=dict)
    background: dict[str, BgTask] = field(default_factory=dict)
    bg_subagents: int = 0         # subagent entries in the last Stop's background_tasks
    # Claude Code lists Monitor-tool tasks as type "shell"; remember what the Monitor
    # calls looked like so their entries can be labeled as monitors.
    monitor_keys: deque = field(default_factory=lambda: deque(maxlen=16))
    pending_descriptions: deque = field(default_factory=lambda: deque(maxlen=16))

    @property
    def sid(self) -> str:
        return self.session_id[:8]

    @property
    def project(self) -> str:
        name = os.path.basename(self.cwd.rstrip("/")) if self.cwd else ""
        return name or "session"


def first_line(text: object, limit: int = 300) -> str:
    for line in str(text or "").splitlines():
        line = line.strip()
        if line:
            return line[:limit]
    return ""


def _is_system_prompt(prompt: str) -> bool:
    text = prompt.lstrip()
    return (text.startswith("<") or text.startswith("[SYSTEM") or "task-notification" in text
            or text.startswith("Another Claude session sent a message"))


def _rel(path: object, cwd: str) -> str:
    p = str(path or "")
    if not p:
        return ""
    if cwd and p.startswith(cwd.rstrip("/") + "/"):
        return p[len(cwd.rstrip("/")) + 1:]
    home = os.path.expanduser("~")
    if p.startswith(home + "/"):
        return "~/" + p[len(home) + 1:]
    return p


def _shorten_paths(text: str, cwd: str) -> str:
    """Drop the project directory and abbreviate the home directory inside a command."""
    if cwd:
        base = cwd.rstrip("/")
        text = text.replace(base + "/", "")
        # The bare directory only where it stands alone (not /w/proj inside /w/projector).
        text = re.sub(re.escape(base) + r"(?=$|[\s'\";|&)])", ".", text)
    home = os.path.expanduser("~")
    return text.replace(home + "/", "~/")


def summarize(tool: str, ti: dict, cwd: str) -> str:
    """One short line describing a tool call, in the spirit of Claude Code's own UI."""
    ti = ti if isinstance(ti, dict) else {}
    if tool == "Bash":
        return "$ " + _shorten_paths(first_line(ti.get("command"), 300), cwd)[:200]
    if tool in ("Read", "Write", "Edit", "MultiEdit"):
        verb = "Read" if tool == "Read" else ("Write" if tool == "Write" else "Edit")
        return f"{verb} {_rel(ti.get('file_path'), cwd)}".strip()
    if tool == "NotebookEdit":
        return f"Edit {_rel(ti.get('notebook_path'), cwd)}".strip()
    if tool == "Grep":
        where = _rel(ti.get("path"), cwd)
        return f'Grep "{ti.get("pattern", "")}"' + (f" in {where}" if where else "")
    if tool == "Glob":
        return f"Glob {ti.get('pattern', '')}".strip()
    if tool == "WebFetch":
        parts = urlsplit(str(ti.get("url", "")))
        return f"Fetch {parts.netloc}{parts.path}".strip()
    if tool == "WebSearch":
        return f'Search "{ti.get("query", "")}"'
    if tool in ("Task", "Agent"):
        return "Agent: " + (first_line(ti.get("description")) or str(ti.get("subagent_type") or "task"))
    if tool == "TodoWrite":
        return "Update todos"
    if tool == "AskUserQuestion":
        return "Asking you a question"
    if tool == "ExitPlanMode":
        return "Plan ready for review"
    if tool == "SubagentHandback":
        return "Reporting back"
    if tool == "Monitor":
        return "Monitor: " + (first_line(ti.get("description")) or _shorten_paths(first_line(ti.get("command")), cwd))
    if tool == "Skill":
        return f"Skill {ti.get('skill') or ti.get('name') or ''}".strip()
    if tool.startswith("mcp__"):
        parts = tool.split("__")
        if len(parts) >= 3:
            return f"{parts[1]}: {'__'.join(parts[2:])}"
    return tool or "tool"


def permission_detail(tool: str, ti: dict, cwd: str) -> str:
    ti = ti if isinstance(ti, dict) else {}
    if tool == "Bash":
        command = str(ti.get("command", "")).strip()
        description = first_line(ti.get("description"))
        return command + (f"\n# {description}" if description else "")
    if tool in ("Write", "Edit", "MultiEdit", "Read"):
        return _rel(ti.get("file_path"), cwd)
    if tool == "NotebookEdit":
        return _rel(ti.get("notebook_path"), cwd)
    if tool == "WebFetch":
        return str(ti.get("url", ""))
    return summarize(tool, ti, cwd)


OPTION_MAX_BYTES = 72        # each answer option keeps at least its first ~24 CJK characters
QUESTION_MIN_BYTES = 120     # the question itself never shrinks below this


def question_detail(ti: dict) -> tuple[str, str]:
    """Title and body for an AskUserQuestion prompt, sized for the device.

    The body limit (LIMITS["Q_BODY"]) is shared by the question and its options.
    Cutting from the end would drop the options, which are what the user needs to
    see, so each option is capped first and the question gets the remaining room.
    """
    questions = ti.get("questions") if isinstance(ti, dict) else None
    if not isinstance(questions, list) or not questions or not isinstance(questions[0], dict):
        return "Question", "Claude is asking you a question"
    q = questions[0]
    title = str(q.get("header") or "Question")
    budget = LIMITS["Q_BODY"] - 1
    options = []
    for i, option in enumerate(q.get("options") or [], 1):
        label = option.get("label") if isinstance(option, dict) else option
        options.append(truncate_utf8(" ".join(f"{i}. {label}".split()), OPTION_MAX_BYTES))
    tail = [f"(+{len(questions) - 1} more)"] if len(questions) > 1 else []
    # Drop trailing options (with a count) only if even capped options cannot fit.
    while options and _joined_bytes(options + tail) > budget - QUESTION_MIN_BYTES:
        options.pop()
        tail = [f"(+{len(q.get('options') or []) - len(options)} options)"] + tail[-1:]
    room = budget - _joined_bytes(options + tail) - (1 if options or tail else 0)
    question = truncate_utf8(" ".join(str(q.get("question") or "").split()), max(room, 0))
    return title, "\n".join(line for line in [question, *options, *tail] if line)


def _joined_bytes(lines: list[str]) -> int:
    return sum(len(line.encode("utf-8")) for line in lines) + max(len(lines) - 1, 0)


class SessionTracker:
    def __init__(self, done_alert_min_s: float = 10.0, stale_s: float = 8 * 3600,
                 clock: Callable[[], float] = time.time):
        self.sessions: dict[str, Session] = {}
        self.done_alert_min_s = done_alert_min_s
        self.stale_s = stale_s
        self.clock = clock
        self.version = 0

    # -- helpers --------------------------------------------------------------
    def _changed(self) -> None:
        self.version += 1

    @staticmethod
    def _set_state(s: Session, state: str, ts: float) -> bool:
        if ts < s.state_ts:
            return False
        if s.state != state:
            log.info("%s %s: %s -> %s", s.sid, s.project, s.state, state)
            s.state_since = ts
        s.state = state
        s.state_ts = ts
        return True

    @staticmethod
    def _set_activity(s: Session, text: str, ts: float) -> None:
        if ts >= s.activity_ts:
            s.activity = text
            s.activity_ts = ts

    def _agent(self, s: Session, ev: dict, ts: float) -> Agent | None:
        aid = ev.get("agent_id")
        if not aid:
            return None
        agent = s.agents.get(aid)
        if agent is None:
            agent = Agent(str(aid), str(ev.get("agent_type") or "agent"), since=ts, ts=ts)
            s.agents[aid] = agent
        return agent

    # -- public API -------------------------------------------------------------
    def apply(self, ev: dict) -> list[Alert]:
        name = ev.get("hook_event_name")
        session_id = ev.get("session_id")
        if not name or not session_id:
            return []
        ts = float(ev.get("ts") or self.clock())
        if name == "SessionEnd":
            if self.sessions.pop(session_id, None) is not None:
                self._changed()
            return []

        s = self.sessions.get(session_id)
        if s is None:
            s = Session(session_id, str(ev.get("cwd") or ""), started=ts, state_since=ts,
                        state_ts=0.0, activity_ts=0.0, last_seen=ts)
            if name not in ("SessionStart", "UserPromptSubmit"):
                # First seen mid-turn (e.g. the bridge restarted): time the job from here
                # so its finished alert is not lost.
                s.turn_started = ts
            self.sessions[session_id] = s
        elif ev.get("cwd"):
            s.cwd = str(ev["cwd"])
        s.last_seen = max(s.last_seen, ts)
        alerts: list[Alert] = []
        handler = getattr(self, "_on_" + name, None)
        if handler is not None:
            handler(s, ev, ts, alerts)
        self._changed()
        return alerts

    def _on_SessionStart(self, s, ev, ts, alerts):
        source = ev.get("source")
        if source == "clear":
            s.agents.clear()
            s.note = ""
            s.attention = None
        if source in ("startup", "clear") or s.state_ts == 0.0:
            self._set_state(s, "idle", ts)
            self._set_activity(s, "Resumed" if source == "resume" else "Session started", ts)

    def _on_UserPromptSubmit(self, s, ev, ts, alerts):
        previous = s.state
        if self._set_state(s, "work", ts):
            # A turn that continues background work (Claude Code re-invokes the
            # session when a task finishes) keeps the chain's start time, so the
            # finished alert measures the whole job.
            if previous not in ("bg", "work", "perm", "ask") or not s.turn_started:
                s.turn_started = ts
            s.attention = None
            s.agents = {k: a for k, a in s.agents.items() if a.state == "run"}
            prompt = first_line(ev.get("prompt"))
            # Turns Claude Code starts itself (a background task finished) carry a
            # notification as the prompt; keep the user's last real prompt instead.
            if prompt and not _is_system_prompt(prompt):
                s.note = "> " + prompt
        self._set_activity(s, "Thinking", ts)

    def _on_PreToolUse(self, s, ev, ts, alerts):
        tool = str(ev.get("tool_name") or "")
        ti = ev.get("tool_input") if isinstance(ev.get("tool_input"), dict) else {}
        if tool in ("Task", "Agent"):
            s.pending_descriptions.append((str(ti.get("subagent_type") or ""),
                                           first_line(ti.get("description"))))
        summary = summarize(tool, ti, s.cwd)
        agent = self._agent(s, ev, ts)
        if tool == "Monitor":
            for key in (ti.get("description"), ti.get("command")):
                if key:
                    s.monitor_keys.append(first_line(key))
        if (tool == "Bash" and ti.get("run_in_background")) or tool == "Monitor":
            # Shown right away; the next Stop's background_tasks replaces these.
            key = "tu:" + str(ev.get("tool_use_id") or len(s.background))
            s.background[key] = BgTask(key, "shell" if tool == "Bash" else "monitor",
                                       summary[2:] if tool == "Bash" else summary[9:], ts)
        if agent is not None and ts >= agent.ts:
            agent.activity = summary
            agent.ts = ts
        elif agent is None:
            self._set_activity(s, summary, ts)
        if tool == "AskUserQuestion":
            if self._set_state(s, "ask", ts):
                title, body = question_detail(ti)
                s.attention = ("ask", title, body)
                alerts.append(Alert("ask", s.sid, s.project, body.split("\n", 1)[0] or title))
        elif agent is None and s.state not in ("perm", "ask"):
            # Only the main agent moves the session; a background subagent may
            # keep running after the main turn has finished.
            self._set_state(s, "work", ts)

    def _on_PermissionRequest(self, s, ev, ts, alerts):
        tool = str(ev.get("tool_name") or "tool")
        ti = ev.get("tool_input") if isinstance(ev.get("tool_input"), dict) else {}
        if tool == "AskUserQuestion":
            return          # the question itself is the prompt; PreToolUse already showed it
        agent = self._agent(s, ev, ts)
        title = f"{agent.type}: {tool}" if agent is not None else tool
        if self._set_state(s, "perm", ts):
            s.attention = ("perm", title, permission_detail(tool, ti, s.cwd))
            alerts.append(Alert("perm", s.sid, s.project, f"{tool}: {summarize(tool, ti, s.cwd)}"))

    def _tool_finished(self, s, ev, ts):
        agent = self._agent(s, ev, ts)
        if agent is not None and ts >= agent.ts:
            agent.ts = ts
        if s.state in ("perm", "ask") and ts > s.state_ts:
            self._set_state(s, "work", ts)
            s.attention = None

    def _on_PostToolUse(self, s, ev, ts, alerts):
        self._tool_finished(s, ev, ts)

    def _on_PostToolUseFailure(self, s, ev, ts, alerts):
        self._tool_finished(s, ev, ts)

    def _on_PermissionDenied(self, s, ev, ts, alerts):
        tool = str(ev.get("tool_name") or "tool")
        self._set_activity(s, f"Denied: {summarize(tool, ev.get('tool_input') or {}, s.cwd)}", ts)

    def _on_Notification(self, s, ev, ts, alerts):
        kind = ev.get("notification_type")
        message = first_line(ev.get("message")) or "Claude needs your attention"
        # While a question is open, Claude Code's prompt notification belongs to it.
        if kind == "permission_prompt" and s.state not in ("perm", "ask"):
            if self._set_state(s, "perm", ts):
                s.attention = s.attention or ("perm", str(ev.get("title") or "Permission"), message)
                alerts.append(Alert("perm", s.sid, s.project, message))
        elif kind in ("elicitation_dialog", "agent_needs_input") and s.state != "ask":
            if self._set_state(s, "ask", ts):
                s.attention = ("ask", str(ev.get("title") or "Input needed"), message)
                alerts.append(Alert("ask", s.sid, s.project, message))

    def _on_SubagentStart(self, s, ev, ts, alerts):
        agent = self._agent(s, ev, ts)
        if agent is None:
            return
        agent.type = str(ev.get("agent_type") or agent.type)
        description = ""
        for i, (kind, text) in enumerate(s.pending_descriptions):
            if not kind or kind == agent.type:
                description = text
                del s.pending_descriptions[i]
                break
        agent.state = "run"
        agent.activity = description or "Starting"
        agent.since = ts
        agent.ts = ts

    def _on_SubagentStop(self, s, ev, ts, alerts):
        agent = self._agent(s, ev, ts)
        if agent is None:
            return
        agent.state = "done"
        agent.activity = first_line(ev.get("last_assistant_message")) or "Done"
        agent.since = ts
        agent.ts = max(agent.ts, ts)
        self._update_background(s, ev, ts)     # SubagentStop also lists the session's tasks
        if s.state == "bg":
            self._settle_background(s, ev, ts, alerts)

    def _on_Stop(self, s, ev, ts, alerts):
        if ts < s.state_ts:
            return
        self._update_background(s, ev, ts)
        s.attention = None
        message = first_line(ev.get("last_assistant_message"))
        if message:
            s.note = message
            s.last_reply = message
        self._settle_background(s, ev, ts, alerts)

    def _update_background(self, s, ev, ts):
        """Replace background rows with the Stop hook's authoritative list."""
        tasks = ev.get("background_tasks")
        if not isinstance(tasks, list):
            return                      # older Claude Code: keep what PreToolUse added
        fresh: dict[str, BgTask] = {}
        subagents = 0
        for task in tasks:
            if not isinstance(task, dict):
                continue
            if str(task.get("status") or "").lower() in TERMINAL_STATUSES:
                continue
            kind = str(task.get("type") or "task").lower()
            if kind == "shell" and (first_line(task.get("description")) in s.monitor_keys
                                    or first_line(task.get("command")) in s.monitor_keys):
                kind = "monitor"
            if kind == "subagent":
                subagents += 1          # rows come from SubagentStart/SubagentStop
                continue
            task_id = str(task.get("id") or f"bg{len(fresh)}")
            server = task.get("server")
            # A description ("Run Gemini reviewer lane") reads better than the raw command.
            label = (task.get("description") or task.get("command") or task.get("name")
                     or (f"{server}: {task.get('tool') or ''}" if server else kind))
            previous = s.background.get(task_id)
            fresh[task_id] = BgTask(task_id, kind, _shorten_paths(first_line(label), s.cwd),
                                    previous.since if previous else ts)
        s.background = fresh
        s.bg_subagents = subagents

    @staticmethod
    def background_summary(s: Session) -> str:
        counts: dict[str, int] = {}
        running_agents = sum(1 for a in s.agents.values() if a.state == "run")
        if max(running_agents, s.bg_subagents):
            counts["subagent"] = max(running_agents, s.bg_subagents)
        for task in s.background.values():
            counts[task.kind] = counts.get(task.kind, 0) + 1
        parts = []
        for kind in sorted(counts, key=lambda k: (KIND_ORDER.index(k) if k in KIND_ORDER else 99, k)):
            singular, plural = KIND_NAMES.get(kind, (kind, kind + "s"))
            parts.append(f"{counts[kind]} {singular if counts[kind] == 1 else plural}")
        return (", ".join(parts) + " running") if parts else ""

    def _settle_background(self, s, ev, ts, alerts):
        """After the main turn ended: 'bg' while anything runs, otherwise 'done'."""
        summary = self.background_summary(s)
        if summary:
            self._set_state(s, "bg", ts)
            s.activity = summary
            s.activity_ts = ts
            return
        if not self._set_state(s, "done", ts):
            return
        self._set_activity(s, "Waiting for your next prompt", ts)
        if s.turn_started and ts - s.turn_started >= self.done_alert_min_s:
            alerts.append(Alert("done", s.sid, s.project, s.last_reply or "Finished"))
        s.turn_started = 0.0

    def _on_StopFailure(self, s, ev, ts, alerts):
        if not self._set_state(s, "err", ts):
            return
        s.attention = None
        error = (first_line(ev.get("error_type")) or first_line(ev.get("error"))
                 or first_line(ev.get("error_details")) or "API error")
        self._set_activity(s, f"Stopped: {error}", ts)
        alerts.append(Alert("err", s.sid, s.project, error))

    def prune(self, now: float | None = None) -> None:
        now = self.clock() if now is None else now
        stale = [k for k, s in self.sessions.items() if now - s.last_seen > self.stale_s]
        for key in stale:
            del self.sessions[key]
        if stale:
            self._changed()

    def views(self, now: float | None = None) -> tuple[list[SessionView], int]:
        now = self.clock() if now is None else now
        sessions = list(self.sessions.values())

        def order(s: Session):
            prio = PRIORITY.get(s.state, 5)
            # Waiting sessions oldest first (FIFO); the rest most recent first.
            return (prio, s.state_since if prio == 0 else -s.last_seen)

        sessions.sort(key=order)
        labels = _labels(sessions)
        out = []
        for s in sessions:
            running = [a for a in s.agents.values() if a.state == "run"]
            finished = [a for a in s.agents.values() if a.state != "run"]
            rows = [AgentView(a.agent_id[:8], a.state, int(max(0, now - a.since)), a.type, a.activity)
                    for a in sorted(running, key=lambda a: -a.since)]
            rows += [AgentView(t.task_id[-8:], "run", int(max(0, now - t.since)), t.kind, t.label)
                     for t in sorted(s.background.values(), key=lambda t: -t.since)]
            rows += [AgentView(a.agent_id[:8], a.state, int(max(0, now - a.since)), a.type, a.activity)
                     for a in sorted(finished, key=lambda a: -a.since)]
            view = SessionView(
                sid=s.sid, state=s.state, age_s=int(max(0, now - s.state_since)),
                project=labels[s.session_id], activity=s.activity, note=s.note,
                agents_total=len(s.agents) + len(s.background),
                agents_running=len(running) + len(s.background),
                agents=rows[:MAX_AGENTS],
            )
            if s.attention and s.state in ("perm", "ask"):
                view.q_kind, view.q_title, view.q_body = s.attention
            out.append(view)
        return out, len(sessions)


def _labels(sessions: list[Session]) -> dict[str, str]:
    """Project labels, numbering sessions that share a directory name by start time."""
    by_name: dict[str, list[Session]] = {}
    for s in sessions:
        by_name.setdefault(s.project, []).append(s)
    labels = {}
    for name, group in by_name.items():
        if len(group) == 1:
            labels[group[0].session_id] = name
            continue
        for i, s in enumerate(sorted(group, key=lambda x: x.started), 1):
            labels[s.session_id] = f"{name} #{i}"
    return labels
