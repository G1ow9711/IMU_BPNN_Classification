"""验证不可由主机替身直接执行的 ESP-IDF 启动与硬件低功耗接线合同。"""

# 导入 unittest，使用标准库执行确定性源码合同测试。
import unittest
# 导入 Path，从当前仓库根读取固定生产源码。
from pathlib import Path


class FirmwareStartupContractTests(unittest.TestCase):
    """锁定中文自检错误页在真实外设初始化失败时可达。"""

    # 仓库根由本文件所在 tools 目录的父目录确定，不依赖调用者当前目录。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向 ESP-IDF 唯一产品入口。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"

    def test_visible_self_test_precedes_external_sensor_and_storage_checks(self) -> None:
        """显示和 SELF_TEST 必须先于可能失败的传感器与存储初始化。"""

        # source 使用 UTF-8 读取中文注释，缺文件或乱码应直接使测试失败。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # main_body 只分析 app_main 调用顺序，避免被前面的函数定义位置干扰。
        main_body = source[source.index("void app_main(void)\n{") :]
        # production_body 从 NVS 注释开始，排除当前真板分级隔离分支中的独立传感器调用。
        production_body = main_body[
            main_body.index("/* 初始化 NVS；失败阻断绑定和安全配置。 */") :
        ]
        # board_position 是最早能建立物理显示的板级运行时调用位置。
        board_position = production_body.index("app_init_board_runtime()")
        # renderer_position 是中文 LVGL 页面树建立位置。
        renderer_position = production_body.index("app_init_ui_renderer()")
        # self_test_position 是 BOOT 后显示 SELF_TEST 页的位置。
        self_test_position = production_body.index("app_show_startup_pages(&startup_ui)")
        # sensor_position 是 QMI8658/AXP2101/RTC 可能失败的自检位置。
        sensor_position = production_body.index("app_init_external_sensors()")
        # storage_position 是 LittleFS/会话仓储可能失败的位置。
        storage_position = production_body.index("app_init_session_store()")
        # 严格顺序保证传感器或存储失败时显示资源和 SELF_TEST 上下文已经存在。
        self.assertLess(board_position, renderer_position)
        # renderer 必须先于启动页面渲染。
        self.assertLess(renderer_position, self_test_position)
        # SELF_TEST 必须在传感器检查前可见。
        self.assertLess(self_test_position, sensor_position)
        # 传感器检查完成后才允许访问依赖 Flash 的会话仓储。
        self.assertLess(sensor_position, storage_position)

    def test_each_blocking_startup_failure_routes_to_error_page(self) -> None:
        """传感器、存储和领域失败必须显示稳定故障码并保留页面。"""

        # source 保存所有生产失败分支。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 三个稳定故障码都必须传给统一中文 ERROR 页函数。
        for fault_name in (
            "APP_STARTUP_FAULT_SENSOR",
            "APP_STARTUP_FAULT_STORAGE",
            "APP_STARTUP_FAULT_DOMAIN",
        ):
            # 每个名称必须至少出现在宏定义和失败接线两处。
            self.assertGreaterEqual(source.count(fault_name), 2)
            # 失败接线必须显式使用当前启动上下文，而不是只写串口。
            self.assertIn(f"app_show_startup_error(&startup_ui, {fault_name})", source)
        # ERROR 页显示后暂停 app_main，避免返回导致产品表现为黑屏或无响应。
        self.assertGreaterEqual(source.count("vTaskSuspend(NULL);"), 4)


class FirmwareLowPowerContractTests(unittest.TestCase):
    """锁定当前联调版优先复用官方 BSP，不再修改厂家显示初始化。"""

    # 仓库根与上一个测试类使用相同确定性解析方式。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent

    def test_runtime_does_not_call_removed_private_bsp_power_hooks(self) -> None:
        """联调版不得为低功耗重新引入会改变官方显示组件的私有接口。"""

        # backend_path 指向真实 ESP-IDF 后端，Mock 不代表物理功耗。
        backend_path = self.REPOSITORY_ROOT / (
            "esp32/firmware/components/board_runtime/board_runtime_esp.c"
        )
        # source 保存真实 BSP 边界实现。
        source = backend_path.read_text(encoding="utf-8")
        # 官方 BSP 2.0.0 不公开面板休眠句柄，联调阶段不能再修改受管组件增加私有调用。
        self.assertNotIn("bsp_display_panel_power_set", source)
        # 官方 BSP 2.0.0 不公开触摸硬休眠句柄，当前只允许开关 LVGL 输入分派。
        self.assertNotIn("bsp_touch_hardware_active_set", source)
        # 触摸逻辑开关仍需使用官方 LVGL 输入句柄，保证现有上层接口可编译和可诊断。
        self.assertIn("lv_indev_enable((lv_indev_t *)runtime->platform_touch, enabled)", source)

    def test_ft5x06_driver_registers_sleep_callbacks(self) -> None:
        """公共触摸休眠 API 必须由实际 FT5x06 兼容驱动实现。"""

        # driver_path 指向厂家 BSP 当前依赖的受管触摸组件。
        driver_path = self.REPOSITORY_ROOT / (
            "esp32/firmware/managed_components/"
            "espressif__esp_lcd_touch_ft5x06/esp_lcd_touch_ft5x06.c"
        )
        # source 读取当前锁定依赖源码。
        source = driver_path.read_text(encoding="utf-8")
        # 进入休眠回调必须注册到 esp_lcd_touch 公共对象。
        self.assertIn("->enter_sleep = esp_lcd_touch_ft5x06_enter_sleep", source)
        # 退出休眠回调必须通过复位恢复主动扫描。
        self.assertIn("->exit_sleep = esp_lcd_touch_ft5x06_exit_sleep", source)
        # PMODE hibernate 常量固定为芯片合同值 3。
        self.assertIn("FT5x06_PMODE_HIBERNATE          (0x03)", source)

    def test_menuconfig_does_not_offer_unimplemented_native_drivers(self) -> None:
        """用户不能在 menuconfig 选择尚未接入的 CO5300/CST9220 原生路径。"""

        # kconfig_path 指向板型选择合同。
        kconfig_path = self.REPOSITORY_ROOT / (
            "esp32/firmware/components/board_adapter/Kconfig"
        )
        # source 读取完整菜单文本。
        source = kconfig_path.read_text(encoding="utf-8")
        # 隐藏兼容符号可以保留，但不得出现可选的中文原生驱动提示。
        self.assertNotIn('bool "CO5300 原生驱动路径', source)
        # 触摸同样不得暴露无法构建的选项。
        self.assertNotIn('bool "CST9220 原生驱动路径', source)


class FirmwareBenchAlwaysOnContractTests(unittest.TestCase):
    """锁定当前真板联调固件不会因自动低功耗再次熄屏或失联。"""

    # 仓库根由测试文件路径确定，避免调用目录改变断言对象。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向唯一产品事件编排入口。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"
    # ui_source_path 指向真板开机页和主页文案生成器，用于锁定肉眼可见的固件版本证据。
    UI_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_presenter.c"
    # ble_header_path 指向 NimBLE 适配层公开合同，确保广播恢复不是 main.c 私有桩。
    BLE_HEADER_PATH = REPOSITORY_ROOT / "esp32/firmware/components/ble_service/include/ble_service_nimble.h"
    # ble_source_path 指向真实 GAP 广播实现，源码合同不得由桌面 Mock 替代。
    BLE_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/components/ble_service/src/ble_service_nimble.c"

    def test_bench_mode_skips_automatic_screen_off_and_deep_sleep(self) -> None:
        """现场调试模式必须拦截空闲熄屏和长空闲深睡，但不删除用户主动关机代码。"""

        # source 读取当前真板将要烧录的生产入口。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 显式常量让日志、评审和后续恢复低功耗时能定位当前模式。
        self.assertIn("#define APP_BENCH_ALWAYS_ON (true)", source)
        # 空闲事件分支必须在计时器产生 SCREEN_TIMEOUT/LONG_IDLE 前短路。
        idle_branch = source[source.index("case APP_EVENT_IDLE_POLL:") :]
        # 只分析当前 case 到下一个 APP_EVENT_STORAGE_RESULT，避免其它分支同名文本干扰。
        idle_branch = idle_branch[: idle_branch.index("case APP_EVENT_STORAGE_RESULT:")]
        # 调试常亮判断必须出现在 power_idle_timer_poll 之前。
        self.assertLess(
            idle_branch.index("if (APP_BENCH_ALWAYS_ON)"),
            idle_branch.index("power_idle_timer_poll("),
        )
        # 用户主动关机仍必须保留 PMIC 请求，避免把常亮误实现成无法关机。
        self.assertIn("board_runtime_request_pmic_shutdown", source)

    def test_bench_firmware_has_visible_version_marker_on_boot_and_home(self) -> None:
        """开机页和主页必须显示联调版本，防止只凭串口写入成功误判新固件已启动。"""

        # source 读取实际烧录固件使用的 UI 状态文案，避免测试预览器中的假标记通过合同。
        source = self.UI_SOURCE_PATH.read_text(encoding="utf-8")
        # marker 使用中文与日期组成，用户重启后无需串口即可分辨本次联调固件。
        marker = "常亮联调版 0716"
        # 标记必须至少出现两次，分别覆盖短暂的开机页和随后长期停留的主页。
        self.assertGreaterEqual(source.count(marker), 2)

    def test_functional_bench_enables_ble_and_initial_power_policy(self) -> None:
        """功能联调版必须恢复 BLE 和首个电源策略，同时继续由常亮门禁止自动睡眠。"""

        # source 读取真板唯一应用入口，确保联调宏实际控制生产接线而非测试替身。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # BLE 必须恢复，才能实测 Windows 扫描、配对、RawStream 和 LiveState。
        self.assertIn("#define APP_BENCH_DISABLE_BLE (false)", source)
        # 初始电源策略必须恢复，HOME 与训练态才会真实切换 QMI ACTIVE/WOM 门控。
        self.assertIn("#define APP_BENCH_SKIP_INITIAL_POWER_POLICY (false)", source)
        # BLE 启动调用仍位于显式宏的否定分支，后续可单变量重新隔离。
        ble_start = source.index("if (!APP_BENCH_DISABLE_BLE)")
        self.assertLess(ble_start, source.index("app_start_ble(battery_percent)", ble_start))
        # 初始电源队列仍受独立宏保护，但本轮常量要求实际执行。
        power_start = source.index("if (!APP_BENCH_SKIP_INITIAL_POWER_POLICY)")
        self.assertLess(power_start, source.index("app_queue_power(&initial_policy, false)", power_start))
        # 自动低功耗仍必须短路，防止联调时 COM10、BLE 和屏幕被空闲计时关闭。
        self.assertIn("#define APP_BENCH_ALWAYS_ON (true)", source)
        # 最终 ready 日志必须保留 ESP-IDF 复位原因，重启时可区分掉电、软件和看门狗。
        self.assertIn("reset_reason=%d", source)

    def test_functional_bench_runs_full_sensor_model_and_task_chain(self) -> None:
        """功能联调版必须退出显示隔离，真实启动传感器、模型、任务和 BLE。"""

        # source 读取真板唯一入口，测试不能由桌面预览器或 Mock 分支代替。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 显示-only 必须关闭，否则 app_main 会永久停在单页 HOME，QMI/BLE/模型均不可达。
        self.assertIn("#define APP_BENCH_DISPLAY_ONLY (false)", source)
        # 传感器-only 同样关闭，确保执行完整产品任务链而非只初始化三个 I2C 外设。
        self.assertIn("#define APP_BENCH_SENSOR_ONLY (false)", source)
        # main_body 只分析 app_main 定义，防止前置声明中的同名文本干扰顺序判断。
        main_body = source[source.index("void app_main(void)\n{") :]
        # NVS 之后的正式分支必须按顺序包含全部真机链路入口。
        product_branch = main_body[main_body.index("/* 初始化 NVS；失败阻断绑定和安全配置。 */") :]
        # 五个关键入口依次覆盖 QMI/AXP/RTC、LittleFS、297 维双 M0、NimBLE 与九任务。
        required_calls = (
            "app_init_external_sensors()",
            "app_init_session_store()",
            "app_init_domains()",
            "app_start_ble(battery_percent)",
            "app_wait_ble_ready()",
            "app_create_tasks()",
        )
        # 每个入口必须真实存在于可达产品分支。
        for required_call in required_calls:
            # 缺少任一调用都表示真板联调链不完整。
            self.assertIn(required_call, product_branch)
        # 关键入口必须保持传感器→存储→模型→BLE→任务的启动顺序；控制器先保留连续片内 RAM。
        positions = [product_branch.index(call) for call in required_calls]
        # 严格递增既保护模型依赖，也避免九个任务栈先碎片化 NimBLE 控制器所需片内内存。
        self.assertEqual(positions, sorted(positions))
        # BLE 异步主机必须完成同步和广播后才允许九任务抢占片内 RAM。
        self.assertIn("static bool app_wait_ble_ready(void)", source)
        # 等待超时必须停止已启动但不可发现的服务，稳定离线运行而不是继续触发复位。
        self.assertIn("(void)ble_service_nimble_stop();", product_branch)
        # 入口必须读取片内总空闲量和最大连续块，串口才能证明 BLE 启动前的真实内存条件。
        self.assertIn(
            "heap_caps_get_free_size(\n            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            product_branch,
        )
        # 最大连续块是控制器分配能否成功的关键事实，不能只记录总空闲字节。
        self.assertIn(
            "heap_caps_get_largest_free_block(\n            "
            "MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)",
            product_branch,
        )
        # 正式分支必须创建完整操作系统对象，QMI 互斥量和全部队列由统一入口负责。
        self.assertIn("app_create_os_objects()", product_branch)
        # 常亮联调版只在 RAM 强制打开开发者门，允许 PC 命令 11 实测 RawStream 且不污染 NVS。
        self.assertIn("s_device_config.developer_mode = true", product_branch)
        # 显示与传感器隔离支路仍保留以便回退，但本轮两个编译期常量都为 false。
        isolation_start = main_body.index("if (APP_BENCH_DISPLAY_ONLY || APP_BENCH_SENSOR_ONLY)")
        # 普通产品分支位于隔离支路之后，false 常量会直接越过该永久等待路径。
        self.assertLess(isolation_start, main_body.index("app_init_nvs()"))
        # 触摸回调继续保护未来隔离构建的空队列，不能因本轮恢复全链而删除。
        callback_start = source.index("static void app_ui_command_callback")
        # 截止到下一个 BLE 回调，限定触摸函数本体。
        callback_end = source.index("static void app_ble_connection_changed", callback_start)
        # guard 必须同时检查两个隔离阶段和空事件队列。
        self.assertIn(
            "if (APP_BENCH_DISPLAY_ONLY || APP_BENCH_SENSOR_ONLY || "
            "(s_app_event_queue == NULL))",
            source[callback_start:callback_end],
        )
        # 功能联调不允许重新启用任何自动睡眠入口。
        for forbidden_call in (
            "#define APP_BENCH_ALWAYS_ON (false)",
        ):
            # 报错时直接显示意外启用的低功耗常量。
            self.assertNotIn(forbidden_call, source)

    def test_functional_bench_keeps_nimble_advertising_discoverable(self) -> None:
        """常亮功能联调版必须周期确认未连接手柄仍在广播，不能只调用一次异步 start。"""

        # main_source 读取每秒空闲事件分支，证明保活运行在产品应用任务而非测试工具。
        main_source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # ble_header 读取跨组件公开 API，避免 main.c 访问 NimBLE 私有全局状态。
        ble_header = self.BLE_HEADER_PATH.read_text(encoding="utf-8")
        # ble_source 读取真实同步、连接和广播活动位判断。
        ble_source = self.BLE_SOURCE_PATH.read_text(encoding="utf-8")
        # 公开 API 必须存在，调用者只获得 ESP-IDF 结果而不借用主机栈内部指针。
        self.assertIn("esp_err_t ble_service_nimble_ensure_advertising(void);", ble_header)
        # 主机同步事实必须显式保存；未同步时禁止把广播失败误报为已恢复。
        self.assertIn("g_ble_state.host_synced = UINT8_C(1);", ble_source)
        # 启动失败时不得进入任何 GAP API；离线诊断只能返回 advertising=false。
        self.assertIn(
            "status->advertising = status->started && status->host_synced &&\n"
            "        (ble_gap_adv_active() != 0);",
            ble_source,
        )
        # 保活只在服务已启动、主机已同步、无连接且功耗策略非 OFF 时允许补启广播。
        self.assertIn("esp_err_t ble_service_nimble_ensure_advertising(void)", ble_source)
        self.assertIn("ble_gap_adv_active()", ble_source)
        # 保活必须位于独立 BLE 发布任务，不能依赖可能被 125 Hz QMI 事件挤掉的 IDLE_POLL。
        ble_task = main_source[main_source.index("static void app_ble_task") :]
        # 截止到下一任务函数，避免其它调用点让断言误绿。
        ble_task = ble_task[: ble_task.index("static void app_storage_task")]
        # BLE 任务必须按单调时钟调用恢复 API。
        self.assertIn("ble_service_nimble_ensure_advertising()", ble_task)
        # 恢复后必须读取运行快照，串口才能区分未启动、未同步、未广播和已有连接。
        self.assertIn("ble_service_nimble_get_runtime_status(&runtime_status)", ble_task)
        # 五秒状态日志字段固定，真板捕获脚本可直接解析。
        self.assertIn("BLE_STATUS started=%d synced=%d advertising=%d", ble_task)

    def test_renderer_static_mode_uses_immediate_screen_load_and_blocks_text_fades(self) -> None:
        """关闭动画后必须使用立即切页，并阻止页面、动作和计数淡入。"""

        # renderer_source 是真板链接的唯一 LVGL 页面实现。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # 公开设置函数必须存在，主入口才能在首个 BOOT 页面之前关闭动画。
        self.assertIn("ui_lvgl_renderer_set_animations_enabled", renderer_source)
        # 页面切换在动画关闭分支必须调用 LVGL 立即加载 API。
        self.assertIn("lv_screen_load(page->root)", renderer_source)
        # 厂家 BSP 的 QSPI flush 是异步链；持锁调用 lv_refr_now 会阻塞并引发看门狗复位，必须禁止。
        self.assertNotIn("lv_refr_now(", renderer_source)
        # 动作和计数淡入都必须受同一开关保护，防止静态页仍持续重绘文字。
        self.assertGreaterEqual(renderer_source.count("renderer->animations_enabled"), 3)

    def test_display_only_static_home_skips_visible_boot_and_self_test_frames(self) -> None:
        """显示质量诊断必须只绘制一张 HOME，避免旧启动页像素叠在最终中文字上。"""

        # source 读取真板唯一应用入口，避免桌面预览器通过但设备路径仍连续切三页。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # helper_start 定位显示-only 专用函数；该函数不能复用会实际绘制 BOOT 的旧入口。
        helper_start = source.index("static bool app_show_static_home_once")
        # helper_end 截止到启动错误函数，保证统计只覆盖单页 helper 本体。
        helper_end = source.index("static void app_show_startup_error", helper_start)
        # helper_body 保存单页状态推进和绘制代码，供下面检查真实调用次数。
        helper_body = source[helper_start:helper_end]
        # 状态机仍必须合法经过 BOOT_READY，不能直接篡改 context.state。
        self.assertIn("UI_EVENT_BOOT_READY", helper_body)
        # 状态机仍必须合法经过 SELF_TEST_OK，最终模型才能按 HOME presenter 生成中文内容。
        self.assertIn("UI_EVENT_SELF_TEST_OK", helper_body)
        # 单页实验严禁调用旧的三页渲染入口。
        self.assertNotIn("app_show_startup_pages", helper_body)
        # helper 只允许一次真板渲染；多次调用会重新引入页面残留变量。
        self.assertEqual(helper_body.count("ui_lvgl_renderer_render("), 1)

    def test_home_diagnostic_uses_fixed_pixel_label_geometry(self) -> None:
        """HOME 五个文字槽必须固定坐标和高度，禁止 flex 重排在异步刷新中留下旧字。"""

        # renderer_source 读取真板 LVGL 对象树实现，桌面预览坐标不能替代设备合同。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # helper_start 定位 HOME 专用绝对布局，避免全文件其它页面的 flex 调用干扰断言。
        helper_start = renderer_source.index("static void ui_lvgl_apply_static_home_layout")
        # helper_end 截止到下一页面创建函数，限定固定布局函数本体。
        helper_end = renderer_source.index("static bool ui_lvgl_create_page", helper_start)
        # helper_body 保存 HOME 标签和按钮行的几何设置。
        helper_body = renderer_source[helper_start:helper_end]
        # HOME 根对象必须关闭 flex，标签位置才不会随其它文本高度变化。
        self.assertIn("lv_obj_set_layout(page->root, LV_LAYOUT_NONE)", helper_body)
        # 五个普通标签必须切换为裁剪模式，短主页文案不再触发自动换行重算。
        self.assertEqual(helper_body.count("LV_LABEL_LONG_CLIP"), 5)
        # 标题、状态、主文案、次文案、页脚和按钮行六个对象都必须设置固定位置。
        self.assertEqual(helper_body.count("lv_obj_set_pos("), 6)
        # 同一六个对象都必须设置固定宽高，禁止空文本到中文文本时改变对象高度。
        self.assertEqual(helper_body.count("lv_obj_set_size("), 6)
        # HOME 页面创建结束时必须实际调用专用布局函数。
        self.assertIn("ui_lvgl_apply_static_home_layout(page)", renderer_source)

    def test_factory_firmware_rounder_is_owned_once_by_board_display(self) -> None:
        """SH8601 的 2×2 刷新区对齐只能由板级显示组件注册一次。"""

        # renderer_source 读取上层产品 UI；该层不得重复接管面板传输约束。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # bsp_source 读取真板链接的 Waveshare 板级组件；rounder 的唯一所有权位于此处。
        bsp_source = (
            self.REPOSITORY_ROOT
            / "esp32/firmware/managed_components/"
            "waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c"
        ).read_text(encoding="utf-8")
        # 板级组件必须且只能注册一次 INVALIDATE_AREA 回调，避免重复回调掩盖真实显示链差异。
        self.assertEqual(
            bsp_source.count(
                "lv_display_add_event_cb(disp, rounder_event_cb, LV_EVENT_INVALIDATE_AREA, NULL);"
            ),
            1,
        )
        # 上层 UI 不得保留第二份 rounder 定义、注册或反注册逻辑。
        self.assertNotIn("ui_lvgl_rounder_event_cb", renderer_source)
        # 厂家异步 QSPI flush 不允许在持锁路径中加入同步强刷。
        self.assertNotIn("lv_refr_now", renderer_source)
        # 已取消的字体 A/B 试验不得残留在生产渲染器中。
        self.assertNotIn("font_ab_diagnostic", renderer_source)

    def test_renderer_uses_rounded_screen_safe_area_for_top_text(self) -> None:
        """标题与状态栏必须避开 410×502 圆角屏幕左上不可视区。"""

        # renderer_source 读取真板唯一 LVGL 页面实现，避免只修桌面预览坐标。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # 左右至少各保留 32 像素；标题首字不能再从 x=20 的圆角边缘开始。
        self.assertIn("#define UI_SAFE_HORIZONTAL_PX (32)", renderer_source)
        # 顶部至少保留 32 像素；标题不能紧贴 AMOLED 上圆角切线。
        self.assertIn("#define UI_SAFE_TOP_PX (32)", renderer_source)
        # 根 screen 必须分别应用左右安全区，不能再被 pad_all(20) 覆盖。
        self.assertIn("lv_obj_set_style_pad_left(screen, UI_SAFE_HORIZONTAL_PX", renderer_source)
        self.assertIn("lv_obj_set_style_pad_right(screen, UI_SAFE_HORIZONTAL_PX", renderer_source)
        # 顶部安全区同样必须由根布局执行，保证全部页面一致。
        self.assertIn("lv_obj_set_style_pad_top(screen, UI_SAFE_TOP_PX", renderer_source)
        # 标签和按钮宽度必须缩进到安全内容宽度，防止右上角发生对称裁切。
        self.assertGreaterEqual(renderer_source.count("UI_SAFE_CONTENT_WIDTH_PX"), 6)

    def test_bench_display_keeps_official_bsp_startup_brightness(self) -> None:
        """显示稳定性回归必须保留已在真板通过的官方 BSP 2.0.0 启动值。"""

        # bsp_source_path 指向真板实际链接的 Waveshare 受管 BSP。
        bsp_source_path = self.REPOSITORY_ROOT / (
            "esp32/firmware/managed_components/"
            "waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c"
        )
        # source 读取真实 SH8601 亮度初始化函数。
        source = bsp_source_path.read_text(encoding="utf-8")
        # 官方例程使用 100% 启动，并已在同一硬件连续 120 秒稳定；本轮不得擅自改变基准。
        self.assertIn("bsp_display_brightness_set(100)", source)
        # 只检查亮度初始化函数，避免其它业务层亮度设置干扰受管组件合同。
        brightness_init = source[source.index("esp_err_t bsp_display_brightness_init") :]
        brightness_init = brightness_init[: brightness_init.index("esp_err_t bsp_display_brightness_set")]
        # 旧诊断版的 35% 私有补丁必须从官方受管组件移除。
        self.assertNotIn("bsp_display_brightness_set(35)", brightness_init)


class FirmwareDisplayDmaContractTests(unittest.TestCase):
    """锁定 FactoryFirmWare 的 SH8601 QSPI 配置透传与片内 DMA 显示路径。"""

    # 仓库根由测试文件位置解析，保证从 VS Code 或 PowerShell 启动时检查同一份固件。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # bsp_source_path 指向真板实际链接的 Waveshare 2.0.0 受管 BSP 源码。
    BSP_SOURCE_PATH = REPOSITORY_ROOT / (
        "esp32/firmware/managed_components/"
        "waveshare__esp32_s3_touch_amoled_2_06/esp32_s3_touch_amoled_2_06.c"
    )

    # bsp_manifest_path 指向组件自身版本声明，用于防止锁文件与实际源码再次不一致。
    BSP_MANIFEST_PATH = REPOSITORY_ROOT / (
        "esp32/firmware/managed_components/"
        "waveshare__esp32_s3_touch_amoled_2_06/idf_component.yml"
    )

    # lvgl_port_source_path 指向实际分配绘制与旋转缓冲的 LVGL 端口实现。
    LVGL_PORT_SOURCE_PATH = REPOSITORY_ROOT / (
        "esp32/firmware/managed_components/"
        "espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port_disp.c"
    )
    # sdkconfig_path 是本轮 ESP-IDF 构建真实采用的已解析配置。
    SDKCONFIG_PATH = REPOSITORY_ROOT / "esp32/firmware/sdkconfig"
    # sdkconfig_defaults_path 保存清理构建目录后仍可恢复的产品默认值。
    SDKCONFIG_DEFAULTS_PATH = REPOSITORY_ROOT / "esp32/firmware/sdkconfig.defaults"

    def test_qspi_display_matches_factory_brookesia_transport_contract(self) -> None:
        """Factory 私有 BSP 必须把调用方 cfg 透传到 SPI LVGL 端口。"""

        # source 读取最终 ESP-IDF 构建直接编译的受管组件，不检查无关桌面预览器。
        source = self.BSP_SOURCE_PATH.read_text(encoding="utf-8")
        # 组件自身必须声明 2.0.0，不能只修改 dependencies.lock 伪装升级完成。
        manifest = self.BSP_MANIFEST_PATH.read_text(encoding="utf-8")
        # 版本行使用厂家组件原始 YAML 语法。
        self.assertIn("version: 2.0.0", manifest)
        # Factory Brookesia 二进制暴露 bsp_display_lcd_init(cfg)，板级初始化必须接收调用方配置。
        self.assertIn(
            "static lv_display_t *bsp_display_lcd_init(const bsp_display_cfg_t *cfg)",
            source,
        )
        # 函数必须拒绝空配置，避免后续字段解引用未定义行为。
        self.assertIn("assert(cfg != NULL);", source)
        # SPI 单次最大传输字节数必须跟随 RGB565 调用方缓冲，不再固定为整屏或另一套 Kconfig。
        self.assertIn(".max_transfer_sz = cfg->buffer_size * sizeof(uint16_t)", source)
        # LVGL 缓冲像素数必须原样使用调用方配置。
        self.assertIn(".buffer_size = cfg->buffer_size", source)
        # 双缓冲开关必须原样使用调用方配置。
        self.assertIn(".double_buffer = cfg->double_buffer", source)
        # DMA 能力必须由调用方配置传入，不能在私有函数内硬编码关闭。
        self.assertIn(".buff_dma = cfg->flags.buff_dma", source)
        # PSRAM 位置必须由调用方配置传入，避免 DMA 与 PSRAM 组合被静默改写。
        self.assertIn(".buff_spiram = cfg->flags.buff_spiram", source)
        # SH8601 明确使用 QSPI；必须通过 I2C/SPI/I8080 通用端口注册完成回调。
        self.assertIn("lvgl_port_add_disp(&disp_cfg)", source)
        # RGB 专用端口不能绑定到 QSPI 面板。
        self.assertNotIn("lvgl_port_add_disp_rgb(&disp_cfg, &rgb_cfg)", source)
        # start_with_config 必须把同一 cfg 传给板级 LCD 初始化，禁止再次丢失参数。
        self.assertIn("BSP_NULL_CHECK(disp = bsp_display_lcd_init(cfg), NULL);", source)

    def test_default_factory_display_buffer_is_internal_dma_memory(self) -> None:
        """默认 Factory 显示配置必须使用片内 DMA 缓冲，禁止运行期临时复制。"""

        # source 读取 bsp_display_start 默认配置；产品入口调用该函数取得显示器。
        source = self.BSP_SOURCE_PATH.read_text(encoding="utf-8")
        # 默认缓冲使用当前 BSP 的 LVGL 行高，保持 410 像素宽度和已有 Kconfig 尺寸。
        self.assertIn(".buffer_size = BSP_LCD_H_RES * LVGL_BUFFER_HEIGHT", source)
        # 默认缓冲必须具备 DMA 能力，SPI 驱动可直接发送 LVGL 绘制结果。
        self.assertIn(".buff_dma = true", source)
        # DMA 缓冲必须位于片内 SRAM；ESP32-S3 的普通 PSRAM 不满足该传输合同。
        self.assertIn(".buff_spiram = false", source)
        # 读取实际和可重建配置，产品同时运行 NimBLE 与九任务时只保留 20 行分块。
        sdkconfig = self.SDKCONFIG_PATH.read_text(encoding="utf-8")
        # 清理构建目录后仍必须恢复相同的 20 行片内 DMA 配置。
        defaults = self.SDKCONFIG_DEFAULTS_PATH.read_text(encoding="utf-8")
        # 410×20×RGB565 占 16,400 字节，比 100 行释放 65,600 字节片内 RAM。
        self.assertIn("CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20", sdkconfig)
        # 默认文件必须显式锁定值，禁止 Kconfig 默认 100 行在干净构建中回归。
        self.assertIn("CONFIG_BSP_DISPLAY_LVGL_BUF_HEIGHT=20", defaults)

    def test_native_orientation_does_not_allocate_unused_rotation_buffer(self) -> None:
        """原生 0° 方向不得再申请一块 82 KB 的软件旋转 DMA 缓冲。"""

        # source 读取真板 BSP 的最终显示配置，保证测试约束实际链接的代码。
        source = self.BSP_SOURCE_PATH.read_text(encoding="utf-8")
        # 当前 rotation 三项均为 false，软件旋转没有任何像素变换用途，必须关闭。
        self.assertIn(".sw_rotate = false", source)
        # 禁止旧值回归，否则完整产品启动时会再次耗尽片内 DMA 内存并循环重启。
        self.assertNotIn(".sw_rotate = true", source)

    def test_lvgl_display_allocation_failure_returns_null(self) -> None:
        """LVGL 缓冲分配失败必须删除半成品 display 并返回 NULL。"""

        # source 读取实际触发 IllegalInstruction 的 LVGL 9 端口错误路径。
        source = self.LVGL_PORT_SOURCE_PATH.read_text(encoding="utf-8")
        # error_start 定位统一错误清理分支，避免匹配正常删除接口。
        error_start = source.index("err:\n    if (ret != ESP_OK)")
        # error_body 截取到函数最终返回，限定断言只检查初始化失败处理。
        error_body = source[error_start : source.index("#if LVGL_PORT_HANDLE_FLUSH_READY", error_start)]
        # 半成品 LVGL display 必须先删除，不能把已释放 driver_data 的句柄留在全局链表。
        self.assertIn("lv_display_delete(disp);", error_body)
        # 删除后必须清空返回句柄，让 Waveshare BSP 停止后续触摸和页面初始化。
        self.assertIn("disp = NULL;", error_body)


class FirmwareWorkoutEvidenceContractTests(unittest.TestCase):
    """锁定动作段分类证据在会话边界和 IMU 连续性破坏时被清空。"""

    # 仓库根由当前测试文件的父目录确定，保证任意工作目录运行结果一致。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向协调 UI、BLE 和 IMU 流水线的唯一应用入口。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"

    def test_start_resume_and_data_gaps_reset_stale_evidence(self) -> None:
        """新会话、恢复和数据缺口不得继承旧窗口或旧动作段 logits。"""

        # source 读取 UTF-8 中文注释与生产接线，缺少任一调用都应阻止交付。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # UI START/RESUME 成功分支必须重置 62 点推理流水线。
        self.assertIn(
            "(command == UI_COMMAND_START) || (command == UI_COMMAND_RESUME)",
            source,
        )
        # BLE START/RESUME 必须执行同一隔离规则，不能只修本机触摸路径。
        self.assertIn(
            "(control == DEVICE_CONTROL_START) || (control == DEVICE_CONTROL_RESUME)",
            source,
        )
        # 两条控制来源各有一次显式流水线重置，防止暂停前窗口进入恢复后的分类。
        self.assertGreaterEqual(source.count("imu_pipeline_reset_session(&s_imu_pipeline);"), 2)
        # 严重质量位掩码必须排除马达污染，但覆盖六种数据连续性破坏。
        bout_mask = source[source.index("const uint32_t bout_reset_mask =") :]
        # 只截取掩码定义到第一个分号，避免后面的计数冻结掩码影响断言。
        bout_mask = bout_mask[: bout_mask.index(";")]
        # 六种连续性破坏全部必须触发动作段历史清空。
        for quality_name in (
            "IMU_QUALITY_ACCEL_GAP",
            "IMU_QUALITY_GYRO_GAP",
            "IMU_QUALITY_OUT_OF_ORDER",
            "IMU_QUALITY_QUEUE_OVERFLOW",
            "IMU_QUALITY_DRIVER_DROP",
            "IMU_QUALITY_RESAMPLER_RESET",
        ):
            # 每个质量位必须出现在专用动作段重置掩码中。
            self.assertIn(quality_name, bout_mask)
        # 马达污染只冻结当前点，不能清空整个连续动作段证据。
        self.assertNotIn("IMU_QUALITY_HAPTIC_CONTAMINATED", bout_mask)
        # 应用入口必须实际调用领域层重置 API，而不是只写注释或日志。
        self.assertGreaterEqual(
            source.count("workout_engine_reset_bout_evidence(&s_coordinator.workout);"),
            2,
        )


class FirmwarePairingUiContractTests(unittest.TestCase):
    """锁定六位配对码、清除原因和双端忘记绑定的设备端接线。"""

    # 仓库根使用当前测试文件位置解析，不依赖 PowerShell 当前目录。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向 NimBLE、应用任务与 LVGL 的生产编排入口。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"
    # ble_source_path 指向设备端安全管理与固定联调 PIN 的生产实现。
    BLE_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/components/ble_service/src/ble_service_nimble.c"

    def test_debug_pairing_uses_fixed_pin_123456_with_authenticated_security(self) -> None:
        """现场联调必须固定使用 123456，同时保留 MITM、SC 和绑定保护。"""

        # source 读取设备端真实 NimBLE 配置，禁止只修改上位机提示文字。
        source = self.BLE_SOURCE_PATH.read_text(encoding="utf-8")
        # 固定 PIN 必须由唯一命名常量定义，避免处理器各分支出现不同数字。
        self.assertIn("#define BLE_SERVICE_PAIRING_PASSKEY UINT32_C(123456)", source)
        # Display Only 回调必须把该常量注入安全管理器，不能继续生成随机值。
        self.assertIn("const uint32_t passkey = BLE_SERVICE_PAIRING_PASSKEY;", source)
        # 随机数调用必须移除，否则用户无法提前知道本轮 PIN。
        self.assertNotIn("esp_random()", source)
        # 固定 PIN 不等于关闭身份保护；设备仍要求绑定。
        self.assertIn("ble_hs_cfg.sm_bonding = UINT8_C(1);", source)
        # 固定 PIN 不等于 Just Works；设备仍要求中间人保护。
        self.assertIn("ble_hs_cfg.sm_mitm = UINT8_C(1);", source)
        # 固定 PIN 继续使用 LE Secure Connections。
        self.assertIn("ble_hs_cfg.sm_sc = UINT8_C(1);", source)

    def test_pairing_mailbox_is_connected_to_nimble_and_lvgl(self) -> None:
        """NimBLE 回调必须经原子邮箱进入应用任务，且设置页能够删除绑定。"""

        # source 读取生产入口中的全部中文接线说明和函数调用。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 配置必须同时注册显示和清除回调，避免只在串口打印或成功后残留旧码。
        self.assertIn(".passkey_display = app_ble_passkey_display", source)
        # 清除回调覆盖成功、失败、断线、忘记和停服路径。
        self.assertIn(".passkey_clear = app_ble_passkey_clear", source)
        # 两个回调必须共享静态原子邮箱，不能让 NimBLE 任务直接调用 LVGL。
        self.assertIn(".passkey_context = &s_pairing_mailbox", source)
        # 应用任务必须消费邮箱并把事件交给唯一 UI 状态机。
        self.assertIn("app_drain_pairing_mailbox();", source)
        # 60 秒超时必须由单调时钟生成清除事件。
        self.assertIn("ui_app_pairing_build_timeout_event(", source)
        # 设置页命令必须调用 NimBLE 官方绑定删除包装 API。
        self.assertIn("ble_service_nimble_forget_all_bonds();", source)
        # 配对覆盖层显示期间不得被普通熄屏门槛提前关闭。
        self.assertIn("if (s_coordinator.ui.view.pairing_active)", source)

    def test_repeat_pairing_replaces_only_the_current_peer_bond(self) -> None:
        """Windows 与设备绑定状态不一致时必须按 NimBLE 官方流程删除当前对端并重试。"""

        # source 读取真实 GAP 回调，避免只在上位机提示用户手工清缓存。
        source = self.BLE_SOURCE_PATH.read_text(encoding="utf-8")
        # repeat_start 把断言限制在重复配对分支，防止其它忘记设备 API造成误通过。
        repeat_start = source.index("if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING)")
        # repeat_end 截止到其它 GAP 事件出口，形成单一处理块。
        repeat_end = source.index("// 其它 GAP 事件不需要业务处理。", repeat_start)
        # repeat_body 保存重复配对的完整恢复路径。
        repeat_body = source[repeat_start:repeat_end]
        # 必须先按连接句柄查询当前对端身份，不能清除全部电脑绑定。
        self.assertIn("ble_gap_conn_find(event->repeat_pairing.conn_handle, &connection)", repeat_body)
        # 只删除查询到的 peer_id_addr，禁止调用全局 ble_store_clear。
        self.assertIn("ble_store_util_delete_peer(&connection.peer_id_addr)", repeat_body)
        # 删除成功后必须请求 NimBLE 继续同一次配对。
        self.assertIn("return BLE_GAP_REPEAT_PAIRING_RETRY;", repeat_body)
        # 当前分支不得调用全量绑定删除 API。
        self.assertNotIn("ble_store_clear", repeat_body)


# 直接执行本文件时运行全部合同测试，便于开发者单独复验。
if __name__ == "__main__":
    # verbosity=2 输出中文测试名称和失败位置。
    unittest.main(verbosity=2)
