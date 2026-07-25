#ifndef IMU_HANDHELD_UI_APP_PAIRING_H
#define IMU_HANDHELD_UI_APP_PAIRING_H

/* 引入 UI 事件和上下文；邮箱消费者把快照转换成纯状态机事件。 */
#include "ui_state_machine.h"

/* 引入布尔值、定长整数和 C11 原子类型，提供跨 FreeRTOS 任务的数据竞争保护。 */
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 描述原子邮箱操作结果；发布函数不等待锁，适合 NimBLE 主机回调。 */
typedef enum ui_app_pairing_result {
    /* 事件已完整写入原子邮箱。 */
    UI_APP_PAIRING_OK = 0,
    /* 邮箱指针、输出事件或清除原因无效。 */
    UI_APP_PAIRING_ERR_ARGUMENT = -1,
    /* passkey 超过 BLE 六位上限 999999。 */
    UI_APP_PAIRING_ERR_RANGE = -2,
    /* 另一发布者正在写；调用方应记录并在应用任务补发清除事件。 */
    UI_APP_PAIRING_BUSY = -3
} ui_app_pairing_result_t;

/*
 * 保存 NimBLE 任务到应用任务的最新配对事件。
 * sequence 使用偶数表示稳定快照、奇数表示写入中；consumed_sequence 仅由单一应用任务推进。
 * 所有字段均为 32 位原子量，ESP32-S3 无需动态内存；结构不保存任何指针。
 */
typedef struct ui_app_pairing_mailbox {
    /* sequence 每次成功发布增加 2；低位为写锁，release/acquire 保证其余字段可见。 */
    atomic_uint_least32_t sequence;
    /* consumed_sequence 保存应用任务最近成功取得的稳定序列。 */
    atomic_uint_least32_t consumed_sequence;
    /* event_type 保存 UI_EVENT_PAIRING_CODE_SHOWN 或 UI_EVENT_PAIRING_CODE_CLEARED。 */
    atomic_uint_least32_t event_type;
    /* passkey 保存 0～999999；清除事件固定写 0，防止旧码残留。 */
    atomic_uint_least32_t passkey;
    /* monotonic_ms 保存事件源捕获的 uint32 单调毫秒低位。 */
    atomic_uint_least32_t monotonic_ms;
    /* clear_reason 保存 ui_pairing_clear_reason_t；显示事件固定写 NONE。 */
    atomic_uint_least32_t clear_reason;
} ui_app_pairing_mailbox_t;

/* 初始化静态邮箱；调用时不得有并发生产者或消费者。 */
void ui_app_pairing_mailbox_init(ui_app_pairing_mailbox_t *mailbox);
/* NimBLE passkey 回调发布六位码；函数无阻塞、无动态内存、时间复杂度 O(1)。 */
ui_app_pairing_result_t ui_app_pairing_publish_code(
    ui_app_pairing_mailbox_t *mailbox,
    uint32_t passkey,
    uint32_t monotonic_ms);
/* NimBLE 成功、失败、断线、停止或忘记电脑路径发布清除原因。 */
ui_app_pairing_result_t ui_app_pairing_publish_clear(
    ui_app_pairing_mailbox_t *mailbox,
    ui_pairing_clear_reason_t reason,
    uint32_t monotonic_ms);
/* 单一应用任务尝试取得最新稳定事件；无新事件或写入中返回 false。 */
bool ui_app_pairing_try_take(ui_app_pairing_mailbox_t *mailbox, ui_event_t *event);
/* 根据 UI 上下文和当前单调时刻生成超时清除事件；未到期返回 false。 */
bool ui_app_pairing_build_timeout_event(
    const ui_context_t *context,
    uint32_t monotonic_ms,
    ui_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
