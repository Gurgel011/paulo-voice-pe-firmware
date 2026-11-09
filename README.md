# Voice PE · Firmware em português

`ESP32-S3` · `ESP-IDF` · `C` · `HTTP` · `Protótipo`

[Estado atual](#estado-atual) · [Configuração](#configuração) · [Próximas etapas](#proxima-fase) · [← Todos os projetos](https://github.com/Gurgel011#projetos)

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

- Conecta no Wi-Fi configurado (ver Configuração abaixo).
- Sobe API HTTP no proprio ESP.
- Endpoints:
  - `GET /health`
  - `GET /state`
  - `GET /events`
  - `POST /command`
- O sentido ESP -> servidor apresentou timeout TCP na rede de teste original,
  entao o controle padrao agora e computador -> ESP.

## Configuração

Wi-Fi, endereço do servidor local e identidade do dispositivo ficam em opções
de Kconfig, sem valor padrão real (o `sdkconfig.defaults` versionado não tem
nenhum segredo):

```bash
idf.py menuconfig
```

Vá em `Paulo Voice PE` e preencha `Wi-Fi SSID`, `Wi-Fi password` e
`Servidor local`. Isso grava num `sdkconfig` local, **não versionado**.

## Build e flash

```bash
cd <diretorio-do-projeto>
source <caminho-do-esp-idf>/export.sh
idf.py build
idf.py -p <porta-serial> flash
```

## Backup original

Antes de gravar este firmware por cima do original, extraia um backup:

```bash
esptool.py --chip esp32s3 -p <porta-serial> read_flash 0x0 0x1000000 \
  <diretorio-de-backups>/voice-pe-original-$(date +%Y%m%d).bin
```

Restaurar:

```bash
esptool.py --chip esp32s3 -p <porta-serial> write_flash 0x0 \
  <diretorio-de-backups>/voice-pe-original-<data>.bin
```

## Proxima fase

1. Criar bridge no servidor local para descobrir/pollar `GET /events` do ESP.
2. Validar fisicamente clique central e giro do dial.
3. Implementar audio proprio por I2S com AIC3204.
4. Fazer fluxo: clique -> toque curto -> grava pergunta -> STT PT-BR -> LLM -> TTS PT-BR -> fala no aparelho.
5. Registrar outros ESP32 no servidor e expor comandos em portugues.
