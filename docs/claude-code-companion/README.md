<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Claude Code Companion

A desk and pocket status display for the Claude Code CLI. The AI Passport shows
every Claude Code session running on your Mac: what the main agent is doing,
how its subagents are doing, which session waits for a permission decision or
an answer, and which one just finished or failed. Up and Down switch sessions;
OK opens the details.

The device only displays and alerts. You still approve tools and answer
questions in the terminal; nothing on the device can run anything on the Mac.

## How it works

```text
Claude Code sessions --hooks--> hook.py --HTTP 127.0.0.1--> bridge.py --BLE--> AI Passport
   (any terminal)      (async, never blocks)          (aggregates sessions)   (screens, chimes)
```

1. **Status source: Claude Code hooks.** Every session fires hooks at its
   lifecycle points (`SessionStart`, `UserPromptSubmit`, `PreToolUse`,
   `PermissionRequest`, `PostToolUse`, `SubagentStart`, `SubagentStop`,
   `Notification`, `Stop`, `StopFailure`, `SessionEnd`, ...). Each hook gets
   JSON with the `session_id`, `cwd`, and event details. The companion installs
   one asynchronous command hook per event, so Claude never waits for it (only
   `SessionEnd` is synchronous, bounded by a 0.5 s timeout).
2. **`tools/cc_companion/hook.py`** trims the JSON (tool output is dropped,
   strings are capped, the hook start time is added) and posts it to the local
   bridge. It always exits 0, even when the bridge is not running.
3. **`tools/cc_companion/bridge.py`** keeps one record per session, orders
   them (needs-you first), and sends complete snapshots to the device over BLE,
   plus one-shot alerts for approvals, questions, finished turns, and errors.
4. **The firmware** stores the latest snapshot, renders it, and chimes.

The wire format is documented in [protocol.md](protocol.md).

### Why Bluetooth LE

The ESP32-C3 offers 2.4 GHz Wi-Fi and Bluetooth LE (no Classic Bluetooth). BLE
needs no Wi-Fi credentials on the device, works on any network (hotel, travel
router, phone tethering), and uses less power. The link requires LE Secure
Connections with a six-digit passkey shown on the device, so traffic is
encrypted and authenticated; once a Mac is bonded, another computer can pair
only after you open the pairing window on the device.

## Set up

Requirements: macOS with Bluetooth, Python 3.9 or newer, and
[uv](https://docs.astral.sh/uv/) for the bridge's single dependency (`bleak`).

1. **Flash the firmware** (see [Build and flash](#build-and-flash)) and turn the
   device on. It shows *Waiting for your Mac* and its name, `CC-Companion-XXXX`.
2. **Start the bridge** in a terminal and leave it running:

   ```bash
   uv run --no-project --with bleak==3.1.0 python tools/cc_companion/bridge.py
   ```

   The first time, macOS asks the terminal app for Bluetooth permission, then
   shows a pairing dialog. Type the six-digit code shown on the device.
   `--scan` lists nearby devices; `--device CC-Companion-XXXX` pins one.
3. **Install the hooks** (user settings, all projects). Preview first:

   ```bash
   python3 tools/cc_companion/install_hooks.py --dry-run
   python3 tools/cc_companion/install_hooks.py
   ```

   The installer backs up `~/.claude/settings.json`, adds one async hook per
   event (`SessionEnd` runs synchronously, at most 0.5 s, so a closed session
   leaves the device), and leaves your other hooks untouched. Remove them later with
   `--uninstall`. Hooks apply to sessions started after the change.
4. Start `claude` anywhere. The session appears on the device.

To try the screens without Claude, run the bridge and replay a scripted set of
sessions (approval, question, Chinese text, subagents, an error):

```bash
python3 tools/cc_companion/simulate.py --speed 2
```

`GET http://127.0.0.1:47820/status` shows what the bridge is tracking; send the
token as `Authorization: Bearer $(cat ~/.config/cc-companion/token)`. `--dry-run`
prints the protocol lines instead of using BLE.

## Security

| Path | Protection | What remains |
| --- | --- | --- |
| Mac to device over the air | LE Secure Connections only, passkey shown on the device (MITM protection), bonding, AES-CCM link encryption. RX, TX, and the TX subscription require an authenticated link. After the first Mac is bonded, new pairing is refused unless *Settings > Pair new Mac* is open. | None known for nearby attackers who cannot see the passkey. |
| Other apps on the Mac | The bond belongs to macOS, not to the bridge, so any app with Bluetooth permission can use the bonded link (the same caveat Android documents for BLE). CoreBluetooth does not let one app observe another app's writes, and the device exposes no readable session data; its notifications carry only firmware version, battery, ACKs, and errors. | Such an app could write fake protocol lines and show misleading status or alerts. The device cannot approve or run anything. |
| Hooks to bridge (localhost HTTP) | `POST /event` and `GET /status` require `Authorization: Bearer <token>`. The bridge creates the token on first start in `~/.config/cc-companion/token` (directory 0700, file 0600) and compares it in constant time; `hook.py` and `simulate.py` read it. | Processes running as your user can read the token, as they can read `~/.claude` transcripts. |
| Device storage | Bonds and settings in NVS. Session text lives only in RAM and is cleared on reboot. | Flash encryption is not enabled; someone with the device can read the bond keys from flash. |

There is no application-layer encryption or authentication on top of BLE.
If spoofing by other local apps matters, the next step would be a device-
generated key entered once into the bridge and a per-message authentication tag.

## Screens and controls

| Screen | Shows | Up / Down | OK click | OK hold |
| --- | --- | --- | --- | --- |
| Home | Focused session card (state, project, activity, subagent dots) and up to three other sessions | Previous / next session (loops around) | Open details | Settings |
| Details | Pending approval or question, main agent activity, latest prompt or reply, subagent list | Scroll | Back | Back |
| Attention | Full-screen card when a session needs approval or an answer, with a chime | Up: open details; Down: next waiting session | Dismiss | Dismiss |
| Settings | Sound, brightness, screen timeout, pair new Mac, forget Macs, about | Select | Change | Back |
| Waiting / Pairing | Link status, device name, six-digit passkey | — | — | Settings |

The home card keeps your Up/Down selection while you use the keys; 30 s after
the last key press it returns to the most important session (waiting for you,
then errors, then working, then finished). Session states use one color each: coral **WORKING** (with an animated spark), teal **BACKGROUND** (Claude finished
its turn but shells, subagents, or monitors it started are still running; also
animated, with a line such as "2 shells, 1 subagent running"),
amber **NEEDS APPROVAL** / **QUESTION**, green **DONE**, red **ERROR**, grey
**IDLE**. A banner appears for 15 s (any key dismisses it) when a job that
took at least ten seconds is finished, meaning nothing is left running in the
background, or when a turn fails. The battery level is shown at the top right;
when the gauge is unavailable only the icon is drawn.

The first key press on a dimmed or dark screen only wakes it. The screen dims
after the selected timeout (default one minute) and turns off one minute later;
approval and question alerts keep it lit for a minute. Any alert wakes it.

## Chinese text and fonts

Labels are English. Dynamic content such as prompts, questions, file names, and
replies may contain Chinese. The body font is a 16 px Noto Sans SC Medium
subset covering ASCII, all 6,763 GB2312 hanzi, GB2312 symbols, and common
punctuation (7,782 glyphs, 0.9 MB of Flash). Any character outside the subset,
such as emoji or rare hanzi, is drawn as a white square (U+25A1) on purpose
instead of LVGL's placeholder box. Font provenance and the regeneration command
are in the [assets README](../../assets/README.md#fonts).

## Build and flash

```bash
. <path-to-esp-idf-v5.5.3>/export.sh
./tools/validate.sh            # static checks, host tests, firmware build
```

The verified merged image is `build/FoloToy-AI-Passport-full.bin`; flash it at
offset `0x0`. A merged flash rewrites NVS, which clears this app's settings and
bonds, so pair again afterwards. See the
[flashing policy](../development/engineering/firmware-layout.md#flashing-and-stored-data).

## On-device acceptance checklist

- Boot log shows `glyph self-test: 0 missing`, the LVGL pool line, and free heap.
- *Waiting for your Mac* screen, device name, battery in the top-right corner.
- First bridge connection shows the passkey; pairing succeeds; reconnects after
  a device or bridge restart need no new pairing.
- With another Mac already bonded, a new pairing attempt is refused until
  *Settings > Pair new Mac* is used.
- `simulate.py` drives every state: card colors, spark animation, subagent dots,
  approval and question alerts with chimes, finished and error banners,
  Chinese project name and prompt text, details scrolling.
- Real Claude Code sessions: tool activity, subagents, a permission prompt, an
  `AskUserQuestion` prompt, completion, and `/exit` removing the session.
- Screen dims and turns off; the first key only wakes it.
- Settings persist across a reboot; *Forget Macs* removes the bond.

## Limits

- Hooks run asynchronously; the bridge orders events by the hook start time,
  so a status can lag a few hundred milliseconds behind the terminal.
- At most six sessions and five subagents per session are sent to the device;
  the counts still include the rest.
- Whether every tool event inside a subagent carries `agent_id` depends on the
  Claude Code version; subagent rows then show start, finish, and the last
  known activity.
- The bridge must run on the Mac that runs Claude Code. BLE range is a few
  meters and is not measured on this board.
- Power draw with an always-on BLE link is not measured; the backlight policy
  is the only power saving beyond codec suspend.
