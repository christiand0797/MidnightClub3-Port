"""The game's own System Link datagrams (MC3 "mcnet"), as decoded from recorded play.

Recorded with proxy.py --record during a two-copy race (2026-11-04). Every datagram on port 1000 is:

    count u8 | session u8 | total length u16 | sequence u32            (8-byte packet header)
    then `count` messages, each:  type u16 | length u16 | id u32 | body (length - 8)

The id's top bit marks a reliable message. Types 0x40-0x68 are the game's named messages (the string table at
0x1AFD10, docs/notes/career_save_network_trace_20261104.md); lower types are the transport's own (acks 0x01,
keepalive 0x0B with timestamps, connect 0x02-0x06, disconnect 0x07).

Observed so far: RACE_CONFIG from the host when a match is set up, START_COUNTDOWN, HOST_DONE_WAITING, RACE_STATE
from the host with state 4 (countdown) then 5 three seconds later (racing), PLAYER_AND_INPUT (56-byte body) every tick of a race, PLAYER_DAMAGE on a
collision. CHECKPOINT and PLAYER_SCORE bodies have not been seen yet (no recorded race was finished); the relay keeps
the ones players' tunnels report so they can be decoded from real races.
"""
import struct

NAMES = {
    0x40: "INPUT", 0x41: "PLAYER", 0x42: "PLAYER_AND_INPUT", 0x43: "RACE_CONFIG", 0x44: "CAR_CONFIG",
    0x45: "PAUSE_SIMULATION", 0x46: "RESUME_SIMULATION", 0x47: "LOAD_RACE", 0x48: "RESET", 0x49: "GAME_STATE",
    0x4A: "RACE_STATE", 0x4B: "PLAYER_CONFIG", 0x4C: "CHECKPOINT", 0x56: "PLAYER_DAMAGE", 0x58: "CHAT",
    0x5A: "CUSTOM_RACE", 0x5B: "START_COUNTDOWN", 0x5C: "STOP_COUNTDOWN", 0x5E: "VOICE_CONFIG", 0x5F: "HOST_INFO",
    0x60: "UPDATE_HOST_INFO_ACK", 0x62: "CLAN_INFO", 0x63: "NETWORK_AVAILABLE", 0x66: "PLAYER_SCORE",
    0x67: "MIGRATE_HOST_INDEX", 0x68: "HOST_DONE_WAITING_FOR_PEERS",
}
RACE_CONFIG, GAME_STATE, RACE_STATE, CHECKPOINT, PLAYER_SCORE = 0x43, 0x49, 0x4A, 0x4C, 0x66
PHASE = 0x09  # unnamed host -> guests session phase, byte 0: 05 loading, 07 ready, 09 leaving race, 03 lobby
RACE_COUNTDOWN, RACE_STARTED = 4, 5    # RACE_STATE body (u32): countdown, then racing
EVENT_TYPES = {RACE_CONFIG, GAME_STATE, RACE_STATE, CHECKPOINT, PLAYER_SCORE, PHASE}  # reported to the relay


def messages(datagram):
    """(type, id, body) for each message in one datagram; nothing for anything malformed."""
    if len(datagram) < 8:
        return
    _count, _session, total, _seq = struct.unpack_from("<BBHI", datagram, 0)
    end = min(total, len(datagram))
    o = 8
    while o + 8 <= end:
        typ, length, mid = struct.unpack_from("<HHI", datagram, o)
        if length < 8 or o + length > end:
            return
        yield typ, mid, datagram[o + 8:o + length]
        o += length


def race_events(datagram):
    """The messages worth reporting: race setup, race state, checkpoints and scores."""
    for typ, mid, body in messages(datagram):
        if typ in EVENT_TYPES:
            event = {"type": NAMES.get(typ, "PHASE"), "body": body.hex()}
            if typ == RACE_STATE and len(body) >= 4:
                event["state"] = struct.unpack_from("<I", body)[0]
            yield event
