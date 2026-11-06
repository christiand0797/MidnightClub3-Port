"""Bounded host pairing for video/controller access, independent of account credentials."""
import hmac
import json
import re
import time
from urllib.parse import urlsplit

TOKEN = re.compile(r"^[0-9a-f]{64}$")
MAX_MESSAGE = 16 * 1024


def valid_origin(value):
    if not isinstance(value, str) or len(value) > 2048:
        return False
    try:
        url = urlsplit(value)
        return (url.scheme in ("http", "https") and bool(url.hostname) and
                not url.username and not url.password and not url.path and
                not url.query and not url.fragment and url.port != 0)
    except ValueError:
        return False


def decode_message(raw):
    if not isinstance(raw, str) or len(raw.encode("utf-8")) > MAX_MESSAGE:
        raise ValueError("Invalid message")
    def reject_constant(_):
        raise ValueError("Nonfinite JSON number")
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result or key in ("__proto__", "constructor", "prototype"):
                raise ValueError("Invalid JSON key")
            result[key] = value
        return result
    value = json.loads(raw, parse_constant=reject_constant, object_pairs_hook=unique)
    if type(value) is not dict or len(value) > 12:
        raise ValueError("Expected message object")
    return value


class Budget:
    def __init__(self, clock=time.monotonic):
        self.clock = clock
        self.at = clock()
        self.messages = 180.0
        self.bytes = 128 * 1024.0

    def take(self, size):
        now = self.clock()
        elapsed = max(0, now - self.at)
        self.at = now
        self.messages = min(180, self.messages + elapsed * 90)
        self.bytes = min(128 * 1024, self.bytes + elapsed * 64 * 1024)
        if size < 0 or size > MAX_MESSAGE or self.messages < 1 or self.bytes < size:
            return False
        self.messages -= 1
        self.bytes -= size
        return True


class Pairing:
    def __init__(self, token, origins, max_clients=8):
        if not isinstance(token, str) or not TOKEN.fullmatch(token):
            raise ValueError("MC3_STREAM_PAIRING_TOKEN must be 32 random bytes encoded as lowercase hex")
        if not origins or len(origins) > 8 or any(not valid_origin(o) for o in origins):
            raise ValueError("Specify exact browser origins for host pairing")
        self.token, self.origins, self.max_clients = token, frozenset(origins), max_clients
        self.pending = set()
        self.paired = set()

    def admit(self, peer, origin):
        if origin not in self.origins or len(self.pending) + len(self.paired) >= self.max_clients:
            return False
        self.pending.add(peer)
        return True

    def authenticate(self, peer, raw):
        if peer not in self.pending:
            return False
        try:
            msg = decode_message(raw)
            supplied = msg.get("token")
            good = (set(msg) == {"type", "token"} and msg["type"] == "pair" and
                    isinstance(supplied, str) and TOKEN.fullmatch(supplied) and
                    hmac.compare_digest(supplied, self.token))
        except (ValueError, TypeError, RecursionError):
            good = False
        self.pending.discard(peer)
        if good:
            self.paired.add(peer)
        return bool(good)

    def release(self, peer):
        self.pending.discard(peer)
        self.paired.discard(peer)
