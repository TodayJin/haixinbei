/**
 * @file  app_ui.h
 * @brief KK_UI 应用层：首页 / 菜单 / 自定义页 / 输入归一化
 *
 * 分工：
 *   - KK_UI 负责导航、交互、动画、整屏刷新调度（应用不再自己清屏/提交）
 *   - 本文件负责页面拓扑、变量绑定、业务事件和自定义页绘制
 */
#ifndef APP_UI_H
#define APP_UI_H

#include "kk_ui.h"
#include <stdbool.h>
#include <stdint.h>

/* ---- 页面 ID（1 起，routes[] 用 page-1 索引）---- */
#define PAGE_HOME            1
#define PAGE_MENU_SETTINGS   2
#define PAGE_INFO_ABOUT      3
#define PAGE_CUSTOM_TRACE    4
#define PAGE_CUSTOM_TASK_A   5
#define PAGE_CUSTOM_TASK_B   6
#define PAGE_CUSTOM_TEST     7
#define PAGE_MENU_TASK       8
#define PAGE_CUSTOM_GYRO     9
#define PAGE_SPLASH         10
#define PAGE_MENU_PID       11   /* PID 参数：二级菜单入口（速度环 / 角度环 / 循迹环） */
#define PAGE_MENU_PID_SPEED 12   /* 速度环参数：控制电机转速 */
#define PAGE_MENU_PID_ANGLE 13   /* 角度环参数：控制偏航角 */
#define PAGE_MENU_PID_TRACE 14   /* 循迹环参数：灰度偏差 -> 目标角度 */
#define PAGE_CUSTOM_POWER   15   /* 电流检测：INA226 测的电压 / 电流 / 功率 */
#define PAGE_CUSTOM_MOTOR   16   /* 电机测试：左右轮各定方向，退出后继续转（用户 m01540） */
#define PAGE_CUSTOM_SERVO   17   /* 舵机测试：两路舵机各定角度，退出后保持（用户 m13059） */
#define PAGE_COUNT          17

/* ---- 自定义页在 custom 数组里的下标 ---- */
#define CUSTOM_TRACE   0
#define CUSTOM_TASK_A  1
#define CUSTOM_TASK_B  2
#define CUSTOM_TEST    3
#define CUSTOM_GYRO    4
#define CUSTOM_SPLASH  5
#define CUSTOM_POWER   6
#define CUSTOM_MOTOR   7
#define CUSTOM_SERVO   8
#define CUSTOM_COUNT   9

/* ---- 业务事件 ---- */
#define EV_NONE        0
#define EV_PID_CHANGED 1     /* 控制参数（PID/循迹基准速度）被改：app_ui_tick 里 push 到 host_link 参数表 */

/* ---- 界面可见的实时数据（上层往这里写，UI 只读）---- */
typedef struct {
    uint8_t  trace[8];       /* 8 路循迹电平，1=检测到黑线 */
    uint8_t  gray[8];        /* 8 路灰度原始值 0..255，0=全黑 255=全白（温度计条用） */
    int32_t  yaw_x10;        /* 偏航角 ×10（0.1 度） */
    int32_t  pitch_x10;      /* 俯仰角 ×10 */
    int32_t  roll_x10;       /* 横滚角 ×10 */
    uint8_t  task_state;     /* 0=待机 1=运行 */
    uint8_t  gyro_ok;        /* 陀螺仪是否有数据 */
    uint32_t sysclk_hz;      /* 供 STATUS 页显示 */
} app_live_t;

extern app_live_t g_live;

/* ---- 可编辑设置（绑定到菜单的整数/开关编辑）---- */
typedef struct {
    int32_t trace_speed;    /* 循迹基础速度（PWM 刻度 0..7200） */

    /* ---- 三层串级（用户定的结构）：
     *      循迹环  pid.c          8 路灰度位置误差 -> 目标角度
     *      角度环  gyro_control.c 目标角度         -> 左右轮目标速度
     *      速度环  **待写**        目标速度(编码器反馈) -> PWM
     *    *_x100 字段是定点刻度：屏幕按 decimals=2 显示成两位小数，
     *    下发参数表时 ÷100；*_ilim 是原值（整数）。 ---- */

    /* 速度环（HP_SPEED_*）：PI 增益 + 前馈。
     * 前馈是这台电机的硬件标定量（稳态 PWM ≈ slope×目标 + fric），
     * 换电机/减速比/电池电压必须重标，标定方法见 speed_control.c 的注释。 */
    int32_t speed_kp_x100;
    int32_t speed_ki_x100;
    int32_t speed_kd_x100;
    int32_t speed_ilim;
    int32_t speed_ff_slope_x1000;   /* 前馈斜率 ×1000（0.662 -> 662，屏幕显示三位小数） */
    int32_t speed_ff_fric;          /* 前馈摩擦项（PWM 常量，2327） */

    /* 角度环（HP_GYRO_*） */
    int32_t gyro_kp_x100;
    int32_t gyro_ki_x100;
    int32_t gyro_kd_x100;
    int32_t gyro_ilim;

    /* 循迹环（HP_LINE_*） */
    int32_t line_kp_x100;
    int32_t line_ki_x100;
    int32_t line_kd_x100;
    int32_t line_ilim;

    bool    use_gyro;       /* 是否启用陀螺仪闭环 */
    bool    beep_key;       /* 按键时响一下 */
    bool    beep_marker;    /* 任务每过一个点响一下 */
    bool    trace_invert;   /* 循迹黑白反转 */
    bool    trace_adapt;    /* 「自动判线」：单帧自适应（关掉就走固定中点阈值） */
    int32_t gray_mode;      /* 灰度模块通讯方式 TRACE_MODE_xxx（自动/I2C/串口/数字） */
    int32_t trace_cal_set;  /* 用第几套标定：0=普通 1=亚克力（用户 m07888） */
} app_settings_t;

extern app_settings_t g_cfg;

/* ---- 安装方向 ----
 * 屏幕按 180° 装在机器人上时置 1：显示整体翻转，**五向键的物理方向也跟着翻**
 * （上<->下、左<->右），这样用户那一侧看到的上下和菜单里的上下才一致。 */
#define APP_UI_DISPLAY_ROT180   1

/* ---- 生命周期 ---- */
void app_ui_setup_display(void);            /* 紧跟 OLED_Init() 之后调用，设置安装方向 */
void app_ui_init(void);                     /* KK_UI_Init */
void app_ui_tick(uint32_t now_ms);          /* 主循环每 5~10ms 调一次 */
void app_ui_test_pattern(void);             /* 最小可见自检：直接画一屏 */

/* ---- 供 Flash 存档（nvram.c）用的窗口 ----
 * app_ui_settings_loaded() 在 nvram 把 g_cfg 盖回来之后调用，负责把新值
 * 同步到运行时（灰度通道 / PID 参数表 / 重画）。 */
uint8_t app_ui_gyro_axis_mode(void);         /* 陀螺仪方框的轴映射档 0..7（屏上显示 M1..M8） */
void app_ui_set_gyro_axis_mode(uint8_t mode);
void app_ui_settings_loaded(void);

#endif /* APP_UI_H */
