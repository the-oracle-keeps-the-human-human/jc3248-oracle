idf_component_register(
    SRCS "main.c" "relay_signal.c"
         "../../shared/display.c" "../../shared/hud_gfx.c"
    INCLUDE_DIRS "." "../../shared"
    PRIV_REQUIRES esp_peer esp_websocket_client nvs_flash esp_lcd driver
                  esp_timer esp_driver_ledc esp_driver_spi esp_wifi json
)
