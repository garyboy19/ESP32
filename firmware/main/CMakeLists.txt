idf_component_register(SRCS "board_test.c"
                       PRIV_REQUIRES esp_driver_gpio esp_event esp_netif esp_timer esp_wifi nvs_flash spi_flash
                       INCLUDE_DIRS "")
