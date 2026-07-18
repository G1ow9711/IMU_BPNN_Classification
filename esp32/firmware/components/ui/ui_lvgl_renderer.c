/* 引入渲染器公开合同。 */
#include "ui_lvgl_renderer.h"
/* 引入项目自带 Noto Sans SC 中文子集，保证设备界面汉字不会显示为方框。 */
#include "ui_fonts.h"

/* 引入 LVGL 9 页面、样式、事件和动画 API。 */
#include "lvgl.h"

/* 引入动态内存和清零函数；页面只在启动时分配一次。 */
#include <stdlib.h>
#include <string.h>

/* 固定加锁超时为 250 ms，超过后放弃本帧避免阻塞算法任务。 */
#define UI_LVGL_LOCK_TIMEOUT_MS (250U)
/* 固定 AMOLED 黑色背景，减少全屏发光功耗和烧屏风险。 */
#define UI_COLOR_BACKGROUND_HEX (0x05070AU)
/* 固定卡片深灰色。 */
#define UI_COLOR_CARD_HEX (0x10151CU)
/* 固定主强调青绿色。 */
#define UI_COLOR_ACCENT_HEX (0x32E6A1U)
/* 固定次强调蓝色。 */
#define UI_COLOR_SECONDARY_HEX (0x45A3FFU)
/* 固定普通文本浅灰色。 */
#define UI_COLOR_TEXT_HEX (0xF2F5F7U)
/* 固定弱化文本灰色。 */
#define UI_COLOR_MUTED_HEX (0x8C98A4U)
/* SH8601 逻辑横向宽度固定 410 像素，用于计算圆角屏幕安全内容宽度。 */
#define UI_SCREEN_WIDTH_PX (410)
/* 左右各留 32 像素，避免标题首字和右侧状态进入玻璃圆角不可视区。 */
#define UI_SAFE_HORIZONTAL_PX (32)
/* 顶部留 32 像素，使标题基线离开左上和右上圆角切线。 */
#define UI_SAFE_TOP_PX (32)
/* 底部保留 20 像素；底部三按钮在真板照片中未发生裁切。 */
#define UI_SAFE_BOTTOM_PX (20)
/* 安全文本宽度等于 410 减去左右各 32 像素，即 346 像素。 */
#define UI_SAFE_CONTENT_WIDTH_PX (UI_SCREEN_WIDTH_PX - (2 * UI_SAFE_HORIZONTAL_PX))
/* 设置页每个按钮使用 104 像素，三按钮与间距可完整放入 346 像素安全宽度。 */
#define UI_SETTINGS_BUTTON_WIDTH_PX (104)
/* HOME 固定布局的标题纵坐标为 32 像素，与圆角安全区顶边一致。 */
#define UI_HOME_TITLE_Y_PX (32)
/* HOME 固定布局的状态行纵坐标为 78 像素，位于标题行高之后。 */
#define UI_HOME_STATUS_Y_PX (78)
/* HOME 固定布局的主文案纵坐标为 128 像素，预留状态行和视觉间距。 */
#define UI_HOME_PRIMARY_Y_PX (128)
/* HOME 固定布局的次文案纵坐标为 270 像素，保持主信息与设备状态分区。 */
#define UI_HOME_SECONDARY_Y_PX (270)
/* HOME 固定布局的页脚纵坐标为 378 像素，位于按钮行上方。 */
#define UI_HOME_FOOTER_Y_PX (378)
/* HOME 固定布局的按钮行纵坐标为 414 像素，底部仍保留 18 像素圆角安全余量。 */
#define UI_HOME_BUTTON_ROW_Y_PX (414)
/* 16 像素中文字体用于状态栏、页脚和按钮，字形由项目生成脚本固定。 */
#define UI_FONT_CHINESE_16 (&ui_font_noto_sans_sc_16)
/* 20 像素中文字体用于标题和次级指标，禁止改回缺少汉字的 Montserrat。 */
#define UI_FONT_CHINESE_20 (&ui_font_noto_sans_sc_20)
/* 28 像素中文字体用于主动作名和倒计时，保持 410×502 屏幕可读性。 */
#define UI_FONT_CHINESE_28 (&ui_font_noto_sans_sc_28)
/* 36 像素中文字体用于主动作名、倒计时和关键总结数值，满足现场远距离查看。 */
#define UI_FONT_CHINESE_36 (&ui_font_noto_sans_sc_36)

/* 保存单页 LVGL 对象；全部对象由 root 递归拥有。 */
typedef struct {
    /* 保存页面根 screen。 */
    lv_obj_t *root;
    /* 保存标题标签。 */
    lv_obj_t *title_label;
    /* 保存电池/BLE 状态标签。 */
    lv_obj_t *status_label;
    /* 保存动作名、倒计时或总结主值。 */
    lv_obj_t *primary_label;
    /* 保存次数、卡路里和时间。 */
    lv_obj_t *secondary_label;
    /* 保存底部提示。 */
    lv_obj_t *footer_label;
    /* 保存按钮行容器。 */
    lv_obj_t *button_row;
/* 保存五个固定按钮对象；设置页使用三加二双行，其它页面隐藏未用槽位。 */
lv_obj_t *buttons[UI_PRESENTER_MAX_BUTTONS];
/* 保存与五个按钮一一对应的中文文字对象。 */
lv_obj_t *button_labels[UI_PRESENTER_MAX_BUTTONS];
} ui_lvgl_page_t;

/* 保存按钮 user_data；生命周期覆盖全部页面。 */
typedef struct {
    /* 保存所属渲染器，用于调用上层命令回调。 */
    ui_lvgl_renderer_t *renderer;
    /* 保存当前按钮命令；每次 render 可更新。 */
    ui_command_t command;
} ui_lvgl_button_binding_t;

/* 保存全部页面和动画差分状态。 */
typedef struct {
    /* 按 ui_state_t 索引保存真实 LVGL 页面。 */
    ui_lvgl_page_t pages[UI_STATE_COUNT];
    /* 按页面和按钮槽位保存稳定事件绑定。 */
    ui_lvgl_button_binding_t bindings[UI_STATE_COUNT][UI_PRESENTER_MAX_BUTTONS];
    /* 保存当前屏幕，UI_STATE_COUNT 表示尚未加载。 */
    ui_state_t active_state;
    /* 保存上一训练计数，变化时播放轻量反馈动画。 */
    uint32_t previous_count;
    /* 保存上一动作索引，变化时播放淡入动画。 */
    uint8_t previous_action_id;
} ui_lvgl_implementation_t;

/* 动画回调：把 0~255 数值写入文字不透明度。 */
static void ui_lvgl_set_text_opacity(void *object, int32_t value)
{
    /* 动画 var 固定指向 lv_obj_t。 */
    lv_obj_t *label = (lv_obj_t *)object;
    /* 把插值值限制在 LVGL opacity 类型范围。 */
    lv_obj_set_style_text_opa(label, (lv_opa_t)value, LV_PART_MAIN);
}

/* 启动一次文字淡入；仅在状态、动作或计数变化时调用。 */
static void ui_lvgl_start_fade(lv_obj_t *label, uint32_t duration_ms)
{
    /* 空标签不能创建动画。 */
    if (label == NULL) {
        return;
    }
    /* 删除同一对象旧的不透明度动画，避免快速动作切换堆积动画。 */
    lv_anim_delete(label, ui_lvgl_set_text_opacity);
    /* 创建栈上动画描述，lv_anim_start 会复制内部状态。 */
    lv_anim_t animation;
    /* 初始化默认动画参数。 */
    lv_anim_init(&animation);
    /* 设置目标标签。 */
    lv_anim_set_var(&animation, label);
    /* 设置执行回调。 */
    lv_anim_set_exec_cb(&animation, ui_lvgl_set_text_opacity);
    /* 从低不透明度淡入到完全可见。 */
    lv_anim_set_values(&animation, (int32_t)LV_OPA_20, (int32_t)LV_OPA_COVER);
    /* 设置动画毫秒时长。 */
    lv_anim_set_duration(&animation, duration_ms);
    /* 使用 ease-out，减少长时间高频刷新。 */
    lv_anim_set_path_cb(&animation, lv_anim_path_ease_out);
    /* 启动动画。 */
    (void)lv_anim_start(&animation);
}

/* LVGL 点击回调；只把命令投递给上层。 */
static void ui_lvgl_button_event(lv_event_t *event)
{
    /* 仅处理 CLICKED，过滤按下和释放等重复事件。 */
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    /* user_data 指向初始化时创建的稳定 binding。 */
    ui_lvgl_button_binding_t *binding =
        (ui_lvgl_button_binding_t *)lv_event_get_user_data(event);
    /* 指针、命令和回调都必须有效。 */
    if ((binding == NULL) || (binding->renderer == NULL) ||
        (binding->command == UI_COMMAND_NONE) ||
        (binding->renderer->command_callback == NULL)) {
        return;
    }
    /* 上层回调只应写 FreeRTOS 队列，不能阻塞 LVGL。 */
    binding->renderer->command_callback(
        binding->renderer->command_context,
        binding->command);
}

/* 配置 screen 的 AMOLED 友好背景和纵向布局。 */
static void ui_lvgl_style_screen(lv_obj_t *screen)
{
    /* 接近纯黑背景降低 AMOLED 发光功耗。 */
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND_HEX), LV_PART_MAIN);
    /* 背景完全不透明，避免旧 framebuffer 透出。 */
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    /* 左侧留 32 像素，标题“健”不再落入屏幕左上圆角遮挡区。 */
    lv_obj_set_style_pad_left(screen, UI_SAFE_HORIZONTAL_PX, LV_PART_MAIN);
    /* 右侧使用同一安全区，避免长状态文本进入右上圆角。 */
    lv_obj_set_style_pad_right(screen, UI_SAFE_HORIZONTAL_PX, LV_PART_MAIN);
    /* 顶部留 32 像素，保证标题首行完整显示。 */
    lv_obj_set_style_pad_top(screen, UI_SAFE_TOP_PX, LV_PART_MAIN);
    /* 底部保持 20 像素，兼顾按钮触摸面积和下圆角。 */
    lv_obj_set_style_pad_bottom(screen, UI_SAFE_BOTTOM_PX, LV_PART_MAIN);
    /* 使用纵向 flex 适配 410x502 窄长屏。 */
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    /* 元素纵向排列且横向居中。 */
    lv_obj_set_flex_align(
        screen,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER,
        LV_FLEX_ALIGN_CENTER);
    /* 根页面不滚动，防止训练时误触拖动。 */
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

/* 创建标签并设置字体、颜色、宽度和换行。 */
static lv_obj_t *ui_lvgl_create_label(
    lv_obj_t *parent,
    lv_color_t color,
    const lv_font_t *font,
    lv_text_align_t align,
    int32_t width)
{
    /* 创建由 parent 管理生命周期的标签。 */
    lv_obj_t *label = lv_label_create(parent);
    /* 内存不足时返回 NULL。 */
    if (label == NULL) {
        return NULL;
    }
    /* 设置文字颜色。 */
    lv_obj_set_style_text_color(label, color, LV_PART_MAIN);
    /* 使用调用方指定的项目中文字库；字体包含 ASCII 与当前全部可见汉字。 */
    lv_obj_set_style_text_font(label, font, LV_PART_MAIN);
    /* 设置文字对齐方式。 */
    lv_obj_set_style_text_align(label, align, LV_PART_MAIN);
    /* 固定宽度，长动作名可自动换行。 */
    lv_obj_set_width(label, width);
    /* 自动换行，不使用滚动文字以节省刷新。 */
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    /* 初始设置空文本。 */
    lv_label_set_text(label, "");
    /* 返回新标签。 */
    return label;
}

/* 创建一个触摸按钮及稳定 user_data。 */
static bool ui_lvgl_create_button(
    ui_lvgl_renderer_t *renderer,
    ui_lvgl_page_t *page,
    ui_lvgl_button_binding_t *binding,
    size_t index)
{
    /* 在按钮行中创建按钮。 */
    lv_obj_t *button = lv_button_create(page->button_row);
    /* 内存不足时失败。 */
    if (button == NULL) {
        return false;
    }
    /* 固定 54 像素高度，满足触摸目标尺寸。 */
    lv_obj_set_height(button, 54);
    /* 按钮宽度由父 flex 均分。 */
    lv_obj_set_flex_grow(button, 1U);
    /* 使用深灰底色。 */
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CARD_HEX), LV_PART_MAIN);
    /* 按下时使用强调色。 */
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_ACCENT_HEX), LV_PART_MAIN | LV_STATE_PRESSED);
    /* 使用 16 像素圆角。 */
    lv_obj_set_style_radius(button, 16, LV_PART_MAIN);
    /* 保存所属渲染器。 */
    binding->renderer = renderer;
    /* 初始无命令，render 再写入。 */
    binding->command = UI_COMMAND_NONE;
    /* 注册点击事件。 */
    lv_obj_add_event_cb(button, ui_lvgl_button_event, LV_EVENT_CLICKED, binding);
    /* 创建按钮文字。 */
    lv_obj_t *label = lv_label_create(button);
    /* 内存不足时失败。 */
    if (label == NULL) {
        return false;
    }
    /* 初始按钮文字为空。 */
    lv_label_set_text(label, "");
    /* 设置白色文字。 */
    lv_obj_set_style_text_color(label, lv_color_hex(UI_COLOR_TEXT_HEX), LV_PART_MAIN);
    /* 使用 20 像素项目中文字体，提高 2.06 英寸触摸按钮的可读性。 */
    lv_obj_set_style_text_font(label, UI_FONT_CHINESE_20, LV_PART_MAIN);
    /* 居中文字。 */
    lv_obj_center(label);
    /* 保存按钮对象。 */
    page->buttons[index] = button;
    /* 保存文字对象。 */
    page->button_labels[index] = label;
    /* 返回成功。 */
    return true;
}

/*
 * 为显示质量诊断的 HOME 页面应用完全固定的像素几何。
 * 五个文字对象不再参与根 flex 的自动高度与位置重算，避免异步局部刷新把同一文字留在旧坐标。
 */
static void ui_lvgl_apply_static_home_layout(ui_lvgl_page_t *page)
{
    /* HOME 页面对象由创建函数完整建立后才允许调用；空指针仅作防御性保护。 */
    if (page == NULL) {
        /* 空页面没有任何可配置对象，直接返回。 */
        return;
    }
    /* 关闭根 screen 的 flex 布局，使所有子对象只服从下面的绝对坐标。 */
    lv_obj_set_layout(page->root, LV_LAYOUT_NONE);
    /* 绝对坐标已经包含圆角安全区，根对象不再叠加任何内边距。 */
    lv_obj_set_style_pad_all(page->root, 0, LV_PART_MAIN);

    /* 标题短文本使用裁剪模式，禁止内容变化触发自动换行高度重算。 */
    lv_label_set_long_mode(page->title_label, LV_LABEL_LONG_CLIP);
    /* 标题左上角固定在 32 像素圆角安全区。 */
    lv_obj_set_pos(page->title_label, UI_SAFE_HORIZONTAL_PX, UI_HOME_TITLE_Y_PX);
    /* 标题固定为 346×42 像素，可容纳 28 像素字体的 34 像素行高。 */
    lv_obj_set_size(page->title_label, UI_SAFE_CONTENT_WIDTH_PX, 42);

    /* 电池与蓝牙状态使用单行裁剪，避免状态变化改变后续对象位置。 */
    lv_label_set_long_mode(page->status_label, LV_LABEL_LONG_CLIP);
    /* 状态行固定在标题下方。 */
    lv_obj_set_pos(page->status_label, UI_SAFE_HORIZONTAL_PX, UI_HOME_STATUS_Y_PX);
    /* 状态行固定为 346×28 像素，可容纳 20 像素字体的 24 像素行高。 */
    lv_obj_set_size(page->status_label, UI_SAFE_CONTENT_WIDTH_PX, 28);

    /* 主文案使用单行裁剪；当前“常亮联调版 0716”在 346 像素内完整显示。 */
    lv_label_set_long_mode(page->primary_label, LV_LABEL_LONG_CLIP);
    /* 主文案使用固定纵坐标，不再因标题或状态内容宽高变化而移动。 */
    lv_obj_set_pos(page->primary_label, UI_SAFE_HORIZONTAL_PX, UI_HOME_PRIMARY_Y_PX);
    /* 主文案固定为 346×60 像素，可容纳 36 像素字体的 42 像素行高。 */
    lv_obj_set_size(page->primary_label, UI_SAFE_CONTENT_WIDTH_PX, 60);

    /* 次文案同样限制为单行，当前设备就绪提示不会换行。 */
    lv_label_set_long_mode(page->secondary_label, LV_LABEL_LONG_CLIP);
    /* 次文案固定在页面中下部。 */
    lv_obj_set_pos(page->secondary_label, UI_SAFE_HORIZONTAL_PX, UI_HOME_SECONDARY_Y_PX);
    /* 次文案固定为 346×44 像素，可容纳 28 像素字体的 34 像素行高。 */
    lv_obj_set_size(page->secondary_label, UI_SAFE_CONTENT_WIDTH_PX, 44);

    /* 页脚使用单行裁剪，避免按钮行随提示文字高度变化。 */
    lv_label_set_long_mode(page->footer_label, LV_LABEL_LONG_CLIP);
    /* 页脚固定在按钮行上方。 */
    lv_obj_set_pos(page->footer_label, UI_SAFE_HORIZONTAL_PX, UI_HOME_FOOTER_Y_PX);
    /* 页脚固定为 346×28 像素，可容纳 20 像素字体。 */
    lv_obj_set_size(page->footer_label, UI_SAFE_CONTENT_WIDTH_PX, 28);

    /* 按钮行固定在屏幕底部安全区；其内部按钮仍沿用已在真板显示清晰的横向 flex。 */
    lv_obj_set_pos(page->button_row, UI_SAFE_HORIZONTAL_PX, UI_HOME_BUTTON_ROW_Y_PX);
    /* 按钮行固定为 346×70 像素，不随上方五个标签变化。 */
    lv_obj_set_size(page->button_row, UI_SAFE_CONTENT_WIDTH_PX, 70);
}

/* 创建一张独立 LVGL screen。 */
static bool ui_lvgl_create_page(
    ui_lvgl_renderer_t *renderer,
    ui_lvgl_implementation_t *implementation,
    ui_state_t state)
{
    /* 按状态索引获取页面槽位。 */
    ui_lvgl_page_t *page = &implementation->pages[state];
    /* 创建独立 screen。 */
    page->root = lv_obj_create(NULL);
    /* 内存不足时失败。 */
    if (page->root == NULL) {
        return false;
    }
    /* 应用统一背景和布局。 */
    ui_lvgl_style_screen(page->root);
    /* 创建标题。 */
    page->title_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_ACCENT_HEX), UI_FONT_CHINESE_28, LV_TEXT_ALIGN_LEFT, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建电池和 BLE 状态。 */
    page->status_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_MUTED_HEX), UI_FONT_CHINESE_20, LV_TEXT_ALIGN_LEFT, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建主文本。 */
    page->primary_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_TEXT_HEX), UI_FONT_CHINESE_36, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建次文本。 */
    page->secondary_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_SECONDARY_HEX), UI_FONT_CHINESE_28, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建底部提示。 */
    page->footer_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_MUTED_HEX), UI_FONT_CHINESE_20, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 任一标签创建失败时报告内存不足。 */
    if ((page->title_label == NULL) || (page->status_label == NULL) ||
        (page->primary_label == NULL) || (page->secondary_label == NULL) ||
        (page->footer_label == NULL)) {
        return false;
    }
    /* 主文本占两份可用空间。 */
    lv_obj_set_flex_grow(page->primary_label, 2U);
    /* 次文本占一份空间。 */
    lv_obj_set_flex_grow(page->secondary_label, 1U);
    /* 创建底部按钮行。 */
    page->button_row = lv_obj_create(page->root);
    /* 内存不足时失败。 */
    if (page->button_row == NULL) {
        return false;
    }
    /* 按钮行占满 346 像素安全宽度，与顶部文字左右边界一致。 */
    lv_obj_set_width(page->button_row, UI_SAFE_CONTENT_WIDTH_PX);
    /* 按钮行高度为 70 像素。 */
    lv_obj_set_height(page->button_row, 70);
    /* 容器背景透明。 */
    lv_obj_set_style_bg_opa(page->button_row, LV_OPA_TRANSP, LV_PART_MAIN);
    /* 容器无边框。 */
    lv_obj_set_style_border_width(page->button_row, 0, LV_PART_MAIN);
    /* 设置 4 像素内边距。 */
    lv_obj_set_style_pad_all(page->button_row, 4, LV_PART_MAIN);
    /* 按钮横向排列。 */
    lv_obj_set_flex_flow(page->button_row, LV_FLEX_FLOW_ROW);
    /* 按钮等距且居中。 */
    lv_obj_set_flex_align(page->button_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 创建五个固定按钮槽位；未使用槽位由 presenter 以 NONE 隐藏。 */
    for (size_t index = 0U; index < UI_PRESENTER_MAX_BUTTONS; ++index) {
        /* 任一按钮创建失败时停止。 */
        if (!ui_lvgl_create_button(renderer, page, &implementation->bindings[state][index], index)) {
            return false;
        }
    }
    /* HOME 显示诊断使用固定坐标，排除自动 flex 重排造成同一文字多位置残留。 */
    if (state == UI_STATE_HOME) {
        /* 只修改 HOME；其它产品页面继续保持原响应式 flex 布局。 */
        ui_lvgl_apply_static_home_layout(page);
    }
    /* 设置页需要显示五个中文按钮，改为三加二的两行布局避免文字被压缩。 */
    if (state == UI_STATE_SETTINGS) {
        /* 把按钮容器增高到 116 像素，可容纳两行 48 像素触摸目标和行间距。 */
        lv_obj_set_height(page->button_row, 116);
        /* 启用水平排列并自动换行，不改变其它页面的单行按钮。 */
        lv_obj_set_flex_flow(page->button_row, LV_FLEX_FLOW_ROW_WRAP);
        /* 遍历五个已创建按钮，设置固定宽高便于手指点击和中文换行。 */
        for (size_t index = 0U; index < UI_PRESENTER_MAX_BUTTONS; ++index) {
            /* 取消 flex 伸展，防止五个按钮在第一行被压缩。 */
            lv_obj_set_flex_grow(page->buttons[index], 0U);
            /* 104 像素宽度允许 346 像素安全区第一行排列三个按钮并保留间距。 */
            lv_obj_set_width(page->buttons[index], UI_SETTINGS_BUTTON_WIDTH_PX);
            /* 48 像素高度达到手表触摸目标下限并留出第二行间距。 */
            lv_obj_set_height(page->buttons[index], 48);
        }
    }
    /* SCREEN_OFF 页隐藏文字，但保留不可见全屏按钮接收第一下触摸唤醒。 */
    if (state == UI_STATE_SCREEN_OFF) {
        /* 隐藏标题，避免 AMOLED 再次点亮前刷新旧像素。 */
        lv_obj_add_flag(page->title_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏状态栏。 */
        lv_obj_add_flag(page->status_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏主信息。 */
        lv_obj_add_flag(page->primary_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏次信息。 */
        lv_obj_add_flag(page->secondary_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏页脚。 */
        lv_obj_add_flag(page->footer_label, LV_OBJ_FLAG_HIDDEN);
        /* 让透明按钮行占据去除页面安全边距后的全部可用区域。 */
        lv_obj_set_width(page->button_row, LV_PCT(100));
        /* 让透明按钮行吃满纵向剩余空间。 */
        lv_obj_set_flex_grow(page->button_row, 1U);
        /* 让第一个按钮覆盖整行高度，任意触摸位置都能产生 WAKE。 */
        lv_obj_set_height(page->buttons[0], LV_PCT(100));
        /* 隐藏唤醒按钮背景，避免短暂点屏时出现卡片。 */
        lv_obj_set_style_bg_opa(page->buttons[0], LV_OPA_TRANSP, LV_PART_MAIN);
        /* 禁用唤醒按钮圆角阴影的视觉成本。 */
        lv_obj_set_style_shadow_width(page->buttons[0], 0, LV_PART_MAIN);
    }
    /* 返回成功。 */
    return true;
}

/* 更新当前页面对象、按钮和轻量动画。 */
static void ui_lvgl_update_page(
    ui_lvgl_renderer_t *renderer,
    ui_lvgl_implementation_t *implementation,
    const ui_context_t *context,
    const ui_page_model_t *model)
{
    /* 获取当前状态页面。 */
    ui_lvgl_page_t *page = &implementation->pages[context->state];
    /* 更新标题文本。 */
    lv_label_set_text(page->title_label, model->title);
        /* 更新电池和 BLE 状态。 */
        lv_label_set_text(page->status_label, model->status);
        /* 更新主文本。 */
        lv_label_set_text(page->primary_label, model->primary);
        /* 更新次文本。 */
        lv_label_set_text(page->secondary_label, model->secondary);
        /* 更新页脚。 */
        lv_label_set_text(page->footer_label, model->footer);
        /* 更新五个固定按钮槽位；NONE 槽位保持隐藏。 */
        for (size_t index = 0U; index < UI_PRESENTER_MAX_BUTTONS; ++index) {
            /* 保存稳定渲染器指针和当前命令。 */
            implementation->bindings[context->state][index].renderer = renderer;
            /* 保存当前按钮命令。 */
            implementation->bindings[context->state][index].command = model->buttons[index].command;
            /* NONE 槽位隐藏按钮。 */
            if (model->buttons[index].command == UI_COMMAND_NONE) {
                /* 隐藏无命令按钮。 */
                lv_obj_add_flag(page->buttons[index], LV_OBJ_FLAG_HIDDEN);
                /* 继续下一槽位。 */
                continue;
            }
            /* 有命令时显示按钮。 */
            lv_obj_remove_flag(page->buttons[index], LV_OBJ_FLAG_HIDDEN);
            /* 更新按钮文字。 */
            lv_label_set_text(page->button_labels[index], model->buttons[index].label);
            /* 同步禁用状态。 */
            if (model->buttons[index].enabled) {
                /* 有效命令允许点击。 */
                lv_obj_remove_state(page->buttons[index], LV_STATE_DISABLED);
            } else {
                /* 无效命令显示禁用态。 */
                lv_obj_add_state(page->buttons[index], LV_STATE_DISABLED);
            }
        }
    /* 页面切换时播放短淡入，避免大面积滑动动画。 */
    if (implementation->active_state != context->state) {
        /* 产品模式保留短淡入；静态诊断模式禁止分块 QSPI 在旧/新 screen 间反复混合刷新。 */
        if (renderer->animations_enabled) {
            /* 150 ms 淡入只在已通过真板视觉门后使用。 */
            lv_screen_load_anim(page->root, LV_SCR_LOAD_ANIM_FADE_IN, 150U, 0U, false);
            /* 读取产品动画配置。 */
            const ui_animation_profile_t animation = ui_default_animation_profile();
            /* BOOT 播放 800 ms 文字淡入。 */
            if (context->state == UI_STATE_BOOT) {
                /* 只改变主标签不透明度，不移动页面对象。 */
                ui_lvgl_start_fade(page->primary_label, animation.boot_ms);
            }
            /* SHUTDOWN 播放 500 ms 文字淡入。 */
            if (context->state == UI_STATE_SHUTDOWN) {
                /* 关机主文案淡入后由电源状态机处理面板。 */
                ui_lvgl_start_fade(page->primary_label, animation.shutdown_ms);
            }
        } else {
            /* 立即切换到已完整创建的 screen，避免旧 screen 与新中文字形交叉淡入。 */
            lv_screen_load(page->root);
        }
        /* 保存当前页面。 */
        implementation->active_state = context->state;
    }
    /* 训练动作变化时淡入动作名。 */
    if (renderer->animations_enabled &&
        (context->state == UI_STATE_RUNNING) &&
        (context->view.action_id != implementation->previous_action_id)) {
        /* 读取动作交叉淡入时长。 */
        const ui_animation_profile_t animation = ui_default_animation_profile();
        /* 只对新动作名执行透明度动画。 */
        ui_lvgl_start_fade(page->primary_label, animation.action_crossfade_ms);
        /* 保存动作索引，避免同一动作重复创建动画。 */
        implementation->previous_action_id = context->view.action_id;
    }
    /* 指标增长时淡入次数/步数/秒文本。 */
    if (renderer->animations_enabled &&
        (context->state == UI_STATE_RUNNING) &&
        (context->view.count != implementation->previous_count)) {
        /* 读取计数反馈时长。 */
        const ui_animation_profile_t animation = ui_default_animation_profile();
        /* 只对次数指标执行透明度动画。 */
        ui_lvgl_start_fade(page->secondary_label, animation.count_feedback_ms);
        /* 保存计数，避免同一值重复创建动画。 */
        implementation->previous_count = context->view.count;
    }
}

/* 初始化全部页面。 */
ui_lvgl_result_t ui_lvgl_renderer_init(
    ui_lvgl_renderer_t *renderer,
    const ui_lvgl_port_t *port,
    ui_lvgl_command_fn command_callback,
    void *command_context)
{
    /* 渲染器、端口和锁函数必须有效。 */
    if ((renderer == NULL) || (port == NULL) ||
        (port->lock == NULL) || (port->unlock == NULL)) {
        return UI_LVGL_ERR_ARGUMENT;
    }
    /* 已初始化对象不能重复创建页面。 */
    if (renderer->initialized) {
        return UI_LVGL_ERR_STATE;
    }
    /* 获取 BSP LVGL 锁。 */
    if (!port->lock(port->context, UI_LVGL_LOCK_TIMEOUT_MS)) {
        return UI_LVGL_ERR_LOCK;
    }
    /* 分配一次零初始化内部对象集合。 */
    ui_lvgl_implementation_t *implementation =
        (ui_lvgl_implementation_t *)calloc(1U, sizeof(*implementation));
    /* 分配失败时释放锁。 */
    if (implementation == NULL) {
        port->unlock(port->context);
        return UI_LVGL_ERR_MEMORY;
    }
    /* 保存端口和命令回调。 */
    renderer->port = *port;
    renderer->command_callback = command_callback;
    renderer->command_context = command_context;
    renderer->implementation = implementation;
    /* 产品默认保留动画；真板静态诊断由公开接口在首帧前显式关闭。 */
    renderer->animations_enabled = true;
    /* 获取厂家 BSP 创建的默认显示器；页面创建依赖有效的默认显示目标。 */
    lv_display_t *display = lv_display_get_default();
    /* BSP 未创建显示器时不能继续创建页面。 */
    if (display == NULL) {
        /* 释放尚未拥有 LVGL 对象的内部结构。 */
        free(implementation);
        /* 清除公开句柄中的悬空内部指针。 */
        renderer->implementation = NULL;
        /* 释放 BSP LVGL 锁。 */
        port->unlock(port->context);
        /* 返回状态错误，调用方保留串口故障证据。 */
        return UI_LVGL_ERR_STATE;
    }
    /* 初始没有活动页面。 */
    implementation->active_state = UI_STATE_COUNT;
    /* 初始动作为 255 哨兵。 */
    implementation->previous_action_id = UINT8_MAX;
    /* 创建 BOOT 到 SHUTDOWN 全部 screen。 */
    for (ui_state_t state = UI_STATE_BOOT; state < UI_STATE_COUNT; state = (ui_state_t)(state + 1)) {
        /* 任一页面失败时清理已经创建的页面。 */
        if (!ui_lvgl_create_page(renderer, implementation, state)) {
            for (ui_state_t cleanup = UI_STATE_BOOT; cleanup <= state; cleanup = (ui_state_t)(cleanup + 1)) {
                if (implementation->pages[cleanup].root != NULL) {
                    lv_obj_delete(implementation->pages[cleanup].root);
                }
            }
            /* 释放内部结构。 */
            free(implementation);
            /* 清除悬空指针。 */
            renderer->implementation = NULL;
            /* 释放锁。 */
            port->unlock(port->context);
            /* 返回内存错误。 */
            return UI_LVGL_ERR_MEMORY;
        }
    }
    /* 全部页面创建后标记初始化。 */
    renderer->initialized = true;
    /* 释放锁。 */
    port->unlock(port->context);
    /* 返回成功。 */
    return UI_LVGL_OK;
}

/* 启用或禁用页面与文字动画；静态模式用于分离字体数据和刷新时序问题。 */
ui_lvgl_result_t ui_lvgl_renderer_set_animations_enabled(
    ui_lvgl_renderer_t *renderer,
    bool enabled)
{
    /* 渲染器必须已完成页面创建，避免向未初始化句柄写状态。 */
    if ((renderer == NULL) || !renderer->initialized || (renderer->implementation == NULL)) {
        /* 返回状态错误，不修改任何 LVGL 对象。 */
        return UI_LVGL_ERR_STATE;
    }
    /* 获取与 render 相同的 BSP LVGL 锁，保证运行期切换时没有并发读取。 */
    if (!renderer->port.lock(renderer->port.context, UI_LVGL_LOCK_TIMEOUT_MS)) {
        /* 超时表示当前帧仍占用 LVGL；调用方可稍后重试。 */
        return UI_LVGL_ERR_LOCK;
    }
    /* 写入单一布尔开关；false 后所有新页面和文字变化都不再创建动画。 */
    renderer->animations_enabled = enabled;
    /* 关闭动画时删除渲染器此前可能创建的全部 LVGL 动画，避免旧定时器继续重绘文字。 */
    if (!enabled) {
        /* 当前隔离调用发生在首帧前；全局删除不会影响其它组件。 */
        lv_anim_delete_all();
    }
    /* 释放 BSP LVGL 锁。 */
    renderer->port.unlock(renderer->port.context);
    /* 返回成功，后续 render 将读取新开关。 */
    return UI_LVGL_OK;
}

/* 渲染最新状态。 */
ui_lvgl_result_t ui_lvgl_renderer_render(
    ui_lvgl_renderer_t *renderer,
    const ui_context_t *context)
{
    /* 输入不能为空。 */
    if ((renderer == NULL) || (context == NULL)) {
        return UI_LVGL_ERR_ARGUMENT;
    }
    /* 必须初始化且状态在数组范围内。 */
    if (!renderer->initialized || (renderer->implementation == NULL) ||
        (context->state >= UI_STATE_COUNT)) {
        return UI_LVGL_ERR_STATE;
    }
    /* 在锁外构建纯页面模型，缩短 LVGL 临界区。 */
    ui_page_model_t model;
    /* presenter 失败表示业务状态损坏。 */
    if (!ui_presenter_build(context, &model)) {
        return UI_LVGL_ERR_STATE;
    }
    /* 获取 LVGL 锁。 */
    if (!renderer->port.lock(renderer->port.context, UI_LVGL_LOCK_TIMEOUT_MS)) {
        return UI_LVGL_ERR_LOCK;
    }
    /* 更新当前页面。 */
    ui_lvgl_update_page(
        renderer,
        (ui_lvgl_implementation_t *)renderer->implementation,
        context,
        &model);
    /* 释放 LVGL 锁。 */
    renderer->port.unlock(renderer->port.context);
    /* 返回成功。 */
    return UI_LVGL_OK;
}

/* 删除全部页面和内部结构。 */
ui_lvgl_result_t ui_lvgl_renderer_deinit(ui_lvgl_renderer_t *renderer)
{
    /* 输入不能为空。 */
    if (renderer == NULL) {
        return UI_LVGL_ERR_ARGUMENT;
    }
    /* 未初始化对象不能重复释放。 */
    if (!renderer->initialized || (renderer->implementation == NULL)) {
        return UI_LVGL_ERR_STATE;
    }
    /* 获取 LVGL 锁。 */
    if (!renderer->port.lock(renderer->port.context, UI_LVGL_LOCK_TIMEOUT_MS)) {
        return UI_LVGL_ERR_LOCK;
    }
    /* 获取内部实现。 */
    ui_lvgl_implementation_t *implementation =
        (ui_lvgl_implementation_t *)renderer->implementation;
    /* 删除全部 screen；LVGL 递归释放子对象。 */
    for (ui_state_t state = UI_STATE_BOOT; state < UI_STATE_COUNT; state = (ui_state_t)(state + 1)) {
        if (implementation->pages[state].root != NULL) {
            lv_obj_delete(implementation->pages[state].root);
        }
    }
    /* 释放 LVGL 锁。 */
    renderer->port.unlock(renderer->port.context);
    /* 释放内部普通堆内存。 */
    free(implementation);
    /* 清零公开状态。 */
    (void)memset(renderer, 0, sizeof(*renderer));
    /* 返回成功。 */
    return UI_LVGL_OK;
}
