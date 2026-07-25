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
/* AMOLED 主背景使用接近纯黑的石墨色，降低发光面积并形成智能手表质感。 */
#define UI_COLOR_BACKGROUND_HEX (0x05070BU)
/* 主信息卡使用深蓝黑表面，和背景形成低对比层级。 */
#define UI_COLOR_CARD_HEX (0x0D1420U)
/* 抬升按钮和状态胶囊使用稍亮表面色。 */
#define UI_COLOR_RAISED_HEX (0x151F2EU)
/* 主强调色使用高辨识度系统蓝，只用于主操作和关键状态。 */
#define UI_COLOR_ACCENT_HEX (0x0A84FFU)
/* 科技状态辅助色使用清亮青色，不与主按钮争夺层级。 */
#define UI_COLOR_CYAN_HEX (0x32D5FFU)
/* 正常连接、计数和健康电量使用系统绿色。 */
#define UI_COLOR_SUCCESS_HEX (0x30D158U)
/* 休息、低电量和待确认状态使用系统橙色。 */
#define UI_COLOR_WARNING_HEX (0xFF9F0AU)
/* 故障和破坏性操作使用系统红色。 */
#define UI_COLOR_DANGER_HEX (0xFF453AU)
/* 固定次级文本冷灰蓝。 */
#define UI_COLOR_SECONDARY_HEX (0xA9B4C4U)
/* 固定普通文本近白色。 */
#define UI_COLOR_TEXT_HEX (0xF5F7FAU)
/* 固定弱化文本灰蓝色。 */
#define UI_COLOR_MUTED_HEX (0x657187U)
/* 卡片与按钮描边使用低亮度蓝灰，增强结构但不形成装饰噪声。 */
#define UI_COLOR_BORDER_HEX (0x223047U)
/* SH8601 逻辑横向宽度固定 410 像素，用于计算圆角屏幕安全内容宽度。 */
#define UI_SCREEN_WIDTH_PX (410)
/* 左右各留 32 像素，避免标题首字和右侧状态进入玻璃圆角不可视区。 */
#define UI_SAFE_HORIZONTAL_PX (32)
/* 顶部留 28 像素，使品牌标、电池图标离开圆角切线。 */
#define UI_SAFE_TOP_PX (28)
/* 底部保留 22 像素，保证最下方按钮不进入圆角遮挡区。 */
#define UI_SAFE_BOTTOM_PX (22)
/* 安全文本宽度等于 410 减去左右各 32 像素，即 346 像素。 */
#define UI_SAFE_CONTENT_WIDTH_PX (UI_SCREEN_WIDTH_PX - (2 * UI_SAFE_HORIZONTAL_PX))
/* 设置页按钮使用 166 像素，两列按钮在 346 像素安全宽度内保留间距。 */
#define UI_SETTINGS_BUTTON_WIDTH_PX (166)
/* 顶部品牌栏起点固定为 28 像素。 */
#define UI_TOP_BAR_Y_PX (28)
/* 电池外框顶部固定为 31 像素，位于右上第一行。 */
#define UI_BATTERY_SHELL_Y_PX (31)
/* 电池外框高度固定为 20 像素，其底边位于 51 像素。 */
#define UI_BATTERY_SHELL_HEIGHT_PX (20)
/* 电池正极帽顶部固定为 37 像素，与外框垂直居中。 */
#define UI_BATTERY_TIP_Y_PX (37)
/* 电池百分比文字顶部固定为 29 像素，与图标第一行对齐。 */
#define UI_BATTERY_LABEL_Y_PX (29)
/* BLE 状态点顶部固定为 63 像素，独占右上第二行。 */
#define UI_BLE_DOT_Y_PX (63)
/* BLE 状态文字顶部固定为 56 像素，与电池底边保留 5 像素间距。 */
#define UI_BLE_LABEL_Y_PX (56)
/* 主信息卡起点固定为 94 像素，避开顶部品牌和设备状态。 */
#define UI_HERO_CARD_Y_PX (94)
/* 主信息卡高度固定为 250 像素，保证动作与累计值同屏。 */
#define UI_HERO_CARD_HEIGHT_PX (250)
/* 普通页面按钮行固定在 396 像素，底部仍保留圆角安全区。 */
#define UI_BUTTON_ROW_Y_PX (396)
/* 设置页四按钮使用两行，按钮区提前到 354 像素。 */
#define UI_SETTINGS_BUTTON_ROW_Y_PX (354)
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
    /* 保存“毕”字几何品牌标。 */
    lv_obj_t *brand_mark;
    /* 保存品牌标内的“毕”字。 */
    lv_obj_t *brand_mark_label;
    /* 保存“毕昇杯”品牌名称。 */
    lv_obj_t *brand_label;
    /* 保存“智慧运动助手”产品副标题。 */
    lv_obj_t *title_label;
    /* 保存电池外框。 */
    lv_obj_t *battery_shell;
    /* 保存随百分比改变宽度的电池填充。 */
    lv_obj_t *battery_fill;
    /* 保存电池正极帽。 */
    lv_obj_t *battery_tip;
    /* 保存电池百分比文字。 */
    lv_obj_t *battery_label;
    /* 保存 BLE 连接状态点。 */
    lv_obj_t *ble_dot;
    /* 保存 BLE 连接文字。 */
    lv_obj_t *ble_label;
    /* 保存主信息卡背景。 */
    lv_obj_t *hero_card;
    /* 保存页面状态胶囊。 */
    lv_obj_t *status_pill;
    /* 保存页面状态文字。 */
    lv_obj_t *status_label;
    /* 保存动作名、倒计时或总结主值。 */
    lv_obj_t *primary_label;
    /* 保存次数、卡路里和时间。 */
    lv_obj_t *secondary_label;
    /* 保存底部提示。 */
    lv_obj_t *footer_label;
    /* 保存活动状态轨道。 */
    lv_obj_t *activity_track;
    /* 保存活动状态填充。 */
    lv_obj_t *activity_fill;
    /* 保存按钮行容器。 */
    lv_obj_t *button_row;
    /* 保存五个固定按钮对象；设置页使用两列双行，其它页面隐藏未用槽位。 */
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

/* 配置 screen 的 AMOLED 友好背景和固定像素布局。 */
static void ui_lvgl_style_screen(lv_obj_t *screen)
{
    /* 接近纯黑背景降低 AMOLED 发光功耗。 */
    lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND_HEX), LV_PART_MAIN);
    /* 背景完全不透明，避免旧 framebuffer 透出。 */
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
    /* 固定布局自己包含圆角安全区，根 screen 不再叠加内边距。 */
    lv_obj_set_style_pad_all(screen, 0, LV_PART_MAIN);
    /* 禁用自动 flex，避免动态中文长度改变其它对象坐标并产生异步刷新残影。 */
    lv_obj_set_layout(screen, LV_LAYOUT_NONE);
    /* 根页面不滚动，防止训练时误触拖动。 */
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
}

/* 创建固定位置的圆角表面对象；只在初始化阶段调用。 */
static lv_obj_t *ui_lvgl_create_surface(
    lv_obj_t *parent,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height,
    uint32_t color_hex,
    int32_t radius)
{
    /* 父对象必须存在，防止把无主对象挂到默认 screen。 */
    if (parent == NULL) {
        /* 返回空指针，调用方进入统一内存错误路径。 */
        return NULL;
    }
    /* 创建普通对象作为卡片、状态胶囊或图标轮廓。 */
    lv_obj_t *surface = lv_obj_create(parent);
    /* 分配失败时返回空指针。 */
    if (surface == NULL) {
        /* 调用方会递归删除已创建页面。 */
        return NULL;
    }
    /* 写入固定坐标，避免运行期布局重算。 */
    lv_obj_set_pos(surface, x, y);
    /* 写入固定尺寸。 */
    lv_obj_set_size(surface, width, height);
    /* 写入表面底色。 */
    lv_obj_set_style_bg_color(surface, lv_color_hex(color_hex), LV_PART_MAIN);
    /* 表面完全不透明。 */
    lv_obj_set_style_bg_opa(surface, LV_OPA_COVER, LV_PART_MAIN);
    /* 写入统一圆角。 */
    lv_obj_set_style_radius(surface, radius, LV_PART_MAIN);
    /* 默认取消内边距，子对象使用自身固定坐标。 */
    lv_obj_set_style_pad_all(surface, 0, LV_PART_MAIN);
    /* 默认取消边框；需要描边的调用方随后单独设置。 */
    lv_obj_set_style_border_width(surface, 0, LV_PART_MAIN);
    /* 卡片不可滚动，训练过程中拖动不改变布局。 */
    lv_obj_remove_flag(surface, LV_OBJ_FLAG_SCROLLABLE);
    /* 返回新表面对象。 */
    return surface;
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

/* 为全部产品页面应用同一智能手表固定网格。 */
static void ui_lvgl_apply_product_layout(ui_lvgl_page_t *page, ui_state_t state)
{
    /* 页面对象由创建函数完整建立后才允许调用；空指针仅作防御性保护。 */
    if (page == NULL) {
        /* 空页面没有任何可配置对象，直接返回。 */
        return;
    }
    /* 品牌标固定在左上安全区。 */
    lv_obj_set_pos(page->brand_mark, UI_SAFE_HORIZONTAL_PX, UI_TOP_BAR_Y_PX);
    /* 品牌标固定为 38×38 像素，兼顾识别度和圆角屏安全区。 */
    lv_obj_set_size(page->brand_mark, 38, 38);
    /* “毕”字始终位于品牌标中央。 */
    lv_obj_center(page->brand_mark_label);
    /* 品牌名称固定在品牌标右侧第一行。 */
    lv_obj_set_pos(page->brand_label, 80, UI_TOP_BAR_Y_PX - 2);
    /* 品牌名称使用单行裁剪，避免任何状态改变顶栏高度。 */
    lv_label_set_long_mode(page->brand_label, LV_LABEL_LONG_CLIP);
    /* 品牌名称可用宽度固定为 104 像素。 */
    lv_obj_set_size(page->brand_label, 104, 26);
    /* 产品副标题固定在品牌名称下方。 */
    lv_obj_set_pos(page->title_label, 80, UI_TOP_BAR_Y_PX + 22);
    /* 产品副标题单行显示。 */
    lv_label_set_long_mode(page->title_label, LV_LABEL_LONG_CLIP);
    /* 产品副标题宽度固定为 164 像素。 */
    lv_obj_set_size(page->title_label, 164, 22);
    /* 电池百分比位于电池图标左侧并右对齐。 */
    lv_obj_set_pos(page->battery_label, 254, UI_BATTERY_LABEL_Y_PX);
    /* 百分比文字固定 54×22 像素。 */
    lv_obj_set_size(page->battery_label, 54, 22);
    /* BLE 状态点固定在右上第二行。 */
    lv_obj_set_pos(page->ble_dot, 281, UI_BLE_DOT_Y_PX);
    /* BLE 状态文字位于状态点右侧。 */
    lv_obj_set_pos(page->ble_label, 295, UI_BLE_LABEL_Y_PX);
    /* BLE 文字固定单行。 */
    lv_label_set_long_mode(page->ble_label, LV_LABEL_LONG_CLIP);
    /* BLE 文字宽度固定为 82 像素。 */
    lv_obj_set_size(page->ble_label, 82, 22);
    /* 状态胶囊文字使用单行裁剪，避免页面状态改变卡片布局。 */
    lv_label_set_long_mode(page->status_label, LV_LABEL_LONG_CLIP);
    /* 状态文字固定在胶囊内部。 */
    lv_obj_set_pos(page->status_label, 62, UI_HERO_CARD_Y_PX + 24);
    /* 状态文字最多显示 132×24 像素。 */
    lv_obj_set_size(page->status_label, 132, 24);
    /* 主文案使用单行裁剪，动作名和关键状态保持稳定基线。 */
    lv_label_set_long_mode(page->primary_label, LV_LABEL_LONG_CLIP);
    /* 主文案固定在主卡中部。 */
    lv_obj_set_pos(page->primary_label, 52, UI_HERO_CARD_Y_PX + 68);
    /* 主文案固定 306×58 像素。 */
    lv_obj_set_size(page->primary_label, 306, 58);
    /* 次文案限制为单行，次数、步数和热量不改变对象高度。 */
    lv_label_set_long_mode(page->secondary_label, LV_LABEL_LONG_CLIP);
    /* 次文案固定在主文案下方。 */
    lv_obj_set_pos(page->secondary_label, 52, UI_HERO_CARD_Y_PX + 132);
    /* 次文案固定 306×48 像素。 */
    lv_obj_set_size(page->secondary_label, 306, 48);
    /* 页脚使用单行裁剪，显示时长、恢复条件或简短提示。 */
    lv_label_set_long_mode(page->footer_label, LV_LABEL_LONG_CLIP);
    /* 页脚固定在活动轨道上方。 */
    lv_obj_set_pos(page->footer_label, 52, UI_HERO_CARD_Y_PX + 190);
    /* 页脚固定 306×30 像素。 */
    lv_obj_set_size(page->footer_label, 306, 30);
    /* 普通页面按钮行固定在底部安全区。 */
    lv_obj_set_pos(page->button_row, UI_SAFE_HORIZONTAL_PX, UI_BUTTON_ROW_Y_PX);
    /* 固定按钮区每次应用页面几何时都复位到原点，禁止继承任何历史滚动偏移。 */
    lv_obj_scroll_to(page->button_row, 0, 0, LV_ANIM_OFF);
    /* 普通按钮行固定 346×70 像素。 */
    lv_obj_set_size(page->button_row, UI_SAFE_CONTENT_WIDTH_PX, 70);
    /* 设置页使用两行四按钮，需要更高的按钮区域。 */
    if (state == UI_STATE_SETTINGS) {
        /* 设置按钮区提前到 354 像素，避免第二行进入底部圆角。 */
        lv_obj_set_pos(page->button_row, UI_SAFE_HORIZONTAL_PX, UI_SETTINGS_BUTTON_ROW_Y_PX);
        /* 两行按钮区固定为 346×126 像素。 */
        lv_obj_set_size(page->button_row, UI_SAFE_CONTENT_WIDTH_PX, 126);
        /* 真板主题的 flex 行列间距会改变自动换行结果；设置页改用不依赖主题的绝对布局。 */
        lv_obj_set_layout(page->button_row, LV_LAYOUT_NONE);
        /* 绝对坐标直接覆盖完整 346 像素内宽，因此设置页容器不得再叠加四周内边距。 */
        lv_obj_set_style_pad_all(page->button_row, 0, LV_PART_MAIN);
        /* 左上槽位固定为亮度按钮，宽 166、高 52 像素。 */
        lv_obj_set_pos(page->buttons[0], 0, 0);
        /* 写入亮度按钮固定触摸尺寸。 */
        lv_obj_set_size(page->buttons[0], UI_SETTINGS_BUTTON_WIDTH_PX, 52);
        /* 右上槽位固定为诊断按钮，左右两列保留 14 像素间距。 */
        lv_obj_set_pos(page->buttons[1], 180, 0);
        /* 写入诊断按钮固定触摸尺寸。 */
        lv_obj_set_size(page->buttons[1], UI_SETTINGS_BUTTON_WIDTH_PX, 52);
        /* 左下槽位固定为忘记电脑按钮，与第一行保留 12 像素间距。 */
        lv_obj_set_pos(page->buttons[2], 0, 64);
        /* 写入忘记电脑按钮固定触摸尺寸。 */
        lv_obj_set_size(page->buttons[2], UI_SETTINGS_BUTTON_WIDTH_PX, 52);
        /* 右下槽位固定为返回按钮，保证用户始终能回到主页。 */
        lv_obj_set_pos(page->buttons[3], 180, 64);
        /* 写入返回按钮固定触摸尺寸。 */
        lv_obj_set_size(page->buttons[3], UI_SETTINGS_BUTTON_WIDTH_PX, 52);
    }
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
    /* 创建蓝色圆角品牌标，内部只承载一个“毕”字，避免依赖外部图片资源。 */
    page->brand_mark = ui_lvgl_create_surface(
        page->root,
        UI_SAFE_HORIZONTAL_PX,
        UI_TOP_BAR_Y_PX,
        38,
        38,
        UI_COLOR_ACCENT_HEX,
        12);
    /* 创建品牌标文字。 */
    page->brand_mark_label = ui_lvgl_create_label(
        page->brand_mark,
        lv_color_hex(UI_COLOR_TEXT_HEX),
        UI_FONT_CHINESE_20,
        LV_TEXT_ALIGN_CENTER,
        38);
    /* 品牌标固定显示“毕”。 */
    if (page->brand_mark_label != NULL) {
        /* 品牌字不随页面业务状态变化。 */
        lv_label_set_text(page->brand_mark_label, "毕");
    }
    /* 创建“毕昇杯”品牌名称。 */
    page->brand_label = ui_lvgl_create_label(
        page->root,
        lv_color_hex(UI_COLOR_TEXT_HEX),
        UI_FONT_CHINESE_20,
        LV_TEXT_ALIGN_LEFT,
        104);
    /* 品牌名称固定，不由 presenter 覆盖。 */
    if (page->brand_label != NULL) {
        /* 写入赛事主题品牌。 */
        lv_label_set_text(page->brand_label, "毕昇杯");
    }
    /* 创建“智慧运动助手”产品副标题。 */
    page->title_label = ui_lvgl_create_label(
        page->root,
        lv_color_hex(UI_COLOR_MUTED_HEX),
        UI_FONT_CHINESE_16,
        LV_TEXT_ALIGN_LEFT,
        164);
    /* 产品副标题固定，不因页面切换造成顶栏跳动。 */
    if (page->title_label != NULL) {
        /* 写入用户指定产品主题。 */
        lv_label_set_text(page->title_label, "智慧运动助手");
    }
    /* 创建电池外框；填充宽度在 render 中按真实百分比更新。 */
    page->battery_shell = ui_lvgl_create_surface(
        page->root,
        312,
        UI_BATTERY_SHELL_Y_PX,
        44,
        UI_BATTERY_SHELL_HEIGHT_PX,
        UI_COLOR_BACKGROUND_HEX,
        5);
    /* 电池外框使用 2 像素冷灰描边。 */
    if (page->battery_shell != NULL) {
        /* 显示真实电池轮廓。 */
        lv_obj_set_style_border_width(page->battery_shell, 2, LV_PART_MAIN);
        /* 描边颜色不随电量变化，保持图标形状稳定。 */
        lv_obj_set_style_border_color(page->battery_shell, lv_color_hex(UI_COLOR_MUTED_HEX), LV_PART_MAIN);
    }
    /* 创建电池内部填充，宽度最小 2、最大 36 像素。 */
    page->battery_fill = ui_lvgl_create_surface(page->battery_shell, 4, 4, 18, 12, UI_COLOR_SUCCESS_HEX, 3);
    /* 创建电池正极帽。 */
    page->battery_tip = ui_lvgl_create_surface(page->root, 358, UI_BATTERY_TIP_Y_PX, 4, 8, UI_COLOR_MUTED_HEX, 2);
    /* 创建电量百分比文字。 */
    page->battery_label = ui_lvgl_create_label(
        page->root,
        lv_color_hex(UI_COLOR_SECONDARY_HEX),
        UI_FONT_CHINESE_16,
        LV_TEXT_ALIGN_RIGHT,
        54);
    /* 创建 BLE 状态点。 */
    page->ble_dot = ui_lvgl_create_surface(page->root, 281, UI_BLE_DOT_Y_PX, 8, 8, UI_COLOR_MUTED_HEX, 4);
    /* 创建 BLE 连接状态文字。 */
    page->ble_label = ui_lvgl_create_label(
        page->root,
        lv_color_hex(UI_COLOR_MUTED_HEX),
        UI_FONT_CHINESE_16,
        LV_TEXT_ALIGN_RIGHT,
        82);
    /* 创建主信息卡。 */
    page->hero_card = ui_lvgl_create_surface(
        page->root,
        UI_SAFE_HORIZONTAL_PX,
        UI_HERO_CARD_Y_PX,
        UI_SAFE_CONTENT_WIDTH_PX,
        UI_HERO_CARD_HEIGHT_PX,
        UI_COLOR_CARD_HEX,
        28);
    /* 主信息卡增加一像素结构描边。 */
    if (page->hero_card != NULL) {
        /* 描边把卡片从石墨背景中分离，但不使用阴影。 */
        lv_obj_set_style_border_width(page->hero_card, 1, LV_PART_MAIN);
        /* 描边使用低亮度蓝灰。 */
        lv_obj_set_style_border_color(page->hero_card, lv_color_hex(UI_COLOR_BORDER_HEX), LV_PART_MAIN);
    }
    /* 创建状态胶囊；颜色在 render 中按训练、休息或故障状态更新。 */
    page->status_pill = ui_lvgl_create_surface(page->root, 50, UI_HERO_CARD_Y_PX + 18, 148, 32, UI_COLOR_RAISED_HEX, 16);
    /* 创建状态胶囊文字。 */
    page->status_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_CYAN_HEX), UI_FONT_CHINESE_16, LV_TEXT_ALIGN_LEFT, 132);
    /* 创建主文本。 */
    page->primary_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_TEXT_HEX), UI_FONT_CHINESE_36, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建次文本。 */
    page->secondary_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_SECONDARY_HEX), UI_FONT_CHINESE_28, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建底部提示。 */
    page->footer_label = ui_lvgl_create_label(page->root, lv_color_hex(UI_COLOR_MUTED_HEX), UI_FONT_CHINESE_16, LV_TEXT_ALIGN_CENTER, UI_SAFE_CONTENT_WIDTH_PX);
    /* 创建活动状态轨道；运行期填充长度表示当前是否允许推进完整周期。 */
    page->activity_track = ui_lvgl_create_surface(page->root, 52, UI_HERO_CARD_Y_PX + 228, 306, 5, UI_COLOR_BORDER_HEX, 3);
    /* 创建活动状态填充。 */
    page->activity_fill = ui_lvgl_create_surface(page->activity_track, 0, 0, 92, 5, UI_COLOR_ACCENT_HEX, 3);
    /* 任一品牌、状态、卡片或标签创建失败时报告内存不足。 */
    if ((page->brand_mark == NULL) || (page->brand_mark_label == NULL) ||
        (page->brand_label == NULL) || (page->title_label == NULL) ||
        (page->battery_shell == NULL) || (page->battery_fill == NULL) ||
        (page->battery_tip == NULL) || (page->battery_label == NULL) ||
        (page->ble_dot == NULL) || (page->ble_label == NULL) ||
        (page->hero_card == NULL) || (page->status_pill == NULL) ||
        (page->status_label == NULL) || (page->primary_label == NULL) ||
        (page->secondary_label == NULL) || (page->footer_label == NULL) ||
        (page->activity_track == NULL) || (page->activity_fill == NULL)) {
        return false;
    }
    /* 创建底部按钮行。 */
    page->button_row = lv_obj_create(page->root);
    /* 内存不足时失败。 */
    if (page->button_row == NULL) {
        return false;
    }
    /* 固定按钮区不接受拖动或惯性滚动，亮度按钮松手后必须仍停在原位置。 */
    lv_obj_remove_flag(page->button_row, LV_OBJ_FLAG_SCROLLABLE);
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
    /* 全部页面使用同一固定网格，避免异步 QSPI 局部刷新留下旧坐标文字。 */
    ui_lvgl_apply_product_layout(page, state);
    /* SCREEN_OFF 页隐藏文字，但保留不可见全屏按钮接收第一下触摸唤醒。 */
    if (state == UI_STATE_SCREEN_OFF) {
        /* 隐藏品牌标，避免 AMOLED 再次点亮前刷新旧像素。 */
        lv_obj_add_flag(page->brand_mark, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏品牌名称。 */
        lv_obj_add_flag(page->brand_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏产品副标题。 */
        lv_obj_add_flag(page->title_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏电池图标和百分比。 */
        lv_obj_add_flag(page->battery_shell, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏电池正极帽。 */
        lv_obj_add_flag(page->battery_tip, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏电量文字。 */
        lv_obj_add_flag(page->battery_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏 BLE 状态点。 */
        lv_obj_add_flag(page->ble_dot, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏 BLE 状态文字。 */
        lv_obj_add_flag(page->ble_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏主信息卡。 */
        lv_obj_add_flag(page->hero_card, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏状态胶囊。 */
        lv_obj_add_flag(page->status_pill, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏状态文字。 */
        lv_obj_add_flag(page->status_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏主信息。 */
        lv_obj_add_flag(page->primary_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏次信息。 */
        lv_obj_add_flag(page->secondary_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏页脚。 */
        lv_obj_add_flag(page->footer_label, LV_OBJ_FLAG_HIDDEN);
        /* 隐藏活动轨道。 */
        lv_obj_add_flag(page->activity_track, LV_OBJ_FLAG_HIDDEN);
        /* 让透明按钮行覆盖完整 410×502 逻辑屏幕。 */
        lv_obj_set_pos(page->button_row, 0, 0);
        /* 写入完整屏幕尺寸。 */
        lv_obj_set_size(page->button_row, UI_SCREEN_WIDTH_PX, 502);
        /* 取消透明容器内边距。 */
        lv_obj_set_style_pad_all(page->button_row, 0, LV_PART_MAIN);
        /* 让第一个按钮覆盖整屏，任意触摸位置都能产生 WAKE。 */
        lv_obj_set_size(page->buttons[0], UI_SCREEN_WIDTH_PX, 502);
        /* 隐藏唤醒按钮背景，避免短暂点屏时出现卡片。 */
        lv_obj_set_style_bg_opa(page->buttons[0], LV_OPA_TRANSP, LV_PART_MAIN);
        /* 禁用唤醒按钮圆角阴影的视觉成本。 */
        lv_obj_set_style_shadow_width(page->buttons[0], 0, LV_PART_MAIN);
    }
    /* 返回成功。 */
    return true;
}

/* 返回当前页面的语义强调色；颜色只表达状态，不改变业务状态机。 */
static uint32_t ui_lvgl_semantic_color(const ui_context_t *context)
{
    /* 错误和关机使用红色，明确当前不可继续训练。 */
    if ((context->state == UI_STATE_ERROR) || (context->state == UI_STATE_SHUTDOWN)) {
        /* 返回系统危险色。 */
        return UI_COLOR_DANGER_HEX;
    }
    /* 训练中活动门打开时使用健康绿色。 */
    if ((context->state == UI_STATE_RUNNING) && context->view.counting_enabled) {
        /* 返回正常计数色。 */
        return UI_COLOR_SUCCESS_HEX;
    }
    /* 自动休息、手动暂停和停止确认使用橙色。 */
    if (((context->state == UI_STATE_RUNNING) && !context->view.counting_enabled) ||
        (context->state == UI_STATE_PAUSED) ||
        (context->state == UI_STATE_STOP_CONFIRM)) {
        /* 返回暂停警示色。 */
        return UI_COLOR_WARNING_HEX;
    }
    /* 启动、识别、配对和诊断使用科技青色。 */
    if ((context->state == UI_STATE_BOOT) ||
        (context->state == UI_STATE_SELF_TEST) ||
        (context->state == UI_STATE_PREPARE) ||
        (context->state == UI_STATE_DIAGNOSTICS) ||
        context->view.pairing_active) {
        /* 返回信息状态色。 */
        return UI_COLOR_CYAN_HEX;
    }
    /* 总结完成使用健康绿色。 */
    if (context->state == UI_STATE_SUMMARY) {
        /* 返回完成色。 */
        return UI_COLOR_SUCCESS_HEX;
    }
    /* 主页和设置使用品牌蓝。 */
    return UI_COLOR_ACCENT_HEX;
}

/* 更新独立电池图标和 BLE 连接状态。 */
static void ui_lvgl_update_device_status(
    ui_lvgl_page_t *page,
    const ui_context_t *context)
{
    /* 电量未知时使用最小灰色填充并显示占位符。 */
    if (context->view.battery_percent == UINT8_MAX) {
        /* 图标保留 2 像素填充，明确它是未知而不是 0%。 */
        lv_obj_set_width(page->battery_fill, 2);
        /* 未知电量使用弱化色。 */
        lv_obj_set_style_bg_color(page->battery_fill, lv_color_hex(UI_COLOR_MUTED_HEX), LV_PART_MAIN);
        /* 百分比显示占位符。 */
        lv_label_set_text(page->battery_label, "--%");
    } else {
        /* 36 像素内宽按 0～100% 映射，至少保留 2 像素可见填充。 */
        int32_t fill_width = (int32_t)(((uint32_t)context->view.battery_percent * 36U + 99U) / 100U);
        /* 零电量仍保留 2 像素红色提示。 */
        if (fill_width < 2) {
            /* 写入最小图形宽度。 */
            fill_width = 2;
        }
        /* 更新真实填充宽度。 */
        lv_obj_set_width(page->battery_fill, fill_width);
        /* 充电时使用青色；否则按低电门槛选择绿色、橙色或红色。 */
        const uint32_t battery_color = context->view.charging
            ? UI_COLOR_CYAN_HEX
            : (context->view.battery_percent <= 15U
                ? UI_COLOR_DANGER_HEX
                : (context->view.battery_percent <= 30U
                    ? UI_COLOR_WARNING_HEX
                    : UI_COLOR_SUCCESS_HEX));
        /* 更新填充颜色。 */
        lv_obj_set_style_bg_color(page->battery_fill, lv_color_hex(battery_color), LV_PART_MAIN);
        /* 显示真实百分比；充电状态由填充颜色表达，避免顶栏文字拥挤。 */
        lv_label_set_text_fmt(page->battery_label, "%u%%", (unsigned int)context->view.battery_percent);
    }
    /* BLE 已连接时状态点和文字使用绿色。 */
    const uint32_t ble_color = context->view.ble_connected
        ? UI_COLOR_SUCCESS_HEX
        : UI_COLOR_MUTED_HEX;
    /* 更新状态点颜色。 */
    lv_obj_set_style_bg_color(page->ble_dot, lv_color_hex(ble_color), LV_PART_MAIN);
    /* 更新文字颜色。 */
    lv_obj_set_style_text_color(page->ble_label, lv_color_hex(ble_color), LV_PART_MAIN);
    /* 显示稳定中文连接事实。 */
    lv_label_set_text(page->ble_label, context->view.ble_connected ? "已连接" : "未连接");
}

/* 根据命令语义更新按钮视觉；命令本身仍由 presenter 决定。 */
static void ui_lvgl_style_button(
    lv_obj_t *button,
    lv_obj_t *label,
    ui_command_t command)
{
    /* 默认按钮使用抬升表面、冷灰描边和白字。 */
    uint32_t background = UI_COLOR_RAISED_HEX;
    /* 默认描边使用卡片边框色。 */
    uint32_t border = UI_COLOR_BORDER_HEX;
    /* 默认文字使用近白色。 */
    uint32_t text = UI_COLOR_TEXT_HEX;
    /* 开始、继续和完成属于主操作，常态直接使用品牌蓝。 */
    if ((command == UI_COMMAND_START) ||
        (command == UI_COMMAND_RESUME) ||
        (command == UI_COMMAND_DONE)) {
        /* 主操作填充品牌蓝。 */
        background = UI_COLOR_ACCENT_HEX;
        /* 主操作描边与填充一致。 */
        border = UI_COLOR_ACCENT_HEX;
    }
    /* 停止确认和关机属于破坏性操作，使用低亮红底和红色文字。 */
    if ((command == UI_COMMAND_CONFIRM_STOP) || (command == UI_COMMAND_SHUTDOWN)) {
        /* 红色混入深背景，避免整块高亮造成误触吸引。 */
        background = 0x281318U;
        /* 描边使用系统红色。 */
        border = UI_COLOR_DANGER_HEX;
        /* 文字使用系统红色。 */
        text = UI_COLOR_DANGER_HEX;
    }
    /* 写入常态背景。 */
    lv_obj_set_style_bg_color(button, lv_color_hex(background), LV_PART_MAIN);
    /* 写入一像素边框。 */
    lv_obj_set_style_border_width(button, 1, LV_PART_MAIN);
    /* 写入边框颜色。 */
    lv_obj_set_style_border_color(button, lv_color_hex(border), LV_PART_MAIN);
    /* 按下状态统一使用青色，提供即时触摸反馈。 */
    lv_obj_set_style_bg_color(button, lv_color_hex(UI_COLOR_CYAN_HEX), LV_PART_MAIN | LV_STATE_PRESSED);
    /* 更新按钮文字颜色。 */
    lv_obj_set_style_text_color(label, lv_color_hex(text), LV_PART_MAIN);
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
    /* 顶栏由真实电池与 BLE 字段独立渲染，不再依赖拼接字符串。 */
    ui_lvgl_update_device_status(page, context);
    /* 页面标题显示在主卡状态胶囊中。 */
    lv_label_set_text(page->status_label, model->title);
    /* 更新主文本。 */
    lv_label_set_text(page->primary_label, model->primary);
    /* 更新次文本。 */
    lv_label_set_text(page->secondary_label, model->secondary);
    /* 更新页脚。 */
    lv_label_set_text(page->footer_label, model->footer);
    /* 获取当前页面语义色。 */
    const uint32_t semantic_color = ui_lvgl_semantic_color(context);
    /* 状态胶囊使用深色表面和语义色描边。 */
    lv_obj_set_style_bg_color(page->status_pill, lv_color_hex(UI_COLOR_RAISED_HEX), LV_PART_MAIN);
    /* 状态胶囊使用一像素语义色描边。 */
    lv_obj_set_style_border_width(page->status_pill, 1, LV_PART_MAIN);
    /* 写入状态描边颜色。 */
    lv_obj_set_style_border_color(page->status_pill, lv_color_hex(semantic_color), LV_PART_MAIN);
    /* 状态文字使用同一语义色。 */
    lv_obj_set_style_text_color(page->status_label, lv_color_hex(semantic_color), LV_PART_MAIN);
    /* 活动轨道填充使用语义色。 */
    lv_obj_set_style_bg_color(page->activity_fill, lv_color_hex(semantic_color), LV_PART_MAIN);
    /* 活动门打开时轨道全长，休息时缩短但累计和动作不变。 */
    const int32_t activity_width =
        ((context->state == UI_STATE_RUNNING) && context->view.counting_enabled)
            ? 306
            : ((context->state == UI_STATE_PREPARE)
                ? 204
                : (((context->state == UI_STATE_PAUSED) ||
                    ((context->state == UI_STATE_RUNNING) && !context->view.counting_enabled))
                    ? 72
                    : 122));
    /* 更新状态轨道长度，不创建动画定时器。 */
    lv_obj_set_width(page->activity_fill, activity_width);
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
            /* 根据命令语义应用主操作、普通或破坏性样式。 */
            ui_lvgl_style_button(
                page->buttons[index],
                page->button_labels[index],
                model->buttons[index].command);
            /* 同步禁用状态。 */
            if (model->buttons[index].enabled) {
                /* 有效命令允许点击。 */
                lv_obj_remove_state(page->buttons[index], LV_STATE_DISABLED);
            } else {
                /* 无效命令显示禁用态。 */
                lv_obj_add_state(page->buttons[index], LV_STATE_DISABLED);
            }
    }
    /* 页面切换直接加载固定 screen，避免多轮训练积累 screen 动画生命周期。 */
    if (implementation->active_state != context->state) {
        /* 厂家 QSPI flush 为异步链；整页切换始终使用立即加载以保证触摸对象立即归属新 screen。 */
        lv_screen_load(page->root);
        /* 产品动画只保留 BOOT/SHUTDOWN 的局部文字反馈，不再动画加载整张 screen。 */
        if (renderer->animations_enabled) {
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
