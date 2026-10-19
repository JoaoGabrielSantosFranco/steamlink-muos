# Steam Link para muOS

Cliente **Steam Link nativo** para portáteis com [muOS](https://muos.dev), feito para o
**Anbernic RG40XXH**. Jogue os jogos do Steam do seu PC pela rede local usando o próprio
protocolo do Steam Remote Play: no PC fica só o Steam, sem Sunshine, Moonlight ou outro servidor.

> **Status: experimental.** O streaming (vídeo H.264 60 fps, áudio Opus e controles) já rodou num
> RG40XXH real. O pareamento direto no aparelho é novo e ainda está sendo validado. Relatos e logs
> são bem-vindos.

## Recursos

- **Tudo pelo aparelho:** na primeira vez o app mostra um PIN; você digita esse PIN no Steam do PC e
  pronto. Não precisa de outro computador, script ou arquivo para copiar.
- Encontra sozinho os PCs com Steam na rede (descoberta UDP 27036).
- Streaming de vídeo H.264 até 60 fps, ajustado à tela (faixas pretas quando o formato não bate).
- Áudio Opus 48 kHz estéreo, com baixa latência.
- Botões do aparelho viram um **controle Xbox 360** no PC (HID remoto do Steam). MENU = botão Steam.
- Avisos na tela: PC não encontrado, PC recusou, PC na tela de bloqueio do Windows, conexão perdida.
- Um único binário ARM64; depende só da SDL2 do muOS e de glibc ≥ 2.29.

## Aparelhos

| Aparelho | Situação |
|---|---|
| Anbernic RG40XXH | streaming testado no aparelho |
| Outros com muOS e chip H700 (RG35XX Plus/H/SP/2024, RG40XX V, RG CubeXX, RG34XX, RG28XX) | devem funcionar, não testados |
| Outros muOS ARM64 (TrimUI, Rockchip) | podem funcionar; desempenho do vídeo não avaliado |

O vídeo é decodificado pelo processador (FFmpeg). Em 640x360/640x480 a 60 fps isso cabe folgado
num H700; aparelhos mais fracos podem engasgar.

## Requisitos

- **No PC:** Steam aberto, logado, com **Remote Play ativado** (Steam → Configurações → Remote Play),
  na mesma rede do portátil. O Windows precisa estar desbloqueado (o Steam não transmite a tela de
  bloqueio nem os avisos do UAC).
- **No portátil:** muOS com Wi-Fi conectado. Na primeira vez também precisa de **internet**, porque o
  PIN é gerado pelos servidores da Steam (depois disso o streaming é só pela rede local).

## Instalação

### 1. Copiar para o cartão SD

Baixe `SteamLink-muOS.zip` na página de **Releases** e extraia na **raiz** do cartão SD do muOS.
Fica assim:

```
MUOS/application/Steam Link/
├── mux_launch.sh
├── mux_lang.ini
├── glyph/steamlink.png
├── cacert.pem       (certificados para falar com a Steam com segurança)
└── steamlink
```

### 2. Parear (só na primeira vez)

No muOS, abra **Aplicativos → Steam Link**. O app mostra um **PIN** de 4 dígitos.
No PC, abra Steam → Configurações → Remote Play → **Parear Steam Link** e digite o PIN.
O app mostra "Pareado!" e grava a chave do aparelho em `device.json`, na própria pasta.

> `device.json` é a chave do seu aparelho. Não compartilhe esse arquivo; apague-o para parear de novo.

### 3. Jogar

Depois do pareamento o app já procura o PC, conecta e abre o Big Picture. Nas próximas vezes ele vai
direto para o streaming.

## Controles

| Ação | Botão |
|---|---|
| Controle no jogo | todos os botões, como um controle Xbox 360 |
| Menu do Steam | MENU |
| Sair durante o streaming | segurar **SELECT + START** por ~1 s |
| Sair enquanto pareia, procura ou conecta ao PC | **B** (ou SELECT) |
| Telas de erro | **A** tenta de novo, **Y** pareia de novo, **B** sai |

Se o muOS não fornecer o mapeamento do controle, o app aplica sozinho o layout padrão "muOS-Keys".

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
| "Sem internet?" ou "Nenhum servidor da Steam respondeu" ao parear | o portátil precisa de internet para gerar o PIN |
| "Certificado ... rejeitado" ao parear | falta o `cacert.pem` na pasta do app; extraia o zip de novo |
| "Tempo esgotado" ao parear | o PIN não foi digitado no PC em 10 minutos; tente de novo |
| "Nenhum PC encontrado" | Steam fechado, PC em outra rede ou firewall bloqueando o Steam; tente fixar `host` |
| "Dispositivo não autorizado" | o pareamento foi removido no Steam; aperte **Y** para parear de novo |
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
