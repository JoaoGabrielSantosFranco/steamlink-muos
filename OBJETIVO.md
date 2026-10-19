# SteamLink nativo para MuOS (Anbernic RG40XXH)

## Objetivo

Criar um **cliente Steam Link nativo** que rode diretamente no **Anbernic RG40XXH** com **MuOS** e conecte no Steam do PC usando o protocolo oficial do **Steam Remote Play**, sem precisar instalar nenhum software extra no computador.

Para isso, vamos fazer **engenharia reversa do Steam Link** (cliente e protocolo de Remote Play) e reimplementar o necessário como um app leve, empacotado como aplicação do MuOS.

## Problema atual

Hoje, para jogar os jogos do PC no RG40XXH, preciso:

- instalar e configurar um servidor de streaming no PC (ex.: Sunshine), além do Steam;
- instalar e configurar um cliente no Anbernic (ex.: Moonlight), além do MuOS;
- parear, ajustar e manter tudo isso sincronizado.

São muitos componentes, muita configuração e muitos pontos de falha para algo que o Steam já faz sozinho com o Steam Link.

## Resultado esperado

1. No PC: **só o Steam**, aberto e com Remote Play ativado. Nada mais.
2. No RG40XXH: um app **"Steam Link"** no menu de aplicações do MuOS.
3. Ao abrir o app:
   - ele encontra automaticamente os PCs com Steam na rede local;
   - faz o pareamento com PIN, igual ao Steam Link oficial (só na primeira vez);
   - inicia o streaming (Big Picture / jogo) com vídeo, áudio e controles funcionando.

## Hardware e plataforma alvo

| Item | Detalhe |
|---|---|
| Dispositivo | Anbernic RG40XXH |
| SoC | Allwinner H700 (4x Cortex-A53, ARM64) |
| GPU | Mali-G31 |
| RAM | 1 GB |
| Tela | 640x480 (4:3) |
| Rede | Wi-Fi + Bluetooth |
| Sistema | MuOS (Linux; apps ficam em `MUOS/application`) |

Pontos de atenção:

- **Decodificação de vídeo por hardware** (H.264 via VPU do H700) é essencial. Decodificar por software no A53 provavelmente não vai dar conta.
- Resolução de stream ideal: 640x480 ou 854x480, até 60 fps, para reduzir banda e latência.
- Controles nativos do RG40XXH (botões, D-pad, analógicos) mapeados como controle de Steam/Xbox.
- Consumo de RAM baixo (1 GB no total, dividido com o MuOS).

## Escopo da engenharia reversa

O que precisa ser entendido e reimplementado:

1. **Descoberta**: broadcast UDP na porta 27036 (`CMsgRemoteClientBroadcast*`).
2. **Autenticação e pareamento**: fluxo de PIN e troca de chaves entre o cliente e o Steam do PC.
3. **Controle de sessão**: pedido de streaming e negociação de codec, resolução, bitrate e fps.
4. **Transporte do stream**: protocolo UDP do Remote Play (canais de vídeo, áudio, input e controle; criptografia; retransmissão e controle de perda).
5. **Vídeo**: recebimento e montagem dos quadros H.264 (talvez HEVC) para o decodificador.
6. **Áudio**: recebimento, decodificação (Opus) e saída via ALSA/SDL.
7. **Input**: envio dos eventos de controle (botões, eixos, gatilhos) no formato que o Steam espera.

Fontes de referência:

- Instaladores do Steam Link já baixados em `SteamLink/` (`SteamLink.msi`, `setup.exe`, versão Windows) para análise do cliente.
- O cliente **Steam Link oficial para Raspberry Pi / Linux ARM**, que é o mais próximo da plataforma alvo.
- Definições protobuf do Steam (repositório público `SteamDatabase/Protobufs`), especialmente as mensagens de Remote Play / remote client.
- Captura de tráfego entre o Steam Link oficial e o Steam do PC (Wireshark).

## Abordagens possíveis

1. **Portar o Steam Link oficial de Linux ARM para o MuOS**
   Verificar se o binário oficial do Raspberry Pi roda no H700 com as bibliotecas certas (SDL2, decodificador de vídeo etc.), usando shims para as partes específicas do Raspberry Pi. É o caminho mais rápido, se funcionar.

2. **Cliente próprio (reimplementação do protocolo)**
   Escrever um cliente leve (C/C++ ou Rust, com SDL2) que implemente o protocolo descoberto na engenharia reversa. Dá mais trabalho, mas o resultado é um app enxuto, otimizado para o RG40XXH e sem depender de binário fechado.

**Decisão:** seguir a **abordagem 2**. O Steam Link do Raspberry Pi é a referência principal da engenharia reversa: analisamos o binário, as bibliotecas e o tráfego para entender o protocolo e depois recriamos o cliente de forma nativa para o MuOS do RG40XXH. Rodar o binário oficial no aparelho serve só como experimento de apoio (para capturar tráfego e logs), não como produto final.

## Fora de escopo (por enquanto)

- Streaming pela internet (fora da rede local).
- Outros dispositivos além do RG40XXH. Pode vir depois, já que os outros Anbernic com H700 são parecidos.
- Qualquer modificação no Steam do PC.

## Critérios de sucesso

- [ ] O app aparece e abre pelo menu do MuOS.
- [ ] O PC com Steam é descoberto automaticamente na rede.
- [ ] O pareamento com PIN funciona.
- [ ] Vídeo a 60 fps em 480p, com decodificação por hardware.
- [ ] Áudio sincronizado.
- [ ] Todos os controles do RG40XXH funcionam no jogo.
- [ ] Latência jogável (meta: menos de 50 ms na rede local).
- [ ] Nenhum software extra instalado no PC além do Steam.

## Observação legal

A engenharia reversa aqui tem como objetivo a **interoperabilidade**: usar o Steam que já tenho com um dispositivo que a Valve não suporta oficialmente, para uso pessoal. O projeto não deve redistribuir binários nem código proprietário da Valve.
