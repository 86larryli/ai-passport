#!/usr/bin/env python3
"""Claude Code Companion bridge: hook events in, CCP snapshots out over BLE.

  Claude Code hooks --(hook.py, HTTP POST 127.0.0.1)--> bridge --(BLE GATT)--> AI Passport

Run (bleak is the only third-party dependency):

  uv run --no-project --with bleak==3.1.0 python tools/cc_companion/bridge.py

Useful options: --scan lists nearby devices, --dry-run prints the CCP lines
instead of using BLE, --device pins a device (name or CoreBluetooth address).
GET http://127.0.0.1:47820/status returns the tracked sessions as JSON.

The HTTP endpoint requires `Authorization: Bearer <token>`; the token is created
on first start in ~/.config/cc-companion/token (0600) and read by hook.py and
simulate.py (see auth.py).
"""

from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
import time
from dataclasses import asdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import ccp  # noqa: E402
from auth import authorized, load_or_create_token, token_path  # noqa: E402
from state import Alert, SessionTracker  # noqa: E402

BRIDGE_VERSION = "0.1.0"
DEFAULT_PORT = 47820
DEBOUNCE_S = 0.15
PING_S = 5.0
RESYNC_S = 30.0
MAX_BODY = 256 * 1024

log = logging.getLogger("cc_companion")


def utc_offset_minutes(now: float) -> int:
    local = time.localtime(now)
    return int((local.tm_gmtoff or 0) // 60)


class Bridge:
    def __init__(self, tracker: SessionTracker, link_factory, token: str):
        self.tracker = tracker
        self.token = token
        self.link = link_factory(self._on_link_ready, self._on_device_line)
        self.dirty = asyncio.Event()
        self.seq = 0
        self.sent_version = -1
        self.last_send = 0.0
        self.last_snapshot = 0.0
        self.pending_alerts: list[Alert] = []
        self._seen_task_shapes: set = set()

    # -- link callbacks ----------------------------------------------------------
    async def _on_link_ready(self) -> None:
        now = time.time()
        await self.link.send(ccp.hello(BRIDGE_VERSION) + ccp.clock(now, utc_offset_minutes(now)))
        self.sent_version = -1
        self.dirty.set()

    def _on_device_line(self, line: str) -> None:
        message = ccp.parse_device_line(line)
        if message.kind == "HI":
            log.info("device says hi: proto=%s firmware=%s battery=%s%%",
                     *(message.fields + ["?"] * 3)[:3])
        elif message.kind == "ERR":
            log.warning("device reported a protocol error: %s", " ".join(message.fields))
        elif message.kind == "ACK":
            log.debug("device applied snapshot %s", message.fields[:1])

    # -- ingest -----------------------------------------------------------------
    def ingest(self, payload: dict) -> None:
        tasks = payload.get("background_tasks")
        if tasks:
            shape = tuple(sorted(tasks[0])) if isinstance(tasks[0], dict) else type(tasks[0]).__name__
            if shape not in self._seen_task_shapes:
                self._seen_task_shapes.add(shape)
                log.info("background_tasks sample (%s, %d): %s", payload.get("hook_event_name"),
                         len(tasks), json.dumps(tasks[:3], ensure_ascii=False)[:600])
        alerts = self.tracker.apply(payload)
        if alerts:
            self.pending_alerts.extend(alerts)
        self.dirty.set()

    async def handle_http(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        status, body = 400, b""
        try:
            head = await asyncio.wait_for(reader.readuntil(b"\r\n\r\n"), timeout=2.0)
            lines = head.decode("latin-1").split("\r\n")
            method, path, _ = (lines[0].split(" ") + ["", "", ""])[:3]
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    key, value = line.split(":", 1)
                    headers[key.strip().lower()] = value.strip()
            length = int(headers.get("content-length", "0") or 0)
            if not authorized(headers.get("authorization"), self.token):
                status = 401   # every route carries session data or accepts it
            elif length > MAX_BODY:
                status = 413
            elif method == "POST" and path == "/event":
                data = await asyncio.wait_for(reader.readexactly(length), timeout=2.0)
                payload = json.loads(data.decode("utf-8"))
                if isinstance(payload, dict):
                    self.ingest(payload)
                    status = 204
            elif method == "GET" and path == "/status":
                views, total = self.tracker.views()
                body = json.dumps({"total": total, "link_ready": self.link.ready,
                                   "sessions": [asdict(v) for v in views]},
                                  ensure_ascii=False, indent=2).encode("utf-8")
                status = 200
            else:
                status = 404
        except (asyncio.IncompleteReadError, asyncio.LimitOverrunError, asyncio.TimeoutError,
                ValueError, UnicodeDecodeError):
            status = 400
        reason = {200: "OK", 204: "No Content", 400: "Bad Request", 401: "Unauthorized",
                  404: "Not Found", 413: "Payload Too Large"}[status]
        writer.write(f"HTTP/1.1 {status} {reason}\r\nContent-Length: {len(body)}\r\n"
                     "Content-Type: application/json\r\nConnection: close\r\n\r\n".encode() + body)
        try:
            await writer.drain()
        finally:
            writer.close()

    # -- sending -----------------------------------------------------------------
    def _snapshot_text(self) -> str:
        views, total = self.tracker.views()
        self.seq = (self.seq + 1) % 1_000_000_000
        return "".join(ccp.snapshot(self.seq, views, total))

    async def sender(self) -> None:
        while True:
            try:
                await asyncio.wait_for(self.dirty.wait(), timeout=1.0)
                await asyncio.sleep(DEBOUNCE_S)   # coalesce bursts of hook events
            except asyncio.TimeoutError:
                pass
            self.dirty.clear()
            now = time.time()
            if not self.link.ready:
                self.pending_alerts.clear()       # stale alerts are not replayed later
                continue
            text = ""
            for alert in self.pending_alerts:
                log.info("alert %s for %s %s", alert.kind, alert.sid, alert.project)
                text += ccp.event(alert.kind, alert.sid, alert.project, alert.text)
            self.pending_alerts.clear()
            changed = self.tracker.version != self.sent_version
            if changed or now - self.last_snapshot >= RESYNC_S:
                if now - self.last_snapshot >= RESYNC_S:
                    text += ccp.clock(now, utc_offset_minutes(now))
                version = self.tracker.version
                text += self._snapshot_text()
                if await self.link.send(text):
                    self.sent_version = version
                    self.last_snapshot = self.last_send = now
                continue
            if text or now - self.last_send >= PING_S:
                if await self.link.send(text or ccp.ping()):
                    self.last_send = now

    async def pruner(self) -> None:
        while True:
            await asyncio.sleep(60)
            self.tracker.prune()
            self.dirty.set()


async def run(args) -> int:
    tracker = SessionTracker(done_alert_min_s=args.done_alert_min_s,
                             stale_s=args.stale_hours * 3600)
    if args.dry_run:
        from ble_link import DryRunLink

        def factory(on_ready, _on_line):
            return DryRunLink(on_ready)
    else:
        from ble_link import BleLink

        def factory(on_ready, on_line):
            return BleLink(args.device, on_ready, on_line)

    bridge = Bridge(tracker, factory, load_or_create_token())
    server = await asyncio.start_server(bridge.handle_http, "127.0.0.1", args.port)
    log.info("listening for hook events on http://127.0.0.1:%d/event (token: %s)",
             args.port, token_path())
    async with server:
        await asyncio.gather(server.serve_forever(), bridge.link.run(), bridge.sender(),
                             bridge.pruner())
    return 0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--device", help="device name (CC-Companion-XXXX) or CoreBluetooth address")
    parser.add_argument("--dry-run", action="store_true", help="print CCP lines instead of using BLE")
    parser.add_argument("--scan", action="store_true", help="list nearby companion devices and exit")
    parser.add_argument("--done-alert-min-s", type=float, default=10.0,
                        help="only alert on turns that took at least this long")
    parser.add_argument("--stale-hours", type=float, default=8.0,
                        help="forget sessions with no events for this long")
    parser.add_argument("-v", "--verbose", action="store_true")
    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
                        stream=sys.stderr)
    if args.scan:
        from ble_link import scan

        for name, address, rssi in asyncio.run(scan()):
            print(f"{name}\t{address}\tRSSI {rssi}")
        return 0
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 0


if __name__ == "__main__":
    sys.exit(main())
