/* 引入配对码原子邮箱合同。 */
#include "ui_app_pairing.h"

/* 引入 memset，消费事件前清除未使用负载。 */
#include <string.h>

/* 发布前尝试把偶数稳定序列切成奇数写入序列；失败时不得自旋阻塞 NimBLE。 */
static bool ui_app_pairing_begin_write(
    ui_app_pairing_mailbox_t *mailbox,
    uint_least32_t *stable_sequence)
{
    /* 两个输出对象都必须有效。 */
    if ((mailbox == NULL) || (stable_sequence == NULL)) {
        /* 参数错误不能取得写权限。 */
        return false;
    }
    /* acquire 读取上一稳定发布及可能正在写入的低位状态。 */
    uint_least32_t expected = atomic_load_explicit(&mailbox->sequence, memory_order_acquire);
    /* 奇数表示另一生产者正在更新多字段快照；立即返回保持回调无阻塞。 */
    if ((expected & (uint_least32_t)UINT32_C(1)) != (uint_least32_t)UINT32_C(0)) {
        /* 调用方收到 BUSY 后记录诊断。 */
        return false;
    }
    /* 使用强 CAS 只尝试一次；竞争失败交给调用方处理，不在主机任务自旋。 */
    if (!atomic_compare_exchange_strong_explicit(
            &mailbox->sequence,
            &expected,
            expected + (uint_least32_t)UINT32_C(1),
            memory_order_acq_rel,
            memory_order_acquire)) {
        /* 未取得唯一写权限。 */
        return false;
    }
    /* 保存发布前偶数序列，提交时增加 2 并恢复偶数。 */
    *stable_sequence = expected;
    /* 返回已取得写权限。 */
    return true;
}

/* 写入全部字段后以 release 提交偶数序列，使消费者看到一致快照。 */
static void ui_app_pairing_finish_write(
    ui_app_pairing_mailbox_t *mailbox,
    uint_least32_t stable_sequence)
{
    /* mailbox 由 begin_write 验证，提交序列增加 2；uint32 回绕仍保持偶数。 */
    atomic_store_explicit(
        &mailbox->sequence,
        stable_sequence + (uint_least32_t)UINT32_C(2),
        memory_order_release);
}

/* 初始化邮箱。 */
void ui_app_pairing_mailbox_init(ui_app_pairing_mailbox_t *mailbox)
{
    /* 空指针没有可初始化对象。 */
    if (mailbox == NULL) {
        /* 安全返回。 */
        return;
    }
    /* 初始稳定序列为 0。 */
    atomic_init(&mailbox->sequence, (uint_least32_t)UINT32_C(0));
    /* 消费序列同为 0，表示暂无新事件。 */
    atomic_init(&mailbox->consumed_sequence, (uint_least32_t)UINT32_C(0));
    /* 初始事件类型为 0；只有新序列才会读取。 */
    atomic_init(&mailbox->event_type, (uint_least32_t)UINT32_C(0));
    /* 初始不保存配对码。 */
    atomic_init(&mailbox->passkey, (uint_least32_t)UINT32_C(0));
    /* 初始事件时刻为 0。 */
    atomic_init(&mailbox->monotonic_ms, (uint_least32_t)UINT32_C(0));
    /* 初始没有清除原因。 */
    atomic_init(&mailbox->clear_reason, (uint_least32_t)UI_PAIRING_CLEAR_NONE);
}

/* 发布六位配对码。 */
ui_app_pairing_result_t ui_app_pairing_publish_code(
    ui_app_pairing_mailbox_t *mailbox,
    uint32_t passkey,
    uint32_t monotonic_ms)
{
    /* 邮箱为空时拒绝。 */
    if (mailbox == NULL) {
        /* 返回参数错误。 */
        return UI_APP_PAIRING_ERR_ARGUMENT;
    }
    /* BLE passkey 必须位于 000000～999999。 */
    if (passkey > UI_PAIRING_PASSKEY_MAX) {
        /* 七位值无法安全显示为六位码。 */
        return UI_APP_PAIRING_ERR_RANGE;
    }
    /* 保存写入前稳定序列。 */
    uint_least32_t stable_sequence = (uint_least32_t)UINT32_C(0);
    /* 单次 CAS 失败时立即返回，不阻塞 NimBLE 主机。 */
    if (!ui_app_pairing_begin_write(mailbox, &stable_sequence)) {
        /* 报告邮箱忙。 */
        return UI_APP_PAIRING_BUSY;
    }
    /* 写入显示事件类型；relaxed 由最终 sequence release 统一发布。 */
    atomic_store_explicit(
        &mailbox->event_type,
        (uint_least32_t)UI_EVENT_PAIRING_CODE_SHOWN,
        memory_order_relaxed);
    /* 写入合法六位数值。 */
    atomic_store_explicit(&mailbox->passkey, (uint_least32_t)passkey, memory_order_relaxed);
    /* 写入单调毫秒。 */
    atomic_store_explicit(
        &mailbox->monotonic_ms,
        (uint_least32_t)monotonic_ms,
        memory_order_relaxed);
    /* 显示事件没有清除原因。 */
    atomic_store_explicit(
        &mailbox->clear_reason,
        (uint_least32_t)UI_PAIRING_CLEAR_NONE,
        memory_order_relaxed);
    /* 提交完整快照。 */
    ui_app_pairing_finish_write(mailbox, stable_sequence);
    /* 返回成功。 */
    return UI_APP_PAIRING_OK;
}

/* 发布配对码清除。 */
ui_app_pairing_result_t ui_app_pairing_publish_clear(
    ui_app_pairing_mailbox_t *mailbox,
    ui_pairing_clear_reason_t reason,
    uint32_t monotonic_ms)
{
    /* 邮箱不能为空，原因必须是成功到服务停止之间的公开值。 */
    if ((mailbox == NULL) ||
        (reason <= UI_PAIRING_CLEAR_NONE) ||
        (reason >= UI_PAIRING_CLEAR_REASON_COUNT)) {
        /* 返回参数错误。 */
        return UI_APP_PAIRING_ERR_ARGUMENT;
    }
    /* 保存写入前偶数序列。 */
    uint_least32_t stable_sequence = (uint_least32_t)UINT32_C(0);
    /* 不等待并发发布者。 */
    if (!ui_app_pairing_begin_write(mailbox, &stable_sequence)) {
        /* 报告邮箱忙。 */
        return UI_APP_PAIRING_BUSY;
    }
    /* 写入清除事件类型。 */
    atomic_store_explicit(
        &mailbox->event_type,
        (uint_least32_t)UI_EVENT_PAIRING_CODE_CLEARED,
        memory_order_relaxed);
    /* 清除事件固定覆盖旧 passkey，邮箱本身也不保留敏感码。 */
    atomic_store_explicit(
        &mailbox->passkey,
        (uint_least32_t)UINT32_C(0),
        memory_order_relaxed);
    /* 保存清除时刻。 */
    atomic_store_explicit(
        &mailbox->monotonic_ms,
        (uint_least32_t)monotonic_ms,
        memory_order_relaxed);
    /* 保存稳定清除原因。 */
    atomic_store_explicit(
        &mailbox->clear_reason,
        (uint_least32_t)reason,
        memory_order_relaxed);
    /* 发布完整清除快照。 */
    ui_app_pairing_finish_write(mailbox, stable_sequence);
    /* 返回成功。 */
    return UI_APP_PAIRING_OK;
}

/* 尝试取得最新事件。 */
bool ui_app_pairing_try_take(ui_app_pairing_mailbox_t *mailbox, ui_event_t *event)
{
    /* 邮箱和输出均不能为空。 */
    if ((mailbox == NULL) || (event == NULL)) {
        /* 无法读取。 */
        return false;
    }
    /* 最多尝试三次，覆盖消费者读取期间一次新发布；禁止无限循环。 */
    for (uint8_t attempt = UINT8_C(0); attempt < UINT8_C(3); ++attempt) {
        /* acquire 读取发布序列。 */
        const uint_least32_t begin_sequence = atomic_load_explicit(
            &mailbox->sequence,
            memory_order_acquire);
        /* 奇数表示正在写入，当前轮跳过。 */
        if ((begin_sequence & (uint_least32_t)UINT32_C(1)) !=
            (uint_least32_t)UINT32_C(0)) {
            /* 继续下一次有界尝试。 */
            continue;
        }
        /* 已消费同一序列时没有新事件。 */
        if (atomic_load_explicit(&mailbox->consumed_sequence, memory_order_acquire) ==
            begin_sequence) {
            /* 返回无新事件。 */
            return false;
        }
        /* 在 begin/end 序列之间读取多字段快照。 */
        const uint_least32_t event_type = atomic_load_explicit(
            &mailbox->event_type,
            memory_order_relaxed);
        /* 读取 passkey；清除事件固定为 0。 */
        const uint_least32_t passkey = atomic_load_explicit(
            &mailbox->passkey,
            memory_order_relaxed);
        /* 读取事件时刻。 */
        const uint_least32_t monotonic_ms = atomic_load_explicit(
            &mailbox->monotonic_ms,
            memory_order_relaxed);
        /* 读取清除原因。 */
        const uint_least32_t clear_reason = atomic_load_explicit(
            &mailbox->clear_reason,
            memory_order_relaxed);
        /* 再次 acquire；序列相同且为偶数才是完整一致快照。 */
        const uint_least32_t end_sequence = atomic_load_explicit(
            &mailbox->sequence,
            memory_order_acquire);
        /* 发布在读取期间变化时重试。 */
        if ((begin_sequence != end_sequence) ||
            ((end_sequence & (uint_least32_t)UINT32_C(1)) !=
             (uint_least32_t)UINT32_C(0))) {
            /* 继续有界重试。 */
            continue;
        }
        /* 只允许两个稳定事件类型。 */
        if ((event_type != (uint_least32_t)UI_EVENT_PAIRING_CODE_SHOWN) &&
            (event_type != (uint_least32_t)UI_EVENT_PAIRING_CODE_CLEARED)) {
            /* 损坏快照不标记已消费，方便诊断后复位邮箱。 */
            return false;
        }
        /* 清零 UI 事件未使用负载。 */
        (void)memset(event, 0, sizeof(*event));
        /* 写入稳定事件类型。 */
        event->type = (ui_event_type_t)event_type;
        /* 写入事件捕获时刻。 */
        event->monotonic_ms = (uint32_t)monotonic_ms;
        /* 写入显示码；清除事件保持 0。 */
        event->pairing_code = (uint32_t)passkey;
        /* 写入清除原因；显示事件为 NONE。 */
        event->pairing_clear_reason = (ui_pairing_clear_reason_t)clear_reason;
        /* release 标记当前稳定序列已由应用任务消费。 */
        atomic_store_explicit(
            &mailbox->consumed_sequence,
            end_sequence,
            memory_order_release);
        /* 返回成功取得事件。 */
        return true;
    }
    /* 写入持续变化或繁忙时本轮没有事件。 */
    return false;
}

/* 构造超时清除事件。 */
bool ui_app_pairing_build_timeout_event(
    const ui_context_t *context,
    uint32_t monotonic_ms,
    ui_event_t *event)
{
    /* 上下文和输出必须有效。 */
    if ((context == NULL) || (event == NULL)) {
        /* 无法生成事件。 */
        return false;
    }
    /* 未显示配对码时不生成清除。 */
    if (!context->view.pairing_active) {
        /* 返回未到期。 */
        return false;
    }
    /* 有符号差支持 uint32 单次回绕；60 秒远小于 2^31 毫秒。 */
    if ((int32_t)(monotonic_ms - context->view.pairing_expires_ms) < 0) {
        /* 当前仍在有效期内。 */
        return false;
    }
    /* 清零全部无关负载。 */
    (void)memset(event, 0, sizeof(*event));
    /* 设置统一清除事件。 */
    event->type = UI_EVENT_PAIRING_CODE_CLEARED;
    /* 保存真正检测到超时的单调时刻。 */
    event->monotonic_ms = monotonic_ms;
    /* 保存超时原因供日志。 */
    event->pairing_clear_reason = UI_PAIRING_CLEAR_TIMEOUT;
    /* 返回已生成事件。 */
    return true;
}
