"""Shared helpers: LAN broadcast framing (UDP 27036) and persisted device state."""
import json
import os
import pathlib
import random
import struct
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "re" / "pb"))

import steammessages_remoteclient_discovery_pb2 as disc  # noqa: E402

PORT = 27036
SIGNATURE = struct.pack("<iI", -1, 0xA05F4C21)
STATE_FILE = ROOT / "state" / "device.json"
KEYS_DIR = ROOT / "re" / "keys"


def pack(header, body):
    h = header.SerializeToString()
    b = body.SerializeToString()
    return SIGNATURE + struct.pack("<I", len(h)) + h + struct.pack("<I", len(b)) + b


def unpack(data):
    """Returns (CMsgRemoteClientBroadcastHeader, body_bytes) or None if not a framed packet."""
    if data[:8] != SIGNATURE:
        return None
    (hlen,) = struct.unpack_from("<I", data, 8)
    header = disc.CMsgRemoteClientBroadcastHeader.FromString(data[12:12 + hlen])
    off = 12 + hlen
    (blen,) = struct.unpack_from("<I", data, off)
    return header, data[off + 4:off + 4 + blen]


def universe_key(euniverse):
    name = {1: "universe1_public", 2: "universe2_beta"}.get(euniverse, "universe3plus")
    return (KEYS_DIR / (name + ".pem")).read_bytes()


def load_state():
    if STATE_FILE.exists():
        s = json.loads(STATE_FILE.read_text())
        s["secret"] = bytes.fromhex(s["secret"]) if s.get("secret") else None
        return s
    return {"device_id": random.getrandbits(64), "secret": None, "device_name": "RG40XXH"}


def save_state(state):
    STATE_FILE.parent.mkdir(parents=True, exist_ok=True)
    out = dict(state)
    out["secret"] = state["secret"].hex() if state.get("secret") else None
    tmp = STATE_FILE.with_suffix(".tmp")
    tmp.write_text(json.dumps(out, indent=2))
    os.chmod(tmp, 0o600)
    tmp.replace(STATE_FILE)
