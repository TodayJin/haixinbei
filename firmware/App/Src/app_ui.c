/**
 * @file  app_ui.c
 * @brief KK_UI 应用层实现
 */
#include "app_ui.h"
#include "board.h"
#include "disp_heal.h"     /* 屏幕故障自愈的决策逻辑（纯逻辑，主机侧 t_dispheal.c 单测） */
#include "kk_font.h"
#include "kk_icons.h"
#include "kk_oled.h"
#include "motor.h"
#include "servo.h"         /* 舵机测试页：两路舵机角度直给 Servo_SetAngle() */
#include "bsp_board.h"     /* MOTOR_PWM_MAX：电机测试页的速度档 → 占空比换算 */
#include "speed_control.h" /* 电机测试页：读 20ms 窗口计数 + 把滑条写成速度环目标（闭环） */
#include "host_link.h"
#include "tracing.h"      /* trace_invert：界面「循迹反转」同步给循迹底层 */
#include "task_run.h"     /* 任务页：读解释器真状态（在跑哪个任务的第几块）+ 中键停机 */
#include "task_store.h"   /* 任务页：数一数车上 Flash 里存了几个任务 */
#include "trace_cal.h"    /* 循迹标定向导 + 模拟量柱高/阈值线 */
#include "nvram.h"        /* 设置存 Flash：锁按键 + "正在保存/保存成功"提示 */

#include <stdio.h>
#include <string.h>

app_live_t     g_live;
/* 默认值与 host_link 参数表（HP_*）的默认值一致，
 * 开机 pull_pid_params() 会再按参数表刷一遍。 */
app_settings_t g_cfg = {
    /* 循迹速度：旧板 `motor_tracing_pid()` 里写死 1000，但旧板 PWM 满量程就是 1000（=满油门）。
     * 本板满量程 7200 ⇒ 等价刻度 = 1000×7.2 = 7200；取 6480（= 旧 900，90%）留一点余量，
     * 让循迹环的差速能往 7200 顶一顶。不能照抄 1000（13.9% 占空比，低于静摩擦死区 2500~2900）。 */
    .trace_speed  = 6480,
    /* 速度环（HP_SPEED_*）：新板独有（旧板没有编码器闭环），量纲是 PWM/(计数/5ms)，不参与 ×7.2 */
    .speed_kp_x100 = 2000,   /* HP_SPEED_KP           20.0 */
    .speed_ki_x100 = 200,    /* HP_SPEED_KI            2.0 */
    .speed_kd_x100 = 0,      /* HP_SPEED_KD            0.0 */
    .speed_ilim    = 3000,   /* HP_SPEED_ILIMIT     3000   */
    /* 前馈：2026-09-30 台架实测拟合，稳态 PWM ≈ 0.662×目标 + 2327。
     * 高速精度主要靠它，PI 只修残差（目标 5000 时 PI 稳态贡献≈0）。 */
    .speed_ff_slope_x1000 = 662,   /* HP_SPEED_FF_SLOPE 0.662 */
    .speed_ff_fric        = 2327,  /* HP_SPEED_FF_FRIC  2327  */
    /* 循迹环增益：旧板「误差 → 加在左右轮上的 PWM 偏置」整条链都在旧刻度（满量程 1000），
     * 本板满量程 7200 ⇒ 照抄的增益在新板上只有 1/7.2 的转向力度，一律 ×7.2 换算
     * （2026-09-30 m02112 同步，物理等价：旧 375 偏置 = 满量程 37.5%）。
     * **积分限幅不换算**：它的量纲是「误差」本身（循迹=灰度重心、角度=度），不是 PWM。 */
    .line_kp_x100 = 270000,  /* HP_LINE_KP      2700.0 = 旧 375.0 ×7.2 */
    .line_ki_x100 = 1080,    /* HP_LINE_KI        10.8 = 旧   1.5 ×7.2 */
    .line_kd_x100 = 36000,   /* HP_LINE_KD       360.0 = 旧  50.0 ×7.2 */
    .line_ilim    = 50,      /* HP_LINE_ILIMIT   50（误差单位，不换算） */
    /* 角度环增益（HP_GYRO_*）：**不能**沿用「旧值 ×7.2」。2026-10-01 原地台架实测
     * （out/_angletest.ps1，只改 RAM 不烧录）：
     *   - 旧刻度换算值 720/72 在本板必自激：Δ=3°..20° 全振荡，尾晃 20~38°、
     *     峰值 400~600°/s、原地来回甩。原因是角度环输出直接是「轮速目标」，
     *     plant 增益 k_v≈0.06 (°/s)/PWM ⇒ 环路增益 Kp·k_v = 43 /s，远超
     *     1/有效滞后（20ms 滑动平均 + 速度环 ramp ≈ 30~50ms）。
     *   - Kp=150 / Kd=15（≈Kp/10）稳定：3° 阶跃 0.44s 到位、误差 0.6°、尾晃 0.04°；
     *     20° 过冲 4~10°、稳态误差 <1.3°；90° 分六次 15° 增量转、累计误差 1.1°。
     * 改这里必须同步 host_link.c 的 SPECS[]、本文件 pull_pid_params() 的兜底值与
     * 下面 CFG_GYRO_*_OLD_X100 的迁移判断（三处一致）。 */
    .gyro_kp_x100 = 15000,   /* HP_GYRO_KP       150.0（实测值，非 ×7.2） */
    .gyro_ki_x100 = 0,       /* HP_GYRO_KI         0.0（默认不用积分：积分会被死区"喂"成常驻推力，车停在带外抽） */
    .gyro_kd_x100 = 1500,    /* HP_GYRO_KD        15.0（≈Kp/10，压过冲） */
    .gyro_ilim    = 1000,    /* HP_GYRO_ILIMIT 1000（误差×秒单位，不换算） */
    .use_gyro     = true,
    .beep_key     = false,
    .beep_marker  = true,
    .trace_invert = false,
    .trace_adapt  = true,    /* 「自动判线」默认开（用户 m07030 要的单帧自适应） */
    .gray_mode    = TRACE_MODE_AUTO,
    .trace_cal_set = 0       /* 默认用第 0 套（普通路面） */
};

/* =====================================================================
 * 电机测试页（PAGE_CUSTOM_MOTOR）的状态
 *   模块测试 -> 「电机」进这一页：左右两路各有一个**带符号的速度档**
 *   s_motor_level[2]（-100..+100，正 = 朝车头，100 = 正满速），
 *   **退出后继续转**（所以速度档存在 static 里跨页保留，原来那句
 *   "离开就 Load(0,0)"已删掉）。
 *   操作（用户 m02868 定的版式）：
 *     上/下 = 换选中项（左 / 右 / 清零 / 返回）
 *     左/右 = 在左/右这两行上拖速度滑条：短按一步 1，长按越按越快；
 *             选中「返回」/「清零」时（同在标题行）左/右在两项间切换（用户 m03156）
 *     中键  = 在「清零」上两路一起归零（并把速度环停掉），在「返回」上退出页面
 *   屏上两个数字：右边大的是**设定值**（-100..+100），左边小的是**实测**
 *   编码器速度（SpeedControl_WindowCounts()，最近 20ms 计数，满速 ≈100，
 *   倒转为负）—— 用户 m01975 问过的"读数怎么才 40"就是它。
 *   **闭环（用户 m03300 定）**：设定值直接当**速度环目标**（100 = 目标满速），
 *   不再是"开环占空比"。所以两个数字同刻度，实测会追上设定（稳态差几格
 *   正常；档位 <21 即目标 <1500 计数刻度掉进静摩擦粘滑区，追不上）。
 * ===================================================================== */
#define MOTOR_TEST_N      4     /* 选中项个数：左 / 右 / 清零 / 返回 */
#define MOTOR_SEL_LEFT    0U
#define MOTOR_SEL_RIGHT   1U
#define MOTOR_SEL_CLEAR   2U
#define MOTOR_SEL_BACK    3U

#define MOTOR_LEVEL_MAX   100   /* 滑条量程 ±100 = 速度环目标 ±MOTOR_PWM_MAX
                                 * （target 7200 ↔ 满速 4950 计数/s ↔ 屏上 100）。
                                 * 闭环能追上的下限是目标 1500（档位 21），再低就是
                                 * 静摩擦粘滑区：左轮死区 2500~2900、右轮 3400~3550。 */

/* 本页要四个方向（上/下选、左/右拖），而 kk_ui 只有三键（左/右被折成 上/下），
 * 所以这一页不吃 kk_ui 的 UP/DOWN 事件，直接读 keys_raw() 自己做消抖 + 长按连发。
 * 位序与 board.h 的 keys_raw() 一致：bit0上 bit1下 bit2左 bit3右 bit4中。 */
#define MOTOR_KEY_UP      0
#define MOTOR_KEY_DOWN    1
#define MOTOR_KEY_LEFT    2
#define MOTOR_KEY_RIGHT   3
#define MOTOR_KEY_OK      4
#define MOTOR_KEY_N       5

#define MOTOR_KEY_DEBOUNCE_MS   20U   /* 与 key.c 的 4×5ms 消抖同量级 */
#define MOTOR_KEY_FIRST_MS     600U   /* 按住多久开始连发（= 第 1 档加速点） */
#define MOTOR_KEY_REPEAT_MS    120U   /* 连发间隔 */
/* 连发的步进随按住时长变大 —— 用户 m02868："长按的话也会越来越快" */
#define MOTOR_STEP_2_MS       1200U
#define MOTOR_STEP_5_MS       2000U
#define MOTOR_STEP_10_MS      3000U

static uint8_t  s_motor_sel;        /* 当前选中项：0=左 1=右 2=清零 3=返回 */
static int8_t   s_motor_level[2];   /* [0]=左 [1]=右 的速度档 -100..+100 */
static int      s_motor_show[2];    /* 屏上正显示的实测编码器窗口计数（变了才重画） */
static uint32_t s_motor_last;       /* 上次刷实测读数的时刻（200ms 一次） */

static uint8_t  s_mkey_raw;                     /* 上一拍原始位图 */
static uint8_t  s_mkey_stable;                  /* 已消抖的稳定位图 */
static uint32_t s_mkey_changed;                 /* 原始位图最后一次变化的时刻 */
static uint32_t s_mkey_hold_at[MOTOR_KEY_N];    /* 各键按下时刻（长按加速用） */
static uint32_t s_mkey_repeat_at[MOTOR_KEY_N];  /* 各键下一次连发时刻 */

/* 舵机测试页（PAGE_CUSTOM_SERVO）的状态 —— 放在这里是因为模块测试页的
 * 行尾要显示这两个角度（"舵机"那行的 "60/90"），定义必须早于它。
 * 页面主体（按键机/绘制）在下面"舵机测试页"那一整块里。 */
static int16_t  s_servo_deg[2];         /* [0]=舵机1 [1]=舵机2 的角度档 0..180（跨页保留） */

/* 把两个速度档写成**速度环目标**（闭环，用户 m03300 定）：±100 -> ±MOTOR_PWM_MAX，
 * 由 speed_control.c 每 5ms 闭环算 PWM（软启动斜坡 20000 PWM/s 照旧，所以档位跳变
 * 不会猛冲）。**不再自己调 Load()**：环使能时 Load() 每 5ms 会被它盖掉，两边打架。
 * 两路都是 0 时直接 SpeedControl_Stop()：一是真停（Load(0,0)），二是清掉积分
 * ——带着热积分把目标降到 0，环会拿 ki·∫err（限幅后最多 ±6000 PWM）反向踹一脚。
 * 离开页面不停电机：环保持使能，继续以这个目标跑（要看电流就靠这个）。 */
static void motor_test_apply(void)
{
    int tgt[2];
    int i;

    for (i = 0; i < 2; i++) {
        tgt[i] = (int)s_motor_level[i] * MOTOR_PWM_MAX / MOTOR_LEVEL_MAX;
    }
    if (tgt[0] == 0 && tgt[1] == 0) {
        SpeedControl_Stop();
        return;
    }
    SpeedControl_Enable(1U);
    SpeedControl_SetTarget(tgt[0], tgt[1]);
}

/* 陀螺仪页三维方框的轴映射，中键在陀螺仪页循环切换（屏幕右上角显示 M1..M8）。
 * 之所以做成运行时可切：模型到底该吃哪一组角只取决于"传感器在车上怎么装的"，
 * 光看代码推不出来，只能让用户对着真车逐个试；试出来之后把默认值改成那一档。
 *
 * 记号：SP = g_live.pitch_x10、SR = g_live.roll_x10。画面横滚 = r、画面俯仰 = p
 * （渲染语义：R=Rz(y)Ry(p)Rx(r) 且投影只取旋转后的 X/Y（俯视））。
 *
 * **必须 8 档，不能只有 4 档**（用户 m02505："M1 到 M4 应该是不能列出所有情况吧"）：
 * 两路取反互相独立，4 档只能覆盖"两轴符号相同"的那 4 种，缺"一轴取反另一轴不取反"。
 * 位定义（= 档号-1）：
 *   bit0 = 交换轴：0 -> 画面横滚取 SP、俯仰取 SR；1 -> 两者对调
 *   bit1 = 画面横滚取反
 *   bit2 = 画面俯仰取反
 *   M1(m0) 不换 +/+    r=+SP p=+SR      M5(m4) 不换 +/-    r=+SP p=-SR
 *   M2(m1) 换轴 +/+    r=+SR p=+SP      M6(m5) 换轴 +/-    r=+SR p=-SP
 *   M3(m2) 不换 -/+ ★  r=-SP p=+SR      M7(m6) 不换 -/-    r=-SP p=-SR
 *   M4(m3) 换轴 -/+    r=-SR p=+SP      M8(m7) 换轴 -/-    r=-SR p=-SP
 *
 * 默认档 = M3（bit1，横滚取反、俯仰不取反）= 用户 m02505 实测的"把 M1 的俯仰和
 * M2 的横滚结合起来"那一组：旧 M1 的俯仰对（p=+SR）、旧 M2 的横滚对（r=-SP）。
 * 更早的两条记录（m02306 前默认旧 M1、m02820 用户说旧 M3 对）都是在只有 4 档的
 * 表里挑的，本组合当时不存在，所以那两次"确认"都只是最接近的一档，不是对的。 */
static uint8_t s_gyro_axis_mode = 2U;
static uint8_t s_nv_last;           /* 上一次看到的 Flash 保存状态（用于弹提示） */
static uint8_t s_nv_lock;           /* 1 = "正在保存"已真的画上屏，按键才锁 */
static uint8_t s_adapt_applied = 0xFFU;  /* 「自动判线」上一次推给 trace_cal 的值（0xFF = 还没推） */

/* ---- 屏幕故障自愈（用户 m26333：屏幕有时候特别卡、帧率特别低）----
 * KK_UI 只要 latch 一次显示错误（OLED 传输报错，或者一次传输拖过
 * KK_UI_DISPLAY_TIMEOUT_MS=250ms），kk_ui.c:731 之后每一拍都直接 return：
 * 不画、不收按键，看着就是"卡住了"，只能断电。而它的恢复入口
 * KK_UI_RecoverDisplay() 在整个工程里没有任何调用者（死代码）。
 * 什么时候该去自愈由 App/Svc/disp_heal.c 决定（纯逻辑、主机侧有 t_dispheal.c）；
 * 这里只负责把动作做出来：调 KK_UI_RecoverDisplay()，必要时先叫一次
 * board_i2c_bus_recover()（它会把速率与 DMA 一起复位），免得
 * "屏已经能画了但总线还歪着"时无限打转。自愈次数进 1Hz RTT 诊断行。 */
static disp_heal_t s_disp_heal;

/* KK_UI 过渡动画的水平位移，由 KK_UI_CustomOnDraw 每帧设定。
 * 内置页都是"内容坐标 += x_offset + 窗口按 clip 裁"，自定义页原来把
 * x_offset 丢掉、只让裁剪边在扫，于是标题看着"偏一下"。 */
static int16_t s_dx;
/* KK_UI 这一帧给本页的裁剪窗口（KK_UI_CustomOnDraw 记下来）。
 * 页面内部要临时改窗口时必须跟它求交集、画完再原样恢复，
 * 否则会把框架的翻页窗口顶掉（kk_oled 的 SetClipWindow 是替换不是求交）。 */
static int16_t s_clip_x;
static uint16_t s_clip_w = 128U;

/* 这里曾经有过"表盘停一拍"（SPLASH_DWELL_MS 450）——被用户 m02919 否掉：
 * 长按本来一次比一次快，加了停顿时长就变成"卡住一下然后重新加速"。
 * 表盘不需要额外停顿：翻页动画本身 220ms 期间输入就被框架屏蔽
 * （kk_ui.c:596），重复事件再快也推不走它。 */

/* 蜂鸣器非阻塞计时：beep_ms() 只负责"开响 + 记截止时刻"，到点由
 * app_ui_tick() 里的 beep_poll() 关掉。按键蜂鸣是从 KK_UI 的输入处理里
 * 回调的，若在那里 HAL_Delay() 阻塞 30ms 就会卡住主循环。 */
static uint32_t s_beep_until;

/* 导航 API（KK_UI_NavigateTo / NavigateToDirected / NavigateBack /
 * NavigateBackDirected / KK_UI_SetEntrySelection）和 KK_UI_HomeOnMove 钩子
 * 都在 kk_ui.h 里声明了，应用侧不用再自己 extern（审计 agent 4：原来这 5 个
 * 只有内部头有声明，改签名应用侧不会报错）。
 * Directed 版多一个 reverse：动画方向跟着按键走——按左进来的页从左边
 * 滑入(reverse=true)，按右进来的从右边滑入。
 * KK_UI_SetEntrySelection 用来指定"下一次进某页"的初始选中项：表盘的右邻是
 * 第一项"任务"，左邻是最后一项"设置"。 */

/* =====================================================================
 * 页面拓扑
 * ===================================================================== */
static const KK_UI_PageRoute k_routes[PAGE_COUNT] = {
    [PAGE_HOME - 1]          = { KK_UI_PAGE_HOME,   0 },
    [PAGE_MENU_SETTINGS - 1] = { KK_UI_PAGE_MENU,   0 },
    [PAGE_INFO_ABOUT - 1]    = { KK_UI_PAGE_INFO,   0 },
    [PAGE_CUSTOM_TRACE - 1]  = { KK_UI_PAGE_CUSTOM, CUSTOM_TRACE },
    [PAGE_CUSTOM_TASK - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_TASK },
    [PAGE_CUSTOM_TEST - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_TEST },
    [PAGE_CUSTOM_GYRO - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_GYRO },
    [PAGE_SPLASH - 1]        = { KK_UI_PAGE_CUSTOM, CUSTOM_SPLASH },
    [PAGE_MENU_PID - 1]       = { KK_UI_PAGE_MENU,  1 },
    [PAGE_MENU_PID_SPEED - 1] = { KK_UI_PAGE_MENU,  2 },
    [PAGE_MENU_PID_ANGLE - 1] = { KK_UI_PAGE_MENU,  3 },
    [PAGE_MENU_PID_TRACE - 1] = { KK_UI_PAGE_MENU,  4 },
    [PAGE_CUSTOM_POWER - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_POWER },
    [PAGE_CUSTOM_MOTOR - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_MOTOR },
    [PAGE_CUSTOM_SERVO - 1]   = { KK_UI_PAGE_CUSTOM, CUSTOM_SERVO }
};

/* 数组顺序就是环上的顺序：从表盘往右依次是
 *   任务 / PID / 循迹测试 / 模块测试 / 陀螺仪 / 电流 / 设置，
 * 于是从表盘往左第一格是"设置"、第二格是"电流"
 * （用户要求：设置放在陀螺仪后面；PID 放在任务后面）。 */
static const KK_UI_HomeItem k_home_items[] = {
    { "任务",     kk_icon_task_a,   PAGE_CUSTOM_TASK },
    { "PID",      kk_icon_pid,      PAGE_MENU_PID },
    { "循迹测试", kk_icon_track,    PAGE_CUSTOM_TRACE },
    { "模块测试", kk_icon_test,     PAGE_CUSTOM_TEST },
    { "陀螺仪",   kk_icon_gyro,     PAGE_CUSTOM_GYRO },
    { "电流",     kk_icon_power,    PAGE_CUSTOM_POWER },
    { "设置",     kk_icon_settings, PAGE_MENU_SETTINGS }
};
/* 计数一律从数组推导：KK_UI 不校验 item_count 和数组真实长度，硬编码写错
 * 会多画一行并读到越界的字符串。 */
#define APP_UI_ITEM_COUNT(arr) ((uint16_t)(sizeof(arr) / sizeof((arr)[0])))
static const KK_UI_HomePage k_home_pages[] = {
    { k_home_items, APP_UI_ITEM_COUNT(k_home_items) }
};

/* 智能手表式的两屏环：[表盘 PAGE_SPLASH] 既在图标菜单最左项之前、
 * 也在最右项之后。图标菜单内部**不回绕**，所以走到"缝"上就滑回表盘：
 *   最左项继续往左 -> 表盘从左边滑进来（回退动画）
 *   最右项继续往右 -> 表盘从右边滑进来（前进动画）
 * 都用弹出（不增长导航栈，避免 KK_UI_NAV_DEPTH=8 撑满后 reject_flash 白框）。
 * 返回 true 表示这次移动被应用接管。 */
bool KK_UI_HomeOnMove(int16_t direction, uint16_t selected,
                       KK_UI_InputSource source)
{
    KK_UI_Status st;
    uint16_t count = (uint16_t)(sizeof(k_home_items) / sizeof(k_home_items[0]));

    /* 长按产生的重复事件走同一条路（不再像 m02517 那样直接交还框架在环内
     * 回绕）：用户 m02820——"长按切换菜单经过主页会滤过去、不显示动画和
     * 页面"——因为回绕把表盘整屏跳掉了。现在无论按下还是长按，走到"缝"上
     * 都真的滑回表盘，于是长按的环是 表盘->任务->…->设置->表盘->… 连续的
     * 7 屏，每一屏都有完整的翻页动画。
     * 注意 source（PRESS/REPEAT/ENCODER）与 action（UP/DOWN/OK）不是一回事。 */
    (void)source;

    if (direction < 0) {
        if (selected != 0U) return false;          /* 环内：交给框架滑 */
        st = KK_UI_NavigateBack(HAL_GetTick());    /* 最左项继续往左 -> 表盘从左边滑入 */
    } else {
        if ((uint16_t)(selected + 1U) < count) return false;
        st = KK_UI_NavigateBackDirected(false, HAL_GetTick()); /* 最右项继续往右 -> 从右边滑入 */
    }
    /* OK/BUSY 都吃掉这次的移动：BUSY 是"正在翻页"，交还框架的话它会在环内
     * 就地回绕，动画方向就跟按键对不上了。只有栈空（正常不会发生：表盘永远
     * 是菜单下面那一层）才交还框架兜底。 */
    return st != KK_UI_NOT_ALLOWED;
}

/* 菜单末尾的"返回"行由 KK_UI 在 item_count 处自动合成（画 "<"、走 NavigateBack），
 * 应用不要再自己加一项，否则会多出一行而且那个是自己声明的 PAGE 项、画的是 ">"。 */
/* 「蜂鸣器」总开关已按用户要求删掉（没用）：现在只剩"按键蜂鸣/过点蜂鸣"两个
 * 各自独立的开关。 */
static const KK_UI_MenuItem k_settings_items[] = {
    { "循迹速度", KK_UI_MENU_INT,    0 },
    { "灰度接口", KK_UI_MENU_INT,   13 },   /* 0=自动 1=I2C 2=串口 3=数字 */
    { "自动判线", KK_UI_MENU_BOOL,   4 },   /* 单帧自适应；关掉 = 固定中点阈值 */
    { "陀螺仪闭环", KK_UI_MENU_BOOL, 0 },
    { "按键蜂鸣", KK_UI_MENU_BOOL,  2 },
    { "过点蜂鸣", KK_UI_MENU_BOOL,  3 },
    { "循迹反转", KK_UI_MENU_BOOL,  1 },
    { "关于", KK_UI_MENU_PAGE,      PAGE_INFO_ABOUT }
};
/* 「任务」二级菜单已删（2026-10-03，用户 m19902）：它只有「任务A / 任务B」两个旧
 * 内置流程入口，首页现在直接进唯一的任务状态页，跟"循迹测试/陀螺仪/电流"一致。 */
/* PID：首页"PID" -> 这一页 -> 选环 -> 每一项都是一个 INT 编辑框。
 * 三层串级（用户定的结构）：
 *   速度环（HP_SPEED_*）目标转速 -> PWM，**环本身还没写**（缺编码器）
 *   角度环（HP_GYRO_*） gyro_control.c，目标角度 -> 左右轮目标速度
 *   循迹环（HP_LINE_*） pid.c，8 路灰度 -> 目标角度
 * 参数值统一存在 host_link 参数表里，改完要 push 回参数表才生效。 */
static const KK_UI_MenuItem k_pid_items[] = {
    { "速度环", KK_UI_MENU_PAGE, PAGE_MENU_PID_SPEED },
    { "角度环", KK_UI_MENU_PAGE, PAGE_MENU_PID_ANGLE },
    { "循迹环", KK_UI_MENU_PAGE, PAGE_MENU_PID_TRACE }
};
static const KK_UI_MenuItem k_pid_speed_items[] = {
    { "速度Kp",   KK_UI_MENU_INT, 1 },
    { "速度Ki",   KK_UI_MENU_INT, 2 },
    { "速度Kd",   KK_UI_MENU_INT, 3 },
    { "速度限幅", KK_UI_MENU_INT, 4 },
    /* 前馈：第三个数是 k_int_bindings 的下标，这两项接在该数组末尾（14/15），
     * 这样不用重排前面已有的 14 项（0..13，最后一项是"灰度接口"）。 */
    { "前馈斜率", KK_UI_MENU_INT, 14 },
    { "前馈摩擦", KK_UI_MENU_INT, 15 }
};
static const KK_UI_MenuItem k_pid_angle_items[] = {
    { "角度Kp",   KK_UI_MENU_INT, 5 },
    { "角度Ki",   KK_UI_MENU_INT, 6 },
    { "角度Kd",   KK_UI_MENU_INT, 7 },
    { "角度限幅", KK_UI_MENU_INT, 8 }
};
static const KK_UI_MenuItem k_pid_trace_items[] = {
    { "循迹Kp",   KK_UI_MENU_INT, 9 },
    { "循迹Ki",   KK_UI_MENU_INT, 10 },
    { "循迹Kd",   KK_UI_MENU_INT, 11 },
    { "循迹限幅", KK_UI_MENU_INT, 12 }
};
static const KK_UI_MenuPage k_menu_pages[] = {
    { "设置",   k_settings_items,  NULL, APP_UI_ITEM_COUNT(k_settings_items) },
    { "PID",    k_pid_items,       NULL, APP_UI_ITEM_COUNT(k_pid_items) },
    { "速度环", k_pid_speed_items, NULL, APP_UI_ITEM_COUNT(k_pid_speed_items) },
    { "角度环", k_pid_angle_items, NULL, APP_UI_ITEM_COUNT(k_pid_angle_items) },
    { "循迹环", k_pid_trace_items, NULL, APP_UI_ITEM_COUNT(k_pid_trace_items) }
};

/* 信息页（陀螺仪的原生信息页已删：首页"陀螺仪"进的是带小车模型的自定义页，
 * 那个 info 页没有任何入口，只会白占 flash 和每帧一次 snprintf） */
static const KK_UI_InfoRow k_about_rows[] = {
    { "主控", "STM32G474RBT6" },
    { "主频", "170MHz" },
    { "显示", "SSD1315 128x64" },
    { "UI",   "KK_UI + KK_OLED" },
    { "固件", __DATE__ }
};
static const KK_UI_InfoPage k_info_pages[] = {
    { "关于", k_about_rows, APP_UI_ITEM_COUNT(k_about_rows) }
};

/* 可编辑变量绑定。取值范围/步长与 host_link 参数表的 min/max 对齐
 * （表里的 Kp 上限 5000.0 -> ×100 就是 500000）。 */
/* 最后一项 decimals：2 表示值本身是 ×100 的刻度、显示时插小数点
 * （37500 -> "375.00"），0 表示原样显示整数。限幅是 PWM/积分刻度，整数即可。 */
/* 灰度模块通讯方式的枚举文案（KK_UI_IntBinding.names：非 NULL 时对话框
 * 和菜单行都直接显示这里第 value-minimum 个字符串，不再显示数字）。 */
static const char *const k_gray_mode_names[TRACE_MODE_COUNT] = {
    "自动", "I2C", "串口", "数字"
};
static const KK_UI_IntBinding k_int_bindings[] = {
    { "循迹速度", &g_cfg.trace_speed,   0,   7200, 100, "", EV_PID_CHANGED, 0, NULL },
    { "速度Kp",   &g_cfg.speed_kp_x100, 0, 200000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "速度Ki",   &g_cfg.speed_ki_x100, 0,  20000,  10, "", EV_PID_CHANGED, 2, NULL },
    { "速度Kd",   &g_cfg.speed_kd_x100, 0,  50000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "速度限幅", &g_cfg.speed_ilim,    0,  20000,  10, "", EV_PID_CHANGED, 0, NULL },
    { "角度Kp",   &g_cfg.gyro_kp_x100,  0, 200000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "角度Ki",   &g_cfg.gyro_ki_x100,  0,  20000,  10, "", EV_PID_CHANGED, 2, NULL },
    { "角度Kd",   &g_cfg.gyro_kd_x100,  0,  50000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "角度限幅", &g_cfg.gyro_ilim,     0,  20000,  10, "", EV_PID_CHANGED, 0, NULL },
    { "循迹Kp",   &g_cfg.line_kp_x100,  0, 500000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "循迹Ki",   &g_cfg.line_ki_x100,  0,  20000,  10, "", EV_PID_CHANGED, 2, NULL },
    { "循迹Kd",   &g_cfg.line_kd_x100,  0, 100000, 100, "", EV_PID_CHANGED, 2, NULL },
    { "循迹限幅", &g_cfg.line_ilim,     0,  10000,  10, "", EV_PID_CHANGED, 0, NULL },
    /* 灰度接口：不是数值而是四选一，靠 names 显示文案 */
    { "灰度接口", &g_cfg.gray_mode, TRACE_MODE_AUTO, TRACE_MODE_DIGITAL, 1, "", EV_NONE, 0,
      k_gray_mode_names },
    /* 速度环前馈（下标 14/15，被 k_pid_speed_items 引用）。斜率是 ×1000 的刻度，
     * 屏幕按 decimals=3 显示（662 -> "0.662"），步长 10 就是 0.010 一档。 */
    { "前馈斜率", &g_cfg.speed_ff_slope_x1000, 0, 2000, 10, "", EV_PID_CHANGED, 3, NULL },
    { "前馈摩擦", &g_cfg.speed_ff_fric,        0, 5000, 10, "", EV_PID_CHANGED, 0, NULL }
    /* 「标定套」这一项已按用户 m09558 删掉：跑任务时用哪套由任务程序调
     * trace_cal_select_set() 决定，循迹页的右键只是"看另一套的效果"，不存 Flash。
     * nvram 记录里的 cal_set 字段保留（v4 布局不动），没人写它就一直是 0 = 普通。 */
};
static const KK_UI_BoolBinding k_bool_bindings[] = {
    { "陀螺仪闭环", &g_cfg.use_gyro,     EV_NONE },
    { "循迹反转",   &g_cfg.trace_invert, EV_NONE },
    { "按键蜂鸣",   &g_cfg.beep_key,     EV_NONE },
    { "过点蜂鸣",   &g_cfg.beep_marker,  EV_NONE },
    /* 「自动判线」= 单帧自适应开关（trace_cal_set_adapt），设置页第 3 行引用它。
     * 名字不能叫"循迹自适应"：字库是子集，"适"没字形（会静默不画）。 */
    { "自动判线",   &g_cfg.trace_adapt,  EV_NONE }
};

static const KK_UI_App k_app = {
    .root_page = PAGE_SPLASH,
    .routes           = k_routes,
    .route_count     = PAGE_COUNT,
    .home_pages       = k_home_pages,
    .home_page_count  = 1,
    .menu_pages       = k_menu_pages,
    /* 用宏而不是写字面量：删/加二级菜单忘了改这个数，菜单项就会索引越界/打不开 */
    .menu_page_count  = APP_UI_ITEM_COUNT(k_menu_pages),
    .info_pages       = k_info_pages,
    .info_page_count  = 1,
    .custom_page_count = CUSTOM_COUNT,
    .int_bindings      = k_int_bindings,
    /* 用宏而不是写字面量：加绑定忘了改这个数，菜单项就会索引越界/打不开编辑框 */
    .int_binding_count = APP_UI_ITEM_COUNT(k_int_bindings),
    .bool_bindings     = k_bool_bindings,
    .bool_binding_count = APP_UI_ITEM_COUNT(k_bool_bindings),
    .confirm_descs     = NULL,
    .confirm_desc_count = 0,
    .fonts = { kk_font_home, kk_font_body, kk_font_body },
    .texts = { "返回", "取消", "确定", "开", "关", "提示" }
};

/* =====================================================================
 * PID 参数 <-> host_link 参数表
 *   控制环从参数表取值（pid.c 循迹环取 HP_LINE_*，gyro_control.c 角度环取
 *   HP_GYRO_*），菜单编辑的是 g_cfg 里 ×100 的整数镜像：
 *     开机    参数表 -> g_cfg   （pull，默认值本来就是一致的）
 *     改完     g_cfg -> 参数表  （push，收到 EV_PID_CHANGED 事件时）
 *   两边都用本地接口，不走串口；上位机 SET 的参数要在下次开机才会同步进 UI。 */
static void push_pid_params(void)
{
    /* 循迹基准速度也在参数表里（motion/循迹环从 HP_BASE_SPEED 取），
     * 菜单里改了「循迹速度」就跟 PID 一起推过去。 */
    host_param_set_local(HP_BASE_SPEED,  (float)g_cfg.trace_speed);
    host_param_set_local(HP_SPEED_KP,    (float)g_cfg.speed_kp_x100 / 100.0f);
    host_param_set_local(HP_SPEED_KI,    (float)g_cfg.speed_ki_x100 / 100.0f);
    host_param_set_local(HP_SPEED_KD,    (float)g_cfg.speed_kd_x100 / 100.0f);
    host_param_set_local(HP_SPEED_ILIMIT,(float)g_cfg.speed_ilim);
    /* 前馈：控制环每拍从参数表取，所以推进参数表就算生效（不需要重启） */
    host_param_set_local(HP_SPEED_FF_SLOPE, (float)g_cfg.speed_ff_slope_x1000 / 1000.0f);
    host_param_set_local(HP_SPEED_FF_FRIC,  (float)g_cfg.speed_ff_fric);
    host_param_set_local(HP_LINE_KP,     (float)g_cfg.line_kp_x100 / 100.0f);
    host_param_set_local(HP_LINE_KI,     (float)g_cfg.line_ki_x100 / 100.0f);
    host_param_set_local(HP_LINE_KD,     (float)g_cfg.line_kd_x100 / 100.0f);
    host_param_set_local(HP_LINE_ILIMIT, (float)g_cfg.line_ilim);
    host_param_set_local(HP_GYRO_KP,     (float)g_cfg.gyro_kp_x100 / 100.0f);
    host_param_set_local(HP_GYRO_KI,     (float)g_cfg.gyro_ki_x100 / 100.0f);
    host_param_set_local(HP_GYRO_KD,     (float)g_cfg.gyro_kd_x100 / 100.0f);
    host_param_set_local(HP_GYRO_ILIMIT, (float)g_cfg.gyro_ilim);
}

/* 参数表 -> UI 镜像，×100 / ×1000 量化（都是非负数，+0.5 四舍五入）。 */
static int32_t param_to_x100(uint16_t id, float fallback)
{
    return (int32_t)(host_param_get(id, fallback) * 100.0f + 0.5f);
}

static int32_t param_to_x1000(uint16_t id, float fallback)
{
    return (int32_t)(host_param_get(id, fallback) * 1000.0f + 0.5f);
}

static void pull_pid_params(void)
{
    g_cfg.speed_kp_x100 = param_to_x100(HP_SPEED_KP,    20.0f);
    g_cfg.speed_ki_x100 = param_to_x100(HP_SPEED_KI,     2.0f);
    g_cfg.speed_kd_x100 = param_to_x100(HP_SPEED_KD,     0.0f);
    g_cfg.speed_ilim    = (int32_t)host_param_get(HP_SPEED_ILIMIT, 3000.0f);
    g_cfg.speed_ff_slope_x1000 = param_to_x1000(HP_SPEED_FF_SLOPE, SPEED_FF_SLOPE);
    g_cfg.speed_ff_fric        = (int32_t)host_param_get(HP_SPEED_FF_FRIC, SPEED_FF_FRIC);
    /* 兜底值必须与 host_link.c 的 SPECS 默认值、本文件 g_cfg 默认值三处一致：
     * 循迹环增益是旧板值 ×7.2（满量程 1000 → 7200）；角度环是 2026-10-01 原地
     * 实测选定值 150/15（旧刻度 ×7.2 的 720/72 在本板自激，见 g_cfg 默认值注释）。 */
    g_cfg.line_kp_x100 = param_to_x100(HP_LINE_KP,    2700.0f);
    g_cfg.line_ki_x100 = param_to_x100(HP_LINE_KI,      10.8f);
    g_cfg.line_kd_x100 = param_to_x100(HP_LINE_KD,     360.0f);
    g_cfg.line_ilim    = (int32_t)host_param_get(HP_LINE_ILIMIT, 50.0f);
    g_cfg.gyro_kp_x100 = param_to_x100(HP_GYRO_KP,     150.0f);
    g_cfg.gyro_ki_x100 = param_to_x100(HP_GYRO_KI,       0.0f);
    g_cfg.gyro_kd_x100 = param_to_x100(HP_GYRO_KD,      15.0f);
    g_cfg.gyro_ilim    = (int32_t)host_param_get(HP_GYRO_ILIMIT, 1000.0f);
}

/* =====================================================================
 * 五向键原始位图（已按屏幕安装方向做旋转）
 *   bit0上 bit1下 bit2左 bit3右 bit4中，1=按下 —— 与 board.c:keys_raw() 同序。
 * 电机测试页要四个方向（见 motor_key_scan()），所以它直接吃这个函数；
 * 其余页面走下面的三键归一化。
 * ===================================================================== */
static uint8_t app_ui_raw_keys(void)
{
    uint8_t k = keys_raw();

#if APP_UI_DISPLAY_ROT180
    {
        uint8_t s = (uint8_t)(k & 0x10U);
        if (k & 0x01U) s |= 0x02U;
        if (k & 0x02U) s |= 0x01U;
        if (k & 0x04U) s |= 0x08U;
        if (k & 0x08U) s |= 0x04U;
        k = s;
    }
#endif

    return k;
}

/* =====================================================================
 * 输入归一化：五向键 -> KK_UI 的三键
 *   上/左 -> UP   下/右 -> DOWN   中 -> OK
 * ===================================================================== */
static KK_UI_Input app_ui_read_input(void)
{
    uint8_t k = app_ui_raw_keys();
    KK_UI_Input in;

    /* 存 Flash 期间（"正在保存"挂着 + 真擦写）把按键全吃掉：擦写会把 CPU
     * 停住 ~30ms，这会儿别让用户再改东西（用户 m03944："什么都不让我动,
     * 这样来保证擦血顺利"）。"保存成功"那条提示期间是放开的，不然改完
     * 一项要愣 1.5s。 */
    if (nvram_state() == NVRAM_STATE_BUSY && s_nv_lock != 0U) {
        in.keys = 0U;
        in.encoder_delta = 0;
        return in;
    }

    in.keys = 0U;
    if ((k & 0x01U) || (k & 0x04U)) in.keys |= KK_UI_KEY_UP;
    if ((k & 0x02U) || (k & 0x08U)) in.keys |= KK_UI_KEY_DOWN;
    if  (k & 0x10U)                 in.keys |= KK_UI_KEY_OK;
    in.encoder_delta = 0;
    return in;
}

/* ---- 电机测试页的按键处理（原始位图 + 长按加速） ----
 * 为什么不走 kk_ui：框架只有三键（左/右在 app_ui_read_input() 里被折成 上/下），
 * 分不出左右，而这一页要"上/下选、左/右拖"。所以本页自己读五向键原始位图，
 * 自己做 20ms 消抖 + 长按连发。中键仍然走 kk_ui 的 OK 事件（框架已消抖、
 * 且长按的 REPEAT 被本页显式忽略）。 */

/* 连发步进：按住越久，一次走几格（用户 m02868："长按的话也会越来越快"） */
static int motor_key_step(uint32_t held)
{
    if (held < MOTOR_KEY_FIRST_MS) return 1;
    if (held < MOTOR_STEP_2_MS)    return 2;
    if (held < MOTOR_STEP_5_MS)    return 5;
    if (held < MOTOR_STEP_10_MS)   return 10;
    return 20;
}

/* 拖选中那一路的速度档（夹在 ±100 内；真的变了才写 PWM + 重画） */
static bool motor_level_nudge(uint8_t wheel, int delta)
{
    int v;

    if (wheel > MOTOR_SEL_RIGHT) {
        return false;   /* 兜底：选中的是「清零」/「返回」（左/右由 motor_key_press() 处理成换项） */
    }
    v = (int)s_motor_level[wheel] + delta;
    if (v > MOTOR_LEVEL_MAX)  v = MOTOR_LEVEL_MAX;
    if (v < -MOTOR_LEVEL_MAX) v = -MOTOR_LEVEL_MAX;
    if (v == (int)s_motor_level[wheel]) {
        return false;
    }
    s_motor_level[wheel] = (int8_t)v;
    motor_test_apply();
    return true;
}

/* 一个方向键按下（或连发）：上/下换选中项，左/右拖滑条。
 * 「返回」「清零」同在标题行，所以左/右在这一行用来在两项之间切换
 * （用户 m03156：它们是一排的，从「返回」按右就该到「清零」）。
 * 中键在 KK_UI_CustomOnInput() 里处理（返回 true = 需要重画）。 */
static bool motor_key_press(int key, int step)
{
    switch (key) {
    case MOTOR_KEY_UP:
        s_motor_sel = (uint8_t)((s_motor_sel + MOTOR_TEST_N - 1U) % MOTOR_TEST_N);
        return true;
    case MOTOR_KEY_DOWN:
        s_motor_sel = (uint8_t)((s_motor_sel + 1U) % MOTOR_TEST_N);
        return true;
    case MOTOR_KEY_LEFT:
    case MOTOR_KEY_RIGHT:
        if (s_motor_sel == MOTOR_SEL_BACK || s_motor_sel == MOTOR_SEL_CLEAR) {
            uint8_t want;
            want = (key == MOTOR_KEY_LEFT) ? MOTOR_SEL_BACK : MOTOR_SEL_CLEAR;
            if (s_motor_sel == want) {
                return false;   /* 已经在那一头（长按连发不重复重画） */
            }
            s_motor_sel = want;
            return true;
        }
        return motor_level_nudge(s_motor_sel, (key == MOTOR_KEY_LEFT) ? -step : step);
    default:
        return false;
    }
}

/* 扫描原始按键：处理"新按下"（短按 = 一步）和"按住连发"。
 * 返回 true = 需要重画。只在 KK_UI_CustomOnTick() 里调（框架保证"当前页是
 * 本页且没有翻页过渡"），所以不会和翻页动画抢按键。 */
static bool motor_key_scan(uint32_t now)
{
    uint8_t raw = app_ui_raw_keys();
    bool redraw = false;
    int i;

    if (raw != s_mkey_raw) {
        s_mkey_raw = raw;
        s_mkey_changed = now;
        return false;           /* 刚变化：等消抖 */
    }
    if ((uint32_t)(now - s_mkey_changed) < MOTOR_KEY_DEBOUNCE_MS) {
        return false;
    }
    for (i = 0; i < MOTOR_KEY_N; i++) {
        uint8_t bit = (uint8_t)(1U << i);

        if ((raw & bit) == 0U) {
            continue;           /* 没按 */
        }
        if ((s_mkey_stable & bit) == 0U) {
            /* 新按下（已消抖）：先走一步 —— 短按就是这一步 */
            s_mkey_hold_at[i] = now;
            s_mkey_repeat_at[i] = now + MOTOR_KEY_FIRST_MS;
            if (motor_key_press(i, 1)) {
                redraw = true;
            }
        } else if ((int32_t)(now - s_mkey_repeat_at[i]) >= 0) {
            /* 还按着且到了连发点：步进随按住时长变大 */
            s_mkey_repeat_at[i] = now + MOTOR_KEY_REPEAT_MS;
            if (motor_key_press(i, motor_key_step(now - s_mkey_hold_at[i]))) {
                redraw = true;
            }
        }
    }
    s_mkey_stable = raw;        /* 20ms 没变化的位图就是稳定态（按下和松开同一套） */
    return redraw;
}

/* =====================================================================
 * 绘制小工具
 * ===================================================================== */
#define LINE_H 13

static void draw_text(int16_t x, int16_t y, const uint8_t *font, const char *s)
{
    OLED_SetFont(font);
    OLED_SetFontPosition(OLED_FONT_POS_TOP);
    OLED_DrawUTF8((int16_t)(x + s_dx), y, s);
}

/* ×10 定点角度统一格式化。
 * 必须自己处理符号：-123 直接 "%%ld.%%ld" 会得到 "-12.-3"（负余数）。 */
static void fmt_x10(char *buf, size_t n, int32_t v)
{
    int32_t a = (v < 0) ? -v : v;
    snprintf(buf, n, "%s%ld.%ld", (v < 0) ? "-" : "", (long)(a / 10), (long)(a % 10));
}

/* 显示用的角度（×10）。用户约定的正方向：
 *   向左偏 -> 横滚增大；向下低头 -> 俯仰增大；向左转 -> 偏航增大。
 * 源数据是 JY61P 的 roll/pitch，轴序和符号与约定不一致，
 * 所以这里做**一次**轴序交换 + 取反，全工程只在这一个地方改。 */
/* 与 fmt_x10 同格式，但 |角度|>=100.0 时只留整数位，
 * 保证"横/俯/偏"三列（每列 42px）并排时不越界。 */
static void fmt_x10_compact(char *buf, size_t n, int32_t v)
{
    if (v > 1000 || v < -1000) {
        snprintf(buf, n, "%ld", (long)(v / 10));
    } else {
        fmt_x10(buf, n, v);
    }
}

static void gyro_angles(int32_t *roll, int32_t *pitch, int32_t *yaw)
{
    *roll  = -g_live.pitch_x10;
    *pitch = -g_live.roll_x10;
    *yaw   =  g_live.yaw_x10;
}

/* 蜂鸣器：响一下就好（不要常响）。**非阻塞**——只开响并记下截止时刻。 */
static void beep_ms(uint16_t ms)
{
    board_buzzer_init();
    HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_SET);
    s_beep_until = HAL_GetTick() + (uint32_t)ms;
}

/* 到点关掉蜂鸣器；由 app_ui_tick() 每轮主循环调用。 */
static void beep_poll(void)
{
    if (s_beep_until != 0U && (int32_t)(HAL_GetTick() - s_beep_until) >= 0) {
        s_beep_until = 0U;
        HAL_GPIO_WritePin(BUZZER_PORT, BUZZER_PIN, GPIO_PIN_RESET);
    }
}

/* KK_UI 中间件在每一次"新的物理按下"（去抖后）回调一次，长按重复不算。
 * 「按键蜂鸣」以前只写在任务 A/B 按中键那一个分支里，别的页面全不响。 */
void KK_UI_OnKeyPress(uint8_t keys, uint32_t now)
{
    (void)keys;
    (void)now;
    if (g_cfg.beep_key) {
        beep_ms(30);
    }
}

static void draw_title(const char *title)
{
    OLED_SetDrawMode(OLED_DRAW_SET);
    draw_text(2, 1, kk_font_body, title);
    OLED_DrawHLine(s_dx, 14, 128);
}

/* =====================================================================
 * 循迹测试：8 条柱子 + 每路的 0/1 + 0/1 分界线（阈值线）
 *
 *   - 柱高 = 该路的灰度（0..255，255 = 压黑线）：有模拟量用模拟量，
 *     丢线/断链时**按住上一帧好帧**（trace_cal_disp_level，页面另打「丢线」标记），
 *     用户显式选了"数字"接口时才是数字口的 0/255
 *   - 分界线 = 标定出来的 0/1 阈值位置（XOR 画，压在实心柱上也看得见）
 *   - 底部一行 = 界面上看到的 0/1（已含「循迹反转」，丢线时同柱高一起按住；
 *     控制逻辑用的是 trace_cal_channel/level 那份真值，别拿这一行当判据）
 *   - 中键进黑白两点标定向导：全黑按一下 -> 全白按一下 -> 自动算每路阈值
 *   - 柱子上方留 17px 给标题、底部留 13px 给 0/1，所以柱高从 44 降到 32
 * ===================================================================== */
#define TC_TOP     17
#define TC_H       32
#define TC_W       12
#define TC_STEP    15
#define TC_X0       5
#define TC_DIGIT_Y 51

/* 8 条柱子 + 每路的 0/1 + 阈值线 —— 循迹页和任务页**共用同一份画法**
 * （用户 m24999：任务页下面整块留给"寻迹当前的扫描状态"，他离线跑任务时就盯着它，
 * 两个页面看到的东西必须一模一样，换个页面重新认一遍就可能判错线）。 */
static void draw_trace_bars(void)
{
    int i;

    for (i = 0; i < 8; i++) {
        /* xr = 不带过渡位移的基准 x。图元自己加 s_dx，文字交给 draw_text()
         * （它内部已经加过一次）——原来数字传的是 x+3（x 已含 s_dx），
         * 于是过渡时数字按 2 倍位移滑、跟柱子错位。 */
        int16_t xr  = (int16_t)(TC_X0 + i * TC_STEP);
        int16_t x   = (int16_t)(xr + s_dx);
        int16_t h   = (int16_t)((uint16_t)g_live.gray[i] * (uint16_t)TC_H / 255U);
        int16_t y0  = (int16_t)(TC_TOP + TC_H);
        int16_t thr = trace_cal_thr_level((uint8_t)i);
        char    d[2];

        OLED_SetDrawMode(OLED_DRAW_CLEAR);
        OLED_DrawBox(x, TC_TOP, TC_W, TC_H);
        OLED_SetDrawMode(OLED_DRAW_SET);
        OLED_DrawFrame(x, TC_TOP, TC_W, TC_H);

        /* 填满整个方框：原来左右各留 2px 黑边、只填 8px 宽，用户看着"没填满" */
        if (h > 0) OLED_DrawBox(x, (int16_t)(y0 - h), TC_W, h);

        /* 0/1 分界线：XOR 画一条横线，柱里有填充时变黑、没填充时变白，两边都看得见 */
        if (thr >= 0) {
            int16_t yy = (int16_t)(y0 - 1 - (int16_t)((uint16_t)thr * (uint16_t)(TC_H - 1) / 255U));
            OLED_SetDrawMode(OLED_DRAW_XOR);
            OLED_DrawHLine(x, yy, (uint16_t)TC_W);
        }
        OLED_SetDrawMode(OLED_DRAW_SET);

        /* 底部：这一路实际用的 0/1；标定过但这一路无效（黑白跨度太小或采样在抖）
         * 就画 '!'，提醒这一路的阈值不可信 */
        if (trace_cal_valid && !trace_cal_ch_ok((uint8_t)i)) {
            d[0] = '!';
        } else {
            d[0] = (char)('0' + (g_live.trace[i] ? 1 : 0));
        }
        d[1] = '\0';
        draw_text((int16_t)(xr + 3), TC_DIGIT_Y, kk_font_body, d);
    }
}

/* 右键切换的「检验视图」：8 路原始值 + 当前帧范围 + 标定质量。
 * 柱状图回答"哪几路压线"，这一页回答"模块的数值本身对不对、这份标定可不可信"
 * （用户 m05894：标定流程要能检验）。原始值是模块回的大端 16 位原样，没做任何
 * 归一化——标定前后都能看，正好用来判断"中间黑两边白"这类问题。 */
static uint8_t s_trace_verify;

/* "这套标定算起来是什么效果"的**预览**：8 根柱子画的永远是 trace_cal 当前生效那套的
 * 数据，所以右键只是把 trace_cal 指到另一套（`trace_cal_select_set()`，**只动 RAM**）。
 * 用户 m09558 讲清了这一页的定位：**它只是用来对比两套标定效果的显示页**，
 * 不是"选哪套去跑"的开关 —— 真跑任务时由任务程序自己调 `trace_cal_select_set()`
 * 决定用哪套算，所以这里**不写 Flash、不弹"保存成功"、也不需要那个标志位**；
 * 掉电不保存也没关系（进来默认看「普通」= 第 0 套）。 */

/* 只切换"正在算哪套/屏上看哪套"，**不碰 g_cfg**（因此 nvram 不会为它写 Flash）。
 * trace_cal_select_set() 会把白底基线清掉重新起头 —— 这是必须的，两套的白值不一样。 */
static void cal_set_preview(uint8_t set)
{
    set &= 1U;
    if (trace_cal_active_set() == set) {
        return;                     /* 已经是这一套：别白清一次基线 */
    }
    trace_cal_select_set(set);
}

/* ---- 循迹页的按键处理（读五向键原始位图，只要"新按下"那一下） ----
 * 框架把 上/左 折成 UP、下/右 折成 DOWN（见 app_ui_read_input()），**分不出
 * 右键与下键**。用户 m08812/m09335 要求这两条并存在同一个柱状图页上：
 *   右键   = 换成另一套标定**预览**（只动 RAM，见 cal_set_preview()，用户 m09558）
 *   下键   = 柱状图 <-> 详细数据（灰度原值 / 标定质量）
 *   左/上  = 返回（还是走框架的 UP 事件）
 *   中键   = 进标定（走框架的 OK 事件，已消抖；标定界面就是原来那套
 *            "全黑按中键" -> "全白按中键" -> "标定完成"）
 * 所以本页像电机测试页那样自己扫原始位图。两处不同：一是只要"新按下"的边沿
 * （不要长按连发，切套/切视图一下就够了），二是标定中不动作 —— 标题行要留给
 * "全黑/全白/不稳!重按"这些提示，被检验视图盖掉就不知道下一步该干什么。 */
#define TRACE_KEY_DEBOUNCE_MS 20U   /* 与 motor_key_scan() / key.c 同量级 */
static uint8_t  s_trk_raw;          /* 上一拍原始位图 */
static uint8_t  s_trk_stable;       /* 已消抖的稳定位图（用来取"新按下"边沿） */
static uint32_t s_trk_changed;      /* 原始位图最后一次变化的时刻 */

static bool trace_key_scan(uint32_t now)
{
    uint8_t raw = app_ui_raw_keys();
    uint8_t fresh;
    bool redraw = false;

    if (raw != s_trk_raw) {
        s_trk_raw = raw;
        s_trk_changed = now;
        return false;               /* 刚变化：等消抖 */
    }
    if ((uint32_t)(now - s_trk_changed) < TRACE_KEY_DEBOUNCE_MS) {
        return false;
    }
    fresh = (uint8_t)(raw & (uint8_t)~s_trk_stable);   /* 这一拍新按下的位 */
    s_trk_stable = raw;

    if ((fresh & 0x08U) != 0U) {            /* bit3 右键：换另一套标定来预览 */
        cal_set_preview((uint8_t)(trace_cal_active_set() ^ 1U));
        redraw = true;
    } else if ((fresh & 0x02U) != 0U) {     /* bit1 下键：柱状图 <-> 详细数据 */
        s_trace_verify = (uint8_t)(s_trace_verify ? 0U : 1U);
        redraw = true;
    }
    return redraw;
}

/* 标题行上"现在用哪套标定"的标记：'普' / '亚'。只放一个字 ——
 * kk_font_body 汉字前进宽 ≈11.3px，标题行右边通常只剩 12px，完整名字
 * （普通 / 亚克力）放不下；完整名字在检验视图和文档的语境里。
 * x 由各页自己给（循迹页挨着页名放，向导页放右上角）。 */
static void draw_cal_set_mark(int16_t x)
{
    draw_text(x, 1, kk_font_body, (trace_cal_active_set() != 0U) ? "亚" : "普");
}

/* 完整名字版（「普通」/「亚克力」）：只在标定向导的 WAIT_BLACK / WAIT_WHITE 两个
 * 状态里用 —— 那时标题（"全黑按中键" 6 个字 ≈68px）右边一直到 x≈123 都是空的，
 * 放得下 3 个汉字。用户 m09876：标定只有一个入口（中键），必须一眼看出这一按
 * 到底写进哪一套。DONE/FAILED 的 x=60 起要报质量/原因，只能用上面那个单字标记。 */
static void draw_cal_set_name(int16_t x_right)
{
    const char *n = (trace_cal_active_set() != 0U) ? "亚克力" : "普通";
    int16_t w;

    OLED_SetFont(kk_font_body);          /* OLED_GetUTF8Width 用的是"当前字体" */
    w = (int16_t)OLED_GetUTF8Width(n);
    draw_text((int16_t)(x_right - w), 1, kk_font_body, n);
}

static void draw_trace_verify(void)
{
    uint8_t i;
    uint16_t lo = 0xFFFFU, hi = 0U;
    float w[TRACE_CAL_CH];
    trace_frame_t fr = trace_cal_frame(w);   /* 这一帧的自适应权重（和循迹环用的是同一份） */
    char buf[32];

    draw_title("灰度原值");
    /* 标题行中间报"现在看的是哪套标定"（用户 m07888 的两套标定）。
     * 只放一个字：这个 12px 字库汉字前进宽 ≈11.3px，"灰度原值"4 个字到 x≈47、
     * 右边 x=76 是"好8/8"，中间只剩 47..75 这 28px —— 放"亚克力"3 个字会压到
     * 好8/8 上（实测撞过）。完整名字在标定向导的「全黑/全白按中键」页右上角
     * （draw_cal_set_name，用户 m09876）。 */
    draw_cal_set_mark(52);
    /* 右上角：标定质量 / 丢线 / 未标定。（"位置"这一行放到下面汇总行去画：
     * 12px 字体一行只放得下 128/13 ≈ 9 个汉字宽，标题行再塞就画到屏外了。） */
    if (fr == TRACE_FRAME_WEAK) {
        snprintf(buf, sizeof(buf), "丢线");
    } else if (trace_cal_valid) {
        snprintf(buf, sizeof(buf), "好%u/8", (unsigned)trace_cal_quality());
    } else {
        snprintf(buf, sizeof(buf), "未标定");
    }
    draw_text(76, 1, kk_font_body, buf);

    for (i = 0; i < TRACE_CAL_CH; i++) {
        uint16_t v = trace_cal_raw[i];
        int16_t  x = (int16_t)(4 + (int16_t)(i % 4U) * 30);
        int16_t  y = (int16_t)(16 + (int16_t)(i / 4U) * 13);

        /* 两行四列：12px 字体的数字宽 ≈6px，"9999" 占 24px，列距 30px -> 末尾 x=118 */
        snprintf(buf, sizeof(buf), "%4u", (unsigned)v);
        draw_text(x, y, kk_font_body, buf);
        /* 标定过但这一路无效：数字后面点一个 '!'（列内还剩 6px，正好一个字符） */
        if (trace_cal_valid && !trace_cal_ch_ok(i)) {
            draw_text((int16_t)(x + 24), y, kk_font_body, "!");
        }
        if (v < lo) lo = v;
        if (v > hi) hi = v;
    }
    if (lo == 0xFFFFU) lo = 0U;

    /* 汇总行（12px 字体一屏只放得下三行：值两行 + 这里一行，所以自适应那三个数
     * 只挑最关键的画 —— 屏上从 y≈30 到 y≈41 都被第二行原值占着，再往下只能放一行）：
     *   位置 = 送给 PID 的那个重心（1..8，车在线上正中 = 4.5）；
     *   黑   = 本帧最黑那一路的黑度 0~255（还剩多少对比度，掉到 TRACE_ADAPT_MIN_LEVEL
     *          以下判丢线）—— "亚克力上还有没有对比度"就看这个数；
     *   噪   = 采样抖动折算成黑度（"黑/噪"= 这帧的信噪比，<2 别指望）；
     *   偏   = 本帧整体偏置（原值刻度，正 = 比白底基线更亮）。偏/噪 现场要看就用
     *          out/_gray.ps1 读（SWD 只读，不动板子）。 */
    if (fr == TRACE_FRAME_OK) {
        float num = 0.0f, sum = 0.0f;
        uint32_t pos10;

        for (i = 0; i < TRACE_CAL_CH; i++) {
            num += w[i];
            sum += w[i] * (float)(i + 1);
        }
        /* 位置按"十分位整数"打印：本工程链的是 newlib-nano，snprintf 的 %f 不支持，
         * 会静默输出空串（踩过一次：屏上只剩"位置 黑240"）。 */
        pos10 = (num > 0.0f) ? (uint32_t)((sum / num) * 10.0f + 0.5f) : 0U;
        snprintf(buf, sizeof(buf), "位置%u.%u 黑%u", (unsigned)(pos10 / 10U),
                 (unsigned)(pos10 % 10U), (unsigned)trace_cal_frame_dbg.lvl_max);
    } else if (fr == TRACE_FRAME_WEAK) {
        snprintf(buf, sizeof(buf), "丢线 黑%u 噪%u", (unsigned)trace_cal_frame_dbg.lvl_max,
                 (unsigned)trace_cal_frame_dbg.noise);
    } else if (!trace_cal_analog_ok) {
        snprintf(buf, sizeof(buf), "现%u~%u 无模拟量", (unsigned)lo, (unsigned)hi);
    } else if (!trace_cal_valid) {
        snprintf(buf, sizeof(buf), "现%u~%u 未标定", (unsigned)lo, (unsigned)hi);
    } else {
        /* 「差」= 有效路里最小的黑白对比度（不是"跨度"：跨 U+8DE8 不在字模子集里，
         * 会静默不画；改文案前先跑 tools/font/check_charset.mjs）。 */
        snprintf(buf, sizeof(buf), "现%u~%u 差%u", (unsigned)lo, (unsigned)hi,
                 (unsigned)trace_cal_min_span());
    }
    draw_text(4, 44, kk_font_body, buf);
}

static void draw_trace_page(void)
{
    trace_cal_state_t st = trace_cal_state();
    const char *src_txt;

    if (st == TRACE_CAL_IDLE && s_trace_verify) {
        draw_trace_verify();
        return;
    }

    /* 标定中标题就是提示语；平时左边是页名、右边是入口提示。
     * 采样是一次连采 32 帧（20~30ms）完成的，没有"采集中"这个可见状态。
     * 标题行右上角（x=116，四种状态下那里都是空的）画当前标定套：标定是**写进
     * 当前这一套**的，标题必须让用户一眼看出"现在标的是哪套"（用户 m07888）。
     * 注意不能给提示语加"普 "前缀：标题最长的"全黑按中键"再带前缀会顶到
     * DONE/FAILED 右侧 x=60 起的那串字上。 */
    switch (st) {
    case TRACE_CAL_WAIT_BLACK:
        /* 上一点抖动超限被整点作废了：明说要重按，别让用户以为"按过了"。
         * 用「不稳」不用「抖」：抖 U+6296 不在 kk_font_body 子集里，会被静默跳过。 */
        draw_title(trace_cal_retry_hint() ? "不稳!重按" : "全黑按中键");
        break;
    case TRACE_CAL_WAIT_WHITE:
        draw_title(trace_cal_retry_hint() ? "不稳!重按" : "全白按中键");
        break;
    case TRACE_CAL_DONE: {
        char q[12];
        unsigned n = (unsigned)trace_cal_quality();

        draw_title("标定完成");
        /* 右边报质量：8 路全好 "8/8"，有无效路 "7/8!"（哪几路无效看柱子下面那个 '!'） */
        snprintf(q, sizeof(q), "%u/%u%s", n, (unsigned)TRACE_CAL_CH,
                 (n < (unsigned)TRACE_CAL_CH) ? "!" : "");
        draw_text(60, 1, kk_font_body, q);
        break;
    }
    case TRACE_CAL_FAILED:
        /* 两种失败：压根读不到模拟量，或读到了但 8 路的黑白对比度都不够（没跨过黑线）。
         * 「黑白太近」= 想要的"跨度太小"，但 跨/小 两个字不在字模子集里。 */
        draw_title("标定失败");
        draw_text(60, 1, kk_font_body, trace_cal_analog_ok ? "黑白太近" : "无模拟量");
        break;
    default:
        /* 页名从「循迹测试」缩成「循迹」：这个 12px 字库汉字前进宽 ≈11.3px，
         * 「循迹测试」(到 x≈47) + 数据源(x52) + 入口提示(x78..123) 已经把 128px
         * 排满，中间塞不下"当前用哪套标定"的标记（实测压到"试"上）。
         * 缩成两个字后：循迹 x2..24 / 标记 x28 / 数据源 x48 / 提示 x78..123。 */
        draw_title("循迹");
        /* x48..70 放数据源指示：显示**这一帧真的在用**哪条通路，而不是设置里
         * 选的那条——选 I2C/串口但那条不通时 trace_cal_analog_ok=0。
         * 丢线/断链时这一格改打「丢线」：柱子是按住上一帧的旧值（见 trace_cal.h
         * 的「丢线时按住上一帧」，用户 m26712 第 3 条），比"哪条通路"更要紧。 */
        if (trace_cal_held()) {
            src_txt = "丢线";
        } else if (!trace_cal_analog_ok) {
            src_txt = "数字";
        } else {
            src_txt = (trace_cal_src == TRACE_SRC_UART) ? "串口" : "I2C";
        }
        draw_text(48, 1, kk_font_body, src_txt);
        /* 入口提示：中键 = 标定 */
        draw_text(78, 1, kk_font_body, "中键标定");
        break;
    }
    /* 标记位置：循迹页挨着页名（x28），向导放右上角。画在最后，这样万一哪天文案
     * 变长压上来，也是标记盖文字而不是反过来。
     * WAIT 两个状态右边到 x≈123 都是空的 ⇒ 写完整名字（这一按标的是哪一套）；
     * DONE/FAILED 的 x=60 起有质量/原因，只能用单字。 */
    if (st == TRACE_CAL_WAIT_BLACK || st == TRACE_CAL_WAIT_WHITE) {
        draw_cal_set_name(122);
    } else {
        draw_cal_set_mark((st == TRACE_CAL_IDLE) ? 28 : 116);
    }

    /* 柱子/0/1/阈值线：与任务页共用同一份画法（draw_trace_bars()） */
    draw_trace_bars();
}

/* =====================================================================
 * 任务页（PAGE_CUSTOM_TASK）—— 用户 m24999 定的布局
 *
 * 背景：这一页原来只报「车上 N 个」，**没地方启动**（中键只停机，启动一律靠上位机
 * 0x67 RUN 或开机自启）。用户要的是"在车上离线跑"：调好线以后不接电脑，直接在
 * 车上选一个任务按中键就跑起来。所以：
 *   上/下  = 在车上已有的任务之间选（标题栏右边显示选中任务的名字）
 *   中键   = 跑选中的这个 / 正在跑就是停
 *   左键   = 返回（上键已经被"选上一个"占了，框架归一化里 上/左 都是 UP，
 *            所以返回只能自己读原始位图的 bit2 左键）
 *   标题行 = 状态（任务/运行）+ 第几个/共几个 + 选中任务名（最右边）
 *   主体   = 8 路循迹扫描状态（与循迹页同一份画法 draw_trace_bars()）
 *
 * 为什么不走 kk_ui 的三键：框架把 上/左 折成同一个 UP、下/右 折成同一个 DOWN
 * （app_ui_read_input()），按上键会顺手把页面关掉 —— 所以本页像电机页/循迹页那样
 * 自己扫原始位图（task_key_scan()），UP/DOWN 事件在 KK_UI_CustomOnInput() 里吞掉。
 * 中键仍然走框架的 OK 事件（框架已消抖，长按 REPEAT 也不用管）。
 * ===================================================================== */
#define TASK_KEY_DEBOUNCE_MS 20U    /* 与 motor_key_scan() / trace_key_scan() 同量级 */

static uint8_t  s_task_sel = TASK_STORE_NO_SLOT;  /* 选中的槽；NO_SLOT = 车上没有任务 */
static uint8_t  s_tkey_raw;         /* 上一拍原始位图 */
static uint8_t  s_tkey_stable;      /* 已消抖的稳定位图（用来取"新按下"边沿） */
static uint32_t s_tkey_changed;     /* 原始位图最后一次变化的时刻 */
static char     s_task_name[24];    /* 选中任务名的显示前缀（太长就截，见下） */
static int16_t  s_task_name_w;      /* 前缀的像素宽（右对齐用；提前量好，不在 50Hz 里量） */
static int32_t  s_task_sig = -1;    /* 名字缓存的签名（槽/长度/CRC）：变了才重新量宽度 */

/* 从 from 槽出发朝 dir(+1/-1) 找最近的有任务的槽（绕一圈）。返回 NO_SLOT = 一个都没有 */
static uint8_t task_sel_step(uint8_t from, int dir)
{
    uint8_t n = TaskStore_SlotCount();
    uint8_t i;
    uint8_t s;

    if (n == 0U || from >= n) {
        return TASK_STORE_NO_SLOT;
    }
    for (i = 1U; i <= n; i++) {
        s = (uint8_t)((from + ((dir > 0) ? i : (uint8_t)(n - i))) % n);
        if (TaskStore_Present(s)) {
            return s;
        }
    }
    return TASK_STORE_NO_SLOT;
}

/* 选中项兜底：上次选的那个被上位机删了/换槽了就落到车上第一个任务上 */
static uint8_t task_sel_ensure(void)
{
    if (s_task_sel == TASK_STORE_NO_SLOT || s_task_sel >= TaskStore_SlotCount() ||
        !TaskStore_Present(s_task_sel)) {
        s_task_sel = task_sel_step(0U, +1);
    }
    return s_task_sel;
}

/* 量一次名字宽度 + 太长就按字符边界截断（保证不切开一个 UTF-8 汉字）。
 * 只在"进页/换选中/任务被重新烧写"时调 —— 名字最长 30 字节，逐前缀量宽度是 O(n²)。 */
static void task_sel_name_refresh(void)
{
    char    full[TASK_STORE_NAME_MAX + 1];
    const int16_t avail = 122 - 52;     /* 标题行 `任务`+`2/3` 之后到最右边 */
    int32_t sig;
    int     k;
    int     len;

    if (s_task_sel == TASK_STORE_NO_SLOT) {
        s_task_name[0] = '\0';
        s_task_name_w  = 0;
        s_task_sig     = -1;
        return;
    }
    sig = ((int32_t)s_task_sel << 24) |
          ((int32_t)TaskStore_Len(s_task_sel) << 8) |
          (int32_t)(TaskStore_Crc(s_task_sel) & 0xFFU);
    if (sig == s_task_sig) {
        return;                         /* 同一份记录：宽度不用再量 */
    }
    s_task_sig = sig;

    (void)TaskStore_Name(s_task_sel, full, sizeof(full));
    len = (int)strlen(full);
    OLED_SetFont(kk_font_body);         /* OLED_GetUTF8Width 用的是"当前字体" */
    k = 0;
    while (k < len) {
        int cl = 1;
        if (((uint8_t)full[k] & 0xE0U) == 0xC0U)      cl = 2;
        else if (((uint8_t)full[k] & 0xF0U) == 0xE0U) cl = 3;
        else if (((uint8_t)full[k] & 0xF8U) == 0xF0U) cl = 4;
        if ((k + cl) > len) {
            break;                      /* 结尾半个字符：丢掉 */
        }
        memcpy(s_task_name, full, (size_t)(k + cl));
        s_task_name[k + cl] = '\0';
        if ((int16_t)OLED_GetUTF8Width(s_task_name) > avail) {
            break;
        }
        k += cl;
    }
    if (k == 0 && len > 0) {
        /* 一个字都放不下（理论上不会）：宁可显示空，也不画半个字 */
        s_task_name[0] = '\0';
        s_task_name_w  = 0;
        return;
    }
    s_task_name[k] = '\0';
    s_task_name_w  = (k == 0) ? 0 : (int16_t)OLED_GetUTF8Width(s_task_name);
}

/* 任务页按键：只取"新按下"那一下（同 trace_key_scan()，不做长按连发 ——
 * 选任务按一下走一格就够了，连发容易一冲到底）。返回 true = 需要重画。 */
static bool task_key_scan(uint32_t now)
{
    uint8_t raw = app_ui_raw_keys();
    uint8_t fresh;
    bool redraw = false;

    if (raw != s_tkey_raw) {
        s_tkey_raw = raw;
        s_tkey_changed = now;
        return false;                   /* 刚变化：等消抖 */
    }
    if ((uint32_t)(now - s_tkey_changed) < TASK_KEY_DEBOUNCE_MS) {
        return false;
    }
    fresh = (uint8_t)(raw & (uint8_t)~s_tkey_stable);   /* 这一拍新按下的位 */
    s_tkey_stable = raw;

    if ((fresh & 0x04U) != 0U) {                /* bit2 左键：返回上一级 */
        (void)KK_UI_CustomRequestClose();
        return false;
    }
    if ((fresh & 0x01U) != 0U) {                /* bit0 上键：上一个任务 */
        if (task_sel_ensure() != TASK_STORE_NO_SLOT) {
            s_task_sel = task_sel_step(s_task_sel, -1);
            task_sel_name_refresh();
        }
        redraw = true;
    } else if ((fresh & 0x02U) != 0U) {         /* bit1 下键：下一个任务 */
        if (task_sel_ensure() != TASK_STORE_NO_SLOT) {
            s_task_sel = task_sel_step(s_task_sel, +1);
            task_sel_name_refresh();
        }
        redraw = true;
    }
    return redraw;
}

/* 中键：跑 / 停。启动 = 先把选中的槽载进解释器（TaskStore_Load 从 Flash 解 4096B
 * 到 RAM 并校验），再 TaskRun_Start(槽号) —— 与上位机 0x67 RUN 走的是同一条路，
 * 只是**不受上位机看门狗约束**（task_run.h 的约定：比赛要离线自主跑）。
 * 返回 true = 状态变了（调用方重画）。 */
static bool task_run_toggle(void)
{
    if (TaskRun_IsRunning()) {
        TaskRun_Stop();
        return true;
    }
    if (task_sel_ensure() != TASK_STORE_NO_SLOT) {
        if (TaskStore_Load(s_task_sel) == TASK_STORE_OK) {
            (void)TaskRun_Start((uint16_t)s_task_sel);
        }
        return true;
    }
    return false;
}

/* 标题行右边：选中任务名（右对齐到 x=122）。名字太长由 task_sel_name_refresh()
 * 提前截好，这里只负责按宽度摆位置。 */
static void draw_task_name(void)
{
    if (s_task_name_w <= 0) {
        draw_text(4, 1, kk_font_body, "车上没任务");
        return;
    }
    draw_text((int16_t)(122 - s_task_name_w), 1, kk_font_body, s_task_name);
}

static void draw_task_page(void)
{
    uint8_t idx = 0U;
    uint8_t total = 0U;
    uint8_t i;
    char    buf[16];
    int16_t x_min = 28;      /* 「丢线」标记最左能摆到哪（标题右边留空） */

    /* 标题行：左边页名（跑起来写「运行」），中间「第几个/共几个」，最右边任务名。
     * 用户 m24999 明确要求"任务只显示在标题栏、标题栏最右边也要有他的名字"，
     * 下面整块留给循迹扫描状态（他离线跑任务时就盯着这 8 根柱子调线）。 */
    draw_title(TaskRun_IsRunning() ? "运行" : "任务");

    task_sel_ensure();
    for (i = 0U; i < TaskStore_SlotCount(); i++) {
        if (!TaskStore_Present(i)) {
            continue;
        }
        total++;
        if (i <= s_task_sel) {
            idx++;                      /* 选中项是第几个（1 基；选中的一定在场） */
        }
    }
    if (total != 0U) {
        snprintf(buf, sizeof(buf), "%u/%u", (unsigned)idx, (unsigned)total);
        draw_text(28, 1, kk_font_body, buf);
        OLED_SetFont(kk_font_body);              /* 量宽度用的是"当前字体" */
        x_min = (int16_t)(28 + OLED_GetUTF8Width(buf) + 4);
    }
    draw_task_name();

    /* 丢线/断链：柱子与底部 0/1 是"按住上一帧"的旧值，必须说明，不然看着像卡住
     * （用户 m26712 第 2 条报的就是"跑任务时屏幕暂停刷新、都显示一"）。
     * 右对齐挤在「第几/共几」和任务名中间那点空档里；挤不下就干脆不摆，别盖住别人。 */
    if (trace_cal_held()) {
        int16_t mw, x;

        OLED_SetFont(kk_font_body);
        mw = (int16_t)OLED_GetUTF8Width("丢线");
        x  = (s_task_name_w > 0) ? (int16_t)(122 - s_task_name_w - 4 - mw)
                                 : (int16_t)(122 - mw);
        if (x >= x_min) {
            draw_text(x, 1, kk_font_body, "丢线");
        }
    }

    /* 主体：循迹扫描状态（与「循迹测试」页同一份画法，免得两个页面读数不一样） */
    draw_trace_bars();
}

/* =====================================================================
 * 模块测试：电机 / 舵机 / 蜂鸣器 / 返回（顺序见用户 m13437）
 *   上/下 = 换选中项，中键 = 执行，选到「返回」按中键退出；
 *   「电机」不再是"按住中键才转"，而是中键进独立页 PAGE_CUSTOM_MOTOR，
 *   在那里左右轮各自定方向、退出后继续转（用户 m01540）。
 *   「舵机」同样进独立页 PAGE_CUSTOM_SERVO（用户 m13059）：两路舵机各给一个
 *   0..180° 的角度档，进页面就能一支一支地试"哪根线是几号舵机"。
 * ===================================================================== */
#define TEST_N  4
/* 顺序按用户 m13437 排：电机 / 舵机 / 蜂鸣器 / 返回（蜂鸣器挪到最下面） */
#define TEST_SEL_MOTOR  0U
#define TEST_SEL_SERVO  1U
#define TEST_SEL_BEEP   2U
#define TEST_SEL_BACK   3U
static uint8_t s_test_sel;

static const char *const k_test_names[TEST_N] = { "电机", "舵机", "蜂鸣器", "返回" };

/* 与 KK_UI 原生菜单(MenuDraw)完全同款几何：表头 14px、行高 16px、可见 3 行、
 * 标题居中用 15px 字体、选中是 XOR 反白圆角框、值右对齐到 112。
 * 之前自己排版（行高 12、空心方框、底部提示行）才显得"跟别的都不一样"。 */
#define TEST_HEADER 14
#define TEST_ROW_H  16
#define TEST_VIEW   3

static void draw_test_page(void)
{
    /* 滚动窗口：让选中行始终可见（4 项 > 3 行可见） */
    uint8_t top = (s_test_sel > (TEST_VIEW - 1)) ? (uint8_t)(s_test_sel - (TEST_VIEW - 1)) : 0U;
    int i;

    OLED_SetDrawMode(OLED_DRAW_SET);
    /* 标题用 12px body 字体（与 draw_title() 一致）。原来这里用的是 15px 的
     * kk_font_home，字头压到 y=14 的第一行上——用户原话"模块测试这四个字
     * 有点大盖住了蜂鸣器这一行"（那时第一行是蜂鸣器，现在按 m13437 是电机）。
     * 15px 下沿到 y~15，正好咬住 TEST_HEADER 14。 */
    {
        int16_t w;
        /* 量宽度前必须先把"当前字体"切成 body：OLED_GetUTF8Width 用的是当前字体，
         * 从菜单页切进来时当前字体还是 15px 的 kk_font_home，量出来偏宽 ->
         * 居中算出的 x 偏左（用户："模块测试这四个字不在中间好像"）。 */
        OLED_SetFont(kk_font_body);
        w = OLED_GetUTF8Width("模块测试");
        /* 这里**不能**再 + s_dx：draw_text() 内部已经加过一次（见 :384），
         * 加两次的后果是翻页过渡时标题按 2 倍位移滑进来、落页瞬间又弹回
         * 居中位置——用户看到的就是"进去之后标题会偏一下位置"。 */
        draw_text((int16_t)((128 - w) / 2), 1, kk_font_body, "模块测试");
    }

    for (i = 0; i < TEST_VIEW; i++) {
        uint8_t idx = (uint8_t)(top + (uint8_t)i);
        int16_t y = (int16_t)(TEST_HEADER + i * TEST_ROW_H);
        char tail[14];

        if (idx >= TEST_N) break;

        switch (idx) {
        case TEST_SEL_BEEP: snprintf(tail, sizeof(tail), "响一下"); break;
        /* 电机行尾显示"还在不在转"，不进页面就能看出来（速度档跨页保留） */
        case TEST_SEL_MOTOR: snprintf(tail, sizeof(tail), "%s",
                         (s_motor_level[0] != 0 || s_motor_level[1] != 0) ? "运行" : "停"); break;
        /* 舵机行尾显示两路当前角度（跨页保留），进来之前就知道现在摆在哪 */
        case TEST_SEL_SERVO: snprintf(tail, sizeof(tail), "%d/%d",
                         (int)s_servo_deg[0], (int)s_servo_deg[1]); break;
        default: tail[0] = '\0'; break;
        }

        OLED_SetDrawMode(OLED_DRAW_SET);
        if (idx == TEST_SEL_BACK) {
            /* 「返回」项：和 KK_UI 原生菜单的返回行逐像素同款——
             * 顶点 (107, y+8)、两翼 (110, y+5)/(110, y+11)，是一个 "<"。
             * 原来这里画的是 (110,y+4)-(114,y)-(118,y+4)，朝上的 "︿"，
             * 方向、尺寸、垂直位置三样都不对。 */
            draw_text(8, y, kk_font_body, k_test_names[idx]);
            OLED_DrawLine((int16_t)(110 + s_dx), (int16_t)(y + 5), (int16_t)(107 + s_dx), (int16_t)(y + 8));
            OLED_DrawLine((int16_t)(107 + s_dx), (int16_t)(y + 8), (int16_t)(110 + s_dx), (int16_t)(y + 11));
        } else {
            draw_text(8, y, kk_font_body, k_test_names[idx]);
            if (tail[0] != '\0') {
                int16_t w = OLED_GetUTF8Width(tail);
                draw_text((int16_t)(112 - w), y, kk_font_body, tail);
            }
        }

        if (idx == s_test_sel) {
            OLED_SetDrawMode(OLED_DRAW_XOR);
            OLED_DrawRBox((int16_t)(3 + s_dx), y, 118U, TEST_ROW_H, 3U);
            OLED_SetDrawMode(OLED_DRAW_SET);
        }
    }

    /* 滚动条：4 项 > 可见 3 行，照抄原生菜单 kk_ui_menu.c:402-416 的条件与
     * 几何（轨道 VLine(125,14,48)、滑块 Box(124,thumb_y,3,thumb_h)）。
     * 原来自己排版的这页没有滚动条，看不出"下面还有一项"。 */
    if (TEST_N > TEST_VIEW) {
        uint16_t thumb_h = (uint16_t)(48U * TEST_VIEW / TEST_N);
        uint16_t maximum_first = (uint16_t)(TEST_N - TEST_VIEW);
        uint16_t thumb_y = (uint16_t)(TEST_HEADER +
            (48U - thumb_h) * top / maximum_first);
        OLED_DrawVLine((int16_t)(125 + s_dx), TEST_HEADER, 48U);
        OLED_DrawBox((int16_t)(124 + s_dx), (int16_t)thumb_y, 3U, thumb_h);
    }
}

/* =====================================================================
 * 电机测试页（PAGE_CUSTOM_MOTOR）—— 用户 m01540 首版，m01975 重画，
 *   m02868/m02897 改成"双极性速度滑条 + 清零"。
 *   版式：y=1 标题行：左上「返回」x=1、居中「电机测试」、右上「清零」右对齐 x=126；
 *         y=14 分隔线；下面两行「轮子卡片」（y0 = 16 / 39，行高 23）：
 *           [履带图标] 左/右   实测   [--0--滑条--]   设定值
 *         实测 = 最近 20ms 的编码器计数（满速 ≈ 100，倒转为负，右对齐 x=58）；
 *         滑条 = 双极性：中心 x=80 是 0，往右正、往左负，量程 ±100 ↔ ±满占空比；
 *         设定值 = s_motor_level[]（右对齐 x=126，正数显式带 +）。
 *   选中行用 XOR 空心圆角框圈住（**不填实**，填实会把图标和文字反白掉）；
 *   选中「返回」/「清零」时反白的是标题行那两个字。
 *   操作：上/下 = 换选中项（左 / 右 / 清零 / 返回）；
 *         左/右 = 拖选中那一路的速度滑条（短按 1 格，长按越来越快）；
 *         中键  = 「清零」两路一起归零、「返回」退出。**退出不停电机**（状态跨页保留）。
 * ===================================================================== */
static void draw_motor_page(void)
{
    static const int16_t k_icon_x = 4;
    static const int16_t k_label_x = 24;
    static const int16_t k_meas_right = 60;   /* 实测右对齐：实测最宽是 "-100"=23px
                                               * （tools/font/measure_text.mjs），
                                               * 从 60 往左到 37，不会碰到 x=24 起的「左」「右」 */
    static const int16_t k_bar_x = 62;
    static const int16_t k_bar_w = 37;
    static const int16_t k_bar_zero = 80;
    int i;

    OLED_SetDrawMode(OLED_DRAW_SET);

    /* 标题行：12px body、居中、y=1。draw_text() 内部已经 +s_dx，这里不要再加。 */
    {
        int16_t w;
        OLED_SetFont(kk_font_body);
        w = OLED_GetUTF8Width("电机测试");
        draw_text((int16_t)((128 - w) / 2), 1, kk_font_body, "电机测试");
    }
    draw_text(1, 1, kk_font_body, "返回");      /* 左上角 */
    {
        int16_t w = (int16_t)OLED_GetUTF8Width("清零");
        draw_text((int16_t)(126 - w), 1, kk_font_body, "清零");   /* 右上角 */
    }
    /* 选中的是「返回」/「清零」时，反白标题行那两个字 */
    if (s_motor_sel == MOTOR_SEL_BACK) {
        OLED_SetDrawMode(OLED_DRAW_XOR);
        OLED_DrawRBox((int16_t)(0 + s_dx), 0, 26U, 13U, 4U);
        OLED_SetDrawMode(OLED_DRAW_SET);
    } else if (s_motor_sel == MOTOR_SEL_CLEAR) {
        OLED_SetDrawMode(OLED_DRAW_XOR);
        OLED_DrawRBox((int16_t)(100 + s_dx), 0, 27U, 13U, 4U);
        OLED_SetDrawMode(OLED_DRAW_SET);
    }
    OLED_DrawHLine(0, 14, 128U);

    /* 两行卡片：y0 = 16 / 39，卡内元素全部相对 y0 定位 */
    for (i = 0; i < 2; i++) {
        const int16_t y0 = (int16_t)(16 + 23 * i);
        const int level = (int)s_motor_level[i];
        int v = s_motor_show[i];        /* 实测（编码器窗口计数） */
        int lmag = (level < 0) ? -level : level;
        char buf[8];
        int16_t bw;

        /* 选中行：整行 XOR 空心圆角框（y0-1 起，正好避开 y=14 的分隔线） */
        if (s_motor_sel == (uint8_t)i) {
            OLED_SetDrawMode(OLED_DRAW_XOR);
            OLED_DrawRFrame((int16_t)(1 + s_dx), (int16_t)(y0 - 1), 126U, 23U, 5U);
            OLED_SetDrawMode(OLED_DRAW_SET);
        }

        /* 履带图标：速度档非 0 = 实心（这一路在转），0 = 空心 */
        if (level != 0) {
            OLED_DrawRBox((int16_t)(k_icon_x + s_dx), (int16_t)(y0 + 6), 18U, 10U, 4U);
        } else {
            OLED_DrawRFrame((int16_t)(k_icon_x + s_dx), (int16_t)(y0 + 6), 18U, 10U, 4U);
        }

        /* 左/右（「左轮/右轮」的「轮」不在字模里，只能写「左」「右」） */
        draw_text(k_label_x, (int16_t)(y0 + 5), kk_font_body, (i == 0) ? "左" : "右");

        /* 实测速度：右对齐到 x=58（负号也算进宽度） */
        snprintf(buf, sizeof(buf), "%d", v);
        bw = (int16_t)OLED_GetUTF8Width(buf);
        draw_text((int16_t)(k_meas_right - bw), (int16_t)(y0 + 5), kk_font_body, buf);

        /* 双极性滑条：外框 + 中心 0 刻度线 + 从 0 往两边填的实心条 */
        OLED_DrawRFrame((int16_t)(k_bar_x + s_dx), (int16_t)(y0 + 7),
                        (uint16_t)k_bar_w, 9U, 2U);
        if (lmag > MOTOR_LEVEL_MAX) {
            lmag = MOTOR_LEVEL_MAX;
        }
        if (lmag > 0) {
            int16_t seg = (int16_t)(lmag * 17 / MOTOR_LEVEL_MAX);   /* 单边最多 17px */
            int16_t x = (level > 0) ? k_bar_zero : (int16_t)(k_bar_zero - seg);
            OLED_DrawRBox((int16_t)(x + s_dx), (int16_t)(y0 + 8), (uint16_t)seg, 7U, 1U);
        }
        OLED_DrawVLine((int16_t)(k_bar_zero + s_dx), (int16_t)(y0 + 7), 9U);

        /* 设定值：右对齐到 x=126，正数带 +，便于和左边实测区分 */
        if (level > 0) {
            snprintf(buf, sizeof(buf), "+%d", level);
        } else {
            snprintf(buf, sizeof(buf), "%d", level);
        }
        bw = (int16_t)OLED_GetUTF8Width(buf);
        draw_text((int16_t)(126 - bw), (int16_t)(y0 + 5), kk_font_body, buf);
    }
}

/* =====================================================================
 * 陀螺仪：三维姿态（Q1000 定点，不依赖 libm）
 * ===================================================================== */
static const int16_t k_sin90[91] = { 0,17,35,52,70,87,105,122,139,156,174,191,208,225,242,259,276,292,309,326,342,358,375,391,407,423,438,454,469,485,500,515,530,545,559,574,588,602,616,629,643,656,669,682,695,707,719,731,743,755,766,777,788,799,809,819,829,839,848,857,866,875,883,891,899,906,914,921,927,934,940,946,951,956,961,966,970,974,978,982,985,988,990,993,995,996,998,999,999,1000,1000 };

static int32_t isin_q(int32_t deg)
{
    int32_t d = deg % 360;
    if (d < 0) d += 360;
    if (d <= 90)  return k_sin90[d];
    if (d <= 180) return k_sin90[180 - d];
    if (d <= 270) return -k_sin90[d - 180];
    return -k_sin90[360 - d];
}
static int32_t icos_q(int32_t deg) { return isin_q(deg + 90); }
static int32_t qmul(int32_t a, int32_t b) { return (a * b) / 1000; }

/* 三维线框放屏幕左半边：中心 (GY_CX,GY_CY)，1 个单位 = GY_SC 像素。
 * 可用区：x 2..55（右边 56 起是「横滚/俯仰/偏航」文字列）、y 15..63（上面 14 是标题分隔线）。
 * 投影取旋转后的 X/Y：静止（无转动）时半宽 = 1.4*GY_SC，绕横轴/竖轴转 45° 时最大到
 * 2.42*GY_SC（对角顶点朝观察者）。GY_SC 9->12：静止时 34px 见方（原来 25px），
 * 转 30° 时半幅 23px 仍在框内；只有横滚+俯仰同时超 ~40° 的极端姿态才会顶到标题线，
 * 这里选择"平时尽量大"（用户要求：尽量大一点、不压别的东西）。 */
#define GY_CX      28
#define GY_CY      39
#define GY_SC      12
#define GY_TX      56     /* 右侧文字列左边界 */
#define GY_TRIGHT  126    /* 右侧文字列右边界（数值右对齐到这儿） */

/* 凸四边形填 50% 网点（1bpp 的"阴影面"）。
 * 直接用 OLED_DrawFilledTriangle 填实会得到一个黑方块——轮廓没了、立体感也
 * 没了；隔行填既像实体面，又留住棱线。坐标系跟 gy_box 一样：先投影过的像素。 */
static void gy_fill_quad(const int16_t *px, const int16_t *py, const uint8_t *q)
{
    int16_t ymin = py[q[0]], ymax = py[q[0]];
    int16_t y;
    int i;

    for (i = 1; i < 4; i++) {
        if (py[q[i]] < ymin) ymin = py[q[i]];
        if (py[q[i]] > ymax) ymax = py[q[i]];
    }
    if (ymax < 0 || ymin > 63) return;
    for (y = ymin; y <= ymax; y++) {
        int16_t xa = 0, xb = -1;
        if ((y & 1) != 0) continue;             /* 隔行 = 网点 */
        for (i = 0; i < 4; i++) {
            int16_t ya = py[q[i]];
            int16_t yb = py[q[(i + 1) & 3]];
            /* 半开区间，水平线正好穿过顶点时不会算两遍 */
            if ((y >= ya && y < yb) || (y >= yb && y < ya)) {
                int32_t x = (int32_t)px[q[i]] +
                    (int32_t)(px[q[(i + 1) & 3]] - px[q[i]]) * (int32_t)(y - ya) /
                    (int32_t)(yb - ya);
                if (xb < xa) { xa = (int16_t)x; xb = (int16_t)x; }
                else if (x < xa) xa = (int16_t)x;
                else if (x > xb) xb = (int16_t)x;
            }
        }
        if (xb >= xa) OLED_DrawHLine(xa, y, (uint16_t)(xb - xa + 1));
    }
}

/* 一个长方体：坐标单位是 1/100 单位，1 单位 = GY_SC 像素。
 * 8 个角点用和车身同一个旋转矩阵投影。
 * 用户 m02820："只有线条容易看错"——所以这里做两件事让它是"实体"而不是铁丝：
 *   1) 消隐：只画朝向观察者的那 3 个面的棱，背面的棱不画；
 *   2) 把车顶面（车翻了就是车底面）填成网点，一眼就能看出"车的上方朝哪"。 */
static void gy_box(int32_t m00, int32_t m01, int32_t m02,
                   int32_t m10, int32_t m11, int32_t m12,
                   int32_t cx, int32_t cy, int32_t cz,
                   int32_t sx, int32_t sy, int32_t sz)
{
    static const int8_t sg[8][3] = {
        {-1,-1,-1},{ 1,-1,-1},{ 1, 1,-1},{-1, 1,-1},
        {-1,-1, 1},{ 1,-1, 1},{ 1, 1, 1},{-1, 1, 1}
    };
    static const uint8_t ed[12][2] = {
        {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},{0,4},{1,5},{2,6},{3,7}
    };
    /* 6 个面各自的 4 个角点（逆时针，从面外看） */
    static const uint8_t fc[6][4] = {
        {4,5,6,7},   /* 0: +Z 车顶 */
        {0,3,2,1},   /* 1: -Z 车底 */
        {0,1,5,4},   /* 2: -Y */
        {3,7,6,2},   /* 3: +Y */
        {0,4,7,3},   /* 4: -X */
        {1,2,6,5}    /* 5: +X */
    };
    /* 各面外法线（车体坐标） */
    static const int8_t fn[6][3] = {
        {0,0,1},{0,0,-1},{0,-1,0},{0,1,0},{-1,0,0},{1,0,0}
    };
    int16_t px[8], py[8];
    int32_t nz[6];
    uint8_t vis[12] = {0};
    int32_t m20, m21, m22;
    int i, f;

    /* 旋转矩阵第三行 = 前两行的叉积 = 观察者方向在车体坐标下的分量。
     * 投影只取旋转后的 X/Y（俯视），所以法线的 Z 分量 > 0 就是"朝向我"。 */
    m20 = (m01 * m12 - m02 * m11) / 1000;
    m21 = (m02 * m10 - m00 * m12) / 1000;
    m22 = (m00 * m11 - m01 * m10) / 1000;

    for (i = 0; i < 8; i++) {
        int32_t x = cx + (int32_t)sg[i][0] * sx;
        int32_t y = cy + (int32_t)sg[i][1] * sy;
        int32_t z = cz + (int32_t)sg[i][2] * sz;
        int32_t X = x * m00 + y * m01 + z * m02;
        int32_t Y = x * m10 + y * m11 + z * m12;
        /* 坐标是 1/100 单位、旋转矩阵是 Q1000：
         * X/1000 = 像素基准数，再 *GY_SC/100 才是像素。
         * 原来写成 (X/100)*GY_SC/10，等于把结果放大 100 倍 -> 整车跑到屏外，
         * 屏幕上"黑乎乎啥也没有"。 */
        px[i] = (int16_t)(GY_CX + s_dx + (X / 1000) * GY_SC / 100);
        py[i] = (int16_t)(GY_CY - (Y / 1000) * GY_SC / 100);
    }

    for (f = 0; f < 6; f++) {
        nz[f] = m20 * fn[f][0] + m21 * fn[f][1] + m22 * fn[f][2];
    }

    /* 车顶（车翻了就退化成车底）填网点 */
    f = (nz[0] > 0) ? 0 : ((nz[1] > 0) ? 1 : -1);
    if (f >= 0) {
        gy_fill_quad(px, py, fc[f]);
    }

    /* 消隐：一条棱只要属于任意一个朝向观察者的面，就是可见的 */
    for (f = 0; f < 6; f++) {
        int k;
        if (nz[f] <= 0) continue;
        for (i = 0; i < 4; i++) {
            uint8_t a = fc[f][i];
            uint8_t b = fc[f][(i + 1) & 3];
            for (k = 0; k < 12; k++) {
                if ((ed[k][0] == a && ed[k][1] == b) ||
                    (ed[k][0] == b && ed[k][1] == a)) {
                    vis[k] = 1U;
                    break;
                }
            }
        }
    }
    for (i = 0; i < 12; i++) {
        if (vis[i]) {
            OLED_DrawLine(px[ed[i][0]], py[ed[i][0]], px[ed[i][1]], py[ed[i][1]]);
        }
    }
}

/* 把 IMU 原始角映射成画面用的（车身横滚 r / 车身俯仰 p），见文件顶部 s_gyro_axis_mode
 * 的位定义。yaw 不走这里（四/八档共用，偏航符号本身不受轴映射影响）。 */
static void gyro_view_angles(int32_t *r, int32_t *p)
{
    int32_t a;   /* 画面横滚的源 */
    int32_t b;   /* 画面俯仰的源 */

    if ((s_gyro_axis_mode & 1U) != 0U) {
        a = g_live.roll_x10;    /* 换轴：横滚量的是传感器 pitch 轴，俯仰量 roll 轴 */
        b = g_live.pitch_x10;
    } else {
        a = g_live.pitch_x10;
        b = g_live.roll_x10;
    }
    if ((s_gyro_axis_mode & 2U) != 0U) a = -a;
    if ((s_gyro_axis_mode & 4U) != 0U) b = -b;

    *r = a;
    *p = b;
}

static void draw_gyro_page(void)
{
    int32_t r, p, y, broll, bpitch, byaw;
    int32_t cr, sr, cp, sp, cy, sy;
    int32_t m00, m01, m02, m10, m11, m12;

    /* 方框用的 r/p 由 gyro_view_angles() 按 s_gyro_axis_mode 的 8 种映射给出；
     * 右侧那三行数字仍用 gyro_angles() 的 broll/bpitch/byaw（取反是用户 m01903
     * 要的读数习惯，与方框的映射互不影响）。 */
    gyro_angles(&broll, &bpitch, &byaw);
    gyro_view_angles(&r, &p);
    r /= 10; p /= 10; y = byaw / 10;

    cr = icos_q(r); sr = isin_q(r);
    cp = icos_q(p); sp = isin_q(p);
    cy = icos_q(y); sy = isin_q(y);
    m00 = qmul(cy, cp);
    m01 = qmul(cy, qmul(sp, sr)) - qmul(sy, cr);
    m02 = qmul(cy, qmul(sp, cr)) + qmul(sy, sr);
    m10 = qmul(sy, cp);
    m11 = qmul(sy, qmul(sp, sr)) + qmul(cy, cr);
    m12 = qmul(sy, qmul(sp, cr)) - qmul(cy, sr);

    draw_title("陀螺仪");

    /* 右上角显示当前轴向档位（纯 ASCII，字模子集里一定有）：
     * 用户按中键切档，哪一档动起来跟真车一致就用哪一档，然后报给我固化成默认。 */
    {
        char mb[4];
        mb[0] = 'M';
        mb[1] = (char)('1' + s_gyro_axis_mode);
        mb[2] = '\0';
        draw_text(112, 1, kk_font_body, mb);
    }

    OLED_SetDrawMode(OLED_DRAW_SET);

    /* 右侧三行、每行一个轴，用全名「横滚 / 俯仰 / 偏航」，数值右对齐。
     * 行距 14px（y=18/32/46）。标签 2 个汉字约 24px（56..80），数值最多 5 个
     * 字符（fmt_x10_compact 在 |角度|>=100.0 时省掉小数）约 40px
     * （126-40=86 起），两列永远不重叠。 */
    if (g_live.gyro_ok) {
        static const char *k_axis_name[3] = { "横滚", "俯仰", "偏航" };
        int32_t axis_val[3];
        char b[16];
        int i;

        axis_val[0] = broll;
        axis_val[1] = bpitch;
        axis_val[2] = byaw;
        for (i = 0; i < 3; i++) {
            int16_t row_y = (int16_t)(18 + i * 14);
            int16_t w;

            fmt_x10_compact(b, sizeof(b), axis_val[i]);
            draw_text(GY_TX, row_y, kk_font_body, k_axis_name[i]);
            w = OLED_GetUTF8Width(b);
            draw_text((int16_t)(GY_TRIGHT - w), row_y, kk_font_body, b);
        }
    } else {
        int16_t w = OLED_GetUTF8Width("无信号");
        draw_text((int16_t)(GY_TX + (GY_TRIGHT - GY_TX - w) / 2), 32, kk_font_body, "无信号");
    }

    /* 正方体线框放最后画，而且只允许画在「标题线以下 + 文字列左边」那块窗口里。
     * 用户 m02448：方框放大后和标题栏重叠了，要求标题栏盖住方框。
     * 原因：投影外接半径最大 2.42*GY_SC≈29px（组合姿态时），会顶到 y=14 的标题
     * 分隔线和 x=56 的文字列；这里用裁剪窗口把方框切住，尺寸就不用缩回去。
     * 窗口跟着页面过渡位移 s_dx 走，并且要跟调用者 KK_UI_CustomOnDraw()
     * 给的页窗口 s_clip_x/s_clip_w 求交集（kk_oled 的 SetClipWindow 是替换
     * 不是求交，直接设会把翻页窗口顶掉 → 方框画到隔壁页上、留下残影）；
     * 画完恢复调用者的窗口，而不是 ResetClipWindow 整屏。
     * 车头朝 +X，所以 yaw 增大 = 车头往左转（m01247 要求用正方体，不是小车）。 */
    {
        int32_t cx0 = 2 + s_dx;
        int32_t cx1 = 55 + s_dx;
        int32_t px0 = (int32_t)s_clip_x;
        int32_t px1 = (int32_t)s_clip_x + (int32_t)s_clip_w;
        if (cx0 < px0) cx0 = px0;
        if (cx1 > px1) cx1 = px1;
        if (cx0 < 0) cx0 = 0;
        if (cx1 > 128) cx1 = 128;
        if (cx1 > cx0) {
            OLED_SetDrawMode(OLED_DRAW_SET);
            OLED_SetClipWindow((int16_t)cx0, 15, (uint16_t)(cx1 - cx0), 49U);
        }
        gy_box(m00, m01, m02, m10, m11, m12, 0, 0, 0, 140, 140, 140);
        OLED_SetClipWindow(s_clip_x, 0, s_clip_w, 64U);
    }
}

/* =====================================================================
 * 舵机测试页（PAGE_CUSTOM_SERVO）—— 用户 m13059："模块测试里加一个舵机的调参
 *   菜单，我可以设定给不同舵机角度位置"，用来认出车上哪根线接的是几号舵机。
 *
 *   两路舵机（`servo.c` 只实现了 1、2 号：TIM1_CH1/CH2 = PA8/PA9，50Hz，
 *   CCR 就是脉宽 us）各自一个 0..180° 的角度档 s_servo_deg[]，改一下立刻
 *   Servo_SetAngle() 发下去（不缓动，直接到位 —— 用户要的就是"给个位置看它动"）。
 *   **退出页面不回零、不停**：保持最后角度，跟电机测试页一个规矩。
 *
 *   操作（照抄电机测试页的按键机：kk_ui 只有三键，左/右被折成 上/下，
 *   所以四个方向必须在 KK_UI_CustomOnTick 里读五向键原始位图，见 servo_key_scan()）：
 *     上/下 = 换选中项（舵机1 / 舵机2 / 收回 / 动作预设 / 返回）
 *     左/右 = 拖选中那一项的角度：短按 1°，长按越按越快（1/2/5/10/20）；
 *             选到「动作预设」时左/右按 ±1 号在 1..14 号动作间循环，
 *             切一次就把该动作的**终点姿态**立刻下发，并同步上面两行的数字
 *             （用户 m13303）；
 *             选到「收回」「返回」（同在标题行）时，左/右在两项间切换
 *     中键  = 「收回」两路一起回 0°（= 旧动作表第 1 个动作「收回」，用户 m13266
 *             现场确认 0° 正好就是收回位置、两路也没接反）；
 *             「动作预设」= 完整播放该动作（5/9 号含中间过渡，会阻塞 0.2~0.5s）；
 *             「返回」退出页面
 * ===================================================================== */
#define SERVO_TEST_N      5
#define SERVO_SEL_1       0U
#define SERVO_SEL_2       1U
#define SERVO_SEL_RETRACT  2U
#define SERVO_SEL_ACTION  3U
#define SERVO_SEL_BACK    4U

#define SERVO_DEG_MAX     180
#define SERVO_DEG_RETRACT  0   /* 「收回」= 0°（旧动作表 Servo_Move(1)：舵1=0、舵2=0） */

/* 位序同 board.h 的 keys_raw()：bit0上 bit1下 bit2左 bit3右 bit4中 */
#define SERVO_KEY_UP      0
#define SERVO_KEY_DOWN    1
#define SERVO_KEY_LEFT    2
#define SERVO_KEY_RIGHT   3
#define SERVO_KEY_N       5

#define SERVO_KEY_DEBOUNCE_MS   20U
#define SERVO_KEY_FIRST_MS     600U
#define SERVO_KEY_REPEAT_MS    120U
#define SERVO_STEP_2_MS       1200U
#define SERVO_STEP_5_MS       2000U
#define SERVO_STEP_10_MS      3000U

/* 几何与模块测试页同款：表头 14px、行高 16px、可见 3 行 */
#define SERVO_HEADER  14
#define SERVO_ROW_H   16
#define SERVO_VIEW     3

static uint8_t  s_servo_sel;            /* 0=舵机1 1=舵机2 2=收回 3=动作预设 4=返回 */
static uint8_t  s_servo_action = 1U;    /* 「动作预设」当前选中的动作号 1..SERVO_ACTION_N */
/* s_servo_deg[2] 声明在文件上面（模块测试页要用），这里不再重复声明 */

static uint8_t  s_skey_raw;
static uint8_t  s_skey_stable;
static uint32_t s_skey_changed;
static uint32_t s_skey_hold_at[SERVO_KEY_N];
static uint32_t s_skey_repeat_at[SERVO_KEY_N];

static const char *const k_servo_names[SERVO_TEST_N] = { "舵机1", "舵机2", "收回", "动作预设", "返回" };

/* 把两个角度档发给舵机（1、2 号；Servo_SetAngle 内部会换算脉宽 + 叠中位偏差） */
static void servo_test_apply(void)
{
    Servo_SetAngle(1, (float)s_servo_deg[0]);
    Servo_SetAngle(2, (float)s_servo_deg[1]);
}

/* 把「动作预设」的第 s_servo_action 号动作下发（只走终点、不含 5/9 号的中间延时，
 * 界面不会被 HAL_Delay 卡住），并把上面舵机1/舵机2 两行的数字同步成这个动作的
 * 终点姿态 —— 用户 m13303 要的就是"切换了动作，上面的舵机1/舵机2 也跟着变"。 */
static void servo_action_apply(void)
{
    int a1 = 0;
    int a2 = 0;

    Servo_ApplyAction((int)s_servo_action);
    Servo_ActionPose((int)s_servo_action, &a1, &a2);
    s_servo_deg[0] = (int16_t)a1;
    s_servo_deg[1] = (int16_t)a2;
}

/* 「动作预设」行上的左/右：动作号 ±1 循环（只有 14 项，长按也只走 1 号） */
static bool servo_action_nudge(int delta)
{
    int v = (int)s_servo_action + delta;

    while (v < 1)             v += SERVO_ACTION_N;
    while (v > SERVO_ACTION_N) v -= SERVO_ACTION_N;
    if (v == (int)s_servo_action) {
        return false;
    }
    s_servo_action = (uint8_t)v;
    servo_action_apply();
    return true;
}

/* 连发步进：按住越久、一次走几度（与电机页同表） */
static int servo_key_step(uint32_t held)
{
    if (held < SERVO_KEY_FIRST_MS) return 1;
    if (held < SERVO_STEP_2_MS)    return 2;
    if (held < SERVO_STEP_5_MS)    return 5;
    if (held < SERVO_STEP_10_MS)   return 10;
    return 20;
}

/* 拖选中那一路的角度（夹在 0..180；真的变了才发舵机 + 重画）。
 * 注意：**不在这里做缓动** —— 用户要的是立刻看到它转到那个位置。 */
static bool servo_deg_nudge(uint8_t which, int delta)
{
    int v;

    if (which > SERVO_SEL_2) {
        return false;   /* 「收回」/「返回」行：左/右由 servo_key_press() 换成换项 */
    }
    v = (int)s_servo_deg[which] + delta;
    if (v > SERVO_DEG_MAX) v = SERVO_DEG_MAX;
    if (v < 0)             v = 0;
    if (v == (int)s_servo_deg[which]) {
        return false;
    }
    s_servo_deg[which] = (int16_t)v;
    servo_test_apply();
    return true;
}

/* 一个方向键按下（或连发）：上/下换选中项，左/右拖角度 */
static bool servo_key_press(int key, int step)
{
    switch (key) {
    case SERVO_KEY_UP:
        s_servo_sel = (uint8_t)((s_servo_sel + SERVO_TEST_N - 1U) % SERVO_TEST_N);
        return true;
    case SERVO_KEY_DOWN:
        s_servo_sel = (uint8_t)((s_servo_sel + 1U) % SERVO_TEST_N);
        return true;
    case SERVO_KEY_LEFT:
    case SERVO_KEY_RIGHT:
        if (s_servo_sel == SERVO_SEL_ACTION) {
            return servo_action_nudge((key == SERVO_KEY_LEFT) ? -1 : 1);
        }
        if (s_servo_sel == SERVO_SEL_BACK || s_servo_sel == SERVO_SEL_RETRACT) {
            uint8_t want = (key == SERVO_KEY_LEFT) ? SERVO_SEL_BACK : SERVO_SEL_RETRACT;
            if (s_servo_sel == want) {
                return false;   /* 已经在那一头（长按连发不重复重画） */
            }
            s_servo_sel = want;
            return true;
        }
        return servo_deg_nudge(s_servo_sel, (key == SERVO_KEY_LEFT) ? -step : step);
    default:
        return false;
    }
}

/* 扫描原始按键：新按下走一步，按住连发。返回 true = 需要重画。
 * 与 motor_key_scan() 同款（20ms 消抖；中键仍走 kk_ui 的 OK 事件，这里
 * 即使把 bit4 也扫进来，servo_key_press(4,..) 走 default 返回 false，无害）。 */
static bool servo_key_scan(uint32_t now)
{
    uint8_t raw = app_ui_raw_keys();
    bool redraw = false;
    int i;

    if (raw != s_skey_raw) {
        s_skey_raw = raw;
        s_skey_changed = now;
        return false;           /* 刚变化：等消抖 */
    }
    if ((uint32_t)(now - s_skey_changed) < SERVO_KEY_DEBOUNCE_MS) {
        return false;
    }
    for (i = 0; i < SERVO_KEY_N; i++) {
        uint8_t bit = (uint8_t)(1U << i);

        if ((raw & bit) == 0U) {
            continue;           /* 没按 */
        }
        if ((s_skey_stable & bit) == 0U) {
            /* 新按下（已消抖）：先走一步 —— 短按就是这一步 */
            s_skey_hold_at[i] = now;
            s_skey_repeat_at[i] = now + SERVO_KEY_FIRST_MS;
            if (servo_key_press(i, 1)) {
                redraw = true;
            }
        } else if ((int32_t)(now - s_skey_repeat_at[i]) >= 0) {
            /* 还按着且到了连发点：步进随按住时长变大 */
            s_skey_repeat_at[i] = now + SERVO_KEY_REPEAT_MS;
            if (servo_key_press(i, servo_key_step(now - s_skey_hold_at[i]))) {
                redraw = true;
            }
        }
    }
    s_skey_stable = raw;
    return redraw;
}

/* 每一项占几行：「动作预设」占 2 行（第 1 行 = 动作预设 n/14，第 2 行 = 动作名），
 * 其余各占 1 行 —— 用户 m13356 明确要求名字跟标题**同时**出现、一起滚动，
 * 不能是"滑过去才露出来"（所以这里做成同一项的两行，而不是列表里再插一项）。 */
static uint8_t servo_view_rows(uint8_t idx)
{
    return (idx == SERVO_SEL_ACTION) ? 2U : 1U;
}

/* 可见窗口的起始项：从选中项往前贪心塞满 SERVO_VIEW 行 —— 保证选中项
 * **完整**可见（动作预设那 2 行不会只露一半）。 */
static uint8_t servo_view_first(uint8_t sel)
{
    uint8_t rows = servo_view_rows(sel);
    uint8_t first = sel;

    while (first > 0U) {
        uint8_t prev = (uint8_t)(first - 1U);
        uint8_t need = servo_view_rows(prev);

        if ((uint8_t)(rows + need) > SERVO_VIEW) {
            break;
        }
        rows = (uint8_t)(rows + need);
        first = prev;
    }
    return first;
}

/* 选中项之前一共占了几行（滚动条位置用） */
static uint8_t servo_view_rows_before(uint8_t idx)
{
    uint8_t i;
    uint8_t n = 0U;

    for (i = 0U; i < idx; i++) {
        n = (uint8_t)(n + servo_view_rows(i));
    }
    return n;
}

static void draw_servo_page(void)
{
    uint8_t first = servo_view_first(s_servo_sel);
    uint8_t idx = first;
    int y = SERVO_HEADER;

    OLED_SetDrawMode(OLED_DRAW_SET);
    {
        int16_t w;
        OLED_SetFont(kk_font_body);     /* 量宽度前先切字体（同模块测试页） */
        w = OLED_GetUTF8Width("舵机测试");
        draw_text((int16_t)((128 - w) / 2), 1, kk_font_body, "舵机测试");
    }

    while (idx < SERVO_TEST_N) {
        uint8_t rows = servo_view_rows(idx);
        uint16_t h = (uint16_t)((uint16_t)rows * SERVO_ROW_H);
        char tail[12];

        if ((y - SERVO_HEADER) + (int)h > (SERVO_VIEW * SERVO_ROW_H)) {
            break;      /* 放不下就停（选中项由 servo_view_first() 保证放得下） */
        }

        switch (idx) {
        case SERVO_SEL_1:      snprintf(tail, sizeof(tail), "%d", (int)s_servo_deg[0]); break;
        case SERVO_SEL_2:      snprintf(tail, sizeof(tail), "%d", (int)s_servo_deg[1]); break;
        case SERVO_SEL_RETRACT: snprintf(tail, sizeof(tail), "%d/%d", SERVO_DEG_RETRACT, SERVO_DEG_RETRACT); break;
        case SERVO_SEL_ACTION:  snprintf(tail, sizeof(tail), "%d/%d", (int)s_servo_action, SERVO_ACTION_N); break;
        default:               tail[0] = '\0'; break;
        }

        OLED_SetDrawMode(OLED_DRAW_SET);
        if (idx == SERVO_SEL_BACK) {
            /* 「返回」项：与 KK_UI 原生菜单返回行逐像素同款（同模块测试页） */
            draw_text(8, (int16_t)y, kk_font_body, k_servo_names[idx]);
            OLED_DrawLine((int16_t)(110 + s_dx), (int16_t)(y + 5), (int16_t)(107 + s_dx), (int16_t)(y + 8));
            OLED_DrawLine((int16_t)(107 + s_dx), (int16_t)(y + 8), (int16_t)(110 + s_dx), (int16_t)(y + 11));
        } else {
            draw_text(8, (int16_t)y, kk_font_body, k_servo_names[idx]);
            if (tail[0] != '\0') {
                int16_t w = OLED_GetUTF8Width(tail);
                draw_text((int16_t)(112 - w), (int16_t)y, kk_font_body, tail);
            }
            if (idx == SERVO_SEL_ACTION) {
                /* 第 2 行：当前动作名（缩进 6px，和上一行的数字区分开） */
                draw_text(14, (int16_t)(y + SERVO_ROW_H), kk_font_body, Servo_ActionName((int)s_servo_action));
            }
        }

        if (idx == s_servo_sel) {
            OLED_SetDrawMode(OLED_DRAW_XOR);
            OLED_DrawRBox((int16_t)(3 + s_dx), (int16_t)y, 118U, h, 3U);
            OLED_SetDrawMode(OLED_DRAW_SET);
        }

        y += (int)h;
        idx++;
    }

    /* 滚动条：总行数 > 可见 3 行（几何同原生菜单/模块测试页） */
    if (servo_view_rows_before(SERVO_TEST_N) > SERVO_VIEW) {
        uint16_t total = (uint16_t)servo_view_rows_before(SERVO_TEST_N);
        uint16_t thumb_h = (uint16_t)(48U * SERVO_VIEW / total);
        uint16_t maximum_first = (uint16_t)(total - SERVO_VIEW);
        uint16_t first_row = (uint16_t)servo_view_rows_before(first);
        uint16_t thumb_y = (uint16_t)(SERVO_HEADER +
            (48U - thumb_h) * (first_row > maximum_first ? maximum_first : first_row) / maximum_first);
        OLED_DrawVLine((int16_t)(125 + s_dx), SERVO_HEADER, 48U);
        OLED_DrawBox((int16_t)(124 + s_dx), (int16_t)thumb_y, 3U, thumb_h);
    }
}

/* =====================================================================
 * 电流检测（PAGE_CUSTOM_POWER）—— INA226 测 12V 主线的电压/电流/功率
 *   INA226 挂在 I2C1 上，和 OLED 共用总线：只在"屏不忙"时读，200ms 一次；
 *   数值真的变了才返回 true 置脏重绘（整屏 8 页 @400kHz 要 23ms，别乱刷）。
 * ===================================================================== */
static int32_t  s_pwr_mv, s_pwr_ma, s_pwr_mw;
static uint8_t  s_pwr_ok;
static uint8_t  s_pwr_sat;   /* 1 = 分流输入顶到满量程，电流/功率是钳位值 */
static uint32_t s_pwr_last;

/* 千分之一单位（mV/mA/mW）-> 文本：|值| >= 1000 显示 "12.34V"，
 * 否则显示 "856mA"。符号自己处理（负余数会打出 "-12.-3" 那种）。 */
static void fmt_milli(char *buf, size_t n, int32_t milli,
                      const char *unit_big, const char *unit_small)
{
    int32_t a = (milli < 0) ? -milli : milli;
    const char *sign = (milli < 0) ? "-" : "";

    if (a >= 1000) {
        /* 四舍五入到 1/100，并自己处理进位：12999mV 要显示 13.00V，
         * 不能靠 "%02ld" 打印出 12.100V。 */
        int32_t whole = a / 1000;
        int32_t frac  = (a % 1000 + 5) / 10;
        if (frac >= 100) { frac -= 100; whole++; }
        snprintf(buf, n, "%s%ld.%02ld%s", sign, (long)whole,
                 (long)frac, unit_big);
    } else {
        snprintf(buf, n, "%s%ld%s", sign, (long)a, unit_small);
    }
}

/* 在字符串前插一个字符（这里用 '!' 表示"这个数是钳位值，不是真值"）。*/
static void str_prefix(char *buf, size_t n, char c)
{
    size_t len = strlen(buf);
    if (len + 1u < n) {
        memmove(buf + 1, buf, len + 1u);
        buf[0] = c;
    }
}

static void draw_power_page(void)
{
    static const char *k_name[3] = { "电流", "电压", "功率" };
    char v[3][16];
    int i;

    draw_title("电流检测");

    if (s_pwr_ok) {
        fmt_milli(v[0], sizeof(v[0]), s_pwr_ma, "A", "mA");
        fmt_milli(v[1], sizeof(v[1]), s_pwr_mv, "V", "mV");
        fmt_milli(v[2], sizeof(v[2]), s_pwr_mw, "W", "mW");
        if (s_pwr_sat) {
            /* 分流输入满量程 -> 这两行是钳位值，前面加 '!' 表示不可信 */
            str_prefix(v[0], sizeof(v[0]), '!');
            str_prefix(v[2], sizeof(v[2]), '!');
        }
    } else {
        for (i = 0; i < 3; i++) {
            v[i][0] = '-'; v[i][1] = '-'; v[i][2] = '-'; v[i][3] = '\0';
        }
    }

    for (i = 0; i < 3; i++) {
        int16_t y = (int16_t)(20 + i * 15);
        int16_t w;

        draw_text(4, y, kk_font_body, k_name[i]);
        w = OLED_GetUTF8Width(v[i]);
        draw_text((int16_t)(122 - w), y, kk_font_body, v[i]);
    }
}

/* =====================================================================
 * 电池电量（用户 m08669）
 * 12V 电池包 = 3S（6 节 18650，3 串 2 并）：满 12.6V、没电 9.0V（3.0V/节）。
 * 电压取 INA226 的母线电压（就是 12V 主线，和「电流」页同一颗芯片）。
 * ===================================================================== */
#define BAT_POLL_MS    1000U    /* 1s 读一次；电量是慢量，读快了只是白占 I2C */
#define BAT_EMA_SHIFT  3U       /* 1/8 一阶低通（τ≈8s）：电机一冲母线掉 0.3~0.8V，别跟着跳 */
#define BAT_CUR_EMA_SHIFT 5U    /* 电流 1/32 低通（1s 一次 ⇒ τ≈32s ≈ "最近一分钟平均"） */
#define BAT_PACK_MAH   5000U    /* 6 节 18650 = 3S2P，用户 m08812 确认是 2×2500mAh */
#define BAT_USABLE_PCT   60U    /* 容量只按 6 折算（用户 m09509 要"保守一点"）：标称 5000 里
                                 * 真能用的按 3000mAh 算 —— 覆盖电芯虚标/老化 + 大电流下容量缩水 */
#define BAT_RUN_MIN_MA 1500U    /* "还能跑多久"按**车在跑**算：电流低于这个数也按这个数。
                                 * 实测（2026-10-02，车架起来、两轮全速 = 目标 7200/7200、
                                 * PWM 输出 7200、INA226 1Hz τ≈32s 低通收敛）：**空转稳态只有
                                 * 426mA**（母线 11.43V，轮速 390~440mm/s；静止基线 95mA）。
                                 * 也就是说电机本身并不费电，费电的是地面上顶摩擦/加速/转向，
                                 * 所以这里不取实测空转值，按它的 ~3.5 倍保守取 1.5A —— 取小了
                                 * 剩余时间会乐观得没人信（用户 m09509）。等有地面实测电流再回来改。 */
#define BAT_SOC_FLOOR    15U    /* 放到 15% 以下车已经跑不动了（≈3.45V/节），不算进剩余时间 */

static int32_t  s_bat_mv;       /* 低通后的母线电压 mV（0 = 还没读到过） */
static int32_t  s_bat_ma;       /* 低通后的母线电流 mA（τ≈32s） */
static int16_t  s_bat_run;      /* 预计还能跑几分钟（-1 = 估不出来） */
static uint8_t  s_bat_pct;      /* 0..100，显示用 */
static uint8_t  s_bat_ok;
static uint8_t  s_bat_ma_ok;
static uint32_t s_bat_last;

/* 3S 锂电的**开路**电压 → 剩余量。用户 m08812 要求"不要线性、要用网上已有的固定
 * 结论"，所以整表照抄公开的锂电静止电压-SoC 表（Battery Skills《Lithium Battery
 * Voltage Chart》，https://batteryskills.com/lithium-battery-voltage-chart/）：
 * 单节 4.20/4.15/4.11/4.08/4.02/3.98/3.95/3.91/3.87/3.82/3.78/3.74/3.70/3.66/
 *      3.62/3.58/3.54/3.49/3.42/3.30/3.00 V ↔ 100/95/90/85/80/75/70/65/60/55/
 *      50/45/40/35/30/25/20/15/10/5/0%。
 * 整表 5% 一档：中段平台（3.6~3.9V）特征才带得出来 —— 老版 10 点表是手写的，
 * 陡缓关系不对（用户就是嫌它"线性"）。
 *
 * **电压轴按用户实测重锚（用户 m03190）**：这块电池包充满只有 **12.2V** 左右，
 * 不是公开表假设的 12.6V —— 「只要电压超过 12.2，基本上就是满电」。所以把同一张
 * 曲线的电压轴从 [12.6, 9.0] 线性压到 **[12.2, 9.0]**（下端 9.0V=3.0V/节 不动）：
 *     v_new = 9000 + (v_old - 9000) * 3200 / 3600
 * 于是 ≥12.2V 一律显示 100%，而 12.1V 附近显示 90% 出头（老表那里只有 82%，
 * 明明满电却像掉了两成 —— 用户就是看到这个才提的）。
 *
 * 说明：这是**开路**电压表，带载时读数偏低（大电流下偏低 0.5V 很正常）⇒ 显示的电量
 * 只会**偏小**，不会偏大，方向上是对的。要精确得做库仑积分。
 * 上位机 `tools/console/web/src/lib/battery.ts` 是同一张表（控制台也显示百分比），
 * `battery.test.ts` 会解析本文件核对两边没有各改各的。 */
static const uint16_t kBv[21] = {
    12200, 12070, 11960, 11880, 11720, 11610, 11530, 11430, 11320, 11190, 11080,
    10970, 10870, 10760, 10650, 10550, 10440, 10310, 10120,  9800,  9000
};
static const uint8_t  kBp[21] = {
    100, 95, 90, 85, 80, 75, 70, 65, 60, 55, 50,
     45, 40, 35, 30, 25, 20, 15, 10,  5,  0
};

static uint8_t bat_pct_from_mv(int32_t mv)
{
    uint32_t i;

    if (mv >= (int32_t)kBv[0]) {
        return 100U;
    }
    for (i = 1U; i < (sizeof(kBv) / sizeof(kBv[0])); i++) {
        if (mv >= (int32_t)kBv[i]) {
            int32_t span = (int32_t)kBv[i - 1] - (int32_t)kBv[i];
            int32_t dp   = (int32_t)kBp[i - 1] - (int32_t)kBp[i];
            return (uint8_t)((int32_t)kBp[i] + (dp * (mv - (int32_t)kBv[i]) / span));
        }
    }
    return 0U;
}

/* 每秒读一次 INA226（与电流页共用 I2C1，所以同样要等屏不忙）。
 * 电压或百分比变了就把 *changed 置真，让上层重画。 */
static void battery_poll(uint32_t now_ms, bool *changed)
{
    int32_t mv;
    int32_t ma;
    int32_t run;
    uint8_t pct;
    uint8_t vok;
    uint8_t iok;

    if (OLED_IsBusy() || (now_ms - s_bat_last) < BAT_POLL_MS) {
        return;
    }
    s_bat_last = now_ms;

    /* I2C 总线看门狗：上面刚判过 OLED_IsBusy()，这里总线一定是空闲的。
     * 连续读不到 INA226（比如总线被拉死）就做一次恢复，不然电池那栏会一直 "---"。 */
    (void)board_i2c_watchdog(1U);

    vok = (uint8_t)((ina226_read_bus_voltage_mv(&mv) == HAL_OK) ? 1U : 0U);
    board_i2c_report(vok);
    if (!vok || mv <= 0) {
        if (s_bat_ok != 0U) {           /* 读失败：显示 --- ，不谎报旧值 */
            s_bat_ok = 0U;
            *changed = true;
        }
        return;
    }

    if (s_bat_mv == 0) {
        s_bat_mv = mv;                  /* 第一次直接采用，别从 0 慢慢爬上来 */
    } else {
        s_bat_mv += (mv - s_bat_mv) >> BAT_EMA_SHIFT;
    }

    /* 电流（用户 m08812 要的"预计还能跑多久"）：同一次 I2C 里顺手读回来，
     * 低通成"最近一分钟平均"（τ≈32s）。读不到就置不 ok，剩余时间写 --。 */
    iok = (uint8_t)((ina226_read_current_ma(&ma) == HAL_OK) ? 1U : 0U);
    board_i2c_report(iok);
    if (iok && ma >= 0) {
        if (s_bat_ma_ok == 0U) {
            s_bat_ma = ma;              /* 第一次直接采用 */
        } else {
            s_bat_ma += (ma - s_bat_ma) >> BAT_CUR_EMA_SHIFT;
        }
        s_bat_ma_ok = 1U;
    } else {
        s_bat_ma_ok = 0U;
    }

    pct = bat_pct_from_mv(s_bat_mv);

    /* 剩余时间 = 可用容量 × 可用电量 ÷ 跑起来的电流，向下取整到 5 分。**保守**体现在
     * （用户 m09509 嫌原来 16H39M 太长，这一版全部往短里算）：
     *   ①容量 = 标称 × BAT_USABLE_PCT/100（6 折）；
     *   ②电量先扣掉 BAT_SOC_FLOOR（放到 15% 以下车已经跑不动了，那一段不算数）；
     *   ③电流用低通值，但**下限 BAT_RUN_MIN_MA**：车停着只有 100mA 也别按 100mA 算，
     *      否则显示十几小时没人信（"还能跑多久"问的是跑起来能跑多久）；
     *   ④取整只往下取。读不到电流 / 电量已经到地板 ⇒ -- 或 0min。 */
    if (s_bat_ma_ok == 0U || pct == 0U) {
        run = -1;
    } else {
        int32_t ma_eff  = (s_bat_ma > (int32_t)BAT_RUN_MIN_MA) ? s_bat_ma : (int32_t)BAT_RUN_MIN_MA;
        int32_t pct_use = ((int32_t)pct > (int32_t)BAT_SOC_FLOOR)
                              ? ((int32_t)pct - (int32_t)BAT_SOC_FLOOR) : 0;
        int32_t usable_mah = (int32_t)(BAT_PACK_MAH * BAT_USABLE_PCT / 100U);
        int32_t mins = usable_mah * pct_use * 60 / (100 * ma_eff);

        mins = (mins / 5) * 5;
        run  = (mins > 999) ? 999 : mins;
    }

    if (s_bat_ok == 0U || pct != s_bat_pct || (int16_t)run != s_bat_run) {
        s_bat_ok  = 1U;
        s_bat_pct = pct;
        s_bat_run = (int16_t)run;
        *changed  = true;
    }
}

/* 电量那一列（用户 m09119：**右边**放容量和剩余时间、两行上下排，电池图标再小一点）。
 * 用户 m09335 收口（这一版）：
 *   1) 右列**与左列逐行对齐**：第 1 行（图标 + 百分比）与 `HAIXINBEI` 同一行高，
 *      第 2 行（剩余时间）墨迹底边与标题底下那条分隔线（y=27..28）对齐；
 *   2) 图标左沿 74 -> 78：原来离 `HAIXINBEI`（墨迹到 x=71）只差 2px、挤在一起；
 *      右移 4px 后中间留 6px。百分比文字**原地不动**（图标↔数字仍留 6px ≥ 一格，
 *      用户 m09248 第 2 条的要求）。
 * 布局（draw_text() 内部已经 +s_dx，图元要自己加，别加两遍 —— 见 :919 的坑）：
 *   第 1 行 y=BAT_ROW1_Y：图标 17x11（外壳 + 正极凸点 + 按电量填的内条）+ 百分比；
 *   第 2 行 y=BAT_ROW2_Y：还能跑多久，**居中在整块下面**（不是贴右）。
 * 历史坑（都在这个函数里，别再踩）：
 *   1) 内条要画在 +1、高 ICON_H-2（第一版画在 +2 ⇒ 顶上留一整行空白，"一直是白的"）；
 *   2) 百分比文字的 y 要**减 3**：draw_text() 的 y 是字框顶边，数字墨迹从 y+4 起；
 *   3) 分钟数也要带单位（`2H05M`）、整行居中（第一版右对齐 ⇒ "特别靠右"）。
 * 百分比用 `%3u%%` 固定 4 字符宽（` 58%` / `100%`）：块宽恒定、右边缘永远对齐 BAT_X_END。 */
#define BAT_ICON_X    78    /* 图标外壳左沿（凸点在 +BAT_ICON_W 处） */
#define BAT_TEXT_X    97    /* 百分比文字（字框）起点；前导空格不可见，右边缘到 BAT_X_END */
#define BAT_X_END    119    /* 右列右沿参考：百分比右边缘 / 剩余时间居中在这条线上 */
#define BAT_ICON_W     17
#define BAT_ICON_H     11
#define BAT_NUB_W       2
#define BAT_ROW1_Y      9   /* 与 HAIXINBEI 的墨迹（9..19）同一行高 */
#define BAT_ROW2_Y     19   /* 剩余时间墨迹 23..30：底边与分隔线(29..30)对齐；与上一行留 4px */
static void draw_battery_block(void)
{
    int16_t w, xt;
    char buf[16];

    if (s_bat_ok != 0U) {
        (void)snprintf(buf, sizeof(buf), "%3u%%", (unsigned)s_bat_pct);
    } else {
        (void)snprintf(buf, sizeof(buf), " ---");   /* 同宽，图标不跳位 */
    }

    OLED_DrawFrame((int16_t)(BAT_ICON_X + s_dx), BAT_ROW1_Y, BAT_ICON_W, BAT_ICON_H);
    OLED_DrawBox((int16_t)(BAT_ICON_X + BAT_ICON_W + s_dx), (int16_t)(BAT_ROW1_Y + 3),
                 BAT_NUB_W, 5U);
    if (s_bat_ok != 0U) {
        uint16_t fill = (uint16_t)(((uint32_t)s_bat_pct * (BAT_ICON_W - 2U)) / 100U);

        if (fill > 0U) {
            /* 内条正好占满外壳内沿（上下各让 1px 给边框）：+1 / ICON_H-2。
             * 第一版画在 +2，顶上那一行永远是白的（用户 m09248 第 1 条）。 */
            OLED_DrawBox((int16_t)(BAT_ICON_X + 1 + s_dx), (int16_t)(BAT_ROW1_Y + 1), fill,
                         (uint16_t)(BAT_ICON_H - 2U));
        }
    }

    /* 百分比文字的 y 要**减去 3**才算跟图标一条水平线：kk_font_body 是 12px 字模，
     * draw_text() 的 y 是字框顶边，数字的墨迹落在字框顶边往下第 4 行（y+4..y+11）。
     * 实测：字框 y=6 时墨迹 10..17，而图标占 9..19 ⇒ 减 3 后数字在图标内居中。 */
    draw_text(BAT_TEXT_X, (int16_t)(BAT_ROW1_Y - 3), kk_font_body, buf);

    /* 第 2 行：预计还能跑多久（-1 = 估不出来）。分钟数也带单位（`2H05M`），
     * 整行居中在上一行那块下面 —— 用户 m09248 第 3 条："最后一个没有单位，
     * 而且特别靠右，应该稍微靠左一点"。 */
    if (s_bat_run < 0) {
        (void)snprintf(buf, sizeof(buf), "--");
    } else if (s_bat_run < 60) {
        (void)snprintf(buf, sizeof(buf), "%dmin", (int)s_bat_run);
    } else {
        (void)snprintf(buf, sizeof(buf), "%dH%02dM", (int)(s_bat_run / 60), (int)(s_bat_run % 60));
    }
    w  = (int16_t)OLED_GetUTF8Width(buf);
    xt = (int16_t)((BAT_ICON_X + BAT_X_END - w) / 2);
    draw_text(xt, BAT_ROW2_Y, kk_font_body, buf);
}

/* =====================================================================
 * 上电主页（PAGE_SPLASH）
 * 只用已经在字模子集里的字符：ASCII + 中文（KK_OLED 是静态子集字模，
 * 子集外的字符会被静默跳过，所以文案改动必须同步重生成字模）。
 * ===================================================================== */
static void draw_splash_page(void)
{
    int16_t w;

    OLED_SetDrawMode(OLED_DRAW_SET);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);

    /* 表盘：一圈圆角外框 + 电量块 + 标题 + 「开始游戏」+ 左右两个实心箭头。
     * 外框**归页面所有**（用户 m04657）：和下面的文字、箭头一样加 s_dx，翻页时
     * 跟着表盘一起滑走。全工程只有表盘页有框，不在这儿画，静止时表盘就没框了
     * （常驻贴屏边那版用户不要，因为菜单页也会带上一圈框）。翻页时 kk_ui_nav.c
     * 的 KK_UI_DrawScene 把合缝正好定在这圈框的右边线上：框右边线 = 126 + s_dx
     * = 合缝 - 1，所以整圈框都是完整的、不会被剪掉一条边（用户 m04708）。 */
    OLED_DrawRFrame((int16_t)(1 + s_dx), 1, 126U, 62U, 6U);

    /* 版面栅格（用户 m09221 要"排布好、东西居中、别太靠两边"；m09248 又要"分隔线只放在
     * HAIXINBEI 下面"；m09509 又把中段整体下移 2px、并让电量那一列更散）——
     *   左右内容边距 **8px**（圆角外框内沿在 x≈3 / x≈124，视觉留白 5px）；
     *   顶栏：左 HAIXINBEI（17px 字模，墨迹 9..19、占 x=8..71），右 电量图标
     *                 9..19 + 剩余时间墨迹 23..30 —— 电量两行之间留 4px（原来只 2px）；
     *   分隔线只垫在标题底下：x=8..71、y=29..30（2px 粗）；
     *   中间行 y=31 起（墨迹 35..42）、底部行 y=44 起（墨迹 46..56），两行都水平居中；
     *   箭头 x=8..15 / 112..119、y=46..58，与标题左沿 / 电量块右沿对齐，不贴框。 */
    draw_battery_block();

    OLED_SetFont(kk_font_home);
    draw_text(8, 6, kk_font_home, "HAIXINBEI");
    OLED_DrawBox((int16_t)(8 + s_dx), 29, 64U, 2U);   /* 只垫在 HAIXINBEI 下面（m09509 下移 2） */

    /* 中间那行 "TERRAIN ROBOT"（用户 m09119 要"按之前那个"布局）与底部「开始游戏」，
     * 都水平居中：把各自的实测宽度代进 (128-w)/2。
     * 用户 m09509：这行再往下移两格（29 -> 31，墨迹 35..42），跟分隔线一起把中段散开。 */
    OLED_SetFont(kk_font_body);
    w = OLED_GetUTF8Width("TERRAIN ROBOT");
    draw_text((int16_t)((128 - w) / 2), 31, kk_font_body, "TERRAIN ROBOT");

    /* 两个箭头中间的入口提示（用户要求写「开始游戏」）。
     * 注意：**不能用 kk_font_home**——那个字模子集里 开/始/游/戏 四个字一个都没有
     * （tools/font/check_charset.mjs 实测 108 字形，4 字全缺），KK_OLED 遇到子集外
     * 字符是静默不画，整句会凭空消失（用户就是这么发现"看不见"的）。
     * kk_font_body 里这 4 字齐全，12px：实测宽 ≈47px -> 居中后 x≈40..87，
     * 左右箭头(8..15 / 112..119)离它还有 25px，y=44 起、墨迹 46..56，下沿离框 4px。 */
    OLED_SetFont(kk_font_body);
    w = OLED_GetUTF8Width("开始游戏");
    draw_text((int16_t)((128 - w) / 2), 44, kk_font_body, "开始游戏");

    /* 左右箭头：实心三角，指向环上相邻的两屏（左=设置，右=任务）。
     * x 与标题左沿(8) / 电量块右沿(119) 对齐，y=46..58 与底部那行同带。 */
    OLED_DrawFilledTriangle((int16_t)(8 + s_dx), 52, (int16_t)(15 + s_dx), 46,
                            (int16_t)(15 + s_dx), 58);
    OLED_DrawFilledTriangle((int16_t)(119 + s_dx), 52, (int16_t)(112 + s_dx), 46,
                            (int16_t)(112 + s_dx), 58);
}

/* ---- KK_UI 自定义页适配 ---- */
void KK_UI_CustomOnEnter(KK_UI_PageId page)
{
    if (page == PAGE_CUSTOM_TEST) {
        /* 每次进模块测试都从第一行（电机）开始：s_test_sel 是 static，不复位
         * 的话会停在上次那行（审计 agent 3 项⑦）。 */
        s_test_sel = 0U;
    }
    if (page == PAGE_CUSTOM_MOTOR) {
        /* 进电机测试页：先把速度环停一次（清目标/积分/窗口 + Load(0,0)），
         * 免得上一页（循迹/角度环/任务流程）留下的目标或积分被带进来，
         * 再由 motor_test_apply() 使能闭环并把上次记住的速度档重新下下去 ——
         * 用户下次进来时屏上显示的状态必须等于轮子实际在转的方向。 */
        SpeedControl_Stop();            /* 内部会 Load(0,0) 并清窗口 */
        SpeedControl_SetRamp(20000.0f); /* 归一化软启动斜坡（别的模块可能改过） */
        s_motor_sel = MOTOR_SEL_LEFT;
        s_motor_show[0] = SpeedControl_WindowCounts(0);
        s_motor_show[1] = SpeedControl_WindowCounts(1);
        s_motor_last = HAL_GetTick();
        /* 按键机复位：把当前位图当"已稳定"，避免刚进页面时把上一次的残留
         * 状态当成一条新按下（那会莫名其妙改掉速度档）。 */
        s_mkey_raw = app_ui_raw_keys();
        s_mkey_stable = s_mkey_raw;
        s_mkey_changed = s_motor_last;
        motor_test_apply();
    }
    if (page == PAGE_CUSTOM_SERVO) {
        /* 进舵机测试页：选中项回到「舵机1」、动作预设回到 1 号（收回）；按键机复位
         * （同电机页，免得把上一页残留的按下当成新边沿、一进来就把角度改了）；
         * 再把当前两个角度档立刻下发一遍 —— 这样屏上的数字一定等于舵机实际位置，
         * 不会出现"屏上 60° 其实舵机停在 0°"的错觉（用户 m13059 要拿它认线）。 */
        s_servo_sel = SERVO_SEL_1;
        s_servo_action = 1U;
        s_skey_raw = app_ui_raw_keys();
        s_skey_stable = s_skey_raw;
        s_skey_changed = HAL_GetTick();
        servo_test_apply();
    }
    if (page == PAGE_CUSTOM_TRACE) {
        /* 进循迹页：按键机复位（把当前位图当成"已稳定"，免得把上一页残留的按下
         * 当成一条新边沿，一进来就莫名切套/切视图）+ 默认预览第 0 套（普通）。
         * 这里只是"看"：cal_set_preview() 不写 g_cfg、不存 Flash（用户 m09558）。 */
        s_trk_raw = app_ui_raw_keys();
        s_trk_stable = s_trk_raw;
        s_trk_changed = HAL_GetTick();
        cal_set_preview(0);
    }
    if (page == PAGE_CUSTOM_TASK) {
        /* 进任务页：按键机复位（同上面几页，免得把上一页残留的按下当成一条新边沿、
         * 一进来就把选中项挪走）；正在跑的那个任务就是"选中项"——这一页中键执行
         * 的就是标题栏右边那个名字，跑着的时候名字必须是在跑的那个（用户 m24999）。 */
        s_tkey_raw = app_ui_raw_keys();
        s_tkey_stable = s_tkey_raw;
        s_tkey_changed = HAL_GetTick();
        if (TaskRun_IsRunning()) {
            s_task_sel = (uint8_t)TaskRun_Id();
        }
        (void)task_sel_ensure();
        task_sel_name_refresh();
    }
    /* 表盘不再需要"停一拍"的起始时刻（已按 m02919 移除）。保留函数体以满足
     * KK_UI 的回调签名。 */
}

void KK_UI_CustomOnLeave(KK_UI_PageId page)
{
    /* 这里原来无条件写 `Load(0, 0);`（"离开测试页时确保电机停"）。
     * 用户 m01540 明确要求"我设置好之后退出，它还在转，我好去看电流"，
     * 所以那句删掉了：速度档留在 static 里，退出后速度环继续以这个目标跑，
     * 下次进页面由 KK_UI_CustomOnEnter 重新下一次目标。要停就按「清零」。 */
    if (page == PAGE_CUSTOM_GYRO) {
        /* 陀螺仪页的方框是在临时裁剪窗口里画的，翻页时可能在面板上留下
         * 残影（用户 m02492："从陀螺仪退出来这个方框会停留一会有痕迹"）。
         * 落页这一拍 KK_UI_Update() 紧接着就会画并提交收尾帧，置了整屏
         * 重发标志后那一帧会把面板上任何旧像素全盖掉。 */
        OLED_RequestFullRefresh();
    }
}

void KK_UI_CustomOnInput(KK_UI_PageId page, KK_UI_InputEvent event)
{
    if (page == PAGE_SPLASH) {
        /* 长按：左右键全部放行、不做任何额外停顿——用户 m02919："加停顿
         * 会卡住一下然后重新加速"。表盘靠翻页动画那 220ms 就能看清，过渡
         * 期间的输入本来也被框架屏蔽（kk_ui.c:596）。
         * 只丢弃中键的重复事件：按住中键会顺着一路冲进菜单项里。 */
        if (event.source == KK_UI_INPUT_REPEAT &&
            event.action == KK_UI_INPUT_OK) {
            return;
        }
        /* 表盘：左右滑进图标菜单（中键也行，给不确定方向的用户兜底）。
         * 必须用前进/压栈：KK_UI_CustomRequestClose 在栈空时直接返回
         * KK_UI_NOT_ALLOWED（见 kk_ui.c:434），而表盘是根页、栈是空的。
         * 动画方向跟按键一致：按左->菜单从左边滑入，按右/中->从右边滑入。 */
        if (event.action == KK_UI_INPUT_UP) {
            /* 按左 = 往环上走一格：表盘的左邻是最后一项"陀螺仪" */
            KK_UI_SetEntrySelection(
                (uint16_t)(sizeof(k_home_items) / sizeof(k_home_items[0]) - 1U));
            (void)KK_UI_NavigateToDirected(PAGE_HOME, true, HAL_GetTick());
        } else if (event.action == KK_UI_INPUT_DOWN ||
                   event.action == KK_UI_INPUT_OK) {
            /* 按右/中 = 表盘的右邻是第一项"任务" */
            KK_UI_SetEntrySelection(0U);
            (void)KK_UI_NavigateTo(PAGE_HOME, HAL_GetTick());
        }
        return;
    }

    if (page == PAGE_CUSTOM_TEST) {
        /* 模块测试页有选项框：上/下换项，退出靠列表里的「返回」，
         * 不在这里用上键退出（避免和别处"混着来"）。 */
        if (event.action == KK_UI_INPUT_DOWN) {
            s_test_sel = (uint8_t)((s_test_sel + 1U) % TEST_N);
            KK_UI_Invalidate();
            return;
        }
        if (event.action == KK_UI_INPUT_UP) {
            s_test_sel = (uint8_t)((s_test_sel + TEST_N - 1U) % TEST_N);
            KK_UI_Invalidate();
            return;
        }
        if (event.action == KK_UI_INPUT_OK && event.source == KK_UI_INPUT_REPEAT) {
            return;   /* 按住中键只认第一次：否则 REPEAT 会连着进两级页 */
        }
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            switch (s_test_sel) {
            case TEST_SEL_BEEP:  beep_ms(60); break;                       /* 按一下响一下 */
            case TEST_SEL_MOTOR: (void)KK_UI_NavigateTo(PAGE_CUSTOM_MOTOR, HAL_GetTick()); break;
            case TEST_SEL_SERVO: (void)KK_UI_NavigateTo(PAGE_CUSTOM_SERVO, HAL_GetTick()); break;
            case TEST_SEL_BACK:  (void)KK_UI_CustomRequestClose(); break;  /* 返回 */
            default: break;
            }
            if (s_test_sel != TEST_SEL_BACK) KK_UI_Invalidate();
            return;
        }
        return;
    }

    if (page == PAGE_CUSTOM_MOTOR) {
        /* 本页的 上/下/左/右 全部由 motor_key_scan()（CustomOnTick 里读五向键
         * 原始位图）处理，所以这里的 UP/DOWN 事件必须丢掉 —— 不丢的话按一下上
         * 会走两格（框架把上折成 UP，扫描器也认它一次）。
         * 中键仍走框架（框架已消抖），且只认第一次：REPEAT 是按住连发。 */
        if (event.action == KK_UI_INPUT_UP || event.action == KK_UI_INPUT_DOWN) {
            return;
        }
        if (event.action == KK_UI_INPUT_OK && event.source == KK_UI_INPUT_REPEAT) {
            return;   /* 按住中键只认第一次：否则 REPEAT 会连着触发清零/返回 */
        }
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            if (s_motor_sel == MOTOR_SEL_CLEAR) {
                s_motor_level[0] = 0;    /* 清零 = 两路一起归零（用户 m02897） */
                s_motor_level[1] = 0;
                motor_test_apply();
                KK_UI_Invalidate();
            } else if (s_motor_sel == MOTOR_SEL_BACK) {
                (void)KK_UI_CustomRequestClose();   /* 返回：电机继续转 */
            }
            return;
        }
        return;
    }

    if (page == PAGE_CUSTOM_SERVO) {
        /* 同电机页：上/下/左/右 全由 servo_key_scan()（CustomOnTick 读五向键原始
         * 位图）处理，这里的 UP/DOWN 必须丢掉，否则按一下上会走两格。
         * 中键走框架（已消抖）、只认第一次：在「收回」上两路一起回 0°（用户
         * m13266 现场确认 0° 正好是收回位置、两路没接反），
         * 在「动作预设」上完整播放该动作（5/9 号含中间过渡，会阻塞 0.2~0.5s；
         *   只有这样才能看到复合动作的中间过程 —— 左/右切换只走终点），
         * 在「返回」上退出页面（退出后舵机保持最后角度，不回零）。 */
        if (event.action == KK_UI_INPUT_UP || event.action == KK_UI_INPUT_DOWN) {
            return;
        }
        if (event.action == KK_UI_INPUT_OK && event.source == KK_UI_INPUT_REPEAT) {
            return;
        }
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            if (s_servo_sel == SERVO_SEL_RETRACT) {
                s_servo_deg[0] = SERVO_DEG_RETRACT;
                s_servo_deg[1] = SERVO_DEG_RETRACT;
                servo_test_apply();
                KK_UI_Invalidate();
            } else if (s_servo_sel == SERVO_SEL_ACTION) {
                /* 完整播放（含中间延时），播完把上面两行的数字回读成舵机真实角度 */
                Servo_Move((int)s_servo_action);
                s_servo_deg[0] = (int16_t)(Servo_GetAngle(1) + 0.5f);
                s_servo_deg[1] = (int16_t)(Servo_GetAngle(2) + 0.5f);
                KK_UI_Invalidate();
            } else if (s_servo_sel == SERVO_SEL_BACK) {
                (void)KK_UI_CustomRequestClose();   /* 返回：舵机保持最后角度 */
            }
            return;
        }
        return;
    }

    if (page == PAGE_CUSTOM_GYRO) {
        /* 中键切三维方框的轴映射（M1..M8，见文件顶部 s_gyro_axis_mode 的位定义）。
         * 陀螺仪页只有"左键返回"，中键本来就是空的（见下面通用分支），拿来当调试
         * 开关不占用手感。 */
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            s_gyro_axis_mode = (uint8_t)((s_gyro_axis_mode + 1U) & 7U);
            KK_UI_Invalidate();
            return;
        }
    }

    if (page == PAGE_CUSTOM_TRACE) {
        /* 中键：平时 = 直接进标定（本页预览的就是要标的那套），标定中 = 采当前这一点。
         * 左键：标定中 = 取消标定（留在本页）；平时 = 返回上一级。
         * 右键 / 下键：**不在这里处理** —— 框架把"下/右"折成同一个 DOWN、
         *   "上/左"折成同一个 UP，分不出方向，所以这两条走 trace_key_scan()
         *   自己读原始位图（右键 = 切另一套标定，下键 = 柱状图 <-> 详细数据）。
         *   下面把 DOWN 直接吞掉，免得同一次按键既切套又切视图。 */
        trace_cal_state_t st = trace_cal_state();

        if (event.action == KK_UI_INPUT_UP) {
            if (st != TRACE_CAL_IDLE) {
                trace_cal_wizard_cancel();      /* 标定中按左键 = 取消，留在本页 */
            } else {
                (void)KK_UI_CustomRequestClose();
            }
            KK_UI_Invalidate();
            return;
        }
        if (event.action == KK_UI_INPUT_DOWN) {
            /* 下键与右键都由 trace_key_scan() 按原始位图分别处理，这里只吞事件。 */
            return;
        }
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            if (st == TRACE_CAL_IDLE) {
                /* 直接进标定（用户 m09335：这几个界面按中键都直接进标定）。
                 * 标定写进**当前预览**那一套 —— 预览用的就是 car 在跑的那套参数。
                 * 进标定先退回柱状图：那才是要对着黑的/白的对齐的视图。 */
                s_trace_verify = 0U;
                trace_cal_wizard_start();
            } else {
                trace_cal_wizard_press();
            }
            KK_UI_Invalidate();
            return;
        }
        return;
    }

    if (page == PAGE_CUSTOM_TASK) {
        /* 上/下/左 全由 task_key_scan() 按原始位图处理，这里只把框架归一化出来的
         * UP/DOWN 吞掉：框架把"上/左"折成同一个 UP，放上去的话按上键选任务会顺手
         * 把页面关掉（同 PAGE_CUSTOM_TRACE 的理由，见 app_ui_read_input()）。 */
        if (event.action == KK_UI_INPUT_UP || event.action == KK_UI_INPUT_DOWN) {
            return;
        }
        /* 中键 = 跑选中的任务（用户 m24999：要的就是"在车上点一下就离线跑起来"）；
         * 正在跑就是停机。原来这一页中键**只停机**、启动一律走上位机 0x67 RUN 或
         * 开机自启 —— 用户明确要离线跑，所以启动也接到这里。跑的是谁：按之前标题栏
         * 右边那个名字就是，看得见。 */
        if (event.action == KK_UI_INPUT_OK && event.source != KK_UI_INPUT_ENCODER) {
            if (task_run_toggle()) {
                KK_UI_Invalidate();
            }
            return;
        }
        return;
    }

    /* 其余自定义页没有选项框：左键返回（左键在归一化里映射成 UP） */
    if (event.action == KK_UI_INPUT_UP) {
        (void)KK_UI_CustomRequestClose();
        return;
    }
}

bool KK_UI_CustomOnTick(KK_UI_PageId page, uint32_t now_ms)
{
    (void)now_ms;

    if (page == PAGE_CUSTOM_MOTOR) {
        /* 两件事：
         * 1) motor_key_scan()：读五向键原始位图，管 上/下（换选中项）与 左/右
         *    （拖速度滑条，含长按加速）。本页要四个方向，而 kk_ui 只有三键，
         *    所以只能自己扫；框架保证这里是"当前页 + 无翻页过渡"，不抢键。
         * 2) 每 200ms 刷一次实测编码器读数（最近 20ms 的窗口计数）：数值真的
         *    变了才置脏重绘，和电流页同一个节奏。读数是纯内存量、不碰 I2C，
         *    所以不用像电流页那样等 OLED_IsBusy()。 */
        bool redraw = motor_key_scan(now_ms);

        if (now_ms - s_motor_last >= 200U) {
            int c0 = SpeedControl_WindowCounts(0);
            int c1 = SpeedControl_WindowCounts(1);

            s_motor_last = now_ms;
            if (c0 != s_motor_show[0] || c1 != s_motor_show[1]) {
                s_motor_show[0] = c0;
                s_motor_show[1] = c1;
                redraw = true;
            }
        }
        return redraw;
    }

    if (page == PAGE_CUSTOM_SERVO) {
        /* 舵机页要四个方向（换项 + 拖角度），kk_ui 只有三键 ⇒ 同电机页在这里
         * 扫五向键原始位图；数值直接在按下时发舵机，不需要定时刷新。 */
        return servo_key_scan(now_ms);
    }

    if (page == PAGE_CUSTOM_TRACE) {
        /* 循迹页的 右键(切另一套标定) / 下键(柱状图 <-> 详细数据) 在这里扫原始
         * 位图（框架只有三键，见 trace_key_scan()）。标定中不扫，只把按键机
         * 同步成当前位图，免得标定一结束就补触发一条陈旧的边沿。 */
        if (trace_cal_state() == TRACE_CAL_IDLE) {
            if (trace_key_scan(now_ms)) {
                return true;
            }
        } else {
            s_trk_raw = app_ui_raw_keys();
            s_trk_stable = s_trk_raw;
            s_trk_changed = now_ms;
        }
        return false;
    }

    if (page == PAGE_CUSTOM_TASK) {
        /* 任务页：上/下（选任务）与 左（返回）自己扫原始位图（框架只有三键，
         * 见 task_key_scan()）；顺带对一下"选中任务名"的签名 —— 页面开着的时候
         * 上位机把任务重烧/删掉，屏上的名字要跟着变（量宽度只在签名变了那一拍做）。 */
        bool redraw = task_key_scan(now_ms);
        int32_t sig0 = s_task_sig;

        (void)task_sel_ensure();
        task_sel_name_refresh();
        return (bool)(redraw || (s_task_sig != sig0));
    }

    if (page == PAGE_CUSTOM_POWER) {
        /* INA226 与 OLED 共用 I2C1：屏正在刷（中断/DMA 异步）时不要插进去抢总线。
         * 200ms 采一次，只有数值真的变了才置脏重绘。 */
        if (!OLED_IsBusy() && (now_ms - s_pwr_last >= 200U)) {
            int32_t mv, ma, mw, uv;
            uint8_t sat;

            s_pwr_last = now_ms;
            if (ina226_read_bus_voltage_mv(&mv) == HAL_OK &&
                ina226_read_current_ma(&ma) == HAL_OK &&
                ina226_read_power_mw(&mw) == HAL_OK) {
                /* 分流输入 (01h) 顶到满量程 ±81.92mV 时，电流/功率是芯片的钳位值，
                 * 不是被测值：板上 R2=5mΩ 对应 ±16.38A / ±197W。标出来别当真值。 */
                sat = 0U;
                if (ina226_read_shunt_uv(&uv) == HAL_OK &&
                    (uv >= 80000 || uv <= -80000)) {
                    sat = 1U;
                }
                if (!s_pwr_ok || mv != s_pwr_mv || ma != s_pwr_ma || mw != s_pwr_mw ||
                    sat != s_pwr_sat) {
                    s_pwr_ok = 1U;
                    s_pwr_sat = sat;
                    s_pwr_mv = mv;
                    s_pwr_ma = ma;
                    s_pwr_mw = mw;
                    return true;
                }
            } else if (s_pwr_ok) {
                s_pwr_ok = 0U;   /* 读失败：显示 --- ，不谎报旧值 */
                s_pwr_sat = 0U;
                return true;
            }
        }
        return false;
    }

    return false;
}

void KK_UI_CustomOnDraw(KK_UI_PageId page, int16_t x_offset,
                        int16_t clip_x, uint16_t clip_width)
{
    s_dx = x_offset;
    s_clip_x = clip_x;
    s_clip_w = clip_width;
    OLED_SetClipWindow(clip_x, 0, clip_width, 64);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);

    switch (page) {
    case PAGE_CUSTOM_TRACE:  draw_trace_page();  break;
    case PAGE_CUSTOM_TASK:   draw_task_page(); break;
    case PAGE_CUSTOM_TEST:   draw_test_page(); break;
    case PAGE_CUSTOM_MOTOR:  draw_motor_page(); break;
    case PAGE_CUSTOM_SERVO:  draw_servo_page(); break;
    case PAGE_CUSTOM_GYRO:   draw_gyro_page(); break;
    case PAGE_CUSTOM_POWER:  draw_power_page(); break;
    case PAGE_SPLASH:        draw_splash_page(); break;
    default: break;
    }
    OLED_ResetClipWindow();
    s_dx = 0;
}

/* =====================================================================
 * 供 Flash 存档（nvram.c）读写的窗口
 * ===================================================================== */
uint8_t app_ui_gyro_axis_mode(void)
{
    return s_gyro_axis_mode;
}

void app_ui_set_gyro_axis_mode(uint8_t mode)
{
    s_gyro_axis_mode = (uint8_t)(mode & 7U);
}

/* 老固件写入 Flash 的角度环默认增益（×100）。2026-10-01 原地实测证明 720/72
 * 在本板会自激，默认值改成 150/15；存档里若还是这两个旧默认值，开机迁移一次
 * （用户自己调过的值不动，避免把手工标定的结果冲掉）。迁移后 nvram_poll()
 * 会发现"当前值 != 上次存的值"并自动回写存档。 */
#define CFG_GYRO_KP_OLD_X100 72000
#define CFG_GYRO_KD_OLD_X100  7200

/* nvram 把存档盖回 g_cfg 之后调用：把新值推到运行时（灰度通道选择 + PID
 * 参数表），并让界面重画一遍。顺序很重要——app_ui_init() 里的
 * pull_pid_params() 会把 g_cfg 的 PID 字段按参数表覆盖掉，所以存档恢复
 * 必须在它之后，再由这里 push 回参数表。 */
void app_ui_settings_loaded(void)
{
    /* 角度环默认值迁移（必须在 push_pid_params() 之前：push 才是真正生效的那一步） */
    if (g_cfg.gyro_kp_x100 == CFG_GYRO_KP_OLD_X100 &&
        g_cfg.gyro_kd_x100 == CFG_GYRO_KD_OLD_X100) {
        g_cfg.gyro_kp_x100 = 15000;
        g_cfg.gyro_kd_x100 =  1500;
    }
    /* 角度 Ki 迁移：10.8 是旧默认（补静摩擦死区），现在默认 0 ——
     * 积分会被死区喂成常驻推力，车停在停车带外（实测 2.1~2.4°）一直抽。 */
    if (g_cfg.gyro_ki_x100 == 1080) {
        g_cfg.gyro_ki_x100 = 0;
    }
    trace_cal_set_mode((uint8_t)g_cfg.gray_mode);
    /* Flash 里的「自动判线」可能和上电默认不同：清掉"已推送"标记，
     * 让 app_ui_tick 下一拍推一次（trace_cal_set_adapt 会重新起头基线）。 */
    s_adapt_applied = 0xFFU;
    push_pid_params();
    KK_UI_Invalidate();
}

/* =====================================================================
 * 生命周期
 * ===================================================================== */
void app_ui_setup_display(void)
{
#if APP_UI_DISPLAY_ROT180
    OLED_SetRotation(OLED_ROTATION_180);
#endif
}

void app_ui_init(void)
{
    g_live.sysclk_hz = HAL_RCC_GetHCLKFreq();
    s_test_sel = 0U;
    disp_heal_init(&s_disp_heal);   /* 屏幕故障自愈：从"没故障、没重试"开始 */
    pull_pid_params();          /* host_link 参数表 -> 菜单镜像（host_link_init 已跑过） */
    (void)KK_UI_Init(&k_app);
}

void app_ui_tick(uint32_t now_ms)
{
    KK_UI_EventId ev;
    uint8_t nv_st;

    beep_poll();

    /* 电池电量（用户 m08669）：每秒读一次 INA226 母线电压，低通后算百分比。
     * 和「电流」页共用同一颗芯片、同一条 I2C1，所以由 battery_poll 自己等屏不忙。
     * 百分比变了才重画（表盘页是静态页，不重画就一直显示旧电量）。 */
    {
        bool bat_changed = false;

        battery_poll(now_ms, &bat_changed);
        if (bat_changed) {
            KK_UI_Invalidate();
        }
    }

    /* 设置/标定存 Flash 的提示：状态一变就弹一条（"正在保存" -> "保存成功"）。
     * 擦写完会自动换掉，提示没有按钮、不打断页面。 */
    nv_st = nvram_state();
    if (nv_st != s_nv_last) {
        s_nv_last = nv_st;
        if (nv_st == NVRAM_STATE_BUSY) {
            s_nv_lock = 0U;     /* 提示还没上屏，先别锁键 */
        } else if (nv_st == NVRAM_STATE_DONE) {
            (void)KK_UI_ShowToast("保存成功", NVRAM_DONE_MS);
            KK_UI_Invalidate();
        }
    }
    /* "正在保存"这一条要每拍重试：对话框开着的时候 KK_UI_ShowToast 返回
     * KK_UI_BUSY（kk_ui.c:420、kk_ui_dialog.c:587），原来只在状态跳变那一拍
     * 弹一次，于是"对话框 + 到点保存"这一路既没提示、按键又被吃掉，看着像死机
     * ~600ms 加上擦写时间（审计 agent 3 项①）。现在改成：提示真的画上屏了
     * （KK_UI_OK）才把按键锁住，宁可这次不锁也绝不给用户一个"没提示又按不动"。 */
    if (nv_st == NVRAM_STATE_BUSY && s_nv_lock == 0U) {
        if (KK_UI_ShowToast("正在保存", NVRAM_NOTICE_MS + 600U) == KK_UI_OK) {
            s_nv_lock = 1U;
        }
        KK_UI_Invalidate();     /* 别等 20ms 的帧节拍，这一拍就画上去 */
    }

    {
        KK_UI_Status ui_st = KK_UI_Update(now_ms, app_ui_read_input());
        disp_heal_action_t act;

        /* 显示故障自愈，见文件上部 s_disp_heal 的说明。注意 KK_UI_Update 在故障期间
         * 每拍都返回同一个错误码，所以"什么时候该动手"全靠 disp_heal 自己记节拍。 */
        act = disp_heal_step(&s_disp_heal,
                             (ui_st == KK_UI_DISPLAY_ERROR ||
                              ui_st == KK_UI_DISPLAY_TIMEOUT) ? 1U : 0U,
                             now_ms);
        if (act.bus_recover && !OLED_IsBusy()) {
            board_i2c_bus_recover();       /* 自愈几次都不成：总线（速率 + DMA）复位一次 */
        }
        if (act.try_recover && KK_UI_RecoverDisplay() == KK_UI_OK) {
            disp_heal_recovered(&s_disp_heal);   /* 真的清了故障、下一拍就重画全屏 */
        }
    }

    /* 循迹传感器极性：菜单里的「循迹反转」跟随生效（第五代模块默认压黑线=高） */
    trace_invert = g_cfg.trace_invert ? 1U : 0U;

    /* 设置里的「自动判线」（单帧自适应）跟随生效。
     * **只在变化时调**：trace_cal_set_adapt 会把白底基线清零重新起头，
     * 每拍都调等于永远起不了头。 */
    if (s_adapt_applied != (uint8_t)(g_cfg.trace_adapt ? 1U : 0U)) {
        s_adapt_applied = (uint8_t)(g_cfg.trace_adapt ? 1U : 0U);
        trace_cal_set_adapt(s_adapt_applied);
    }

    /* 设置里的「灰度接口」跟随生效（trace_cal_set_mode 同值调用直接返回） */
    trace_cal_set_mode((uint8_t)g_cfg.gray_mode);

    /* 设置里的「标定套」跟随生效这一条**已经删掉**（用户 m09558）：跑任务时由任务
     * 程序自己调 trace_cal_select_set() 选参数，界面上的右键只是"看另一套的效果"，
     * 不存 Flash、也不该被 nvram 里那个 cal_set 字节来回改（那个字段保留只为兼容
     * v4 记录，没人写它就一直是 0 = 普通）。 */

    while (KK_UI_PollEvent(&ev)) {
        if (ev == EV_PID_CHANGED) {
            push_pid_params();  /* 菜单里刚改完的 PID/基准速度写回控制环的参数表 */
        }
    }
}

/* 上电以来屏幕自愈（KK_UI_RecoverDisplay）成功了几次 —— 只给 1Hz RTT 诊断行用。
 * 一直是 0 说明屏从来没 latch 过显示错误；涨得快说明 OLED 通路（400kHz + DMA +
 * 这条 I2C1 上的三个从机）确实在出错。 */
uint32_t app_ui_display_recover_count(void)
{
    return disp_heal_count(&s_disp_heal);
}

/* 最小可见自检：不走 KK_UI，直接画一屏 */
void app_ui_test_pattern(void)
{
    OLED_Clear();
    OLED_SetDrawMode(OLED_DRAW_SET);
    OLED_SetBackgroundMode(OLED_BG_TRANSPARENT);
    draw_text(0, 0, kk_font_home, "G474");
    draw_text(0, 20, kk_font_body, "KK_UI 字体 OK");
    OLED_DrawFrame(0, 36, 128, 26);
    OLED_DrawBox(4, 40, 24, 18);
    OLED_DrawCircle(96, 49, 9);
    (void)OLED_Update();
}
