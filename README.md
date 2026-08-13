# Paulo Voice PE Firmware

Firmware proprio para o ESP32-S3 Home Assistant Voice PE do Paulo.

## Decisoes preservadas

- Tudo em portugues.
- Botao central chama o assistente de voz.
- Dial para a direita aumenta volume; dial para a esquerda diminui volume.
- Radio Antena 1 sera acionada por comando de voz, nao pelo botao.
- Servidor local com LLM, STT e TTS continua sendo o cerebro do projeto.
- Firmware proprio substitui o caminho ESPHome/Home Assistant para dar controle total do aparelho.

## Hardware conhecido

- ESP32-S3 QFN56 rev v0.2, 16 MB flash, 8 MB PSRAM.
- LED ring: GPIO21, 12 LEDs GRB.
- Botao central: GPIO0, ativo em nivel baixo.
- Dial: GPIO16 e GPIO18.
- Power enable: GPIO45.
- Speaker amp: GPIO47.
- I2C interno: SDA GPIO5, SCL GPIO6.
- DAC AIC3204: I2C 0x18.
- Voice kit/XMOS esperado: I2C 0x42.
- I2S speaker: BCLK GPIO8, LRCLK GPIO7, DOUT GPIO10.
- I2S mic: BCLK GPIO13, LRCLK GPIO14, DIN GPIO15.

## Estado atual

- Conecta no Wi-Fi `Antena  1  94.7  FM`.
- Sobe API HTTP no proprio ESP.
- Testado em `192.168.2.108`.
- Endpoints:
  - `GET /health`
  - `GET /state`
  - `GET /events`
  - `POST /command`
- O sentido ESP -> servidor `192.168.2.160:8091` apresentou timeout TCP nesta rede, entao o controle padrao agora e computador -> ESP.

## Build e flash

```bash
cd /home/paulo/esp32-projects/paulo-voice-pe-firmware
source /home/paulo/esp/esp-idf-v5.5.2/export.sh
idf.py build
idf.py -p /dev/ttyACM0 flash
```

## Backup original

Backup de fabrica preservado:

```text
/home/paulo/esp32s3-voice-pe/original-flash-2026-07-01.bin
```

Script de restore:

```text
/home/paulo/esp32s3-voice-pe/restore-original.sh
```

## Proxima fase

1. Criar bridge no servidor local para descobrir/pollar `GET /events` do ESP.
2. Validar fisicamente clique central e giro do dial.
3. Implementar audio proprio por I2S com AIC3204.
4. Fazer fluxo: clique -> toque curto -> grava pergunta -> STT PT-BR -> LLM -> TTS PT-BR -> fala no aparelho.
5. Registrar outros ESP32 no servidor e expor comandos em portugues.
