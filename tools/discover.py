"""Send a Steam Remote Play discovery packet and print the Status replies.

Usage: uv run --with protobuf python -I tools/discover.py [target_ip ...]
Defaults to 255.255.255.255. Requires re/pb generated from re/protos.
"""
import pathlib
import random
import socket
import struct
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent.parent / "re" / "pb"))
import steammessages_remoteclient_discovery_pb2 as pb  # noqa: E402

PORT = 27036
SIGNATURE = struct.pack("<iI", -1, 0xA05F4C21)


def pack(header, body):
    h = header.SerializeToString()
    b = body.SerializeToString()
    return SIGNATURE + struct.pack("<I", len(h)) + h + struct.pack("<I", len(b)) + b


def unpack(data):
    if data[:8] != SIGNATURE:
        return None
    (hlen,) = struct.unpack_from("<I", data, 8)
    header = pb.CMsgRemoteClientBroadcastHeader.FromString(data[12:12 + hlen])
    off = 12 + hlen
    (blen,) = struct.unpack_from("<I", data, off)
    return header, data[off + 4:off + 4 + blen]


def main():
    targets = sys.argv[1:] or ["255.255.255.255"]
    client_id = random.getrandbits(64)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", 0))
    sock.settimeout(0.5)

    header = pb.CMsgRemoteClientBroadcastHeader(
        client_id=client_id, msg_type=pb.k_ERemoteClientBroadcastMsgDiscovery)
    body = pb.CMsgRemoteClientBroadcastDiscovery(seq_num=1)
    packet = pack(header, body)
    print("sending %d bytes: %s" % (len(packet), packet.hex()))
    for t in targets:
        sock.sendto(packet, (t, PORT))

    deadline = time.time() + 3
    while time.time() < deadline:
        try:
            data, addr = sock.recvfrom(4096)
        except socket.timeout:
            continue
        parsed = unpack(data)
        if not parsed:
            print("non-steam packet from", addr)
            continue
        hdr, raw = parsed
        print("== from %s  header:\n%s" % (addr, hdr))
        if hdr.msg_type == pb.k_ERemoteClientBroadcastMsgStatus:
            print(pb.CMsgRemoteClientBroadcastStatus.FromString(raw))


if __name__ == "__main__":
    main()
