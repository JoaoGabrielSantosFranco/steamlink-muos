# Steam Link para muOS

Cliente **Steam Link nativo** para portáteis com [muOS](https://muos.dev), feito para o
**Anbernic RG40XXH**. Jogue os jogos do Steam do seu PC pela rede local usando o próprio
protocolo do Steam Remote Play: no PC fica só o Steam, sem Sunshine, Moonlight ou outro servidor.

> **Status: experimental.** O cliente já conecta, autentica, recebe vídeo (H.264, 60 fps) e áudio
> (Opus) e o Steam reconhece os botões como um controle. Foi testado contra um Steam real
> (Windows) a partir de um PC e do binário ARM64 em emulação; **ainda não foi validado no aparelho
> físico**. Relatos e logs são bem-vindos.

## Recursos

- Encontra sozinho os PCs com Steam na rede (descoberta UDP 27036).
- Streaming de vídeo H.264 até 60 fps, ajustado à tela (faixas pretas quando o formato não bate).
- Áudio Opus 48 kHz estéreo, com baixa latência.
- Botões do aparelho viram um **controle Xbox 360** no PC (HID remoto do Steam). MENU = botão Steam.
- Avisos na tela: PC não encontrado, PC recusou, PC na tela de bloqueio do Windows, conexão perdida.
- Um único binário ARM64; depende só da SDL2 do muOS e de glibc ≥ 2.29.

## Aparelhos

| Aparelho | Situação |
|---|---|
| Anbernic RG40XXH | alvo do projeto (aguardando teste no aparelho) |
| Outros com muOS e chip H700 (RG35XX Plus/H/SP/2024, RG40XX V, RG CubeXX, RG34XX, RG28XX) | devem funcionar, não testados |
| Outros muOS ARM64 (TrimUI, Rockchip) | podem funcionar; desempenho do vídeo não avaliado |

O vídeo é decodificado pelo processador (FFmpeg). Em 640x360/640x480 a 60 fps isso cabe folgado
num H700; aparelhos mais fracos podem engasgar.

## Requisitos

- **No PC:** Steam aberto, logado, com **Remote Play ativado** (Steam → Configurações → Remote Play),
  na mesma rede do portátil. O Windows precisa estar desbloqueado (o Steam não transmite a tela de
  bloqueio nem os avisos do UAC).
- **No portátil:** muOS com Wi-Fi conectado.
- **Para parear (uma vez):** um computador na mesma rede com Python 3 e [uv](https://docs.astral.sh/uv/)
  (ou `pip install protobuf cryptography websockets`).

## Instalação

### 1. Parear (uma vez)

O pareamento por PIN ainda não roda no portátil; ele é feito por um script no computador, que gera
a identidade e a chave do dispositivo:

```sh
uv run --with protobuf --with cryptography --with websockets python -I tools/proto/pair.py
```

O script mostra um **PIN**. No PC, abra Steam → Configurações → Remote Play → **Parear Steam Link**
e digite o PIN. Ao terminar, ele grava `state/device.json`.

> `device.json` contém a chave do seu dispositivo. Não publique nem compartilhe esse arquivo.

### 2. Copiar para o cartão SD

Baixe `SteamLink-muOS.zip` na página de **Releases** (ou gere com `make package`, veja
[Compilar](#compilar)) e extraia na **raiz** do cartão SD do muOS. Fica assim:

```
MUOS/application/Steam Link/
├── mux_launch.sh
├── mux_lang.ini
├── glyph/steamlink.png
├── steamlink
└── device.json      ← copie o seu aqui
```

Copie o `state/device.json` gerado no passo 1 para dentro de `MUOS/application/Steam Link/`.

### 3. Jogar

No muOS: **Aplicativos → Steam Link**. O app procura o PC, conecta e abre o Big Picture.
Na primeira conexão o Steam pode pedir confirmação no PC.

## Controles

| Ação | Botão |
|---|---|
| Controle no jogo | todos os botões, como um controle Xbox 360 |
| Menu do Steam | MENU |
| Sair do streaming | segurar **SELECT + START** por ~1 s |
| Tentar de novo / sair (telas de erro) | A / B |

O mapeamento dos botões vem do próprio muOS (configuração de layout "retro"/"modern").

## Configuração

Campos opcionais em `device.json`, ao lado dos gerados pelo pareamento:

| Campo | Padrão | Uso |
|---|---|---|
| `host` | (descoberta automática) | IP fixo do PC, ex.: `"192.168.0.10"` |
| `width`, `height` | `640`, `480` | resolução pedida ao PC |
| `fps` | `60` | quadros por segundo pedidos |
| `bitrate_kbps` | `10000` | taxa de vídeo pedida |

Exemplo:

```json
{
  "device_id": 1234567890123456789,
  "secret": "…",
  "steamid": 76561190000000000,
  "host": "192.168.0.10",
  "bitrate_kbps": 8000
}
```

## Solução de problemas

Os logs ficam em `MUOS/application/Steam Link/steamlink.log` e `steamlink.stderr.log`.

| Sintoma | Causa provável |
|---|---|
| "Arquivo de pareamento não encontrado" | falta o `device.json` na pasta do app |
| "Nenhum PC encontrado" | Steam fechado, PC em outra rede ou firewall bloqueando o Steam; tente fixar `host` |
| "Dispositivo não autorizado" | o pareamento foi removido no Steam; pareie de novo |
| Vídeo preto com aviso amarelo | o PC está na tela de bloqueio/UAC do Windows; desbloqueie |
| Imagem travando | Wi-Fi fraco; reduza `bitrate_kbps` ou aproxime do roteador |
| App fecha logo ao abrir | mande o `steamlink.stderr.log` numa issue |

## Compilar

O build roda em Docker (Debian bullseye, glibc 2.31) com cross-compiler ARM64, FFmpeg estático
mínimo (só H.264 e Opus) e libcrypto estática. A SDL2 é ligada dinamicamente à do aparelho.

```sh
docker build -t steamlink-build client/docker          # uma vez (compila o FFmpeg)
make -C client docker                                  # client/build/aarch64/steamlink
make -C client package                                 # client/dist/ + SteamLink-muOS.zip
make -C client package CREDENTIALS=state/device.json   # idem, já com a sua chave (uso pessoal)
```

Versão x86_64 para testar no computador, sem tela (decodifica e mostra estatísticas):

```sh
make -C client docker ARCH=x86_64
docker run --rm --network host -v "$PWD/client":/work -v "$PWD/state/device.json":/cfg/device.json:ro \
  steamlink-build ./build/x86_64/steamlink --headless --host <ip-do-pc> --config /cfg/device.json --seconds 20
```

Opções do binário: `--host IP`, `--config arquivo`, `--log arquivo`, `--headless`, `--seconds N`,
`--dump quadro.pgm`, `--h264 fluxo.h264`.

## Como funciona

1. **Descoberta e pedido de streaming** em UDP 27036 (`CMsgRemoteClientBroadcast*`), com prova de
   posse da chave do dispositivo; o PC devolve a porta e uma chave de sessão cifrada.
2. **Sessão UDP** com cabeçalho de 13 bytes + CRC32C, canais confiáveis e não confiáveis.
3. **Ping de MTU** (pacotes tipo 0) antes do handshake, autenticação por HMAC-SHA256 e mensagens de
   controle cifradas com AES-CBC.
4. **Negociação** de codecs, **mídia** nos canais de dados e **controle** pelo HID remoto do Steam,
   como um gamepad SDL virtual.

Os detalhes do protocolo, obtidos por engenharia reversa do Steam Link oficial para Raspberry Pi,
estão em [`docs/PROTOCOLO.md`](docs/PROTOCOLO.md).

## Estrutura

```
client/src/       cliente C: sessão, descoberta, cripto, protobuf mínimo, FFmpeg, SDL
client/muos/      lançador e ícone do app muOS
client/docker/    ambiente de build
tools/proto/      protótipos em Python (pareamento, streaming)
tools/            descoberta e utilitários de engenharia reversa
re/protos, re/pb  definições protobuf do Steam usadas pelas ferramentas
docs/             documentação do protocolo
```

## Pendências

- Pareamento por PIN direto no portátil.
- Decodificação de vídeo por hardware (VPU do H700).
- Pedir automaticamente a resolução da tela do aparelho.
- Validação nos aparelhos listados acima.

## Créditos

- [ihslib](https://github.com/mariotaku/ihslib), de Mariotaku: referência para o ping de MTU, o
  formato dos quadros de vídeo e o HID remoto.
- [Thalium](https://blog.thalium.re/posts/achieving-remote-code-execution-in-steam-remote-play/) e
  o artigo do SSTIC 2023 sobre o protocolo Remote Play.
- [SteamDatabase/Protobufs](https://github.com/SteamDatabase/Protobufs) e
  [GameNetworkingSockets](https://github.com/ValveSoftware/GameNetworkingSockets).
- Equipe do [muOS](https://muos.dev) pelo sistema e pelo modelo de aplicativos.

## Aviso legal

Projeto independente, sem relação com a Valve. Steam e Steam Link são marcas da Valve Corporation.
A engenharia reversa foi feita para interoperabilidade e uso pessoal; este repositório não contém
binários nem código proprietário da Valve.
