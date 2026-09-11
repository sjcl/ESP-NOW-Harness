idf_component_register(
    SRCS "app_main.c" "protocol.c" "radio.c" "metrics.c" "generator.c" "receiver.c" "control.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer json
)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Werror)

