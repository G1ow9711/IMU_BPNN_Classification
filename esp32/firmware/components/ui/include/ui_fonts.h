#ifndef UI_FONTS_H
#define UI_FONTS_H

/* 引入 lv_font_t 与 LV_FONT_DECLARE；三个符号由 fonts 目录中的自动生成 C 文件定义。 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 声明 16 像素 Noto Sans SC 中文子集；用于状态栏、页脚和触摸按钮。 */
LV_FONT_DECLARE(ui_font_noto_sans_sc_16);
/* 声明 20 像素 Noto Sans SC 中文子集；用于页面标题和次级指标。 */
LV_FONT_DECLARE(ui_font_noto_sans_sc_20);
/* 声明 28 像素 Noto Sans SC 中文子集；用于动作名、倒计时和主要数值。 */
LV_FONT_DECLARE(ui_font_noto_sans_sc_28);
/* 36 像素中文字体用于主动作名、倒计时和关键总结数值，提升 2.06 英寸屏幕可读性。 */
LV_FONT_DECLARE(ui_font_noto_sans_sc_36);

#ifdef __cplusplus
}
#endif

#endif
