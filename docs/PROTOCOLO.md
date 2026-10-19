# Engenharia reversa: Steam Link (Raspberry Pi arm64)

Referência analisada: `steamlink-rpi-bookworm-arm64-1.4.2.321.tar.gz`
(obtido via `https://media.steampowered.com/steamlink/rpi/bookworm/arm64/public_build.txt`,
URL tirada do launcher `steamlink_1.0.16_all.deb` do repositório do Raspberry Pi).

## 1. Anatomia do app

| Componente | O que é |
|---|---|
| `bin/shell` (20 MB, AArch64, stripped) | O app inteiro: UI (Qt5 + SDL3) **e** o cliente de streaming, linkado estaticamente |
| `lib/libSDL3*.so` | Janela, render, áudio, gamepad (SDL3) |
| `Qt-5.14.1/` | UI de menus (linuxfb / eglfs / xcb) |
| Dependência `libavcodec59` | **Decodificação de vídeo via FFmpeg** (não usa API proprietária do RPi) |
| `libdrm`, `libepoxy`, `libgbm`, `libegl` | Apresentação de frames via DRM/KMS + EGL |
| `bin/vhusbdarmslpi*` | VirtualHere (compartilhamento USB), opcional |
| OpenSSL estático (`crypto_rsa_openssl.cpp`, `AES_GCM_*`, `AES_ECB_*`) | Criptografia do Valve `src/common` |

Consequência para o RG40XXH: o caminho natural é **SDL + FFmpeg (com decodificação por hardware via V4L2/Cedrus do H700) + DRM/KMS**, a mesma arquitetura do app oficial, sem Qt.

## 2. Protobufs recuperados

O binário contém os `FileDescriptorProto` serializados. Extraídos com
`tools/extract_protos.py` para `re/protos/` (22 arquivos). Os principais:

- `steammessages_remoteclient_discovery.proto`: descoberta, pareamento e pedido de streaming
- `steammessages_remoteplay.proto`: sessão de streaming (106 mensagens, 26 enums)
- `steammessages_virtualcontroller.proto`, `steammessages_hiddevices.proto`: controles

## 3. Descoberta (VERIFICADO com o Steam real)

UDP porta **27036**, broadcast ou unicast. Formato do pacote (little-endian):

```
int32   -1                (0xFFFFFFFF)
uint32  0xA05F4C21        (assinatura; conferida no disassembly em 0x158d1c)
uint32  header_len
bytes   CMsgRemoteClientBroadcastHeader { client_id, msg_type, instance_id, ... }
uint32  body_len
bytes   corpo (depende de msg_type)
```

O cliente envia `msg_type = Discovery` com `CMsgRemoteClientBroadcastDiscovery{seq_num}`.
O Steam responde com `msg_type = Status` e `CMsgRemoteClientBroadcastStatus` (hostname,
`connect_port`, usuários com `steamid` e `auth_key_id`, IPs, versão 8 / min_version 6...).

Teste: `uv run --with protobuf python -I tools/discover.py <ip>`. O PC `FrancoPC` respondeu.

## 4. Pareamento (VERIFICADO em 2026-10-16 com `tools/proto/pair.py`)

Teste real: PIN gerado pelo backend, digitado no Steam do PC → `AuthorizationResponse` chegou
pelo **WebSocket** (`reply_packet`), `result=Success`, `auth_key` de 32 bytes, e a prova da troca
de chaves X25519 (decifrar `device_token` == `device_id`) **conferiu**. Segredo salvo em
`state/device.json` (contém chave secreta — não versionar).

Código-fonte original: `src/streaming/client/servermanager.cpp`. Endereços abaixo são do
`bin/shell` 1.4.2.321.

### 4.1 Canal: backend da Valve via WebSocket

O pareamento **não é só LAN**. O app fala com o "Remote Client service" da Valve:

1. `GET https://api.steampowered.com/ISteamDirectory/GetCMListForConnect/v1/?cellid=0&cmtype=websockets`
2. Conecta em `wss://<cm>/remoteplay/` (`0x1500c8`).
3. Cada frame binário = protobuf puro, sem cabeçalho (`0x14fe90`: `ByteSizeLong` → `SerializeToArray` → `ws->Send`).
   - cliente → servidor: `CCMRemoteClient_ClientMessage` (oneof: `create_session_request=1`,
     `start_pairing_request=2`, `set_pairing_info_request=3`, `cancel_pairing_request=4`,
     `register_status_update=5`, `unregister_status_update=6`, `remote_packet=7`)
   - servidor → cliente: `CCMRemoteClient_ServerMessage` (`result=1`, `create_session_response=2`,
     `start_pairing_response=3`, `set_pairing_info_response=4`, `cancel_pairing_response=5`,
     `reply_packet=6`, `offline=7`)

Fluxo:

```
CreateSession{}                    → CreateSession_Response{session_id, euniverse}
StartPairing{session_id}           → StartPairing_Response{pin}        (o PIN vem do servidor!)
   [app gera chaves e monta CMsgRemoteDeviceAuthorizationRequest, ver 4.2]
SetPairingInfo{session_id, device_id, request=<AuthorizationRequest serializada>}   (0x150b10)
   [usuário digita o PIN no Steam do PC → o PC busca o pedido]
reply_packet{payload[]}            → contém a CMsgRemoteDeviceAuthorizationResponse (ver 4.3)
```

### 4.2 Montagem do `CMsgRemoteDeviceAuthorizationRequest` (`0x15b460` → `0x1581b0`)

Estado local:

| Item | Como é gerado |
|---|---|
| `device_id` (uint64) | 8 bytes aleatórios, persistidos em `device_id.txt` (`0x153320`) |
| PIN | `"%4.4u"` do PIN recebido (`0x153c70`); `0x153cb0` sabe gerar localmente `rand % 10000` |
| Par X25519 | 32 bytes aleatórios → `CECKeyExchangePrivateKey` + pública (`0x188450`, `keypair.cpp`) |
| `secret` (32 bytes) | aleatório, primeiro byte ≠ 0 (`0x15b4d4`) |

Campos da requisição:

- `device_token` = `SymmetricEncrypt(secret, device_id_le64)` (`0x153d14`)
- `device_name` = nome do dispositivo
- `encrypted_request` = `RSA-OAEP-SHA1(chave_universo, CKeyEscrow_Ticket serializado)`, em
  blocos de `RSA_size - 42` bytes (`0x1824c4`: `d2i_RSA_PUBKEY`, `RSA_public_encrypt(..., padding=4)`)
- `auth_key` = `X25519_pub XOR SHA256(PIN_ascii)` (32 bytes, `0x1586f4`)
- `request_id` = contador local (`this+0x200`)

`CKeyEscrow_Ticket`:

| Campo | Valor |
|---|---|
| `password` | PIN em ASCII (4 dígitos) |
| `identifier` | `device_id` |
| `payload` | `secret` (32 bytes) |
| `usage` | `0` (`k_EKeyEscrowUsageStreamingDevice`) |
| `device_name` / `device_model` / `device_serial` | máx. 64 / 64 / 32 caracteres |
| `device_provisioning_id` | `this+0x1b8` |

Chave RSA por universo (`euniverse` do Status/CreateSession): `1` → `re/keys/universe1_public.pem`,
`2` → `universe2_beta.pem`, `3..4` → `universe3plus.pem`. Todas são RSA-2048 com e=17.

### 4.3 Resposta: `BHandleKeyExchangeAuthorization` (`0x15b054`)

Recebe `CMsgRemoteDeviceAuthorizationResponse` com `auth_key` (32 bytes) e `device_token`:

```
pc_pub   = auth_key XOR SHA256(PIN_ascii)
shared   = SHA256( X25519(nossa_priv, pc_pub) )          (0x17f4e4, curve25519_donna 0x22e4c0)
check    = SymmetricDecrypt(shared, device_token)  == device_id_le64 ?   (8 bytes)
se OK:   secret := shared   (persistido; é a chave do dispositivo pareado)
```

No máximo 3 tentativas ("too many PIN attempts").

### 4.4 Primitivas criptográficas (Valve `src/common`, OpenSSL estático)

- **SymmetricEncrypt** (`0x180520` → `0x17ffb0`, `crypto_aescbc_openssl.cpp`):
  `IV = rand(16)`; saída = `AES-ECB(key, IV) || AES-CBC(key, IV, PKCS7(dados))`; chave de 16 ou 32 bytes.
  Tamanho = `16 + ((n + 16) & ~15)`.
- **SHA-256**: `0x180a94` (contexto de 0x70 bytes, constantes em `0xe7f7c0`).
- **X25519**: `crypto_25519_donna.cpp`.

### 4.5 Mensagens LAN depois do pareamento (`0x15a1c4`)

Todas as mensagens `ERemoteClientBroadcastMsg` (AuthorizationRequest=3, StreamingRequest=5,
ProofResponse=8, StreamingCancel=10, Confirmed=14...) usam **o mesmo framing UDP 27036 da
descoberta**, em unicast para o PC. Cabeçalho:

- `client_id` = `device_id`
- `msg_type` = tipo
- `device_token` = `SymmetricEncrypt(secret, device_id_le64)` (em unicast)

Quando o PC não está na LAN, o mesmo conteúdo vai pelo WebSocket (`remote_packet`/`reply_packet`).
O `AuthorizationRequest` também pode ir direto pela LAN (`0x15b770`), além do caminho via backend.

### 4.6 Prova de posse do segredo (`0x15c754`)

`CMsgRemoteDeviceProofRequest{challenge, request_id, update_secret}` →
`CMsgRemoteDeviceProofResponse{response = SymmetricEncrypt(secret, challenge), request_id, updated_secret}`.

## 5. Pedido de streaming (`0x15b8b0` → msg 5)

`CMsgRemoteDeviceStreamingRequest` leva `request_id`, `device_token`, resolução e fps máximos,
`supported_transport` (para LAN: `k_EStreamTransportUDP`), `stream_interface`, gamepads etc.

Resposta `CMsgRemoteDeviceStreamingResponse{result, port, encrypted_session_key, transport}`
(handler `0x15cca0`):

```
session_key = SymmetricDecrypt(secret, encrypted_session_key)     (0x153ed0)
```

Se `result = k_ERemoteDeviceStreamingPINRequired`, o PC exige PIN (campo `pin` do request).

## 6. Sessão de streaming (UDP, `src/streaming/common/stream*.cpp`)

### 6.1 Pacote (`streamsocket.cpp` 0x172dd0 / `streamnetwork.cpp`)

Cabeçalho fixo de **13 bytes**, little-endian:

| Offset | Tam. | Campo |
|---|---|---|
| 0 | 1 | bits 0–6: tipo do pacote; bit 7: tem CRC |
| 1 | 1 | (reservado / repetição) |
| 2 | 1 | ID de conexão de origem |
| 3 | 1 | ID de conexão de destino |
| 4 | 1 | canal (`EStreamChannel`; < 32) |
| 5 | 2 | fragmento: no 1º pacote = nº de fragmentos; nos seguintes = índice |
| 7 | 2 | ID do pacote (sequência, 16 bits com wraparound) |
| 9 | 4 | timestamp de envio |
| 13 | n | payload |
| fim | 4 | **CRC32C** (Castagnoli) de tudo antes, se bit 7 do byte 0 |

Tamanho mínimo 13; payload máximo por pacote = MTU − 17; mensagens maiores são fragmentadas.

Tipos de pacote (0–12) e para onde o dispatcher (`0x171764`, `0x16f890`) os manda:

| Tipo | Significado (inferido) |
|---|---|
| 0 | **Unconnected**: ping de descoberta de MTU no canal 0, src=dst=0 (seção 6.3) |
| 1 | **Connect**: payload opcional u32 = `CRC32C("Connect")` = `0x3c8f3dc7` (pede modo CRC) |
| 2 | **Connected**: 17 bytes; payload u32 = timestamp do Connect ecoado (RTT); byte[2] = ID do servidor |
| 3 / 4 | dados não confiáveis / fragmento |
| 5 / 6 | dados confiáveis / fragmento |
| 7 | ACK (janela confiável) |
| 8 | NACK / perda (janela confiável) |
| 9 | **Disconnect** (também usado como rejeição) |
| 10 | perda na janela não confiável |
| 11 / 12 | controle de fluxo / ping (a confirmar) |

Fluxo de conexão:

```
cliente → Connect   (tipo 1, src=meu_id, dst=0, canal 0, payload 0x3c8f3dc7, com CRC)
PC      → Connected (tipo 2, src=id_PC, dst=meu_id, payload = timestamp ecoado)
```

### 6.2 Mensagens de controle (canal 1, confiável; `streamclient.cpp`)

Payload = `[tipo EStreamControlMessage: 1 byte] [protobuf]`.

**Em claro** (`0x2a5560`): tipos 1, 2, 6 e 7 (Authentication* e *Handshake). **Todo o resto é
cifrado** (`0x169280`):

```
plain = seq_u64_le || protobuf          (seq = contador por sessão, começa em 0)
iv    = HMAC-MD5(session_key, plain)    (16 bytes)
corpo = iv || AES-CBC(session_key, iv, PKCS7(plain))
```

Sequência:

```
ClientHandshake(6){info{network_test}}          → ServerHandshake(7){info{mtu}}
AuthenticationRequest(1){
    token   = HMAC-SHA256(session_key, "Steam In-Home Streaming"),
    version = 1, steamid }                       → AuthenticationResponse(2){result, version}
NegotiationInit(3) → NegotiationSetConfig(4) → NegotiationComplete(5)     (cifrados)
StartAudioData(50) / StartVideoData(52) → frames nos canais de dados (≥3) + KeepAlive(9)
```

Input: `CInputKey*`, `CInputMouse*` e controles via `CRemoteHIDMsg` / virtualcontroller.
Codecs: vídeo H.264(4) / HEVC(5) / AV1(8); áudio Opus(3).

### 6.3 Ping de MTU antes do ServerHandshake (VERIFICADO 2026-10-16)

Depois do ClientHandshake o PC **não manda ACK nem ServerHandshake de imediato**: ele envia
pacotes tipo 0 (Unconnected, canal 0, src=dst=0) com um `CDiscoveryPingRequest` e só segue em
frente quando o cliente responde. Sem resposta, desiste e manda Disconnect após ~4,5 s.
(Confirmado com o ihslib, `ch_discovery.c`.)

```
payload = [EStreamDiscoveryMessage: 1 byte] [tamanho do protobuf: u32 LE] [protobuf] [padding 0xfe]
PC → 1 = PingRequest  {sequence=1, packet_size_requested=2}   tamanhos 512, 1024, 1280, 1408, 1440…
cli → 2 = PingResponse {sequence (ecoado), packet_size_received = 13 + corpo recebido sem CRC}
```

A resposta é preenchida com `0xfe` até cabeçalho (13) + corpo = `packet_size_requested`; o CRC
vem por cima. Pedidos com `packet_size_requested=0` são respondidos sem padding. Ao fim, o PC
manda `ServerHandshake{mtu=1468}`. O ClientHandshake nunca recebe ACK; o ServerHandshake vale como
confirmação.

### 6.4 Negociação e mídia (VERIFICADO com o cliente C)

- `NegotiationSetConfig`: `config{reliable_data, audio=Opus(3), video=H264(4), enable_remote_hid}`,
  `streaming_client_config{desired_resolution 640x480, fps 60/1, bitrate, audio_channels 2}`,
  `streaming_client_caps{system_info, maximum_decode_bitrate_kbps, codecs}`. O PC responde com
  o seu SetConfig; o cliente manda `NegotiationComplete` e o streaming começa.
- Mensagem de dados (canais ≥ 3, pacotes tipo 3/4; fragmentos com IDs consecutivos):
  `[1 = k_EStreamDataPacket] [id u16] [timestamp u32] [input_mark u16] [input_recv_ts u32]` (13 bytes).
- Áudio: depois do cabeçalho vem o pacote Opus cru (48 kHz estéreo).
- Vídeo: mais 7 bytes `[sequence u16] [flags u8] [subframe_start u16] [subframe_end u16]`, depois
  o H.264 Annex-B. Flags: `0x01` precisa start code, `0x02` precisa escapar (emulation prevention),
  `0x04` avança subquadro, `0x08` fim do quadro, `0x10` keyframe, `0x20` cifrado
  (AES-CBC, chave de sessão, IV zero). `sequence` cresce por fragmento; lacuna → pedir keyframe.
- Pedir keyframe: pacote não confiável no canal de vídeo com payload `[2 = k_EStreamDataLost]`.
- `SetActivity{activity=4}` = *secure desktop* (tela de bloqueio/UAC do Windows): o Steam captura
  preto nesse estado.
- O PC não manda ACK para todas as mensagens confiáveis que processa (NegotiationComplete, várias
  RemoteHID); o ihslib também desiste após 20 retransmissões.

### 6.5 Controle via HID remoto (VERIFICADO: o Steam abre o dispositivo)

`k_EStreamControlRemoteHID(106)` carrega `CRemoteHIDMsg{data = CHIDMessageFromRemote/ToRemote}`.
O cliente anuncia um "gamepad SDL virtual" (`is_generic_gamepad`, usage 1/5, ostype −203,
`path "sdl://0"`); o PC faz DeviceOpen → GetFeatureReport(0x04 caps: controllerType 31 = Xbox 360)
→ StartInputReports. Relatórios completos de 48 bytes: eixos int16 LX, LY, RX, RY, LT, RT (ordem
SDL), u32 flags, u16 botões (bit = `SDL_GameControllerButton`), versão pedida no byte 27
(escrita `0x09`). Rumble chega como escrita `0x01 {lo u16, hi u16, ms u32}`.

## 7. Próximos passos

1. ~~Rotinas de autorização / pareamento~~ (feito, seção 4).
2. ~~Prova de posse, transporte LAN, pedido de streaming, chave de sessão~~ (feito, 4.5–5).
3. ~~Cabeçalho UDP e cifra das mensagens de controle~~ (feito, 6.1–6.2).
4. ~~Negociação, framing de vídeo/áudio, ACKs, HID~~ (feito, 6.3–6.5).
5. ~~Cliente nativo (C + SDL + FFmpeg) para o muOS~~ (`client/`, ver `README.md`).
6. Pareamento por PIN direto no aparelho (hoje: `tools/proto/pair.py` no PC + copiar `device.json`).
7. Decodificação H.264 por hardware (VPU do H700); hoje é FFmpeg por software, 60 fps em 640x360.

## Ferramentas

- `tools/extract_protos.py`: extrai os `.proto` embutidos.
- `tools/discover.py`: descoberta na LAN.
- `tools/xref.py`: referências cruzadas AArch64 (`index`, `str`, `func`, `addr`).
