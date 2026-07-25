/* 引入电源策略公开合同；实现不直接调用 ESP-IDF，便于纯 C 主机测试。 */
#include "power_manager.h"

/* 引入 NULL 定义完成参数检查。 */
#include <stddef.h>

/* 初始化生产空闲计时器；实现不创建任务，因此主机和 ESP32 共用同一门槛语义。 */
void power_idle_timer_init(power_idle_timer_t *timer, uint64_t now_ms)
{
    /* 空指针表示调用方没有可写状态，安全返回且不访问内存。 */
    if (timer == NULL) {
        /* 结束空参数调用。 */
        return;
    }
    /* 启动时刻同时作为首次用户活动基准，避免上电后立即熄屏。 */
    timer->last_activity_ms = now_ms;
    /* 新活动周期尚未发出熄屏事件。 */
    timer->screen_timeout_emitted = false;
    /* 新活动周期尚未发出长空闲事件。 */
    timer->long_idle_emitted = false;
}

/* 记录用户活动；触摸、按键和明确控制均调用本函数。 */
void power_idle_timer_note_activity(power_idle_timer_t *timer, uint64_t now_ms)
{
    /* 复用初始化语义，同时清除两个一次性发出标志。 */
    power_idle_timer_init(timer, now_ms);
}

/* 把单调时间门槛转换为一次性电源事件，不直接修改 power_manager 状态。 */
bool power_idle_timer_poll(
    power_idle_timer_t *timer,
    uint64_t now_ms,
    uint32_t screen_timeout_ms,
    uint32_t long_idle_ms,
    bool session_active,
    power_event_type_t *event_type)
{
    /* 空状态、空输出、零门槛或逆序门槛均无法形成可靠计时合同。 */
    if ((timer == NULL) || (event_type == NULL) ||
        (screen_timeout_ms == 0U) || (long_idle_ms <= screen_timeout_ms)) {
        /* 返回 false 表示没有有效事件。 */
        return false;
    }
    /* 单调时钟理论上只递增；若平台复位或测试注入倒退，则从当前值重新计时。 */
    if (now_ms < timer->last_activity_ms) {
        /* 用当前时刻建立新基准并清除已发出标志。 */
        power_idle_timer_note_activity(timer, now_ms);
        /* 本轮不立即发事件，避免倒退时误进低功耗。 */
        return false;
    }
    /* 计算当前活动周期已空闲毫秒；无符号减法在前述顺序检查后安全。 */
    const uint64_t idle_ms = now_ms - timer->last_activity_ms;
    /* 无活动会话且达到长空闲门槛时优先请求一次 Deep-sleep。 */
    if (!session_active && !timer->long_idle_emitted &&
        (idle_ms >= (uint64_t)long_idle_ms)) {
        /* 锁定长空闲事件，后续轮询不重复投递。 */
        timer->long_idle_emitted = true;
        /* 长空闲隐含屏幕早已到期，同时锁定熄屏标志。 */
        timer->screen_timeout_emitted = true;
        /* 输出电源状态机可直接消费的长空闲类型。 */
        *event_type = POWER_EVENT_LONG_IDLE;
        /* 返回 true 表示调用方应投递事件。 */
        return true;
    }
    /* 达到屏幕门槛且本周期尚未发出时，只产生一次熄屏事件。 */
    if (!timer->screen_timeout_emitted &&
        (idle_ms >= (uint64_t)screen_timeout_ms)) {
        /* 锁定熄屏事件，直到下一次用户活动才重新开放。 */
        timer->screen_timeout_emitted = true;
        /* 输出熄屏事件；训练态只关闭 AMOLED，仍保持 IMU 和推理。 */
        *event_type = POWER_EVENT_SCREEN_TIMEOUT;
        /* 返回 true 表示存在一个新事件。 */
        return true;
    }
    /* 尚未到门槛，或本周期相应事件已发出。 */
    return false;
}

/* 统一切换功耗状态并保存单调时刻。 */
static void power_transition(
    power_manager_t *manager,
    power_state_t next_state,
    uint32_t monotonic_ms)
{
    /* 写入目标功耗状态，硬件任务下一轮应用对应策略。 */
    manager->state = next_state;
    /* 保存状态进入时刻，用于诊断频繁唤醒或异常抖动。 */
    manager->state_entered_ms = monotonic_ms;
}

/* 初始化电源状态机；不访问 PMIC、BLE 或 GPIO。 */
void power_manager_init(
    power_manager_t *manager,
    uint32_t monotonic_ms,
    bool allow_imu_deep_wake)
{
    /* 空指针没有可写目标，直接返回。 */
    if (manager == NULL) {
        /* 调用方没有提供状态机对象，保持外部硬件状态不变。 */
        return;
    }
    /* 冷启动固定处于 BOOT，允许 240 MHz 完成自检。 */
    manager->state = POWER_STATE_BOOT;
    /* 默认熄屏恢复主页，避免异常事件恢复到未定义页面。 */
    manager->screen_off_resume_state = POWER_STATE_HOME;
    /* 冷启动没有训练会话。 */
    manager->session_active = false;
    /* 冷启动尚未建立 BLE 连接。 */
    manager->ble_connected = false;
    /* 冷启动尚未确认充电状态。 */
    manager->charging = false;
    /* 255 表示 PMIC 数据未知。 */
    manager->battery_percent = UINT8_MAX;
    /* 保存启动单调时刻。 */
    manager->state_entered_ms = monotonic_ms;
    /* 保存用户/Kconfig 的运动唤醒偏好；默认关闭防止包内误唤醒。 */
    manager->allow_imu_deep_wake = allow_imu_deep_wake;
}

/* 处理状态事件；安全关机优先于普通页面事件。 */
bool power_manager_dispatch(power_manager_t *manager, const power_event_t *event)
{
    /* 空上下文或空事件不能改变策略。 */
    if ((manager == NULL) || (event == NULL)) {
        /* 返回 false 表示没有状态或属性发生变化。 */
        return false;
    }
    /* 充电变化是正交属性，不强制切换训练/主页状态。 */
    if (event->type == POWER_EVENT_CHARGING_CHANGED) {
        /* 比较新旧充电位，供调用方决定是否刷新 UI。 */
        const bool changed = manager->charging != event->charging;
        /* 保存 AXP2101 上报的当前充电状态。 */
        manager->charging = event->charging;
        /* 只在充电位确实改变时报告状态更新。 */
        return changed;
    }
    /* 主动关机在任意状态进入 SAFE_SHUTDOWN。 */
    if (event->type == POWER_EVENT_SHUTDOWN_REQUESTED) {
        /* 已在安全关机流程中时保持幂等，不重复刷盘或断电。 */
        if (manager->state == POWER_STATE_SAFE_SHUTDOWN) {
            /* 返回 false 表示没有新状态转换。 */
            return false;
        }
        /* 进入安全关机，等待摘要持久化后才允许 PMIC 断电。 */
        power_transition(manager, POWER_STATE_SAFE_SHUTDOWN, event->monotonic_ms);
        /* 返回 true 通知硬件执行层重新应用策略。 */
        return true;
    }
    /* 临界低电量仅在未充电时关机；接入 VBUS 后保留当前状态。 */
    if (event->type == POWER_EVENT_CRITICAL_BATTERY) {
        /* 充电中不关机；已关机状态也不重复触发。 */
        if (manager->charging || (manager->state == POWER_STATE_SAFE_SHUTDOWN)) {
            /* 当前临界电量事件无需产生新策略。 */
            return false;
        }
        /* 电池供电且不高于 5% 时进入保存后关机流程。 */
        power_transition(manager, POWER_STATE_SAFE_SHUTDOWN, event->monotonic_ms);
        /* 返回 true 请求上层应用新的关机策略。 */
        return true;
    }
    /* 外部 VBUS 有效时拒绝长空闲 Deep-sleep；USB 调试、充电和 BLE 联调必须保持可达。 */
    if ((event->type == POWER_EVENT_LONG_IDLE) && manager->charging) {
        /* 保持当前主页或待机状态，拔掉外部供电后下一轮长空闲才允许深睡。 */
        return false;
    }
    /* BLE 属性更新后，熄屏待机状态需在连接/慢广播之间切换。 */
    if ((event->type == POWER_EVENT_BLE_CONNECTED) ||
        (event->type == POWER_EVENT_BLE_DISCONNECTED)) {
        /* 把连接/断开事件折叠为目标布尔值。 */
        const bool next_connected = event->type == POWER_EVENT_BLE_CONNECTED;
        /* 记录属性是否真的变化，非待机状态不需要强制转换。 */
        const bool connection_changed = manager->ble_connected != next_connected;
        /* 保存 BLE 当前连接事实。 */
        manager->ble_connected = next_connected;
        /* 熄屏待机时连接变化需要立即切换射频策略。 */
        if ((manager->state == POWER_STATE_CONNECTED_STANDBY) ||
            (manager->state == POWER_STATE_ADVERTISING_STANDBY)) {
            /* 已连接保持低功耗连接；已断开切到慢广播。 */
            power_transition(
                manager,
                next_connected ? POWER_STATE_CONNECTED_STANDBY : POWER_STATE_ADVERTISING_STANDBY,
                event->monotonic_ms);
            /* 待机策略已经发生变化。 */
            return true;
        }
        /* 非待机状态只报告 BLE 属性是否改变。 */
        return connection_changed;
    }
    /* 按当前状态限定合法转移。 */
    switch (manager->state) {
        case POWER_STATE_BOOT:
            /* 自检完成进入亮屏主页。 */
            if (event->type == POWER_EVENT_BOOT_COMPLETED) {
                /* 切换到主页对应的 35% 亮度和低功耗 IMU 策略。 */
                power_transition(manager, POWER_STATE_HOME, event->monotonic_ms);
                /* 返回 true 请求应用主页硬件策略。 */
                return true;
            }
            break;
        case POWER_STATE_HOME:
            /* 新会话只有在电量允许时才能进入 RUNNING。 */
            if ((event->type == POWER_EVENT_SESSION_STARTED) &&
                power_manager_can_start_session(manager)) {
                /* 标记会话活动，防止待机逻辑误关推理链。 */
                manager->session_active = true;
                /* 切换到训练亮屏策略。 */
                power_transition(manager, POWER_STATE_RUNNING, event->monotonic_ms);
                /* 返回 true 请求开启训练资源。 */
                return true;
            }
            /* 主页熄屏后按 BLE 状态进入连接待机或慢广播待机。 */
            if (event->type == POWER_EVENT_SCREEN_TIMEOUT) {
                /* 记录唤醒后应回到主页。 */
                manager->screen_off_resume_state = POWER_STATE_HOME;
                /* 按 BLE 连接事实选择低功耗连接或慢广播。 */
                power_transition(
                    manager,
                    manager->ble_connected ? POWER_STATE_CONNECTED_STANDBY : POWER_STATE_ADVERTISING_STANDBY,
                    event->monotonic_ms);
                /* 返回 true 请求关闭屏幕并应用待机策略。 */
                return true;
            }
            /* 长时间无会话进入 Deep-sleep；PWRON 始终可重新冷启动。 */
            if (event->type == POWER_EVENT_LONG_IDLE) {
                /* 进入 Deep-sleep，唤醒后重新执行 BOOT。 */
                power_transition(manager, POWER_STATE_DEEP_STANDBY, event->monotonic_ms);
                /* 返回 true 请求执行 Deep-sleep。 */
                return true;
            }
            break;
        case POWER_STATE_RUNNING:
            /* 训练暂停后 QMI 降到 WOM，屏幕仍短时点亮。 */
            if (event->type == POWER_EVENT_SESSION_PAUSED) {
                /* 暂停不结束会话，只降低屏幕和 IMU 功耗。 */
                power_transition(manager, POWER_STATE_PAUSED, event->monotonic_ms);
                /* 返回 true 请求应用暂停策略。 */
                return true;
            }
            /* 会话结束回主页并清除活动标记。 */
            if (event->type == POWER_EVENT_SESSION_STOPPED) {
                /* 清除会话活动标记。 */
                manager->session_active = false;
                /* 返回主页并恢复 WOM。 */
                power_transition(manager, POWER_STATE_HOME, event->monotonic_ms);
                /* 返回 true 请求应用主页策略。 */
                return true;
            }
            /* 训练熄屏仍保持 25 Hz IMU、推理和 BLE。 */
            if (event->type == POWER_EVENT_SCREEN_TIMEOUT) {
                /* 记录触摸唤醒后继续训练页。 */
                manager->screen_off_resume_state = POWER_STATE_RUNNING;
                /* 关闭屏幕但保持采样和推理。 */
                power_transition(manager, POWER_STATE_RUNNING_SCREEN_OFF, event->monotonic_ms);
                /* 返回 true 请求应用熄屏训练策略。 */
                return true;
            }
            break;
        case POWER_STATE_RUNNING_SCREEN_OFF:
            /* 用户触摸或按键只点亮界面，不重置训练状态。 */
            if (event->type == POWER_EVENT_USER_WAKE) {
                /* 恢复训练亮屏，不重置会话或识别累计。 */
                power_transition(manager, POWER_STATE_RUNNING, event->monotonic_ms);
                /* 返回 true 请求点亮屏幕。 */
                return true;
            }
            /* 熄屏期间仍可从 BLE/按键暂停。 */
            if (event->type == POWER_EVENT_SESSION_PAUSED) {
                /* 转到暂停态并保留会话。 */
                power_transition(manager, POWER_STATE_PAUSED, event->monotonic_ms);
                /* 返回 true 请求应用暂停策略。 */
                return true;
            }
            /* 熄屏期间停止会话后回主页。 */
            if (event->type == POWER_EVENT_SESSION_STOPPED) {
                /* 清除活动会话标记。 */
                manager->session_active = false;
                /* 返回主页。 */
                power_transition(manager, POWER_STATE_HOME, event->monotonic_ms);
                /* 返回 true 请求应用主页策略。 */
                return true;
            }
            break;
        case POWER_STATE_PAUSED:
            /* 继续训练恢复 RUNNING 和 25 Hz IMU。 */
            if (event->type == POWER_EVENT_SESSION_RESUMED) {
                /* 恢复训练亮屏和严格采样。 */
                power_transition(manager, POWER_STATE_RUNNING, event->monotonic_ms);
                /* 返回 true 请求应用训练策略。 */
                return true;
            }
            /* 暂停状态结束会话后回主页。 */
            if (event->type == POWER_EVENT_SESSION_STOPPED) {
                /* 清除会话活动标记。 */
                manager->session_active = false;
                /* 返回主页。 */
                power_transition(manager, POWER_STATE_HOME, event->monotonic_ms);
                /* 返回 true 请求应用主页策略。 */
                return true;
            }
            /* 暂停熄屏进入可维持 BLE 的自动 Light-sleep 待机。 */
            if (event->type == POWER_EVENT_SCREEN_TIMEOUT) {
                /* 记录唤醒后仍返回暂停页。 */
                manager->screen_off_resume_state = POWER_STATE_PAUSED;
                /* 依据 BLE 连接事实选择待机射频模式。 */
                power_transition(
                    manager,
                    manager->ble_connected ? POWER_STATE_CONNECTED_STANDBY : POWER_STATE_ADVERTISING_STANDBY,
                    event->monotonic_ms);
                /* 返回 true 请求应用待机策略。 */
                return true;
            }
            break;
        case POWER_STATE_CONNECTED_STANDBY:
        case POWER_STATE_ADVERTISING_STANDBY:
            /* 唤醒后恢复主页或暂停页；训练熄屏使用独立 RUNNING_SCREEN_OFF 状态。 */
            if (event->type == POWER_EVENT_USER_WAKE) {
                /* 返回熄屏前保存的主页或暂停状态。 */
                power_transition(manager, manager->screen_off_resume_state, event->monotonic_ms);
                /* 返回 true 请求点亮对应页面。 */
                return true;
            }
            /* 长时间无操作可断开 BLE 并进入 Deep-sleep。 */
            if (event->type == POWER_EVENT_LONG_IDLE) {
                /* Deep-sleep 前主动清除逻辑 BLE 连接事实。 */
                manager->ble_connected = false;
                /* 关闭 BLE 和显示并请求 Deep-sleep。 */
                power_transition(manager, POWER_STATE_DEEP_STANDBY, event->monotonic_ms);
                /* 返回 true 请求执行 Deep-sleep。 */
                return true;
            }
            break;
        case POWER_STATE_DEEP_STANDBY:
            /* Deep-sleep 后 CPU 不执行该函数；唤醒会重新走 BOOT。 */
            break;
        case POWER_STATE_SAFE_SHUTDOWN:
            /* 安全关机只等待存储刷盘与 PMIC 断电，不再接收普通事件。 */
            break;
        default:
            /* 未知状态不执行任何硬件策略变化。 */
            break;
    }
    /* 当前状态不接受该事件。 */
    return false;
}

/* 根据当前状态生成完整外设策略；先构造最低功耗默认值，再按状态开启资源。 */
power_policy_t power_manager_policy(const power_manager_t *manager)
{
    /* 默认策略全部关闭，适用于空指针、安全关机和未知状态。 */
    power_policy_t policy = {
        /* 默认最低频率 40 MHz，允许后台待机任务运行。 */
        .min_cpu_mhz = 40U,
        /* 默认最高频率保留 240 MHz，推理 PM 锁可按需使用。 */
        .max_cpu_mhz = 240U,
        /* 默认不启用自动 Light-sleep，只有无周期训练任务的待机态打开。 */
        .automatic_light_sleep = false,
        /* 默认允许动态调频。 */
        .dynamic_frequency_scaling = true,
        /* 默认关闭 AMOLED。 */
        .display_on = false,
        /* 显示关闭时亮度为 0%。 */
        .display_brightness_percent = 0U,
        /* 默认关闭 IMU。 */
        .imu_mode = POWER_IMU_OFF,
        /* 默认关闭 BLE。 */
        .ble_mode = POWER_BLE_OFF,
        /* 默认不扫描触摸。 */
        .touch_active = false,
        /* 默认不执行存储事务。 */
        .storage_active = false,
        /* v1 默认关闭扬声器。 */
        .speaker_active = false,
        /* v1 默认关闭双麦克风。 */
        .microphones_active = false,
        /* 默认不请求 Deep-sleep。 */
        .request_deep_sleep = false,
        /* 默认不请求 PMIC 关机。 */
        .request_pmic_shutdown = false,
        /* 默认关闭 IMU 深睡唤醒，防止包内运动误唤醒。 */
        .enable_imu_deep_wake = false,
        /* 默认关闭触摸 Light-sleep 唤醒。 */
        .enable_touch_light_wake = false,
        /* 默认关闭 RTC Light-sleep 唤醒。 */
        .enable_rtc_light_wake = false,
    };
    /* 空上下文返回安全关闭策略。 */
    if (manager == NULL) {
        /* 返回全关闭默认策略。 */
        return policy;
    }
    /* 按状态只打开必需资源。 */
    switch (manager->state) {
        case POWER_STATE_BOOT:
            /* 冷启动点亮屏幕并保持 CPU 最高频率完成快速自检。 */
            /* 自检阶段禁止降到 40 MHz，缩短开机动画等待。 */
            policy.min_cpu_mhz = 240U;
            /* 开启 AMOLED。 */
            policy.display_on = true;
            /* 使用首版均衡亮度 35%。 */
            policy.display_brightness_percent = 35U;
            /* 自检只需运动唤醒模式，不进入正式推理。 */
            policy.imu_mode = POWER_IMU_WAKE_ON_MOTION;
            /* 开启触摸供自检。 */
            policy.touch_active = true;
            /* BOOT 策略设置完成。 */
            break;
        case POWER_STATE_HOME:
            /* 主页使用 35% 亮度、低功耗 IMU 和 BLE 广播/连接。 */
            /* 点亮 AMOLED。 */
            policy.display_on = true;
            /* 使用 35% 主页亮度。 */
            policy.display_brightness_percent = 35U;
            /* QMI8658 使用运动唤醒模式。 */
            policy.imu_mode = POWER_IMU_WAKE_ON_MOTION;
            /* 已连接保持活动连接，否则使用快速广播便于 PC 发现。 */
            policy.ble_mode = manager->ble_connected ?
                POWER_BLE_CONNECTED_ACTIVE : POWER_BLE_FAST_ADVERTISING;
            /* 主页需要触摸按钮。 */
            policy.touch_active = true;
            /* HOME 策略设置完成。 */
            break;
        case POWER_STATE_RUNNING:
            /* 训练亮屏保持 25 Hz IMU；CPU 只在推理调用附近获取 240 MHz 锁。 */
            /* 点亮训练页面。 */
            policy.display_on = true;
            /* 使用 35% 训练亮度。 */
            policy.display_brightness_percent = 35U;
            /* 保持严格 25 Hz 重采样链。 */
            policy.imu_mode = POWER_IMU_ACTIVE_25HZ;
            /* 已连接实时上传；未连接快速广播供上位机接入。 */
            policy.ble_mode = manager->ble_connected ?
                POWER_BLE_CONNECTED_ACTIVE : POWER_BLE_FAST_ADVERTISING;
            /* 训练页保留暂停和停止触摸。 */
            policy.touch_active = true;
            /* RUNNING 策略设置完成。 */
            break;
        case POWER_STATE_RUNNING_SCREEN_OFF:
            /* 训练熄屏继续 25 Hz 和 BLE；周期任务存在，不请求自动 Light-sleep。 */
            /* 继续严格 25 Hz 采样和双 M0 推理。 */
            policy.imu_mode = POWER_IMU_ACTIVE_25HZ;
            /* 已连接继续上传；未连接降低为慢广播。 */
            policy.ble_mode = manager->ble_connected ?
                POWER_BLE_CONNECTED_ACTIVE : POWER_BLE_SLOW_ADVERTISING;
            /* 训练熄屏仍保留触摸控制器工作，使第一下触摸能投递 WAKE；仅 Deep-sleep 与安全关机才让 FT3168 进入硬件休眠。 */
            policy.touch_active = true;
            /* 允许触摸中断把屏幕唤醒到训练页。 */
            policy.enable_touch_light_wake = true;
            /* RUNNING_SCREEN_OFF 策略设置完成。 */
            break;
        case POWER_STATE_PAUSED:
            /* 暂停页亮度降到 15%，冻结算法并让 IMU 进入 WOM。 */
            /* 点亮暂停页面。 */
            policy.display_on = true;
            /* 暂停亮度降为 15%。 */
            policy.display_brightness_percent = 15U;
            /* IMU 降到运动唤醒模式。 */
            policy.imu_mode = POWER_IMU_WAKE_ON_MOTION;
            /* 保持连接或快速广播，允许 PC 继续/停止。 */
            policy.ble_mode = manager->ble_connected ?
                POWER_BLE_CONNECTED_ACTIVE : POWER_BLE_FAST_ADVERTISING;
            /* 暂停页保留恢复和停止触摸。 */
            policy.touch_active = true;
            /* PAUSED 策略设置完成。 */
            break;
        case POWER_STATE_CONNECTED_STANDBY:
            /* 保持 BLE 时使用 modem-sleep 和自动 Light-sleep，不能手动 Light-sleep。 */
            /* 允许 ESP-IDF 在空闲 tick 自动进入 Light-sleep。 */
            policy.automatic_light_sleep = true;
            /* IMU 保持低功耗运动唤醒。 */
            policy.imu_mode = POWER_IMU_WAKE_ON_MOTION;
            /* BLE 使用连接态 modem-sleep。 */
            policy.ble_mode = POWER_BLE_CONNECTED_MODEM_SLEEP;
            /* 保留 LVGL 触摸输入，使 GPIO 唤醒后的同一次触摸可以恢复页面。 */
            policy.touch_active = true;
            /* 允许触摸从 Light-sleep 唤醒。 */
            policy.enable_touch_light_wake = true;
            /* 允许 RTC 周期维护从 Light-sleep 唤醒。 */
            policy.enable_rtc_light_wake = true;
            /* CONNECTED_STANDBY 策略设置完成。 */
            break;
        case POWER_STATE_ADVERTISING_STANDBY:
            /* 未连接待机使用慢广播和自动 Light-sleep。 */
            /* 允许空闲 tick 自动 Light-sleep。 */
            policy.automatic_light_sleep = true;
            /* IMU 保持运动唤醒模式。 */
            policy.imu_mode = POWER_IMU_WAKE_ON_MOTION;
            /* 未连接只做低占空比慢广播。 */
            policy.ble_mode = POWER_BLE_SLOW_ADVERTISING;
            /* 保留 LVGL 触摸输入，使待机黑屏可由第一下触摸恢复。 */
            policy.touch_active = true;
            /* 允许触摸 Light-sleep 唤醒。 */
            policy.enable_touch_light_wake = true;
            /* 允许 RTC Light-sleep 唤醒。 */
            policy.enable_rtc_light_wake = true;
            /* ADVERTISING_STANDBY 策略设置完成。 */
            break;
        case POWER_STATE_DEEP_STANDBY:
            /* Deep-sleep 关闭 BLE、显示和音频；触摸 GPIO38/RTC GPIO39 不可深睡唤醒。 */
            /* 请求硬件执行层进入 Deep-sleep。 */
            policy.request_deep_sleep = true;
            /* 用户显式允许时保留 QMI WOM，否则彻底关闭 IMU。 */
            policy.imu_mode = manager->allow_imu_deep_wake ?
                POWER_IMU_WAKE_ON_MOTION : POWER_IMU_OFF;
            /* 把同一偏好传给唤醒源配置。 */
            policy.enable_imu_deep_wake = manager->allow_imu_deep_wake;
            /* DEEP_STANDBY 策略设置完成。 */
            break;
        case POWER_STATE_SAFE_SHUTDOWN:
            /* 安全关机等待摘要刷盘后请求 AXP2101 断电。 */
            /* 请求 PMIC 软关机；主入口仍需持久化授权。 */
            policy.request_pmic_shutdown = true;
            /* SAFE_SHUTDOWN 策略设置完成。 */
            break;
        default:
            /* 未知状态沿用全部关闭的安全策略。 */
            break;
    }
    /* v1 不使用双麦克风，任何状态都保持 ES7210/麦克风关闭。 */
    policy.microphones_active = false;
    /* TF 与扬声器只由短时事务显式临时开启，常驻策略保持关闭。 */
    policy.storage_active = false;
    policy.speaker_active = false;
    /* 返回完整策略副本，硬件执行层可以做差异应用。 */
    return policy;
}

/* 更新电量并给出级别；策略只使用 AXP 百分比，电压阈值留待电芯实测。 */
power_battery_level_t power_manager_update_battery(
    power_manager_t *manager,
    uint8_t percent,
    bool charging)
{
    /* 空上下文或非法百分比无法更新，按最保守的 CRITICAL 返回。 */
    if ((manager == NULL) || (percent > 100U)) {
        /* 返回临界级别，防止无效 SOC 被当成正常电量。 */
        return POWER_BATTERY_CRITICAL;
    }
    /* 保存 PMIC 百分比供主页和会话启动门控使用。 */
    manager->battery_percent = percent;
    /* 保存充电状态；充电时 can_start_session 可放行低电量。 */
    manager->charging = charging;
    /* 5% 及以下为临界，未充电时上层应保存会话并发送 CRITICAL_BATTERY。 */
    if (percent <= POWER_BATTERY_CRITICAL_PERCENT) {
        /* 返回临界电量级别。 */
        return POWER_BATTERY_CRITICAL;
    }
    /* 8% 及以下禁止启动新会话，允许保存或结束当前会话。 */
    if (percent <= POWER_BATTERY_BLOCK_START_PERCENT) {
        /* 返回禁止新会话级别。 */
        return POWER_BATTERY_BLOCK_START;
    }
    /* 15% 及以下显示低电量横幅和每会话一次的双振提示。 */
    if (percent <= POWER_BATTERY_WARNING_PERCENT) {
        /* 返回低电提示级别。 */
        return POWER_BATTERY_WARNING;
    }
    /* 高于 15% 进入正常电量区间。 */
    return POWER_BATTERY_NORMAL;
}

/* 判断是否允许新会话；未知电量先禁止，避免 PMIC 故障时耗尽电池。 */
bool power_manager_can_start_session(const power_manager_t *manager)
{
    /* 空上下文或未知百分比均不能安全启动。 */
    if ((manager == NULL) || (manager->battery_percent == UINT8_MAX)) {
        /* 返回 false，防止未知电池状态启动高功耗训练。 */
        return false;
    }
    /* 接入充电电源时允许启动，即使 SOC 不高于 8%。 */
    if (manager->charging) {
        /* 外部电源供电时允许开始训练。 */
        return true;
    }
    /* 电池供电时必须严格高于阻止启动阈值。 */
    return manager->battery_percent > POWER_BATTERY_BLOCK_START_PERCENT;
}

/* 计算指定续航目标允许的最大平均电流：I=0.8*C/T。 */
float power_budget_max_average_current_ma(float target_hours)
{
    /* 非正目标没有物理意义，返回零防止除零。 */
    if (target_hours <= 0.0F) {
        /* 返回 0 mA 表示输入无效。 */
        return 0.0F;
    }
    /* 400 mAh 电池按 80% 可用容量除以目标小时数，结果单位为 mA。 */
    return (POWER_BATTERY_CAPACITY_MAH * POWER_USABLE_CAPACITY_RATIO) / target_hours;
}

/* 根据实测平均电流估算续航：T=0.8*C/I。 */
float power_budget_estimated_runtime_hours(float measured_average_current_ma)
{
    /* 非正电流无法形成有限续航估算，返回零作为无效值。 */
    if (measured_average_current_ma <= 0.0F) {
        /* 返回 0 小时表示测量值无效。 */
        return 0.0F;
    }
    /* 使用同一 80% 可用容量口径，便于实测结果与预算直接比较。 */
    return (POWER_BATTERY_CAPACITY_MAH * POWER_USABLE_CAPACITY_RATIO) /
           measured_average_current_ma;
}

/* 返回电源状态名称；字符串位于只读静态区。 */
const char *power_state_name(power_state_t state)
{
    /* 使用 switch 显式覆盖所有状态。 */
    switch (state) {
        /* 返回冷启动状态名。 */
        case POWER_STATE_BOOT: return "BOOT";
        /* 返回主页状态名。 */
        case POWER_STATE_HOME: return "HOME";
        /* 返回训练亮屏状态名。 */
        case POWER_STATE_RUNNING: return "RUNNING";
        /* 返回训练熄屏状态名。 */
        case POWER_STATE_RUNNING_SCREEN_OFF: return "RUNNING_SCREEN_OFF";
        /* 返回暂停状态名。 */
        case POWER_STATE_PAUSED: return "PAUSED";
        /* 返回 BLE 已连接待机状态名。 */
        case POWER_STATE_CONNECTED_STANDBY: return "CONNECTED_STANDBY";
        /* 返回 BLE 慢广播待机状态名。 */
        case POWER_STATE_ADVERTISING_STANDBY: return "ADVERTISING_STANDBY";
        /* 返回 Deep-sleep 状态名。 */
        case POWER_STATE_DEEP_STANDBY: return "DEEP_STANDBY";
        /* 返回保存后 PMIC 关机状态名。 */
        case POWER_STATE_SAFE_SHUTDOWN: return "SAFE_SHUTDOWN";
        /* 未知枚举返回稳定诊断名。 */
        default: return "UNKNOWN";
    }
}
