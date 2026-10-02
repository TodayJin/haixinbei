/**
 * @file  oled_diag.h
 * @brief OLED 通路诊断（只在 APP_OLED_DIAG 构建里被调用）
 *
 * 2026-10-03：以前 main_app.c 是在函数体里写 `extern void oled_diag_run(void);`
 * 来声明的，oled_diag.c 自己反而看不到原型（编译告警 -Wmissing-prototypes，
 * 而且形参/返回值写错编译器不会拦）。收到一个头文件里，两边都包含它。
 */
#ifndef APP_OLED_DIAG_H
#define APP_OLED_DIAG_H

/* 四组合轮播，内部是死循环，正常不返回。 */
void oled_diag_run(void);

#endif /* APP_OLED_DIAG_H */
