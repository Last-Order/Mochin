# time_service 组件在网络取得地址后启动 SNTP，并提供线程安全的本地时间。
idf_component_register(
    SRCS "time_service.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        esp_event
        esp_netif
)
