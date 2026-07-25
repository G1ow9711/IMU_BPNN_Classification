# 注册纯状态机、纯 presenter 和 LVGL 9 渲染层；主机测试只编译前两项。
idf_component_register(
    # 编译 UI 状态机、展示数据转换器、真实 LVGL 9 控件渲染器和四个自动生成中文字体。
    SRCS
        "ui_state_machine.c"
        "ui_app_pairing.c"
        "ui_presenter.c"
        "ui_lvgl_renderer.c"
        "fonts/ui_font_noto_sans_sc_16.c"
        "fonts/ui_font_noto_sans_sc_20.c"
        "fonts/ui_font_noto_sans_sc_28.c"
        "fonts/ui_font_noto_sans_sc_36.c"
    # 向主入口和主机测试公开 UI 状态、页面数据及事件合同。
    INCLUDE_DIRS "include"
    # LVGL 提供 AMOLED 页面、动画、按钮、图标和刷新定时器实现。
    REQUIRES lvgl
)

# 中文位图扩大 UI 组件地址跨度；Xtensa 使用长调用序列，避免 call8 超出可编码范围。
target_compile_options(${COMPONENT_LIB} PRIVATE -mlongcalls)
