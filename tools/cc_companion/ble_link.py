"""BLE transport between the bridge and the AI Passport (bleak, macOS first).

The device exposes one GATT service with an RX characteristic (bridge writes
CCP lines) and a TX characteristic (device notifies). Both require an
encrypted, MITM-authenticated link, so the first subscription makes macOS show
its pairing dialog; type the six-digit code shown on the device. After that the
bond is stored on both sides and reconnects are silent.

bleak is imported lazily so the rest of the bridge (and its tests) run without it.
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
from pathlib import Path
from typing import Awaitable, Callable

from ccp import DEVICE_NAME_PREFIX, RX_UUID, SERVICE_UUID, TX_UUID

log = logging.getLogger("cc_companion.ble")
STATE_FILE = Path(os.path.expanduser("~/.config/cc-companion/device.json"))
MAX_CHUNK = 244
MAX_WRITE_FAILURES = 3


class DryRunLink:
    """Prints the lines that would be sent; useful without hardware."""

    def __init__(self, on_ready: Callable[[], Awaitable[None]] | None = None):
        self.ready = True
        self._on_ready = on_ready

    async def run(self) -> None:
        if self._on_ready:
            await self._on_ready()
        await asyncio.Event().wait()

    async def send(self, text: str) -> bool:
        for line in text.splitlines():
            print(line.replace("\x1f", "⏎"), flush=True)
        return True


class BleLink:
    def __init__(self, device_hint: str | None,
                 on_ready: Callable[[], Awaitable[None]],
                 on_line: Callable[[str], None]):
        self.device_hint = device_hint
        self.ready = False
        self._on_ready = on_ready
        self._on_line = on_line
        self._client = None
        self._lock = asyncio.Lock()
        self._rx_buffer = b""
        self._write_failures = 0
        self._disconnected = asyncio.Event()

    # -- discovery --------------------------------------------------------------
    def _remembered(self) -> str | None:
        try:
            return json.loads(STATE_FILE.read_text()).get("address")
        except (OSError, ValueError):
            return None

    def _remember(self, address: str, name: str | None) -> None:
        try:
            STATE_FILE.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            STATE_FILE.write_text(json.dumps({"address": address, "name": name}))
        except OSError as error:
            log.warning("cannot remember device: %s", error)

    async def _find(self):
        from bleak import BleakScanner

        hint = self.device_hint or self._remembered()
        if hint and not hint.startswith(DEVICE_NAME_PREFIX):
            device = await BleakScanner.find_device_by_address(hint, timeout=8.0)
            if device:
                return device

        def match(device, adv) -> bool:
            uuids = [u.lower() for u in (adv.service_uuids or [])]
            name = adv.local_name or device.name or ""
            if self.device_hint and self.device_hint.startswith(DEVICE_NAME_PREFIX):
                return name == self.device_hint
            return SERVICE_UUID in uuids or name.startswith(DEVICE_NAME_PREFIX)

        return await BleakScanner.find_device_by_filter(match, timeout=10.0)

    # -- connection loop ----------------------------------------------------------
    async def run(self) -> None:
        from bleak import BleakClient
        from bleak.exc import BleakError

        backoff = 2.0
        while True:
            try:
                device = await self._find()
                if device is None:
                    log.info("no %s device found; retrying", DEVICE_NAME_PREFIX)
                    await asyncio.sleep(backoff)
                    backoff = min(backoff * 1.5, 15.0)
                    continue
                log.info("connecting to %s (%s)", device.name, device.address)
                self._disconnected.clear()
                client = BleakClient(device, disconnected_callback=lambda _c: self._disconnected.set(),
                                     timeout=20.0)
                await client.connect()
                self._client = client
                self._rx_buffer = b""
                self._write_failures = 0
                # Reading the protected TX characteristic makes macOS encrypt the link
                # now: silently from the stored bond, or with the passkey dialog on
                # first use. Subscribing alone does not always trigger it.
                try:
                    await client.read_gatt_char(TX_UUID)
                except Exception as error:  # noqa: BLE001 - pairing may still be in progress
                    log.info("security read: %s", error)
                await client.start_notify(TX_UUID, self._notified)
                self._remember(device.address, device.name)
                self.ready = True
                backoff = 2.0
                log.info("link ready (MTU %s)", client.mtu_size)
                await self._on_ready()
                await self._disconnected.wait()
                log.info("disconnected")
            except (BleakError, asyncio.TimeoutError, OSError) as error:
                log.warning("BLE error: %s", error)
            finally:
                self.ready = False
                if self._client is not None:
                    try:
                        await self._client.disconnect()
                    except Exception:  # noqa: BLE001 - best effort cleanup
                        pass
                    self._client = None
            await asyncio.sleep(backoff)

    def _notified(self, _characteristic, data: bytearray) -> None:
        self._rx_buffer += bytes(data)
        while b"\n" in self._rx_buffer:
            line, self._rx_buffer = self._rx_buffer.split(b"\n", 1)
            self._on_line(line.decode("utf-8", errors="replace"))
        if len(self._rx_buffer) > 1024:
            self._rx_buffer = b""

    async def send(self, text: str) -> bool:
        """Write text in MTU-sized chunks with write-with-response; False on failure."""
        client = self._client
        if not self.ready or client is None:
            return False
        data = text.encode("utf-8")
        chunk = max(20, min(MAX_CHUNK, (client.mtu_size or 23) - 3))
        async with self._lock:
            try:
                for start in range(0, len(data), chunk):
                    await client.write_gatt_char(RX_UUID, data[start:start + chunk], response=True)
                self._write_failures = 0
                return True
            except Exception as error:  # noqa: BLE001 - surface any backend error as a dropped link
                self._write_failures += 1
                log.warning("write failed (%d in a row): %s", self._write_failures, error)
                if self._write_failures >= MAX_WRITE_FAILURES:
                    # Reconnecting re-runs encryption; retrying the same link would not.
                    log.warning("reconnecting after repeated write failures")
                    self._disconnected.set()
                return False


async def scan(timeout: float = 8.0) -> list[tuple[str, str, int]]:
    from bleak import BleakScanner

    found = await BleakScanner.discover(timeout=timeout, return_adv=True)
    out = []
    for device, adv in found.values():
        uuids = [u.lower() for u in (adv.service_uuids or [])]
        name = adv.local_name or device.name or ""
        if SERVICE_UUID in uuids or name.startswith(DEVICE_NAME_PREFIX):
            out.append((name, device.address, adv.rssi))
    return out
