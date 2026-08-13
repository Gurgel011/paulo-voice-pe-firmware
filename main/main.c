#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "led_strip.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs_flash.h"

#define PIN_CENTER_BUTTON GPIO_NUM_0
#define PIN_MUTE_SWITCH GPIO_NUM_3
#define PIN_VOICE_KIT_RESET GPIO_NUM_4
#define PIN_I2C_SDA GPIO_NUM_5
#define PIN_I2C_SCL GPIO_NUM_6
#define PIN_LED_RING GPIO_NUM_21
#define PIN_POWER_ENABLE GPIO_NUM_45
#define PIN_INTERNAL_SPEAKER_AMP GPIO_NUM_47
#define PIN_DIAL_A GPIO_NUM_16
#define PIN_DIAL_B GPIO_NUM_18
#define PIN_I2S_SPK_BCLK GPIO_NUM_8
#define PIN_I2S_SPK_LRCLK GPIO_NUM_7
#define PIN_I2S_SPK_DOUT GPIO_NUM_10
#define PIN_I2S_MIC_BCLK GPIO_NUM_13
#define PIN_I2S_MIC_LRCLK GPIO_NUM_14
#define PIN_I2S_MIC_DIN GPIO_NUM_15

#define I2C_PORT 0
#define I2S_SPK_PORT I2S_NUM_0
#define I2S_MIC_PORT I2S_NUM_1
#define LED_COUNT 12
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_MAX_RETRY 20
#define AUDIO_TCP_PORT 8092

static const char *TAG = "paulo_voice_pe";

static EventGroupHandle_t wifi_events;
static int wifi_retry_count;
static led_strip_handle_t led_ring;
static i2c_master_bus_handle_t i2c_bus;
static i2c_master_dev_handle_t aic3204_dev;
static i2c_master_dev_handle_t voice_kit_dev;
static i2s_chan_handle_t i2s_tx;
static i2s_chan_handle_t i2s_rx;
static QueueHandle_t audio_queue;
static SemaphoreHandle_t audio_i2s_mutex;
static int volume_percent = 70;
static int assistant_active;
static char last_event[32] = "boot";
static int last_event_value;
static uint32_t event_seq;

typedef struct {
    uint8_t *data;
    size_t len;
} audio_buffer_t;

static void remember_event(const char *event, int value)
{
    strlcpy(last_event, event, sizeof(last_event));
    last_event_value = value;
    event_seq++;
}

static void set_leds(uint8_t r, uint8_t g, uint8_t b)
{
    if (!led_ring) {
        return;
    }
    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(led_ring, i, r, g, b);
    }
    led_strip_refresh(led_ring);
}

static void set_volume_leds(void)
{
    if (!led_ring) {
        return;
    }
    int lit = (volume_percent * LED_COUNT + 99) / 100;
    for (int i = 0; i < LED_COUNT; i++) {
        if (i < lit) {
            led_strip_set_pixel(led_ring, i, 0, 40, 160);
        } else {
            led_strip_set_pixel(led_ring, i, 0, 0, 0);
        }
    }
    led_strip_refresh(led_ring);
}

static bool aic3204_write(uint8_t reg, uint8_t value)
{
    if (!aic3204_dev) {
        return false;
    }
    uint8_t data[] = {reg, value};
    esp_err_t err = i2c_master_transmit(aic3204_dev, data, sizeof(data), 100);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AIC3204 reg 0x%02x <- 0x%02x falhou: %s", reg, value, esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool voice_kit_transmit(const uint8_t *data, size_t len)
{
    if (!voice_kit_dev) {
        return false;
    }
    esp_err_t err = i2c_master_transmit(voice_kit_dev, data, len, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice kit I2C write falhou: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static bool voice_kit_transmit_receive(const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len)
{
    if (!voice_kit_dev) {
        return false;
    }
    esp_err_t err = i2c_master_transmit_receive(voice_kit_dev, tx, tx_len, rx, rx_len, 1000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Voice kit I2C read falhou: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

static void init_voice_kit(void)
{
    const uint8_t version_req[] = {240, 88 | 0x80, 4};
    uint8_t version_resp[4] = {0};
    if (voice_kit_transmit_receive(version_req, sizeof(version_req), version_resp, sizeof(version_resp)) &&
        version_resp[0] == 0) {
        ESP_LOGI(TAG, "Voice kit/XMOS versao: %u.%u.%u", version_resp[1], version_resp[2], version_resp[3]);
    } else {
        ESP_LOGW(TAG, "Voice kit/XMOS nao respondeu versao; tentando configurar mesmo assim");
    }

    const uint8_t channel_0_agc[] = {241, 0x30, 1, 4};
    const uint8_t channel_1_ns[] = {241, 0x40, 1, 3};
    voice_kit_transmit(channel_0_agc, sizeof(channel_0_agc));
    voice_kit_transmit(channel_1_ns, sizeof(channel_1_ns));
    ESP_LOGI(TAG, "Voice kit pipeline solicitado: canal 0 AGC, canal 1 NS");
}

static bool aic3204_set_volume(int percent)
{
    if (percent > 0 && percent < 35) {
        percent = 35;
    }
    const int dvc_min = -45;
    const int dvc_max = 24;
    int volume_byte = dvc_min + ((dvc_max - dvc_min) * percent / 100);
    if (volume_byte < dvc_min) {
        volume_byte = dvc_min;
    }
    if (volume_byte > dvc_max) {
        volume_byte = dvc_max;
    }
    return aic3204_write(0x00, 0x00) &&
           aic3204_write(0x41, (uint8_t)(int8_t)volume_byte) &&
           aic3204_write(0x42, (uint8_t)(int8_t)volume_byte);
}

static bool aic3204_set_muted(bool muted)
{
    return aic3204_write(0x00, 0x00) && aic3204_write(0x40, muted ? 0x0c : 0x00);
}

static bool init_aic3204(void)
{
    ESP_LOGI(TAG, "Inicializando DAC AIC3204");
    if (!(aic3204_write(0x00, 0x00) &&
          aic3204_write(0x01, 0x01) &&
          aic3204_write(0x0b, 0x82) &&
          aic3204_write(0x0c, 0x82) &&
          aic3204_write(0x0e, 0x80) &&
          aic3204_write(0x1b, 0x30) &&
          aic3204_write(0x38, 0x02) &&
          aic3204_write(0x1f, 0x01) &&
          aic3204_write(0x20, 0x01) &&
          aic3204_write(0x3c, 0x01) &&
          aic3204_write(0x00, 0x01) &&
          aic3204_write(0x02, 0x09) &&
          aic3204_write(0x01, 0x08) &&
          aic3204_write(0x02, 0x01) &&
          aic3204_write(0x0a, 0x40) &&
          aic3204_write(0x03, 0x00) &&
          aic3204_write(0x04, 0x00) &&
          aic3204_write(0x7b, 0x01) &&
          aic3204_write(0x14, 0x25) &&
          aic3204_write(0x0c, 0x08) &&
          aic3204_write(0x0d, 0x08) &&
          aic3204_write(0x0e, 0x08) &&
          aic3204_write(0x0f, 0x08) &&
          aic3204_write(0x10, 0x3e) &&
          aic3204_write(0x11, 0x3e) &&
          aic3204_write(0x12, 0x00) &&
          aic3204_write(0x13, 0x00) &&
          aic3204_write(0x09, 0x3c))) {
        return false;
    }
    vTaskDelay(pdMS_TO_TICKS(2500));
    return aic3204_write(0x00, 0x00) &&
           aic3204_write(0x3f, 0xd4) &&
           aic3204_set_volume(volume_percent) &&
           aic3204_set_muted(false);
}

static esp_err_t init_i2s_speaker(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_SPK_PORT, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &i2s_tx, NULL));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(48000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_SPK_BCLK,
            .ws = PIN_I2S_SPK_LRCLK,
            .dout = PIN_I2S_SPK_DOUT,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    std_cfg.slot_cfg.ws_width = 32;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_tx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_tx));
    ESP_LOGI(TAG, "I2S speaker ativo em 48kHz/32bit/stereo/slave");
    return ESP_OK;
}

static esp_err_t init_i2s_mic(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_MIC_PORT, I2S_ROLE_SLAVE);
    chan_cfg.dma_desc_num = 8;
    chan_cfg.dma_frame_num = 256;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, NULL, &i2s_rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(16000),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = PIN_I2S_MIC_BCLK,
            .ws = PIN_I2S_MIC_LRCLK,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_MIC_DIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_BOTH;
    std_cfg.slot_cfg.slot_bit_width = I2S_SLOT_BIT_WIDTH_32BIT;
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(i2s_rx, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_enable(i2s_rx));
    ESP_LOGI(TAG, "I2S mic ativo em 16kHz/32bit/stereo/slave");
    return ESP_OK;
}

static esp_err_t play_pcm16_mono_48k(const uint8_t *data, size_t len)
{
    if (!i2s_tx || !data || len < 2) {
        return ESP_ERR_INVALID_ARG;
    }

    if (audio_i2s_mutex) {
        xSemaphoreTake(audio_i2s_mutex, portMAX_DELAY);
    }

    int32_t output[1024];
    int samples_total = len / 2;
    const int16_t *pcm = (const int16_t *)data;
    esp_err_t final_err = ESP_OK;
    int effective_volume = volume_percent;
    if (effective_volume > 0 && effective_volume < 35) {
        effective_volume = 35;
    }

    for (int base = 0; base < samples_total; base += 512) {
        int samples = samples_total - base;
        if (samples > 512) {
            samples = 512;
        }
        int out_index = 0;
        for (int i = 0; i < samples; i++) {
            int32_t sample = ((int32_t)pcm[base + i] * effective_volume / 100) << 16;
            output[out_index++] = sample;
            output[out_index++] = sample;
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(i2s_tx, output, out_index * sizeof(int32_t), &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S write falhou/interrompido: %s", esp_err_to_name(err));
            final_err = err;
            break;
        }
    }

    if (audio_i2s_mutex) {
        xSemaphoreGive(audio_i2s_mutex);
    }
    return final_err;
}

static void audio_play_task(void *arg)
{
    audio_buffer_t item;
    while (1) {
        if (xQueueReceive(audio_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        ESP_LOGI(TAG, "Tocando audio PCM: %u bytes", (unsigned)item.len);
        play_pcm16_mono_48k(item.data, item.len);
        free(item.data);
        ESP_LOGI(TAG, "Audio PCM finalizado");
    }
}

static void audio_tcp_server_task(void *arg)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Falha criando socket TCP de audio: errno=%d", errno);
        vTaskDelete(NULL);
        return;
    }

    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AUDIO_TCP_PORT),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        ESP_LOGE(TAG, "Bind TCP audio %d falhou: errno=%d", AUDIO_TCP_PORT, errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, 1) != 0) {
        ESP_LOGE(TAG, "Listen TCP audio falhou: errno=%d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Servidor TCP de audio ativo na porta %d (PCM s16le mono 48kHz)", AUDIO_TCP_PORT);
    while (1) {
        struct sockaddr_in source_addr;
        socklen_t addr_len = sizeof(source_addr);
        int sock = accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
        if (sock < 0) {
            ESP_LOGW(TAG, "Accept TCP audio falhou: errno=%d", errno);
            continue;
        }

        char addr_str[16];
        inet_ntoa_r(source_addr.sin_addr, addr_str, sizeof(addr_str));
        ESP_LOGI(TAG, "Cliente TCP audio conectado: %s", addr_str);
        uint8_t rx[4096];
        uint8_t *audio = NULL;
        size_t audio_len = 0;
        size_t audio_cap = 0;
        size_t total = 0;
        while (1) {
            int received = recv(sock, rx, sizeof(rx), 0);
            if (received < 0) {
                ESP_LOGW(TAG, "Recv TCP audio falhou: errno=%d", errno);
                break;
            }
            if (received == 0) {
                break;
            }

            if (audio_len + received > 2 * 1024 * 1024) {
                ESP_LOGW(TAG, "Audio TCP excedeu 2MB; descartando conexao");
                break;
            }
            if (audio_len + received > audio_cap) {
                size_t new_cap = audio_cap ? audio_cap * 2 : 65536;
                while (new_cap < audio_len + received) {
                    new_cap *= 2;
                }
                uint8_t *new_audio = realloc(audio, new_cap);
                if (!new_audio) {
                    ESP_LOGE(TAG, "Sem memoria para audio TCP (%u bytes)", (unsigned)new_cap);
                    break;
                }
                audio = new_audio;
                audio_cap = new_cap;
            }
            memcpy(audio + audio_len, rx, received);
            audio_len += received;
            total += received;
        }
        shutdown(sock, 0);
        close(sock);
        if (audio && audio_len > 0 && audio_queue) {
            if (audio_len % 2) {
                audio_len--;
            }
            audio_buffer_t item = {
                .data = audio,
                .len = audio_len,
            };
            if (xQueueSend(audio_queue, &item, pdMS_TO_TICKS(100)) == pdTRUE) {
                ESP_LOGI(TAG, "Cliente TCP audio finalizado: %u bytes recebidos e enfileirados", (unsigned)total);
                audio = NULL;
            } else {
                ESP_LOGE(TAG, "Fila de audio cheia; descartando audio TCP");
            }
        }
        free(audio);
    }
}

static esp_err_t http_post_json(const char *path, const char *json)
{
    char url[256];
    snprintf(url, sizeof(url), "%s%s", CONFIG_APP_SERVER_BASE_URL, path);

    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 5000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        return ESP_FAIL;
    }
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, json, strlen(json));
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "POST %s -> HTTP %d", path, status);
    } else {
        ESP_LOGW(TAG, "POST %s falhou: %s", path, esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return err;
}

static void notify_server_event(const char *event, int value)
{
    remember_event(event, value);

#if CONFIG_APP_ENABLE_OUTBOUND_REGISTER
    char path[160];
    char payload[256];
    snprintf(path, sizeof(path), "/devices/%s/command", CONFIG_APP_DEVICE_ID);
    snprintf(payload, sizeof(payload),
             "{\"source\":\"firmware\",\"event\":\"%s\",\"value\":%d}",
             event, value);
    http_post_json(path, payload);
#else
    ESP_LOGI(TAG, "Evento local: %s=%d seq=%lu", event, value, (unsigned long)event_seq);
#endif
}

#if CONFIG_APP_ENABLE_OUTBOUND_REGISTER
static void register_device(void)
{
    char payload[768];
    snprintf(payload, sizeof(payload),
             "{"
             "\"id\":\"%s\","
             "\"name\":\"%s\","
             "\"type\":\"home-assistant-voice-pe-custom\","
             "\"endpoint\":\"esp32-local\","
             "\"capabilities\":["
             "\"button_center\","
             "\"rotary_volume\","
             "\"led_ring\","
             "\"i2c_scan\","
             "\"audio_next\""
             "]"
             "}",
             CONFIG_APP_DEVICE_ID, CONFIG_APP_DEVICE_NAME);
    http_post_json("/devices/register", payload);
}
#endif

static esp_err_t health_handler(httpd_req_t *req)
{
    char response[256];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"id\":\"%s\",\"name\":\"%s\",\"volume\":%d,\"assistant_active\":%s}",
             CONFIG_APP_DEVICE_ID,
             CONFIG_APP_DEVICE_NAME,
             volume_percent,
             assistant_active ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t state_handler(httpd_req_t *req)
{
    char response[384];
    snprintf(response, sizeof(response),
             "{"
             "\"ok\":true,"
             "\"id\":\"%s\","
             "\"volume\":%d,"
             "\"assistant_active\":%s,"
             "\"event\":{\"seq\":%lu,\"name\":\"%s\",\"value\":%d}"
             "}",
             CONFIG_APP_DEVICE_ID,
             volume_percent,
             assistant_active ? "true" : "false",
             (unsigned long)event_seq,
             last_event,
             last_event_value);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t command_handler(httpd_req_t *req)
{
    char body[256] = {0};
    int received = httpd_req_recv(req, body, sizeof(body) - 1);
    if (received < 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo invalido");
        return ESP_FAIL;
    }

    if (strstr(body, "\"led\":\"blue\"")) {
        set_leds(0, 40, 160);
    } else if (strstr(body, "\"led\":\"green\"")) {
        set_leds(0, 80, 0);
    } else if (strstr(body, "\"led\":\"red\"")) {
        set_leds(80, 0, 0);
    } else if (strstr(body, "\"led\":\"white\"")) {
        set_leds(50, 50, 50);
    } else if (strstr(body, "\"led\":\"off\"")) {
        set_leds(0, 0, 0);
    }

    if (strstr(body, "\"assistant_active\":true")) {
        assistant_active = 1;
    } else if (strstr(body, "\"assistant_active\":false")) {
        assistant_active = 0;
    }

    char *volume_pos = strstr(body, "\"volume\"");
    if (volume_pos) {
        char *colon = strchr(volume_pos, ':');
        if (colon) {
            int new_volume = atoi(colon + 1);
            if (new_volume < 0) {
                new_volume = 0;
            }
            if (new_volume > 100) {
                new_volume = 100;
            }
            volume_percent = new_volume;
            aic3204_set_volume(volume_percent);
            set_volume_leds();
            ESP_LOGI(TAG, "Volume definido por API: %d%%", volume_percent);
        }
    }

    if (strstr(body, "\"beep\":true") && i2s_tx) {
        int32_t frame[2];
        for (int i = 0; i < 4800; i++) {
            int32_t sample = ((i / 24) % 2) ? 300000000 : -300000000;
            frame[0] = sample;
            frame[1] = sample;
            size_t written = 0;
            i2s_channel_write(i2s_tx, frame, sizeof(frame), &written, portMAX_DELAY);
        }
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

static esp_err_t audio_raw_handler(httpd_req_t *req)
{
    if (!i2s_tx || !audio_queue) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "i2s nao inicializado");
        return ESP_FAIL;
    }

    if (req->content_len <= 0 || req->content_len > 2 * 1024 * 1024) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tamanho de audio invalido");
        return ESP_FAIL;
    }

    uint8_t *audio = malloc(req->content_len);
    if (!audio) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para audio");
        return ESP_ERR_NO_MEM;
    }

    int offset = 0;
    while (offset < req->content_len) {
        int to_read = req->content_len - offset;
        if (to_read > 4096) {
            to_read = 4096;
        }
        int received = httpd_req_recv(req, (char *)audio + offset, to_read);
        if (received <= 0) {
            free(audio);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "falha lendo audio");
            return ESP_FAIL;
        }
        offset += received;
    }
    ESP_LOGI(TAG, "Audio recebido via HTTP: %d bytes", req->content_len);

    httpd_resp_set_type(req, "application/json");
    esp_err_t resp_err = httpd_resp_sendstr(req, "{\"ok\":true}");

    audio_buffer_t item = {
        .data = audio,
        .len = req->content_len,
    };
    if (xQueueSend(audio_queue, &item, pdMS_TO_TICKS(100)) != pdTRUE) {
        free(audio);
        ESP_LOGE(TAG, "Fila de audio cheia");
        return resp_err;
    }
    ESP_LOGI(TAG, "Audio enfileirado");
    return resp_err;
}

static int parse_int_param_from_uri(const char *uri, const char *key, int fallback, int min_value, int max_value)
{
    char pattern[24];
    snprintf(pattern, sizeof(pattern), "%s=", key);
    const char *param = strstr(uri, pattern);
    if (!param) {
        return fallback;
    }
    int value = atoi(param + strlen(pattern));
    if (value < min_value) {
        value = min_value;
    }
    if (value > max_value) {
        value = max_value;
    }
    return value;
}

static esp_err_t mic_raw_handler(httpd_req_t *req)
{
    if (!i2s_rx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "microfone nao inicializado");
        return ESP_FAIL;
    }

    int ms = parse_int_param_from_uri(req->uri, "ms", 4000, 250, 8000);
    int channel = parse_int_param_from_uri(req->uri, "ch", 0, 0, 1);
    int gain = parse_int_param_from_uri(req->uri, "gain", 2, 1, 8);
    int shift = parse_int_param_from_uri(req->uri, "shift", 16, 8, 28);
    int samples_target = 16000 * ms / 1000;
    size_t out_len = samples_target * sizeof(int16_t);
    int16_t *out = malloc(out_len);
    if (!out) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para gravar microfone");
        return ESP_ERR_NO_MEM;
    }

    int samples_written = 0;
    int32_t *frames = malloc(512 * 2 * sizeof(int32_t));
    if (!frames) {
        free(out);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "sem memoria para buffer do microfone");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Capturando microfone por %d ms canal=%d ganho=%dx shift=%d", ms, channel, gain, shift);
    while (samples_written < samples_target) {
        size_t bytes_read = 0;
        esp_err_t err = i2s_channel_read(i2s_rx, frames, 512 * 2 * sizeof(int32_t), &bytes_read, pdMS_TO_TICKS(500));
        if (err != ESP_OK || bytes_read == 0) {
            ESP_LOGW(TAG, "Leitura do mic falhou/interrompida: %s bytes=%u", esp_err_to_name(err), (unsigned)bytes_read);
            break;
        }
        int stereo_frames = bytes_read / (sizeof(int32_t) * 2);
        for (int i = 0; i < stereo_frames && samples_written < samples_target; i++) {
            int32_t raw = frames[i * 2 + channel];
            int32_t sample = (raw >> shift) * gain;
            if (sample > 32767) {
                sample = 32767;
            } else if (sample < -32768) {
                sample = -32768;
            }
            out[samples_written++] = (int16_t)sample;
        }
    }

    size_t response_len = samples_written * sizeof(int16_t);
    ESP_LOGI(TAG, "Microfone capturado: %u bytes", (unsigned)response_len);
    httpd_resp_set_type(req, "application/octet-stream");
    esp_err_t err = httpd_resp_send(req, (const char *)out, response_len);
    free(frames);
    free(out);
    return err;
}

static void start_http_api(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.lru_purge_enable = true;
    config.stack_size = 8192;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Falha ao iniciar API HTTP do ESP: %s", esp_err_to_name(err));
        return;
    }

    static httpd_uri_t health_uri = {
        .uri = "/health",
        .method = HTTP_GET,
        .handler = health_handler,
    };
    httpd_register_uri_handler(server, &health_uri);

    static httpd_uri_t state_uri = {
        .uri = "/state",
        .method = HTTP_GET,
        .handler = state_handler,
    };
    httpd_register_uri_handler(server, &state_uri);

    static httpd_uri_t events_uri = {
        .uri = "/events",
        .method = HTTP_GET,
        .handler = state_handler,
    };
    httpd_register_uri_handler(server, &events_uri);

    static httpd_uri_t command_uri = {
        .uri = "/command",
        .method = HTTP_POST,
        .handler = command_handler,
    };
    httpd_register_uri_handler(server, &command_uri);

    static httpd_uri_t audio_raw_uri = {
        .uri = "/audio/raw",
        .method = HTTP_POST,
        .handler = audio_raw_handler,
    };
    httpd_register_uri_handler(server, &audio_raw_uri);

    static httpd_uri_t mic_raw_uri = {
        .uri = "/mic/raw",
        .method = HTTP_GET,
        .handler = mic_raw_handler,
    };
    err = httpd_register_uri_handler(server, &mic_raw_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Falha registrando /mic/raw: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "API HTTP do ESP ativa na porta 80: /health /state /events /command /audio/raw /mic/raw");
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry_count < WIFI_MAX_RETRY) {
            wifi_retry_count++;
            ESP_LOGW(TAG, "Wi-Fi desconectado, tentando novamente (%d/%d)", wifi_retry_count, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(wifi_events, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        wifi_retry_count = 0;
        ESP_LOGI(TAG, "Wi-Fi conectado. IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_events, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t init_wifi(void)
{
    wifi_events = xEventGroupCreate();
    if (!wifi_events) {
        return ESP_ERR_NO_MEM;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL));

    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, CONFIG_APP_WIFI_SSID, sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, CONFIG_APP_WIFI_PASSWORD, sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));
    return (bits & WIFI_CONNECTED_BIT) ? ESP_OK : ESP_FAIL;
}

static void init_gpio(void)
{
    gpio_config_t out_conf = {
        .pin_bit_mask = (1ULL << PIN_POWER_ENABLE) | (1ULL << PIN_INTERNAL_SPEAKER_AMP) | (1ULL << PIN_VOICE_KIT_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&out_conf));
    gpio_set_level(PIN_POWER_ENABLE, 1);
    gpio_set_level(PIN_INTERNAL_SPEAKER_AMP, 0);
    gpio_set_level(PIN_VOICE_KIT_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(PIN_VOICE_KIT_RESET, 0);

    gpio_config_t in_conf = {
        .pin_bit_mask = (1ULL << PIN_CENTER_BUTTON) | (1ULL << PIN_MUTE_SWITCH) |
                        (1ULL << PIN_DIAL_A) | (1ULL << PIN_DIAL_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&in_conf));
}

static void init_led_ring(void)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_LED_RING,
        .max_leds = LED_COUNT,
        .led_model = LED_MODEL_WS2812,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .flags = {
            .invert_out = false,
        },
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
        .mem_block_symbols = 64,
        .flags = {
            .with_dma = false,
        },
    };
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_ring));
    led_strip_clear(led_ring);
    set_leds(0, 0, 40);
}

static void init_i2c_and_scan(void)
{
    i2c_master_bus_config_t bus_config = {
        .i2c_port = I2C_PORT,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags = {
            .enable_internal_pullup = true,
        },
    };
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_config, &i2c_bus));
    i2c_device_config_t aic_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x18,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &aic_dev_config, &aic3204_dev));

    i2c_device_config_t voice_dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x42,
        .scl_speed_hz = 400000,
    };
    ESP_ERROR_CHECK(i2c_master_bus_add_device(i2c_bus, &voice_dev_config, &voice_kit_dev));

    ESP_LOGI(TAG, "Aguardando boot do voice kit/XMOS...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    ESP_LOGI(TAG, "Escaneando I2C interno...");
    for (uint8_t addr = 1; addr < 127; addr++) {
        if (i2c_master_probe(i2c_bus, addr, 50) == ESP_OK) {
            ESP_LOGI(TAG, "I2C encontrado: 0x%02x", addr);
        }
    }
    if (i2c_master_probe(i2c_bus, 0x42, 100) == ESP_OK) {
        init_voice_kit();
    } else {
        ESP_LOGW(TAG, "Voice kit/XMOS nao encontrado em I2C 0x42");
    }
}

static void input_task(void *arg)
{
    int last_button = gpio_get_level(PIN_CENTER_BUTTON);
    int last_a = gpio_get_level(PIN_DIAL_A);
    int64_t last_button_ms = 0;

    while (1) {
        int button = gpio_get_level(PIN_CENTER_BUTTON);
        int a = gpio_get_level(PIN_DIAL_A);
        int b = gpio_get_level(PIN_DIAL_B);
        int64_t now_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;

        if (last_button == 1 && button == 0 && now_ms - last_button_ms > 300) {
            last_button_ms = now_ms;
            assistant_active = 1;
            ESP_LOGI(TAG, "Botao central: chamada do assistente");
            set_leds(0, 80, 180);
            notify_server_event("center_button", 1);
        }
        last_button = button;

        if (a != last_a) {
            if (b != a) {
                volume_percent += 5;
            } else {
                volume_percent -= 5;
            }
            if (volume_percent < 0) {
                volume_percent = 0;
            }
            if (volume_percent > 100) {
                volume_percent = 100;
            }
            ESP_LOGI(TAG, "Dial volume: %d%%", volume_percent);
            aic3204_set_volume(volume_percent);
            set_volume_leds();
            notify_server_event("volume", volume_percent);
        }
        last_a = a;

        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "Firmware proprio Paulo Voice PE iniciando");
    ESP_LOGI(TAG, "Backup original preservado em /home/paulo/esp32s3-voice-pe/original-flash-2026-07-01.bin");
    remember_event("boot", 1);

    init_gpio();
    init_led_ring();
    init_i2c_and_scan();
    if (!init_aic3204()) {
        ESP_LOGE(TAG, "Falha ao inicializar AIC3204");
        set_leds(80, 0, 0);
    }
    init_i2s_speaker();
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(PIN_INTERNAL_SPEAKER_AMP, 1);
    ESP_LOGI(TAG, "Amplificador interno do speaker ligado");
    init_i2s_mic();
    audio_queue = xQueueCreate(3, sizeof(audio_buffer_t));
    if (!audio_queue) {
        ESP_LOGE(TAG, "Falha criando fila de audio");
    } else {
        xTaskCreate(audio_play_task, "audio_play_task", 12288, NULL, 3, NULL);
    }
    audio_i2s_mutex = xSemaphoreCreateMutex();
    if (!audio_i2s_mutex) {
        ESP_LOGE(TAG, "Falha criando mutex de audio");
    }

    if (init_wifi() == ESP_OK) {
        set_leds(0, 80, 0);
        start_http_api();
        xTaskCreate(audio_tcp_server_task, "audio_tcp_server", 12288, NULL, 4, NULL);
#if CONFIG_APP_ENABLE_OUTBOUND_REGISTER
        register_device();
#else
        ESP_LOGI(TAG, "Registro outbound desativado; controle principal via API HTTP do ESP");
#endif
    } else {
        set_leds(80, 0, 0);
        ESP_LOGE(TAG, "Falha ao conectar Wi-Fi");
    }

    xTaskCreate(input_task, "input_task", 4096, NULL, 5, NULL);
}
