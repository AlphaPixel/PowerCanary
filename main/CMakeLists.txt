idf_component_register(
    SRCS "main.c" "ecoflow_ble.c" "ef_proto.c" "megatec.c" "power_state.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES bt mbedtls nvs_flash esp_timer esp_driver_usb_serial_jtag micro-ecc
    EMBED_FILES "keydata.bin")
