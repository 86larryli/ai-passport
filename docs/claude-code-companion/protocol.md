<p align="right">
  <a href="protocol.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Claude Code Companion Protocol (CCP/1)

The bridge (`tools/cc_companion`) and the firmware (`main/ccp.c`) exchange UTF-8
text lines over one BLE GATT service. This document is the contract; the
golden transcript `tests/fixtures/ccp_scenario.txt` is produced by the Python
encoder and parsed by the C host test, and `tests/test_cc_companion.py` checks
that the byte limits and UUIDs below match `main/cc_types.h` and `main/cc_ble.c`.

## Transport

| Item | Value |
| --- | --- |
| Service | `9aa10001-aa98-4d97-a56d-233f9c782706` |
| RX (bridge to device) | `9aa10002-aa98-4d97-a56d-233f9c782706`, write and write-without-response |
| TX (device to bridge) | `9aa10003-aa98-4d97-a56d-233f9c782706`, notify |
| Advertising name | `CC-Companion-XXXX` (last two bytes of the BT MAC), in the scan response |
| Security | LE Secure Connections only, MITM (passkey displayed by the device), bonding; RX, TX, and the TX CCCD require an authenticated link |
| Preferred ATT MTU | 247 |

The bridge writes with response, in chunks of at most `MTU - 3` bytes. Chunks
split lines anywhere; the device reassembles lines. The device rejects a write
with an ATT error when its 4 KB receive buffer is full, and the bridge resends
the next snapshot.

## Line format

- One record per line, terminated by `\n` (a trailing `\r` is ignored).
- Fields are separated by `\t`. The first field is the record type.
- Text fields never contain tabs, newlines, or other C0/C1 control characters.
  A line break inside a multi-line field (only `Q.body`) is sent as `0x1F`.
- Integers are unsigned decimal unless noted.
- The longest line is 640 bytes. Longer lines are dropped whole.
- Unknown record types are ignored, and extra trailing fields are ignored, so
  newer bridges can add fields and records.
- Text is truncated by the sender to the limits below (bytes, at a character
  boundary, with `...` appended). The device enforces the same limits.

| Field | Max bytes |
| --- | ---: |
| session/agent id | 11 |
| project | 47 |
| activity, note | 127 |
| agent type | 23 |
| agent activity | 95 |
| question title | 63 |
| question body | 511 |
| event text | 95 |
| bridge version | 23 |

## Bridge to device

| Record | Fields | Meaning |
| --- | --- | --- |
| `HELLO` | protocol (`1`), bridge version | Sent only after the TX subscription succeeded; the device treats it as proof of the subscription and answers `HI`. |
| `CLOCK` | Unix seconds, UTC offset in minutes (-1440..1440) | Wall clock for the top bar. Sent on connect and every 30 s. |
| `SNAP` | seq, total sessions | Starts a snapshot. A new `SNAP` discards an unfinished one. |
| `S` | sid, state, age s, project, activity, agents total, agents running | One session. Rows `M`, `Q`, `A` that follow belong to it. |
| `M` | text | Latest prompt (`> ...`) or the last assistant reply for the current session. |
| `Q` | kind (`perm` or `ask`), title, body | What the session waits for: tool name and command, or question header, question, and numbered options. For questions the bridge caps each option and shortens the question first, so every option stays visible. |
| `A` | aid, state, age s, type, activity | One subagent or background task (type `shell`, `monitor`, `workflow`, ...) of the current session; activity is the command or description. |
| `END` | seq | Commits the snapshot when seq matches `SNAP`; otherwise it is discarded. |
| `EVT` | kind, sid, project, text | One-shot alert: `perm`, `ask`, `done`, `err`. Sent just before the snapshot that reflects it. |
| `PING` | — | Keepalive when nothing else was sent for 5 s. |

Session states: `work`, `perm`, `ask`, `done`, `idle`, `err`, `bg` (the main turn ended but background shells, subagents, monitors, or workflows still run). Agent states:
`run`, `done`, `err`. Ages are seconds in the current state when sent; the
device keeps counting from the moment the snapshot arrived.

The device keeps the first 6 sessions of a snapshot and the first 5 subagents of
each session; the bridge sends them in display order (waiting sessions first,
oldest waiting first; then errors; then working, finished, and idle sessions,
most recent first). Counts in `SNAP` and `S` include the rest.

A snapshot is resent when anything changes (debounced by 150 ms) and at least
every 30 s. The device shows *bridge quiet* after 20 s without any line.

## Device to bridge

| Record | Fields | Meaning |
| --- | --- | --- |
| `HI` | protocol, firmware version, battery percent (-1 unknown) | Reply to `HELLO`. |
| `ACK` | seq | Snapshot committed. |
| `ERR` | reason | Malformed line (rate-limited to one per second). |

## Example

Tabs are shown as `→` and `0x1F` as `⏎`:

```text
HELLO→1→0.1.0
CLOCK→1794095280→-480
EVT→perm→aaaaaaaa→ai-passport→Bash: $ rm -rf build/
SNAP→7→2
S→aaaaaaaa→perm→12→ai-passport→$ idf.py build→0→0
M→> Build the firmware
Q→perm→Bash→rm -rf build/⏎# Clean
S→bbbbbbbb→work→40→cheapbite→Agent: find usages→2→1
A→agent-ex→run→38→Explore→Grep "assay" in data
A→agent-pl→done→30→Plan→Plan ready
END→7
PING
```

## Hook events used by the bridge

| Hook | Effect |
| --- | --- |
| `SessionStart` | Create the session (`idle`). |
| `UserPromptSubmit` | `work`; note becomes `> prompt`; finished subagents are cleared. |
| `PreToolUse` | Activity line (main agent or subagent). `AskUserQuestion` sets `ask` and raises an alert. `Task`/`Agent` calls remember the subagent description. |
| `PermissionRequest` | `perm` with tool and command; alert. |
| `PostToolUse`, `PostToolUseFailure` | Leave `perm`/`ask` back to `work`. |
| `Notification` | `permission_prompt` sets `perm`; `elicitation_dialog` and `agent_needs_input` set `ask`. |
| `SubagentStart`, `SubagentStop` | Add a subagent row; mark it done with its last reply. A `bg` session with nothing left running becomes `done`. |
| `Stop` | Its `background_tasks` list replaces the background rows. With anything still running the session becomes `bg` ("2 shells, 1 subagent running"); otherwise `done`, with an alert when the whole job, including turns that continued background work, took at least 10 s. |
| `StopFailure` | `err`; alert. |
| `SessionEnd` | Remove the session. Sessions silent for 8 h are also removed. |

Hooks run asynchronously and may arrive out of order. Each event carries the
time its hook started, and a state change only applies when it is not older
than the change it would replace.
