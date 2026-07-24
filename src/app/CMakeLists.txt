# app 组件只负责编排应用，因此仅依赖显示和输入两个公开组件。
# PRIV_REQUIRES 会让它们的公开头文件和链接库对 app_main.c 可用。
idf_component_register(
    SRCS "app_main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES display input
)
