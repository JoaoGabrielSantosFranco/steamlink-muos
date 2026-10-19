"""Steam Link streaming session probe (docs/PROTOCOLO.md, sections 4.5-6).

Usage: uv run --with protobuf --with cryptography python -I tools/proto/stream.py <pc_ip> [seconds]

Requests a stream from the paired PC, runs Connect -> handshake -> auth -> negotiation and
captures media for a few seconds into state/capture/ (raw video/audio payloads + a log).
"""
import os
import random
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slcrypto  # noqa: E402
import slnet  # noqa: E402

import steammessages_remoteclient_discovery_pb2 as disc  # noqa: E402
import steammessages_remoteplay_pb2 as rp  # noqa: E402

CAPTURE_DIR = slnet.ROOT / "state" / "capture"

# Packet types (docs 6.1)
PT_CONNECT, PT_CONNECTED, PT_UNRELIABLE, PT_UNRELIABLE_FRAG = 1, 2, 3, 4
PT_RELIABLE, PT_RELIABLE_FRAG, PT_ACK, PT_NACK, PT_DISCONNECT = 5, 6, 7, 8, 9
CHANNEL_CONTROL = 1
CONNECT_MAGIC = 0x3C8F3DC7  # CRC32C("Connect")
UNENCRYPTED_CONTROL = {1, 2, 6, 7}  # Authentication*, *Handshake (0x2a5560)
MSG_NAMES = {v: k for k, v in rp.EStreamControlMessage.items()}


def _crc32c_table():
    table = []
    for i in range(256):
        c = i
        for _ in range(8):
            c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
        table.append(c)
    return table


_CRC_TABLE = _crc32c_table()


def crc32c(data):
    c = 0xFFFFFFFF
    for b in data:
        c = _CRC_TABLE[(c ^ b) & 0xFF] ^ (c >> 8)
    return c ^ 0xFFFFFFFF


def stream_timestamp():
    """16.16 fixed-point seconds, truncated to 32 bits (0x1769e0)."""
    t = time.clock_gettime_ns(time.CLOCK_MONOTONIC)
    return ((t // 1_000_000_000) << 16) + (t % 1_000_000_000) * 65536 // 1_000_000_000 & 0xFFFFFFFF


class Packet:
    HEADER = struct.Struct("<BBBBBHHI")  # type|crc, retries, src, dst, channel, frag, packet_id, timestamp

    def __init__(self, ptype, src, dst, channel, frag, packet_id, timestamp, payload):
        self.type, self.src, self.dst, self.channel = ptype, src, dst, channel
        self.frag, self.packet_id, self.timestamp, self.payload = frag, packet_id, timestamp, payload

    @classmethod
    def parse(cls, data):
        if len(data) < 13:
            return None
        b0, _retries, src, dst, channel, frag, pid, ts = cls.HEADER.unpack_from(data)
        if b0 & 0x80:
            if len(data) < 17 or crc32c(data[:-4]) != struct.unpack_from("<I", data, len(data) - 4)[0]:
                return None
            data = data[:-4]
        return cls(b0 & 0x7F, src, dst, channel, frag, pid, ts, data[13:])

    def encode(self, crc=True):
        raw = self.HEADER.pack(self.type | (0x80 if crc else 0), 0, self.src, self.dst, self.channel,
                               self.frag, self.packet_id, self.timestamp) + self.payload
        return raw + struct.pack("<I", crc32c(raw)) if crc else raw


START = time.monotonic()


def log(f, text):
    line = "%8.3f %s" % (time.monotonic() - START, text)
    print(line)
    f.write(line + "\n")
    f.flush()


def request_stream(state, pc_ip, logf):
    """LAN StreamingRequest (msg 5) incl. ProofRequest handling; returns (port, session_key)."""
    device_id, secret = state["device_id"], state["secret"]

    def token():
        return slcrypto.symmetric_encrypt(secret, struct.pack("<Q", device_id))

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", slnet.PORT))
        print("bound to port %d" % slnet.PORT)
    except OSError:
        sock.bind(("", 0))
        print("warning: port %d busy, using ephemeral" % slnet.PORT)
    sock.settimeout(1.0)
    request_id = random.randint(1, 0x7FFFFFFF)
    req = disc.CMsgRemoteDeviceStreamingRequest(
        request_id=request_id,
        maximum_resolution_x=640, maximum_resolution_y=480,
        audio_channel_count=2,
        device_version="1.4.2.321",
        stream_desktop=True,
        device_token=token(),
        enable_video_streaming=True, enable_audio_streaming=True, enable_input_streaming=True,
        supported_transport=[disc.k_EStreamTransportUDP, disc.k_EStreamTransportUDP_SNS],
        form_factor=disc.k_EStreamDeviceFormFactorPhone,
        gamepad_count=1,
        stream_interface=disc.k_EStreamInterfaceBigPicture,
        maximum_framerate_numerator=60, maximum_framerate_denominator=1,
    )

    def send(msg_type, body):
        header = disc.CMsgRemoteClientBroadcastHeader(client_id=device_id, msg_type=msg_type, device_token=token())
        sock.sendto(slnet.pack(header, body), (pc_ip, slnet.PORT))

    send(disc.k_ERemoteDeviceStreamingRequest, req)
    log(logf, "-> StreamingRequest request_id=%d" % request_id)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        try:
            data, addr = sock.recvfrom(65536)
        except socket.timeout:
            continue
        parsed = slnet.unpack(data)
        if not parsed:
            continue
        hdr, body = parsed
        if hdr.msg_type == disc.k_ERemoteDeviceProofRequest:
            proof = disc.CMsgRemoteDeviceProofRequest.FromString(body)
            log(logf, "<- ProofRequest request_id=%d update_secret=%s" % (proof.request_id, proof.update_secret))
            send(disc.k_ERemoteDeviceProofResponse, disc.CMsgRemoteDeviceProofResponse(
                response=slcrypto.symmetric_encrypt(secret, proof.challenge), request_id=proof.request_id))
            log(logf, "-> ProofResponse")
        elif hdr.msg_type == disc.k_ERemoteDeviceStreamingProgress:
            p = disc.CMsgRemoteDeviceStreamingProgress.FromString(body)
            log(logf, "<- StreamingProgress %.2f" % p.progress)
        elif hdr.msg_type == disc.k_ERemoteDeviceStreamingResponse:
            resp = disc.CMsgRemoteDeviceStreamingResponse.FromString(body)
            log(logf, "<- StreamingResponse result=%s port=%d transport=%s cert=%d_bytes" % (
                disc.ERemoteDeviceStreamingResult.Name(resp.result), resp.port,
                disc.EStreamTransport.Name(resp.transport), len(resp.cert) if resp.cert else 0))
            if resp.result != disc.k_ERemoteDeviceStreamingSuccess:
                sys.exit("streaming refused")
            return resp.port, slcrypto.symmetric_decrypt(secret, resp.encrypted_session_key)
        elif hdr.msg_type not in (disc.k_ERemoteClientBroadcastMsgStatus, disc.k_ERemoteClientBroadcastMsgDiscovery):
            log(logf, "<- %s (%d bytes) from %s" % (
                disc.ERemoteClientBroadcastMsg.Name(hdr.msg_type), len(body), addr))
    sys.exit("no StreamingResponse within 60s")


class Session:
    def __init__(self, pc_ip, port, session_key, state, logf):
        self.addr = (pc_ip, port)
        self.key = session_key
        self.state = state
        self.logf = logf
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.05)
        self.my_id = random.randint(1, 255)
        self.peer_id = 0
        self.next_id = {}          # (channel, reliable) -> next outgoing packet id
        self.send_seq = 0          # encrypted control sequence
        self.media_channels = {}   # channel -> "video" | "audio"
        self.frags = {}            # channel -> [continuations_expected, [payload, ...]]
        CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
        self.out = {"video": open(CAPTURE_DIR / "video.h264", "wb"),
                    "audio": open(CAPTURE_DIR / "audio.opus.bin", "wb")}
        self.counts = {}
        self.first_seen = set()
        self._pings_logged = 0

    def _send(self, ptype, channel, payload, frag=0, packet_id=None, timestamp=None):
        key = (channel, ptype in (PT_RELIABLE, PT_RELIABLE_FRAG))
        if packet_id is None:
            packet_id = self.next_id.get(key, 0)
            self.next_id[key] = (packet_id + 1) & 0xFFFF
        pkt = Packet(ptype, self.my_id, self.peer_id, channel, frag, packet_id,
                     stream_timestamp() if timestamp is None else timestamp, payload)
        self.sock.sendto(pkt.encode(), self.addr)

    def send_control(self, msg_type, msg):
        body = msg.SerializeToString()
        if msg_type not in UNENCRYPTED_CONTROL:
            body = slcrypto.encrypt_control(self.key, self.send_seq, body)
            self.send_seq += 1
        self._send(PT_RELIABLE, CHANNEL_CONTROL, bytes([msg_type]) + body)
        log(self.logf, "-> %s" % MSG_NAMES.get(msg_type, msg_type))

    def ack(self, pkt):
        self._send(PT_ACK, pkt.channel, struct.pack("<I", pkt.timestamp), packet_id=pkt.packet_id)

    def connect(self):
        for _ in range(10):
            self._send(PT_CONNECT, 0, struct.pack("<I", CONNECT_MAGIC), packet_id=0)
            log(self.logf, "-> Connect my_id=%d" % self.my_id)
            end = time.monotonic() + 1
            while time.monotonic() < end:
                pkt = self.recv()
                if pkt and pkt.type == PT_CONNECTED:
                    self.peer_id = pkt.src
                    log(self.logf, "<- Connected peer_id=%d" % self.peer_id)
                    return
                if pkt:
                    log(self.logf, "<- (while connecting) type=%d" % pkt.type)
        sys.exit("no Connected from PC")

    def recv(self):
        try:
            data, addr = self.sock.recvfrom(65536)
        except socket.timeout:
            return None
        pkt = Packet.parse(data)
        if pkt is None:
            log(self.logf, "<- unparseable %d bytes from %s: %s" % (len(data), addr, data[:48].hex()))
        return pkt

    def handle(self, pkt):
        self.counts[pkt.type] = self.counts.get(pkt.type, 0) + 1
        if pkt.type in (PT_RELIABLE, PT_RELIABLE_FRAG):
            self.ack(pkt)
        if pkt.type == PT_DISCONNECT:
            log(self.logf, "<- Disconnect src=%d dst=%d ch=%d" % (pkt.src, pkt.dst, pkt.channel))
            return "disconnect"
        if pkt.type == 0:
            self._on_ping(pkt)
            return None
        if pkt.type not in (PT_RELIABLE, PT_RELIABLE_FRAG, PT_UNRELIABLE, PT_UNRELIABLE_FRAG):
            if pkt.type not in self.first_seen:
                self.first_seen.add(pkt.type)
                log(self.logf, "<- type=%d ch=%d src=%d dst=%d frag=%d pid=%d payload(%d): %s" % (
                    pkt.type, pkt.channel, pkt.src, pkt.dst, pkt.frag, pkt.packet_id,
                    len(pkt.payload), pkt.payload[:48].hex()))
            return None
        payload = self._reassemble(pkt)
        if payload is None:
            return None
        if pkt.channel == CHANNEL_CONTROL:
            return self.on_control(payload[0], payload[1:])
        kind = self.media_channels.get(pkt.channel)
        if kind and payload[0] == rp.k_EStreamDataPacket and len(payload) > 13:
            frame_id, ts, c, d = struct.unpack_from("<HIHI", payload, 1)
            if kind not in self.first_seen:
                self.first_seen.add(kind)
                log(self.logf, "<- first %s packet: frame_id=%d ts=%d c=%d d=%d data[0:16]=%s" % (
                    kind, frame_id, ts, c, d, payload[13:29].hex()))
            self.out[kind].write(payload[13:])
            self.counts[kind] = self.counts.get(kind, 0) + len(payload) - 13
        elif ("chan", pkt.channel) not in self.first_seen:
            self.first_seen.add(("chan", pkt.channel))
            log(self.logf, "<- first data on channel %d: %s" % (pkt.channel, payload[:32].hex()))
        return None

    def _on_ping(self, pkt):
        """Unconnected (type 0) MTU ping, docs/PROTOCOLO.md 6.3: answer every PingRequest(1) with a
        PingResponse(2) padded with 0xfe so header + body reaches packet_size_requested."""
        payload = pkt.payload
        if len(payload) < 5 or payload[0] != 1:
            return
        proto_len = struct.unpack_from("<I", payload, 1)[0]
        if 5 + proto_len > len(payload):
            return
        fields = self._decode_varint_fields(payload[5:5 + proto_len])
        seq, size_requested = fields.get(1, 0), fields.get(2, 0)
        resp_proto = self._encode_varints({1: seq, 2: 13 + len(payload)})
        resp = bytes([2]) + struct.pack("<I", len(resp_proto)) + resp_proto
        if size_requested > 13 + len(resp):
            resp += b"\xfe" * (size_requested - 13 - len(resp))
        self.sock.sendto(Packet(0, 0, 0, 0, 0, 0, stream_timestamp(), resp).encode(), self.addr)
        if self._pings_logged < 5:
            log(self.logf, "<- PingRequest seq=%d size=%d -> PingResponse" % (seq, size_requested))
            self._pings_logged += 1

    @staticmethod
    def _decode_varint_fields(data):
        fields = {}
        i = 0
        while i < len(data):
            tag = data[i]; i += 1
            fnum = tag >> 3; wtype = tag & 7
            if wtype == 0:
                val = 0; shift = 0
                while i < len(data):
                    b = data[i]; i += 1
                    val |= (b & 0x7f) << shift; shift += 7
                    if not (b & 0x80):
                        break
                fields[fnum] = val
            elif wtype == 2:
                vlen = 0; shift = 0
                while i < len(data):
                    b = data[i]; i += 1
                    vlen |= (b & 0x7f) << shift; shift += 7
                    if not (b & 0x80):
                        break
                fields[fnum] = data[i:i + vlen]
                i += vlen
            else:
                break
        return fields

    @staticmethod
    def _encode_varints(fields):
        out = bytearray()
        for fnum, val in sorted(fields.items()):
            out.append((fnum << 3) | 0)  # wire type 0 = varint
            while val > 0x7f:
                out.append((val & 0x7f) | 0x80)
                val >>= 7
            out.append(val & 0x7f)
        return bytes(out)

    def _reassemble(self, pkt):
        if pkt.type in (PT_RELIABLE, PT_UNRELIABLE):
            if pkt.frag > 0:   # first packet of a fragmented message: frag = number of continuation packets
                self.frags[pkt.channel] = [pkt.frag, [pkt.payload]]
                return None
            return pkt.payload
        entry = self.frags.get(pkt.channel)
        if entry is None:
            return None
        entry[1].append(pkt.payload)
        if len(entry[1]) - 1 >= entry[0]:
            del self.frags[pkt.channel]
            return b"".join(entry[1])
        return None

    def on_control(self, msg_type, body):
        name = MSG_NAMES.get(msg_type, str(msg_type))
        if msg_type not in UNENCRYPTED_CONTROL:
            try:
                _seq, body = slcrypto.decrypt_control(self.key, body)
            except ValueError as e:
                log(self.logf, "<- %s (decrypt failed: %s)" % (name, e))
                return None
        log(self.logf, "<- %s" % name)
        if msg_type == rp.k_EStreamControlServerHandshake:
            log(self.logf, "   %s" % str(rp.CServerHandshakeMsg.FromString(body)).replace("\n", " "))
            self.send_control(rp.k_EStreamControlAuthenticationRequest, rp.CAuthenticationRequestMsg(
                token=slcrypto.auth_token(self.key), version=rp.k_EStreamVersionCurrent,
                steamid=self.state.get("steamid", 0)))
        elif msg_type == rp.k_EStreamControlAuthenticationResponse:
            r = rp.CAuthenticationResponseMsg.FromString(body)
            log(self.logf, "   %s" % str(r).replace("\n", " "))
            if r.result != rp.CAuthenticationResponseMsg.SUCCEEDED:
                return "disconnect"
        elif msg_type == rp.k_EStreamControlNegotiationInit:
            init = rp.CNegotiationInitMsg.FromString(body)
            log(self.logf, "   %s" % str(init).replace("\n", " "))
            self.send_control(rp.k_EStreamControlNegotiationSetConfig, self.build_set_config(init))
        elif msg_type == rp.k_EStreamControlNegotiationSetConfig:
            cfg = rp.CNegotiationSetConfigMsg.FromString(body)
            log(self.logf, "   server config: %s" % str(cfg.config).replace("\n", " "))
            self.send_control(rp.k_EStreamControlNegotiationComplete, rp.CNegotiationCompleteMsg())
        elif msg_type == rp.k_EStreamControlStartVideoData:
            m = rp.CStartVideoDataMsg.FromString(body)
            self.media_channels[m.channel] = "video"
            log(self.logf, "   video channel=%d codec=%s %dx%d codec_data=%s" % (
                m.channel, rp.EStreamVideoCodec.Name(m.codec), m.width, m.height, m.codec_data.hex()))
        elif msg_type == rp.k_EStreamControlStartAudioData:
            m = rp.CStartAudioDataMsg.FromString(body)
            self.media_channels[m.channel] = "audio"
            log(self.logf, "   audio channel=%d codec=%s %dHz x%d" % (
                m.channel, rp.EStreamAudioCodec.Name(m.codec), m.frequency, m.channels))
        return None

    @staticmethod
    def build_set_config(init):
        def pick(preferred, offered, none):
            return preferred if preferred in offered else (offered[0] if offered else none)

        audio = pick(rp.k_EStreamAudioCodecOpus, init.supported_audio_codecs, rp.k_EStreamAudioCodecNone)
        video = pick(rp.k_EStreamVideoCodecH264, init.supported_video_codecs, rp.k_EStreamVideoCodecNone)
        return rp.CNegotiationSetConfigMsg(
            config=rp.CNegotiatedConfig(reliable_data=init.reliable_data,
                                        selected_audio_codec=audio, selected_video_codec=video),
            streaming_client_config=rp.CStreamingClientConfig(
                desired_resolution_x=640, desired_resolution_y=480,
                desired_framerate_numerator=60, desired_framerate_denominator=1,
                enable_hardware_decoding=True, audio_channels=2),
            streaming_client_caps=rp.CStreamingClientCaps(
                system_info="Anbernic RG40XXH (MuOS) prototype",
                maximum_decode_bitrate_kbps=15000,
                supported_audio_codecs=[rp.k_EStreamAudioCodecOpus],
                supported_video_codecs=[rp.k_EStreamVideoCodecH264]),
        )

    def run(self, seconds):
        self.connect()
        self.send_control(rp.k_EStreamControlClientHandshake,
                          rp.CClientHandshakeMsg(info=rp.CStreamingClientHandshakeInfo()))
        end = time.monotonic() + seconds
        next_keepalive = time.monotonic() + 1
        while time.monotonic() < end:
            pkt = self.recv()
            if pkt and self.handle(pkt) == "disconnect":
                break
            if time.monotonic() >= next_keepalive and self.media_channels:
                self.send_control(rp.k_EStreamControlKeepAlive, rp.CKeepAliveMsg())
                next_keepalive = time.monotonic() + 1
        self._send(PT_DISCONNECT, 0, b"", packet_id=0)
        log(self.logf, "packet counts: %s" % {k: v for k, v in self.counts.items()})
        for f in self.out.values():
            f.close()


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    pc_ip = sys.argv[1]
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 15
    state = slnet.load_state()
    if not state.get("secret"):
        sys.exit("not paired; run pair.py first")
    CAPTURE_DIR.mkdir(parents=True, exist_ok=True)
    with open(CAPTURE_DIR / "session.log", "w") as logf:
        port, key = request_stream(state, pc_ip, logf)
        log(logf, "session key: %d bytes" % len(key))
        Session(pc_ip, port, key, state, logf).run(seconds)


if __name__ == "__main__":
    main()
