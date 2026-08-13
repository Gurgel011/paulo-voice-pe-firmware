idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_netif
        esp_event
        esp_http_client
        esp_http_server
        nvs_flash
        led_strip
        esp_driver_gpio
        esp_driver_i2c
        esp_driver_i2s
)
