idf_component_register(SRCS "main.c" "epd.c" "epd_text.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio esp_driver_spi nvs_flash esp_wifi esp_event esp_netif esp_http_client esp_http_server esp-tls lwip)
