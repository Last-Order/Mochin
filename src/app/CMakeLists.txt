# app 组件只编排启动顺序，不包含页面或硬件实现。
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES display standby
)
