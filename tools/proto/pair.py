"""Pair this device with Steam through Valve's Remote Client service (docs/PROTOCOLO.md, section 4).

Usage: uv run --with protobuf --with cryptography --with websockets python -I tools/proto/pair.py [pc_ip ...]
(pc_ip defaults to 255.255.255.255; LAN discovery runs in parallel with the WebSocket.)

Prints a PIN; enter it on the PC in Steam > Settings > Remote Play > Pair Steam Link.
On success the shared secret is written to state/device.json.
"""
import asyncio
import datetime
import json
import os
import socket
import struct
import sys
import urllib.request

import websockets

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import slcrypto  # noqa: E402
import slnet  # noqa: E402

import steammessages_remoteclient_discovery_pb2 as disc  # noqa: E402  (path set by slnet)
import steammessages_remoteclient_service_messages_pb2 as svc  # noqa: E402

CM_LIST_URL = "https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets"
PIN_TIMEOUT = 600


def cm_endpoints():
    with urllib.request.urlopen(CM_LIST_URL, timeout=15) as r:
        data = json.load(r)
    return [s["endpoint"] for s in data["response"]["serverlist"] if s.get("type") == "websockets"]


class Session:
    def __init__(self, ws):
        self.ws = ws

    async def send(self, **kwargs):
        await self.ws.send(svc.CCMRemoteClient_ClientMessage(**kwargs).SerializeToString())

    async def recv(self, timeout=30):
        raw = await (asyncio.wait_for(self.ws.recv(), timeout) if timeout else self.ws.recv())
        msg = svc.CCMRemoteClient_ServerMessage.FromString(raw)
        print("<- %s" % ", ".join(f.name for f, _ in msg.ListFields()))
        return msg

    async def expect(self, field, timeout=30):
        while True:
            msg = await self.recv(timeout)
            if msg.HasField(field):
                return getattr(msg, field)
            if msg.HasField("result") and msg.result != 1:
                raise RuntimeError("server returned result=%d while waiting for %s" % (msg.result, field))


def build_authorization_request(state, pin_str, euniverse, kx, secret):
    device_id = state["device_id"]
    ticket = disc.CMsgRemoteDeviceAuthorizationRequest.CKeyEscrow_Ticket(
        password=pin_str.encode(),
        identifier=device_id,
        payload=secret,
        usage=disc.CMsgRemoteDeviceAuthorizationRequest.k_EKeyEscrowUsageStreamingDevice,
        device_name=state["device_name"],
        device_model="Anbernic RG40XXH",
        device_serial="",
        device_provisioning_id=0,
    )
    return disc.CMsgRemoteDeviceAuthorizationRequest(
        device_token=slcrypto.symmetric_encrypt(secret, struct.pack("<Q", device_id)),
        device_name=state["device_name"],
        encrypted_request=slcrypto.rsa_oaep_encrypt(slnet.universe_key(euniverse), ticket.SerializeToString()),
        auth_key=slcrypto.xor32(kx.public, slcrypto.sha256(pin_str.encode())),
        request_id=1,
    )


def finish_key_exchange(resp, pin_str, kx, device_id):
    """Returns the new shared secret, or None if the PC's proof does not verify."""
    if len(resp.auth_key) != 32:
        return None
    pc_pub = slcrypto.xor32(resp.auth_key, slcrypto.sha256(pin_str.encode()))
    shared = kx.shared_key(pc_pub)
    try:
        ok = slcrypto.symmetric_decrypt(shared, resp.device_token) == struct.pack("<Q", device_id)
    except ValueError:
        ok = False
    return shared if ok else None


async def pair(lan_targets):
    state = slnet.load_state()
    endpoints = cm_endpoints()
    url = "wss://%s/remoteplay/" % endpoints[0]
    print("connecting to %s" % url)
    async with websockets.connect(url, max_size=None, open_timeout=20) as ws:
        s = Session(ws)
        await s.send(create_session_request=svc.CRemoteClient_CreateSession_Request())
        created = await s.expect("create_session_response")
        session_id, euniverse = created.session_id, created.euniverse or 1
        print("session_id=%d euniverse=%d" % (session_id, euniverse))

        await s.send(start_pairing_request=svc.CRemoteClient_StartPairing_Request(session_id=session_id))
        pin_str = "%04u" % (await s.expect("start_pairing_response")).pin
        print("\n================  PIN: %s  ================" % pin_str)
        print("No PC: Steam > Configurações > Remote Play > Parear Steam Link, e digite o PIN.\n")

        kx = slcrypto.KeyExchange()
        secret = b"\x01" + os.urandom(31)
        req = build_authorization_request(state, pin_str, euniverse, kx, secret)
        await s.send(set_pairing_info_request=svc.CRemoteClient_SetPairingInfo_Request(
            session_id=session_id, device_id=state["device_id"], request=req.SerializeToString()))

        # Replies may arrive over the WebSocket (reply_packet) or straight over the LAN (UDP 27036),
        # so listen on both while broadcasting discovery with our device_id like the official client.
        loop = asyncio.get_running_loop()
        inbox = asyncio.Queue()
        transport = await open_lan(loop, inbox)
        tasks = [asyncio.create_task(ws_reader(s, inbox)),
                 asyncio.create_task(discovery_sender(transport, state["device_id"], lan_targets))]
        try:
            deadline = loop.time() + PIN_TIMEOUT
            while True:
                left = deadline - loop.time()
                try:
                    source, payload, addr = await asyncio.wait_for(inbox.get(), max(left, 0.01))
                except TimeoutError:
                    sys.exit("timed out waiting for the PIN to be entered on the PC")
                parsed = slnet.unpack(payload)
                if not parsed:
                    print("[%s %s] unframed payload: %s" % (source, addr, payload[:64].hex()))
                    continue
                hdr, body = parsed
                if hdr.msg_type not in (disc.k_ERemoteClientBroadcastMsgStatus,
                                        disc.k_ERemoteClientBroadcastMsgDiscovery):
                    print("[%s %s] msg_type=%s client_id=%d" % (
                        source, addr, disc.ERemoteClientBroadcastMsg.Name(hdr.msg_type), hdr.client_id))
                if hdr.msg_type != disc.k_ERemoteDeviceAuthorizationResponse:
                    continue
                resp = disc.CMsgRemoteDeviceAuthorizationResponse.FromString(body)
                print("result=%s steamid=%d auth_key=%d bytes" % (
                    disc.ERemoteDeviceAuthorizationResult.Name(resp.result), resp.steamid, len(resp.auth_key)))
                if resp.result != disc.k_ERemoteDeviceAuthorizationSuccess:
                    sys.exit("pairing failed")
                shared = finish_key_exchange(resp, pin_str, kx, state["device_id"])
                if resp.auth_key and shared is None:
                    sys.exit("key exchange proof did not verify (wrong PIN?)")
                state["secret"] = shared or secret
                state["steamid"] = resp.steamid
                state["paired_at"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
                slnet.save_state(state)

                header = disc.CMsgRemoteClientBroadcastHeader(
                    client_id=state["device_id"],
                    msg_type=disc.k_ERemoteDeviceAuthorizationConfirmed,
                    device_token=slcrypto.symmetric_encrypt(state["secret"], struct.pack("<Q", state["device_id"])))
                confirm = slnet.pack(header, disc.CMsgRemoteDeviceAuthorizationConfirmed(
                    result=disc.k_ERemoteDeviceAuthorizationSuccess))
                if source == "udp":
                    transport.sendto(confirm, addr)
                else:
                    await s.send(remote_packet=svc.CRemoteClient_RemotePacket_Notification(
                        session_id=session_id, steamid=resp.steamid, payload=confirm))
                print("paired! secret saved to %s (key exchange: %s)" % (slnet.STATE_FILE, bool(shared)))
                return
        finally:
            for t in tasks:
                t.cancel()
            transport.close()


class LanProtocol(asyncio.DatagramProtocol):
    def __init__(self, inbox):
        self.inbox = inbox

    def datagram_received(self, data, addr):
        self.inbox.put_nowait(("udp", data, addr))


async def open_lan(loop, inbox):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    try:
        sock.bind(("", slnet.PORT))
    except OSError:
        print("warning: UDP %d busy, using an ephemeral port" % slnet.PORT)
        sock.bind(("", 0))
    transport, _ = await loop.create_datagram_endpoint(lambda: LanProtocol(inbox), sock=sock)
    return transport


async def ws_reader(s, inbox):
    while True:
        msg = await s.recv(timeout=None)
        if msg.HasField("offline"):
            print("offline notification: %s" % msg.offline)
        if msg.HasField("reply_packet"):
            for payload in msg.reply_packet.payload:
                inbox.put_nowait(("ws", payload, None))


async def discovery_sender(transport, device_id, targets):
    seq = 0
    while True:
        seq += 1
        header = disc.CMsgRemoteClientBroadcastHeader(
            client_id=device_id, msg_type=disc.k_ERemoteClientBroadcastMsgDiscovery)
        packet = slnet.pack(header, disc.CMsgRemoteClientBroadcastDiscovery(seq_num=seq))
        for t in targets:
            transport.sendto(packet, (t, slnet.PORT))
        await asyncio.sleep(2)


if __name__ == "__main__":
    asyncio.run(pair(sys.argv[1:] or ["255.255.255.255"]))
