"""Shared-secret token that guards the bridge's localhost HTTP endpoint.

The bridge listens on 127.0.0.1, which every local process and every other
logged-in macOS user can reach. Session snapshots contain prompt previews,
commands, questions, and file paths, so both POST /event and GET /status
require `Authorization: Bearer <token>`. The token lives in a file only the
current user can read (directory 0700, file 0600); hook.py and simulate.py read
it from the same place. CC_COMPANION_TOKEN_FILE overrides the path (tests).
"""

from __future__ import annotations

import hmac
import os
import secrets
import stat
from pathlib import Path

DEFAULT_TOKEN_FILE = Path("~/.config/cc-companion/token")


def token_path() -> Path:
    return Path(os.environ.get("CC_COMPANION_TOKEN_FILE", str(DEFAULT_TOKEN_FILE))).expanduser()


def load_or_create_token(path: Path | None = None) -> str:
    """Return the token, creating it (0600, parent 0700) on first use."""
    path = path or token_path()
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if stat.S_IMODE(path.parent.stat().st_mode) & 0o077:
        os.chmod(path.parent, 0o700)       # mkdir() does not tighten an existing directory
    try:
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError:
        mode = stat.S_IMODE(path.stat().st_mode)
        if mode & 0o077:
            os.chmod(path, 0o600)          # tighten a token another tool left readable
        token = path.read_text(encoding="utf-8").strip()
        if len(token) >= 32:
            return token
        path.unlink()                      # empty or truncated: replace it
        return load_or_create_token(path)
    token = secrets.token_urlsafe(32)
    with os.fdopen(fd, "w", encoding="utf-8") as handle:
        handle.write(token + "\n")
    return token


def read_token(path: Path | None = None) -> str | None:
    """Read an existing token without creating one (for senders)."""
    try:
        token = (path or token_path()).read_text(encoding="utf-8").strip()
    except OSError:
        return None
    return token or None


def authorized(header_value: str | None, token: str) -> bool:
    if not header_value or not header_value.startswith("Bearer "):
        return False
    return hmac.compare_digest(header_value[len("Bearer "):].strip().encode(), token.encode())
