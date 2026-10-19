"""Crypto primitives used by the Steam Link client (see docs/PROTOCOLO.md, 4.4 and 6.2)."""
import hashlib
import hmac
import os
import struct

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding, x25519
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes


def _pkcs7_pad(data):
    n = 16 - len(data) % 16
    return data + bytes([n]) * n


def _pkcs7_unpad(data):
    n = data[-1]
    if not 1 <= n <= 16 or data[-n:] != bytes([n]) * n:
        raise ValueError("bad padding")
    return data[:-n]


def _aes_ecb(key, block, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.ECB())
    op = c.decryptor() if decrypt else c.encryptor()
    return op.update(block) + op.finalize()


def _aes_cbc(key, iv, data, decrypt=False):
    c = Cipher(algorithms.AES(key), modes.CBC(iv))
    op = c.decryptor() if decrypt else c.encryptor()
    return op.update(data) + op.finalize()


def symmetric_encrypt(key, plain, iv=None):
    """Valve CCrypto::SymmetricEncrypt: AES-ECB(key, IV) || AES-CBC(key, IV, PKCS7(plain))."""
    iv = iv or os.urandom(16)
    return _aes_ecb(key, iv) + _aes_cbc(key, iv, _pkcs7_pad(plain))


def symmetric_decrypt(key, data):
    iv = _aes_ecb(key, data[:16], decrypt=True)
    return _pkcs7_unpad(_aes_cbc(key, iv, data[16:], decrypt=True))


def encrypt_control(key, seq, payload):
    """Encrypted control message body: HMAC-MD5 IV || AES-CBC(key, IV, PKCS7(seq_le64 || payload))."""
    plain = struct.pack("<Q", seq) + payload
    iv = hmac.new(key, plain, hashlib.md5).digest()
    return iv + _aes_cbc(key, iv, _pkcs7_pad(plain))


def decrypt_control(key, body):
    """Returns (seq, payload); raises ValueError if the HMAC IV does not verify."""
    iv = body[:16]
    plain = _pkcs7_unpad(_aes_cbc(key, iv, body[16:], decrypt=True))
    if not hmac.compare_digest(iv, hmac.new(key, plain, hashlib.md5).digest()):
        raise ValueError("control message HMAC mismatch")
    (seq,) = struct.unpack_from("<Q", plain)
    return seq, plain[8:]


def auth_token(session_key):
    return hmac.new(session_key, b"Steam In-Home Streaming", hashlib.sha256).digest()


def sha256(data):
    return hashlib.sha256(data).digest()


def xor32(a, b):
    return bytes(x ^ y for x, y in zip(a, b, strict=True))


def rsa_oaep_encrypt(pem, data):
    """RSA-OAEP(SHA1) in blocks of RSA_size - 42 bytes, concatenated (servermanager 0x1824c4)."""
    key = serialization.load_pem_public_key(pem)
    block = key.key_size // 8 - 42
    pad = padding.OAEP(mgf=padding.MGF1(hashes.SHA1()), algorithm=hashes.SHA1(), label=None)
    return b"".join(key.encrypt(data[i:i + block], pad) for i in range(0, len(data), block))


class KeyExchange:
    """X25519 key pair; shared key = SHA256(X25519(priv, peer_pub)) (keypair.cpp / 0x17f4e4)."""

    def __init__(self):
        self._priv = x25519.X25519PrivateKey.generate()
        self.public = self._priv.public_key().public_bytes(
            serialization.Encoding.Raw, serialization.PublicFormat.Raw)

    def shared_key(self, peer_public):
        peer = x25519.X25519PublicKey.from_public_bytes(peer_public)
        return sha256(self._priv.exchange(peer))
