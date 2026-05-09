idf_component_register(
    SRCS
        "pm_main.c"
        "pm_wifi.c"
        "pm_log.c"
        "pm_config.c"
        "pm_mqtt.c"
        "pm_adc.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_adc
        esp_netif
        esp_event
        esp_wifi
        esp_http_client
        mqtt
        cjson
        nvs_flash
        lwip
)

idf_build_get_property(pm_project_ver PROJECT_VER)
if("${pm_project_ver}" STREQUAL "")
    set(pm_project_ver "1.0.0")
endif()
target_compile_definitions(${COMPONENT_LIB} PRIVATE PM_FW_VERSION="${pm_project_ver}")
