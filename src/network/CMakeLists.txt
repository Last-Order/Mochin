# network 组件拥有 Wi-Fi STA、NVS 初始化和断线重连状态机。
idf_component_register(
    SRCS "network_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_event
        esp_netif
        esp_timer
        esp_wifi
        nvs_flash
)
