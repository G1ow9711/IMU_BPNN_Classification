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


class FirmwareTaskMemoryContractTests(unittest.TestCase):
    """锁定科技 UI、BLE 与业务任务同时启动时的片内内存边界。"""

    # 仓库根由测试文件位置解析，避免依赖调用者当前目录。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向真实 ESP-IDF 任务创建与启动失败接线。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"
    # ble_source_path 指向真板发生栈溢出的独立 BLE 业务任务实现。
    BLE_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/components/ble_service/src/ble_service_nimble.c"
    # sdkconfig_defaults_path 指向可在全新构建目录复现的 PSRAM 分配策略。
    SDKCONFIG_DEFAULTS_PATH = REPOSITORY_ROOT / "esp32/firmware/sdkconfig.defaults"
    # sdkconfig_path 指向当前真板镜像实际使用的 ESP-IDF 配置。
    SDKCONFIG_PATH = REPOSITORY_ROOT / "esp32/firmware/sdkconfig"

    def test_general_heap_prefers_psram_and_keeps_internal_reserve(self) -> None:
        """普通对象必须优先进入 PSRAM，片内保留区只供任务栈和 DMA。"""

        # defaults 保存全新配置时必须继承的产品内存策略。
        defaults = self.SDKCONFIG_DEFAULTS_PATH.read_text(encoding="utf-8")
        # current 保存本轮即将链接并烧录的实际配置，防止只改默认值却复用旧缓存。
        current = self.SDKCONFIG_PATH.read_text(encoding="utf-8")
        # 两份配置都必须让普通 malloc 优先使用 8 MiB PSRAM，避免 LVGL 小对象挤碎片内堆。
        for config_text in (defaults, current):
            # 阈值为零表示任意大小的普通 malloc 都先尝试 PSRAM，失败后才回退片内堆。
            self.assertIn("CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0", config_text)
            # 32 KiB 片内保留区专供显式内部栈和 DMA，防止普通对象耗尽关键内存。
            self.assertIn("CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768", config_text)

    def test_ui_stack_uses_psram_and_owner_stack_keeps_static_margin(self) -> None:
        """UI 栈必须使用 PSRAM，应用栈必须保留已审计调用链余量。"""

        # source 读取完整生产入口，防止只在 sdkconfig 或测试替身中修改内存策略。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 应用任务最深静态调用链约 10.9 KiB；16 KiB 保留约 5 KiB 异步调用余量。
        self.assertIn("#define APP_OWNER_TASK_STACK_BYTES (16U * 1024U)", source)
        # 科技 UI 的渲染任务不执行 Flash/NVS 写入，使用板载 PSRAM 避免挤占 BLE 片内堆。
        self.assertIn("const BaseType_t ui_created = xTaskCreateWithCaps(", source)
        # UI 栈必须同时要求 PSRAM 和八位访问能力，不能退回普通内部堆。
        self.assertIn("MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", source)

    def test_task_allocation_failure_shows_error_instead_of_reboot_loop(self) -> None:
        """任务内存不足必须停在稳定错误页，禁止 ESP_ERROR_CHECK 重启循环。"""

        # source 保存任务创建、故障码和 app_main 启动顺序。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 截取任务创建到首次 HOME 投递，限定断言只覆盖本次启动内存门。
        gate_start = source.index("if (!app_create_tasks())")
        # HOME 投递必须发生在任务与启动状态转移均成功之后。
        gate_end = source.index("app_queue_ui(&s_coordinator.ui);", gate_start)
        # gate_body 包含失败处理和成功转移。
        gate_body = source[gate_start:gate_end]
        # 内存失败必须使用独立稳定故障码，不能伪装成传感器或模型失败。
        self.assertIn("APP_STARTUP_FAULT_TASK_MEMORY", gate_body)
        # 已可见的 SELF_TEST 页面必须切换到中文错误页。
        self.assertIn(
            "app_show_startup_error(&startup_ui, APP_STARTUP_FAULT_TASK_MEMORY)",
            gate_body,
        )
        # 原来的强制 abort 会形成设备检查重启循环，生产入口必须删除。
        self.assertNotIn("ESP_ERROR_CHECK(ESP_ERR_NO_MEM)", gate_body)
        # SELF_TEST 到 HOME 的成功转移必须晚于任务创建，失败时上下文仍可合法进入 ERROR。
        self.assertLess(
            gate_body.index("app_create_tasks()"),
            gate_body.index("app_finish_startup_success(&startup_ui)"),
        )

    def test_ble_business_separates_large_frames_and_keeps_runtime_margin(self) -> None:
        """BLE 控制与传输大缓冲必须跨函数隔离，并覆盖首次绑定后的能力同步突发。"""

        # source 读取真板 ble_business 创建点和两个大缓冲处理函数。
        source = self.BLE_SOURCE_PATH.read_text(encoding="utf-8")
        # 真板首次绑定后的多条控制请求已证明 8 KiB 会越过金丝雀；16 KiB 覆盖同一突发并保留库调用余量。
        self.assertIn("#define BLE_SERVICE_WORKER_STACK_BYTES (16U * 1024U)", source)
        # 独立边界属性阻止优化器把控制响应缓冲合并进常驻 worker 栈帧。
        self.assertIn("#define BLE_SERVICE_STACK_BOUNDARY __attribute__((noinline))", source)
        # 控制响应路径必须保留独立栈帧，避免 1040 字节响应与队列工作项永久叠加。
        self.assertIn(
            "static BLE_SERVICE_STACK_BOUNDARY void ble_service_process_control_work(",
            source,
        )
        # 会话传输路径包含两个最大帧缓冲，也必须与 worker 常驻工作项分帧。
        self.assertIn(
            "static BLE_SERVICE_STACK_BOUNDARY int ble_service_handle_transfer_write(",
            source,
        )
        # 任务创建必须实际使用新的字节常量，禁止只改注释或保留旧 6 KiB 实参。
        self.assertIn("BLE_SERVICE_WORKER_STACK_BYTES,", source)
        # 每次完成工作项后必须读取历史最小剩余字节，真板验收不能只靠“未立即重启”判断栈安全。
        self.assertIn("uxTaskGetStackHighWaterMark(NULL)", source)
        self.assertIn("BLE_WORKER_STACK minimum_free_bytes=", source)
        # 旧的 WORDS 命名和 6144 字节危险边界必须完全移除。
        self.assertNotIn("BLE_SERVICE_WORKER_STACK_WORDS", source)

    def test_ble_security_start_keeps_an_existing_handshake_alive(self) -> None:
        """Windows 已开始恢复绑定时，NimBLE 的 EALREADY 不能被误判为致命连接失败。"""

        # source 读取真实 GAP CONNECT 回调；桌面扫描成功不能替代该真机安全竞态合同。
        source = self.BLE_SOURCE_PATH.read_text(encoding="utf-8")
        # EALREADY 由 ESP-IDF 5.5.4 官方 ble_gap.h 定义为“安全流程已经在进行”。
        self.assertIn(
            "(security_result != BLE_HS_EALREADY)",
            source,
        )
        # 只有真正失败才允许终止链路；正常启动和已在进行都必须等待 ENC_CHANGE 权威结果。
        self.assertIn(
            "if ((security_result != 0) &&\n"
            "                (security_result != BLE_HS_EALREADY))",
            source,
        )


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

    def test_product_home_exposes_training_and_right_wrist_contract(self) -> None:
        """主页必须显示产品训练入口和右腕佩戴域，禁止重新暴露内部联调版本。"""

        # source 读取实际烧录固件使用的 UI 状态文案，避免测试预览器中的假标记通过合同。
        source = self.UI_SOURCE_PATH.read_text(encoding="utf-8")
        # 主页主操作必须使用成熟产品文案，避免开发日期占据视觉中心。
        self.assertIn('"准备开始训练"', source)
        # 当前冻结模型只按右手腕佩戴域验收，手表必须直接提示用户。
        self.assertIn('"右手佩戴 / 自动识别"', source)
        # 旧联调版本标签不得再出现在用户页面。
        self.assertNotIn("常亮联调版 0716", source)

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
        # guard 必须同时检查两个隔离阶段和空 UI 控制邮箱。
        self.assertIn(
            "if (APP_BENCH_DISPLAY_ONLY || APP_BENCH_SENSOR_ONLY || "
            "(s_ui_command_queue == NULL))",
            source[callback_start:callback_end],
        )
        # 功能联调不允许重新启用任何自动睡眠入口。
        for forbidden_call in (
            "#define APP_BENCH_ALWAYS_ON (false)",
        ):
            # 报错时直接显示意外启用的低功耗常量。
            self.assertNotIn(forbidden_call, source)

    def test_raw_stream_temporarily_forces_qmi_active_sampling(self) -> None:
        """阶段一 RawStream 开启后必须临时恢复 QMI ACTIVE，关闭或断线后恢复页面功耗策略。"""

        # source 读取真板唯一应用入口，不能用 Mock 样本替代 QMI 电源门控合同。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # helper_start 定位 RawStream 专用功耗覆盖函数。
        helper_start = source.index("static void app_queue_raw_stream_power_policy")
        # helper_end 截止到下一个应用辅助函数，限制断言只检查该覆盖实现。
        helper_end = source.index("static void app_dispatch_effects", helper_start)
        # helper_body 保存完整策略复制、ACTIVE 覆盖和恢复路径。
        helper_body = source[helper_start:helper_end]
        # 常亮阶段一必须在命令路径短路，Cmd11 不得边回复 ACK 边切换 QMI 或 BLE 电源状态。
        self.assertIn("if (APP_BENCH_ALWAYS_ON)", helper_body)
        # 常亮短路必须直接返回，只保留 RawStream 门控和流水线重置。
        self.assertIn("return;", helper_body)
        # 覆盖必须从协调器当前策略复制，关闭诊断时才能恢复 HOME/WOM 或训练 ACTIVE。
        self.assertIn("power_manager_policy(&s_coordinator.power)", helper_body)
        # 只有 enabled 分支把 IMU 改为 25 Hz 活动采样，不能篡改长期 power_manager 状态。
        self.assertIn("policy.imu_mode = POWER_IMU_ACTIVE_25HZ", helper_body)
        # 临时诊断策略必须显式保持当前 BLE 链路，禁止控制 ACK 期间重新提交 GAP 连接参数。
        self.assertIn("app_queue_power_internal(&policy, false, true)", helper_body)
        # RawStream 覆盖只负责 QMI，不允许悄悄改写协调器给出的 BLE 模式枚举。
        self.assertNotIn("policy.ble_mode =", helper_body)
        # 电源任务必须读取链路保护位，防止新增字段只停留在队列结构而没有运行时效果。
        self.assertIn("if (!request.preserve_ble_link)", source)
        # 常亮版开机初始策略必须预先保持 QMI ACTIVE，不能等 Cmd11 到来才切寄存器。
        self.assertIn("initial_policy.imu_mode = POWER_IMU_ACTIVE_25HZ", source)
        # 常亮初始策略必须显式关闭自动 Light-sleep，保证 25 Hz 时间轴连续。
        self.assertIn("initial_policy.automatic_light_sleep = false", source)
        # 启动后的电池或连接事件也必须保持 ACTIVE；只改初始策略会被长度一电源队列覆盖。
        power_queue_start = source.index("static void app_queue_power_internal")
        # power_queue_end 截止到普通包装函数，只检查每次电源请求的全局联调覆盖。
        power_queue_end = source.index("static void app_queue_power(", power_queue_start)
        # power_queue_body 保存阶段一对所有领域策略的最终覆盖。
        power_queue_body = source[power_queue_start:power_queue_end]
        # 每个阶段一电源请求都必须把 QMI 固定为活动采样。
        self.assertIn("request.policy.imu_mode = POWER_IMU_ACTIVE_25HZ", power_queue_body)
        # 每个阶段一电源请求都必须清除自动 Light-sleep。
        self.assertIn("request.policy.automatic_light_sleep = false", power_queue_body)
        # Cmd11 在返回成功 ACK 前必须投递对应的 ACTIVE 或恢复策略。
        raw_command_start = source.index(
            "if (command.command_id == (uint8_t)DEVICE_COMMAND_SET_RAW_STREAM)"
        )
        # raw_command_end 截止到配置持久化分支，限定 Cmd11 的易失处理块。
        raw_command_end = source.index("/* 保存协调器旧副本", raw_command_start)
        # raw_command_body 保存 Cmd11 的运行态提交顺序。
        raw_command_body = source[raw_command_start:raw_command_end]
        # Cmd11 处理期必须先保持发布门关闭，不能让 25 Hz 通知抢在控制应答前进入 GATT。
        self.assertIn("s_raw_stream_enabled = false", raw_command_body)
        # 开启命令必须安排延迟激活，而不是在控制事务中同步开流。
        self.assertIn("s_raw_stream_activation_pending = true", raw_command_body)
        # 激活门槛必须使用冻结延迟常量，便于测试和协议审计。
        self.assertIn("APP_RAW_STREAM_ACTIVATION_DELAY_MS", raw_command_body)
        # 真正发布门只能由后续 QMI 帧处理路径打开。
        qmi_handler_start = source.index("static APP_STACK_BOUNDARY void app_process_qmi_frame")
        # qmi_handler_end 截止到下一辅助函数，限制断言范围。
        qmi_handler_end = source.index("static void app_project_device_config_to_ui", qmi_handler_start)
        # qmi_handler_body 保存 QMI 到期激活事务。
        qmi_handler_body = source[qmi_handler_start:qmi_handler_end]
        # QMI 处理必须等待 pending 和单调时间门槛同时满足。
        self.assertIn("s_raw_stream_activation_pending &&", qmi_handler_body)
        # 只有到期后的 QMI 帧才能打开发布门。
        self.assertIn("s_raw_stream_enabled = true", qmi_handler_body)
        # 开启和关闭都必须调用统一功耗覆盖函数。
        self.assertIn(
            "app_queue_raw_stream_power_policy(candidate.raw_stream_enabled)",
            raw_command_body,
        )
        # 开启新诊断必须清除 WOM 前的旧重采样窗口，禁止把时间缺口拼进 62 点特征。
        self.assertIn("imu_pipeline_reset_session(&s_imu_pipeline)", raw_command_body)
        # BLE 断线清除 RawStream 后也必须恢复当前页面策略，防止 HOME 长期维持高功耗采样。
        disconnect_start = source.index("case APP_EVENT_BLE_CONNECTION:")
        # disconnect_end 截止到下一事件 case，只检查断线处理。
        disconnect_end = source.index("case APP_EVENT_PAIRING_UPDATE:", disconnect_start)
        # 断线事件必须调用 false 恢复策略。
        self.assertIn(
            "app_queue_raw_stream_power_policy(false)",
            source[disconnect_start:disconnect_end],
        )

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
        """全部产品页必须使用固定智能手表网格，禁止 flex 重排留下旧字。"""

        # renderer_source 读取真板 LVGL 对象树实现，桌面预览坐标不能替代设备合同。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # helper_start 定位全部产品页共用的绝对布局，避免只修 HOME 而其它页面漂移。
        helper_start = renderer_source.index("static void ui_lvgl_apply_product_layout")
        # helper_end 截止到下一页面创建函数，限定固定布局函数本体。
        helper_end = renderer_source.index("static bool ui_lvgl_create_page", helper_start)
        # helper_body 保存品牌顶栏、主卡文字和按钮行的几何设置。
        helper_body = renderer_source[helper_start:helper_end]
        # 品牌、副标题、BLE 与四个动态业务文本必须裁剪，文案变化不触发自动换行重算。
        self.assertEqual(helper_body.count("LV_LABEL_LONG_CLIP"), 7)
        # 品牌、设备状态、三段主卡文字和按钮行必须逐对象设置固定位置。
        for object_name in (
            "page->brand_mark",
            "page->brand_label",
            "page->title_label",
            "page->battery_label",
            "page->ble_dot",
            "page->ble_label",
            "page->status_label",
            "page->primary_label",
            "page->secondary_label",
            "page->footer_label",
            "page->button_row",
        ):
            # 每个产品对象都必须进入绝对定位网格。
            self.assertIn(f"lv_obj_set_pos({object_name}", helper_body)
        # 页面创建结束时必须实际调用统一智能手表布局函数。
        self.assertIn("ui_lvgl_apply_product_layout(page, state)", renderer_source)

    def test_status_bar_reserves_separate_battery_and_ble_rows(self) -> None:
        """电池图标与蓝牙状态必须使用独立纵向区域，不能再次发生文字重叠。"""

        # renderer_source 读取真板链接的固定坐标页面，桌面预览不能替代设备几何合同。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # 电池外框顶部和高度必须使用命名常量，避免创建坐标与布局坐标分裂。
        self.assertIn("#define UI_BATTERY_SHELL_Y_PX (31)", renderer_source)
        self.assertIn("#define UI_BATTERY_SHELL_HEIGHT_PX (20)", renderer_source)
        # 蓝牙文字顶部至少为 56 像素，与电池底部 51 像素保留 5 像素抗锯齿间距。
        self.assertIn("#define UI_BLE_LABEL_Y_PX (56)", renderer_source)
        # 命名常量定义和统一布局必须引用同一蓝牙纵坐标；标签创建后只在布局函数定位一次。
        self.assertGreaterEqual(renderer_source.count("UI_BLE_LABEL_Y_PX"), 2)
        # 生产代码不得再把蓝牙文字放到与电池相交的 49 像素位置。
        self.assertNotIn(
            "lv_obj_set_pos(page->ble_label, 295, UI_TOP_BAR_Y_PX + 21)",
            renderer_source,
        )

    def test_settings_button_grid_cannot_scroll_away_from_brightness(self) -> None:
        """设置页四按钮必须使用显式四宫格，不能依赖主题相关 flex 自动换行。"""

        # renderer_source 读取真实 LVGL 对象树；亮度按钮可见性取决于父容器标志。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # 固定按钮区创建后必须移除默认可滚动标志，纵向拖动只作为普通触摸释放。
        self.assertIn(
            "lv_obj_remove_flag(page->button_row, LV_OBJ_FLAG_SCROLLABLE);",
            renderer_source,
        )
        # 进入固定布局时必须把历史滚动偏移归零，升级后不会继承旧页面位置。
        self.assertIn(
            "lv_obj_scroll_to(page->button_row, 0, 0, LV_ANIM_OFF);",
            renderer_source,
        )
        # 自动换行会叠加主题列间距，使两个 166 像素按钮在真板 338 像素内宽中退化为单列。
        self.assertNotIn(
            "lv_obj_set_flex_flow(page->button_row, LV_FLEX_FLOW_ROW_WRAP);",
            renderer_source,
        )
        # 设置页必须切换为绝对布局，四个槽位的位置由像素合同决定。
        self.assertIn(
            "lv_obj_set_layout(page->button_row, LV_LAYOUT_NONE);",
            renderer_source,
        )
        # 亮度、诊断、忘记电脑、返回依次占据左上、右上、左下、右下。
        for index, x, y in (
            (0, 0, 0),
            (1, 180, 0),
            (2, 0, 64),
            (3, 180, 64),
        ):
            # 每个可见槽位都必须具备独立坐标，隐藏第五槽位不得参与布局。
            self.assertIn(
                f"lv_obj_set_pos(page->buttons[{index}], {x}, {y});",
                renderer_source,
            )

    def test_product_page_switches_do_not_keep_screen_load_animations(self) -> None:
        """多轮训练复用固定 screen 时必须立即切页，避免动画生命周期积累。"""

        # renderer_source 是所有产品页面唯一的 LVGL 切换实现。
        renderer_source = (
            self.REPOSITORY_ROOT / "esp32/firmware/components/ui/ui_lvgl_renderer.c"
        ).read_text(encoding="utf-8")
        # 异步 QSPI 真板采用立即加载；局部计数反馈可独立保留，不得再动画加载整张 screen。
        self.assertNotIn("lv_screen_load_anim(", renderer_source)
        # 页面状态变化仍必须真实加载目标 screen。
        self.assertIn("lv_screen_load(page->root)", renderer_source)

    def test_ui_commands_use_dedicated_lossless_mailbox_and_queue_set(self) -> None:
        """触摸命令不得与 125 Hz QMI 帧竞争同一满队列。"""

        # main_source 读取真板唯一任务和队列装配入口。
        main_source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # UI 邮箱长度固定为一，并用 overwrite 保留用户最后一次有效操作。
        self.assertIn("#define APP_UI_COMMAND_QUEUE_LENGTH UINT32_C(1)", main_source)
        self.assertIn("static QueueHandle_t s_ui_command_queue;", main_source)
        self.assertIn("xQueueOverwrite(s_ui_command_queue, &command_event)", main_source)
        # 主数据队列和 UI 邮箱必须加入同一 Queue Set，应用任务才能无轮询地等待任一来源。
        self.assertIn("static QueueSetHandle_t s_app_input_queue_set;", main_source)
        self.assertIn("xQueueCreateSet(", main_source)
        self.assertIn("xQueueAddToSet(s_app_event_queue, s_app_input_queue_set)", main_source)
        self.assertIn("xQueueAddToSet(s_ui_command_queue, s_app_input_queue_set)", main_source)
        self.assertIn("xQueueSelectFromSet(s_app_input_queue_set, portMAX_DELAY)", main_source)
        # 回调不得再把 UI_COMMAND 写入共享数据队列，否则满队列故障路径仍存在。
        callback_start = main_source.index("static void app_ui_command_callback")
        callback_end = main_source.index("static void app_ble_connection_changed", callback_start)
        callback_body = main_source[callback_start:callback_end]
        self.assertNotIn("xQueueSend(s_app_event_queue", callback_body)

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
        # 顶部品牌栏固定从 28 像素开始，和厂家 410×502 圆角安全区一致。
        self.assertIn("#define UI_SAFE_TOP_PX (28)", renderer_source)
        # 根 screen 必须关闭 flex；产品网格按绝对坐标管理安全区。
        self.assertIn("lv_obj_set_layout(screen, LV_LAYOUT_NONE)", renderer_source)
        # 品牌标和按钮行必须显式引用 32 像素水平安全边距。
        self.assertIn("lv_obj_set_pos(page->brand_mark, UI_SAFE_HORIZONTAL_PX", renderer_source)
        self.assertIn("lv_obj_set_pos(page->button_row, UI_SAFE_HORIZONTAL_PX", renderer_source)
        # 安全内容宽度必须同时约束主卡、按钮行和两列设置按钮。
        self.assertGreaterEqual(renderer_source.count("UI_SAFE_CONTENT_WIDTH_PX"), 4)

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
    """主动作、实时分类门和 IMU 连续性边界必须遵守同一训练合同。"""

    # 仓库根由当前测试文件的父目录确定，保证任意工作目录运行结果一致。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # main_source_path 指向协调 UI、BLE 和 IMU 流水线的唯一应用入口。
    MAIN_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/main/main.c"
    # workout_header_path 保存验证集锁定的窗口数、最大等待和 Q15 概率常量。
    WORKOUT_HEADER_PATH = REPOSITORY_ROOT / "esp32/firmware/components/workout_engine/include/workout_engine.h"
    # workout_source_path 保存准备态独立窗候选、累计兜底和稳定窗生产逻辑。
    WORKOUT_SOURCE_PATH = REPOSITORY_ROOT / "esp32/firmware/components/workout_engine/workout_engine.c"

    def test_preparing_uses_validation_selected_independent_window_policy(self) -> None:
        """准备态必须独立窗连续确认，并仅在第四窗使用累计兜底。"""

        # header 读取冻结策略常量，避免测试只匹配实现中的偶然数字。
        header = self.WORKOUT_HEADER_PATH.read_text(encoding="utf-8")
        # source 读取实际独立窗连续门和有界累计兜底生产逻辑。
        source = self.WORKOUT_SOURCE_PATH.read_text(encoding="utf-8")
        # 独立窗口最优类必须连续保持两窗，首窗不能永久锁定整场。
        self.assertIn("#define WORKOUT_ACTION_LOCK_WINDOWS (2U)", header)
        # 最大准备窗口固定为四，对应首窗后最多 1.44 秒。
        self.assertIn("#define WORKOUT_ACTION_MAX_PREPARE_WINDOWS (4U)", header)
        # 一次 62 点重建再接三次 12 点步进共覆盖 160 点，准备缓存必须保留完整点击开始时间线。
        self.assertIn("#define WORKOUT_PRELOCK_SAMPLE_CAPACITY (160U)", header)
        # 50% Q15 门固定为 32768/65535，不得回退旧 55% 单窗门。
        self.assertIn("#define WORKOUT_ACTION_LOCK_CONFIDENCE_Q15 (32768U)", header)
        # PREPARING 必须先独立分类当前单窗，避免强错误首窗污染后续连续门。
        self.assertIn(
            "const workout_status_t window_classify_status = workout_classify_single_window(",
            source,
        )
        # 低于 50% 的独立窗口必须清空候选，不能延续上一类别。
        self.assertIn(
            "window_confidence_q15 < WORKOUT_ACTION_LOCK_CONFIDENCE_Q15",
            source,
        )
        # 正常锁定要求同一独立窗口类别连续满足验证选择的窗口数。
        self.assertIn("const bool stable_and_confident", source)
        # 低置信或多类摇摆会话最迟第四窗按累计 argmax 结束准备。
        self.assertIn("const bool reached_bounded_limit", source)
        # 正常门使用独立候选，第四窗才允许选择累计最佳类别。
        self.assertIn(
            "stable_and_confident ? engine->candidate_action : cumulative_best_class",
            source,
        )

    def test_metric_event_keeps_original_imu_timestamp_for_csv_marker(self) -> None:
        """补算计数事件必须携带原始 IMU 时刻，禁止用晚到的 BLE 发送时刻标点。"""

        # main_source 读取领域事件进入 BLE 队列及发布调用的唯一生产链。
        main_source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # ble_header 读取公开事件发布签名，确保调用者必须显式提供时刻。
        ble_header_path = (
            self.REPOSITORY_ROOT
            / "esp32/firmware/components/ble_service/include/ble_service_nimble.h"
        )
        # 读取 UTF-8 头文件文本。
        ble_header = ble_header_path.read_text(encoding="utf-8")
        # app_ble_output_t 必须保存事件原始单调毫秒，队列异步发送不能丢失该字段。
        self.assertIn("uint32_t event_monotonic_ms;", main_source)
        # 领域 MetricEvent 的 uint64 时刻必须显式缩窄为冻结协议的 uint32 毫秒。
        self.assertIn(
            "output.event_monotonic_ms = (uint32_t)metric_event->monotonic_ms;",
            main_source,
        )
        # 锁类补算可能同时产生多条历史事件；主任务必须遍历固定有效数量。
        self.assertIn(
            "for (uint8_t index = 0U; index < effects->replay_metric_event_count; ++index)",
            main_source,
        )
        # 每条补算事件必须进入同一单事件编码器，禁止只发送最终累计。
        self.assertIn(
            "&effects->replay_metric_events[index],\n"
            "                &effects->live_state",
            main_source,
        )
        # BLE 任务必须把队列内原始时刻交给事件发布 API。
        self.assertIn(
            "ble_service_nimble_publish_event(\n"
            "                    output.event_payload,\n"
            "                    output.event_length,\n"
            "                    output.event_monotonic_ms)",
            main_source,
        )
        # 公开接口的第三参数固定为 monotonic_ms，禁止回退内部发送时刻。
        self.assertIn("uint32_t monotonic_ms);", ble_header)

    def test_gap_reset_is_committed_and_replay_events_are_retained(self) -> None:
        """真实采样缺口必须提交重置；准备期补算事件不得只保留最终总数。"""

        # workout_source 读取缺口处理和准备缓存回放的生产实现。
        workout_source_path = (
            self.REPOSITORY_ROOT
            / "esp32/firmware/components/workout_engine/workout_engine.c"
        )
        # 读取 UTF-8 训练引擎源码。
        workout_source = workout_source_path.read_text(encoding="utf-8")
        # workout_header 读取固定回放 FIFO 容量和公开弹出接口。
        workout_header_path = (
            self.REPOSITORY_ROOT
            / "esp32/firmware/components/workout_engine/include/workout_engine.h"
        )
        # 读取 UTF-8 头文件。
        workout_header = workout_header_path.read_text(encoding="utf-8")
        # coordinator_source 读取补算事件吸收和 effect 交付链。
        coordinator_source_path = (
            self.REPOSITORY_ROOT
            / "esp32/firmware/components/device_coordinator/device_coordinator.c"
        )
        # 读取 UTF-8 协调器源码。
        coordinator_source = coordinator_source_path.read_text(encoding="utf-8")
        # 真实质量缺口必须由统一掩码识别并重置未完成周期，不能依赖已删除的专用开合跳链。
        self.assertIn("WORKOUT_QUALITY_TIMELINE_BREAK_MASK", workout_source)
        # 通用相位缺口同样必须按成功边界提交，不能永久回滚旧时间戳。
        self.assertIn(
            "本点未计数但重置必须提交；返回 OK 避免协调器事务回滚后永久卡在旧时间基准。",
            workout_source,
        )
        # 固定 FIFO 最多保存 160 点准备缓存能够形成的十二条事件，不使用动态内存。
        self.assertIn("#define WORKOUT_REPLAY_EVENT_QUEUE_CAPACITY (12U)", workout_header)
        # 训练引擎必须公开按序弹出完整 MetricEvent 的接口。
        self.assertIn("workout_engine_pop_replay_metric_event(", workout_header)
        # 协调器必须逐条吸收 event_seq 和会话统计，而不是只读取最终 repetitions。
        self.assertIn("device_drain_replay_metric_events(&candidate, effects)", coordinator_source)
        # 有回放事件时必须请求 BLE Event effect。
        self.assertIn("effects->flags |= DEVICE_EFFECT_BLE_EVENT;", coordinator_source)

    def test_control_and_data_gaps_isolate_incomplete_evidence(self) -> None:
        """开始/恢复隔离旧窗口，采样缺口切断未完成周期但保留准备证据。"""

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
        # 连续性边界掩码必须排除旧执行器污染，但覆盖六种真实数据连续性破坏。
        continuity_mask = source[
            source.index("const uint32_t continuity_break_mask =") :
        ]
        # 只截取掩码定义到第一个分号，避免后面的计数冻结掩码影响断言。
        continuity_mask = continuity_mask[: continuity_mask.index(";")]
        # 六种连续性破坏全部必须通知领域层执行状态相关的边界恢复。
        for quality_name in (
            "IMU_QUALITY_ACCEL_GAP",
            "IMU_QUALITY_GYRO_GAP",
            "IMU_QUALITY_OUT_OF_ORDER",
            "IMU_QUALITY_QUEUE_OVERFLOW",
            "IMU_QUALITY_DRIVER_DROP",
            "IMU_QUALITY_RESAMPLER_RESET",
        ):
            # 每个质量位必须出现在专用连续性边界掩码中。
            self.assertIn(quality_name, continuity_mask)
        # 旧执行器兼容位只冻结当前点，不能触发采样连续性恢复。
        self.assertNotIn(
            "IMU_QUALITY_LEGACY_ACTUATOR_CONTAMINATED",
            continuity_mask,
        )
        # 应用入口必须实际调用领域层边界 API，而不是只写注释或日志。
        self.assertGreaterEqual(
            source.count("workout_engine_reset_bout_evidence(&s_coordinator.workout);"),
            2,
        )
        # 读取领域层边界函数，核对 PREPARING 与 RUNNING 使用不同恢复策略。
        workout_source = self.WORKOUT_SOURCE_PATH.read_text(encoding="utf-8")
        # 只截取边界函数，避免后续函数中的同名 helper 造成假阳性。
        reset_function = workout_source[
            workout_source.index("void workout_engine_reset_bout_evidence(") :
            workout_source.index("workout_status_t workout_engine_push_inference(")
        ]
        # PREPARING 必须直接保留缓存样本和分类候选，防止起步动作被缺口抹掉。
        self.assertIn("engine->state == WORKOUT_STATE_PREPARING", reset_function)
        # RUNNING 的缺口只重建 25 点活动统计。
        self.assertIn("workout_reset_activity_window(engine);", reset_function)
        # RUNNING 同时清除跨缺口的未完成相位或步峰。
        self.assertIn("workout_reset_incomplete_counting(engine);", reset_function)
        # 该边界函数不得清空准备累计或已确认主动作。
        self.assertNotIn("workout_reset_bout(engine);", reset_function)

    def test_running_keeps_primary_counter_and_activity_gate_controls_counting(self) -> None:
        """运行期分类只写诊断；主动作固定，计数只由逐点活动门冻结或恢复。"""

        # main_source 读取双 M0 回调，质量告警窗口仍必须进入领域层保存诊断事实。
        main_source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # workout_source 读取 selected/inferred 双层状态和活动门生产逻辑。
        workout_source = self.WORKOUT_SOURCE_PATH.read_text(encoding="utf-8")
        # 推理回调只能按模型执行状态拒绝，禁止把质量位窗口提前丢弃。
        callback_start = main_source.index("static void app_pipeline_on_inference(")
        # 截取到下一函数，限定断言只检查推理回调。
        callback_body = main_source[
            callback_start : main_source.index(
                "static APP_STACK_BOUNDARY void app_process_qmi_frame(",
                callback_start,
            )
        ]
        # 质量位必须原样交给协调器，供诊断和准备态干净窗口门使用。
        self.assertIn(
            "(uint16_t)(result->quality_flags & UINT16_MAX)",
            callback_body,
        )
        # 禁止恢复旧的“质量位非零就提前 return”逻辑。
        self.assertNotIn("result->quality_flags != 0U", callback_body)
        # 先定位推理 API，避免命中动作段边界重置函数里的另一个 RUNNING 判断。
        inference_start = workout_source.index("workout_status_t workout_engine_push_inference(")
        # 单独截取推理 API 内的 RUNNING 分支，验证 Top-1 只能写诊断。
        running_start = workout_source.index(
            "if (engine->state == WORKOUT_STATE_RUNNING)",
            inference_start,
        )
        # 准备态质量门是 RUNNING 分支之后的首个边界。
        running_end = workout_source.index("if ((quality_flags &", running_start)
        # running_body 只包含运行期单窗分类逻辑。
        running_body = workout_source[running_start:running_end]
        # 每个运行窗口必须把真实 argmax 写入 inferred_action，不能强制复制 selected_action。
        self.assertIn("engine->inferred_action = window_class;", running_body)
        # RUNNING Top-1 不得开关计数门或清空半周期。
        self.assertNotIn("classification_consistent =", running_body)
        # RUNNING Top-1 不得调用相位重置。
        self.assertNotIn("workout_reset_incomplete_counting", running_body)
        # 活动门必须同时包含整体分数和至少五个逐点活动证据。
        self.assertIn("const bool motion_detected =", workout_source)
        # 完整静止窗必须有独立 rest_detected 分支。
        self.assertIn("const bool rest_detected =", workout_source)
        # 只有活动门变化时清空未完成周期，禁止跨休息拼接一次动作。
        self.assertIn("if (next_counting_enabled != engine->classification_consistent)", workout_source)
        # 主动作赋值只允许出现在首次准备态锁类 helper；运行分支不能出现第二个切类入口。
        self.assertEqual(workout_source.count("engine->selected_action = (uint8_t)action;"), 1)

    def test_training_classification_accepts_first_complete_window(self) -> None:
        """开始后首个 62 点完整窗口必须立即进入累计确认，禁止再叠加固定倒计时。"""

        # source 读取生产主入口，合同不能由桌面端延时或注释替代。
        source = self.MAIN_SOURCE_PATH.read_text(encoding="utf-8")
        # 旧 2000 ms 静默常量必须删除，62/25 秒窗口已经提供连续采样边界。
        self.assertNotIn("APP_CLASSIFICATION_ARM_DELAY_MS", source)
        # 状态门只判断是否处于 PREPARING/RUNNING，不读取会话已过毫秒数。
        self.assertIn("static bool app_training_accepts_inference(void)", source)
        # 截取推理回调本体，保证状态门仍早于发布和协调器提交。
        callback_start = source.index("static void app_pipeline_on_inference(")
        # 回调结束于下一个 QMI 帧函数。
        callback_end = source.index(
            "static APP_STACK_BOUNDARY void app_process_qmi_frame",
            callback_start,
        )
        # callback_body 只包含真实生产推理接线。
        callback_body = source[callback_start:callback_end]
        # 训练状态门必须早于上位机诊断发布。
        self.assertLess(
            callback_body.index("if (!app_training_accepts_inference())"),
            callback_body.index("app_queue_inference_diagnostic(result);"),
        )
        # 训练状态门也必须早于领域协调器累计锁类。
        self.assertLess(
            callback_body.index("if (!app_training_accepts_inference())"),
            callback_body.index("device_coordinator_push_inference("),
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


class WindowsBlePairingSourceContractTests(unittest.TestCase):
    """锁定 Windows 配对只能使用可配对的 Association Endpoint 设备对象。"""

    # 仓库根从 tools 目录向上一级解析，避免依赖调用测试时的工作目录。
    REPOSITORY_ROOT = Path(__file__).resolve().parent.parent
    # transport_source_path 指向唯一 WinRT 真蓝牙传输实现。
    TRANSPORT_SOURCE_PATH = REPOSITORY_ROOT / "pc/FitnessCoach.Bluetooth.Windows/WinRtBleTransport.cs"

    def test_windows_pairing_uses_association_endpoint_objects(self) -> None:
        """扫描、连接和忘记设备必须显式请求 Association Endpoint，禁止默认 DeviceInterface。"""

        # source 读取生产 WinRT 传输源码；桌面提示文字不能替代真实配对对象类型。
        source = self.TRANSPORT_SOURCE_PATH.read_text(encoding="utf-8")
        # 所有按 ID 重开设备的路径必须收敛到一个 helper，防止某条重连路径退回默认 DeviceInterface。
        self.assertIn("OpenAssociationEndpointAsync(", source)
        # 生产文件只允许 helper 内直接调用一次 CreateFromIdAsync，其余路径必须复用同一合同。
        self.assertEqual(source.count("DeviceInformation.CreateFromIdAsync("), 1)
        # helper 与缓存枚举都必须显式声明 AssociationEndpoint；无 kind 的 WinRT 重载默认是 DeviceInterface。
        self.assertGreaterEqual(source.count("DeviceInformationKind.AssociationEndpoint"), 2)
        # PairAsync 前必须验证 Windows 确认该对象可配对，避免继续返回含糊的 Failed。
        self.assertIn("selectedInformation.Pairing.CanPair", source)

    def test_display_only_watch_uses_custom_provide_pin_pairing(self) -> None:
        """Display Only 手表必须由 WinRT CustomPairing 提交固定六位联调码。"""

        # source 读取生产配对入口，禁止只在 README 中写 PIN 操作说明。
        source = self.TRANSPORT_SOURCE_PATH.read_text(encoding="utf-8")
        # 上位机联调码必须与固件唯一固定码一致，避免两端各自漂移。
        self.assertIn('private const string DebugPairingPasskey = "123456";', source)
        # 自定义配对只支持无需输入的确认和“设备显示、PC 提供 PIN”两种仪式。
        self.assertIn(
            "DevicePairingKinds.ConfirmOnly | DevicePairingKinds.ProvidePin",
            source,
        )
        # ProvidePin 事件必须把固定码提交给 Windows，不能等待不存在的默认系统 UI。
        self.assertIn("request.Accept(DebugPairingPasskey);", source)
        # ConfirmOnly 仍需显式接受，否则部分 Windows 蓝牙适配器会停在确认阶段。
        self.assertIn("request.Accept();", source)
        # 必须调用 Custom.PairAsync 并保持认证加密等级。
        self.assertIn("customPairing.PairAsync(", source)
        self.assertIn("DevicePairingProtectionLevel.EncryptionAndAuthentication", source)
        # 事件处理器必须在 finally 中解绑，避免多次快速连接累计回调。
        self.assertIn("customPairing.PairingRequested -= OnPairingRequested;", source)


# 直接执行本文件时运行全部合同测试，便于开发者单独复验。
if __name__ == "__main__":
    # verbosity=2 输出中文测试名称和失败位置。
    unittest.main(verbosity=2)
