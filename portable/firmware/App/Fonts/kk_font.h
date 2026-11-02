/**
 * @file  kk_font.h
 * @brief KK_OLED (u8g2 格式) 字模声明
 *
 * 由 LEDFont 在线 API (https://ledfont.botelvdong.com) 按 KK_OLED 的
 * kk-oled-font 流程生成本项目实际会显示的字符子集；字模版权归生成服务，
 * 不随 KK_OLED 的 MIT 许可。
 *
 *   kk_font_body : wenquanyi_9pt  12px —— 标题 + 正文 + 菜单项
 *   kk_font_home : wenquanyi_11pt 15px —— 首页大标签
 *
 * 需要增删字符时重新生成，不要手改数组内容。
 */
#ifndef KK_FONT_H
#define KK_FONT_H

#include <stdint.h>

extern const uint8_t kk_font_body[];   /* 12px，572 字形（ASCII + firmware/App 与 kk_ui 里
                                        * 出现过的全部汉字 —— 见 memory/PROJECT.md
                                        * 「字模查字与重生成」：子集外的字静默不画） */
extern const uint8_t kk_font_home[];   /* 15px，110 字形（ASCII + 首页大标签） */

#endif /* KK_FONT_H */
