# 待机组件拥有页面逻辑和预生成图片资源；display 组件仍只管理显示硬件。
idf_component_register(
    SRCS "standby_page.c"
    INCLUDE_DIRS "include"
    EMBED_FILES "assets/purin_idle.rgb565a8"
    PRIV_REQUIRES
        esp_lvgl_port
        lvgl
)
