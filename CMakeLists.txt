idf_component_register(
    SRCS
        "src/mesh_io.cpp"
        "src/site_roster.cpp"
        "src/site_discovery.cpp"
        "src/site_info.cpp"
        "src/site_onboard.cpp"
        "src/router_cfg.cpp"
    INCLUDE_DIRS "include"
    REQUIRES mesh_lite iot_bridge json esp_wifi nvs_flash freertos esp_event esp_netif esp_app_format esp_timer hub_identity
)
