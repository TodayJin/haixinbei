/**
 * @file  servo.c
 * @brief 四路 LDX-227 PWM 舵机（TIM1_CH1–4 = PA8–PA11，50Hz，1us 刻度）
 *
 * 说明：
 *  - TIM1 ARR=19999、PSC=169 -> 1us/tick、20ms 周期，所以 CCR 直接就是脉宽 us。
 *  - 舵机是位置型，没有速度指令；要"慢慢走"只能软件把脉宽逐级逼近（Servo_MoveTo）。
 *  - 4 路同时堵转约 12A，而板上舵机轨只有 5A：Servo_MoveTo 的缓动天然是错峰的，
 *    新写的动作尽量别让 4 路同时顶到底。
 */
#include "servo.h"
#include "bsp_board.h"   /* bsp_servo_init()（TIM1 + PA8–PA11 的真正初始化，2026-10-03 补） */

static int      s_dev[3]   = {0, 0, 0};   /* 1..2 号舵机的软件中位偏差（us） */
static float    s_cur[3]   = {0.0f, 0.0f, 0.0f};
static float    s_dst[3]   = {0.0f, 0.0f, 0.0f};
static float    s_step[3]  = {0.0f, 0.0f, 0.0f};
static uint8_t  s_moving[3] = {0U, 0U, 0U};
static uint32_t s_last_ms  = 0U;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/**
 * @brief 直接写脉宽（us）。脉宽先夹到 500..2500，再叠加软件中位偏差，最后再夹一次。
 */
void Servo_SetPulse(int x, int pulse_us, int dev_us)
{
    int pulse = clamp_int(pulse_us, SERVO_PULSE_MIN, SERVO_PULSE_MAX);

    pulse += clamp_int(dev_us, -SERVO_DEVIATION_MAX, SERVO_DEVIATION_MAX);
    pulse  = clamp_int(pulse, SERVO_PULSE_MIN, SERVO_PULSE_MAX);

    switch (x) {
    case 1: __HAL_TIM_SetCompare(&htim1, SERVO1_CHANNEL, (uint32_t)pulse); break;
    case 2: __HAL_TIM_SetCompare(&htim1, SERVO2_CHANNEL, (uint32_t)pulse); break;
    default: break;
    }
}

void Servo_SetDeviation(int x, int dev_us)
{
    if (x < 1 || x > 2) return;
    s_dev[x] = clamp_int(dev_us, -SERVO_DEVIATION_MAX, SERVO_DEVIATION_MAX);
}

int Servo_GetDeviation(int x)
{
    if (x < 1 || x > 2) return 0;
    return s_dev[x];
}

/**
 * @brief 角度 -> 脉宽：500 + 2000*angle/180（教程原文：angle/0.09 + 500）
 */
void Servo_SetAngleDev(int x, float angle, int dev_us)
{
    int pulse;

    if (angle < 0.0f) angle = 0.0f;
    if (angle > SERVO_ANGLE_MAX) angle = SERVO_ANGLE_MAX;

    pulse = SERVO_PULSE_MIN + (int)((SERVO_PULSE_MAX - SERVO_PULSE_MIN) * angle / SERVO_ANGLE_MAX + 0.5f);
    Servo_SetPulse(x, pulse, dev_us);

    if (x >= 1 && x <= 2) s_cur[x] = angle;
}

void Servo_SetAngle(int x, float angle)
{
    Servo_SetAngleDev(x, angle, (x >= 1 && x <= 2) ? s_dev[x] : 0);
}

/**
 * @brief 旧接口：0..180 度
 */
void Servo_Control(int x, uint8_t angle)
{
    Servo_SetAngle(x, (float)angle);
}

/**
 * @brief 非阻塞缓动：用 ms 时间从当前角度走到 angle（每 SERVO_STEP_MS 一步）
 */
void Servo_MoveTo(int x, float angle, uint32_t ms)
{
    float delta;

    if (x < 1 || x > 2) return;
    if (angle < 0.0f) angle = 0.0f;
    if (angle > SERVO_ANGLE_MAX) angle = SERVO_ANGLE_MAX;

    delta = angle - s_cur[x];
    s_dst[x] = angle;
    s_last_ms = HAL_GetTick();

    if (ms == 0U || delta == 0.0f) {
        Servo_SetAngle(x, angle);
        s_moving[x] = 0U;
        return;
    }

    /* 每 SERVO_STEP_MS 走一步，每步的角度 = 总角度 / 总步数 */
    s_step[x] = (float)(SERVO_STEP_MS) * delta / (float)ms;
    if (s_step[x] > -0.5f && s_step[x] < 0.5f) {
        s_step[x] = (delta > 0.0f) ? 0.5f : -0.5f;
    }
    s_moving[x] = 1U;
}

/**
 * @brief 缓动任务（TIM7 5ms 回调里调）。舵机 4us 死区，1us 一步没有意义，
 *        所以内部自己每 SERVO_STEP_MS 才动一次。
 */
void Servo_Task(void)
{
    uint32_t now = HAL_GetTick();
    int i;

    if ((uint32_t)(now - s_last_ms) < SERVO_STEP_MS) return;
    s_last_ms = now;

    for (i = 1; i <= 2; i++) {
        float cur = s_cur[i];

        if (!s_moving[i]) continue;

        cur += s_step[i];

        if ((s_step[i] > 0.0f && cur >= s_dst[i]) || (s_step[i] < 0.0f && cur <= s_dst[i])) {
            cur = s_dst[i];
            s_moving[i] = 0U;
        }
        Servo_SetAngle(i, cur);
    }
}

void Servo_Init(void)
{
    /* 2026-10-03 现场报障「进了舵机测试页按了也不动」⇒ 根因就在这里：
     * TIM1 从来**没有**被初始化过。真正配 PA8–PA11(AF6) + TIM1(PSC=169, ARR=19999)
     * + 四路 PWM 通道、并把四路 Start 掉的是 bsp_board.c 的 `bsp_servo_init()`
     * （motor.c 那边是 `Motor_Init() -> bsp_motor_init()`，同一个套路），
     * 而 `bsp_servo_init()` 全工程没有任何调用者 —— 于是 htim1.Instance 一直是 NULL
     * （全局变量零初始化），`HAL_TIM_PWM_Start()` 直接返回 HAL_ERROR、
     * `Servo_SetPulse()` 里的 `__HAL_TIM_SetCompare()` 写在空指针上 ⇒ 舵机没有任何反应。
     * 补上 bsp_servo_init() 就对了；它内部已经把四路 PWM 都 Start 了，
     * 所以原先这里那两句 `HAL_TIM_PWM_Start(&htim1, ...)` 是多余的，一起去掉。 */
    bsp_servo_init();

    /* 上电摆到 0°（旧板行为：收回姿态），同时把 s_cur[] 记成 0 保持一致 */
    Servo_Control(1, 0);
    Servo_Control(2, 0);
}

/**
 * @brief 比赛动作表的**终点姿态**（沿用旧板标定的角度，只把驱动换成新板）
 *
 * 用户 m13303 要求在舵机测试页里能"按号选动作"，所以把这张表抽成数据：
 * `Servo_Move()`（比赛流程用，含 5/9 号动作的中间步骤）、`Servo_ApplyAction()`
 * （界面实时预览用，只下发终点）都从这里取，**只有一个真值来源**。
 * 每一行是 { 舵机1角度, 舵机2角度 }，行号 = `Servo_Move(n)` 的 n - 1。
 * 5 号与 9 号是复合动作，这里记的是它们**最后**写下去的那一对角度。
 */
static const uint8_t k_action_pose[SERVO_ACTION_N][2] = {
    /*  1 收回            */ {  0,   0 },
    /*  2 打下准备        */ { 60, 120 },
    /*  3 打下1           */ {105, 120 },
    /*  4 打下2           */ {135, 150 },
    /*  5 二级台阶2收回（复合：135/170 -> 400ms -> 舵1=90 -> 100ms -> 舵1=80） */ { 80, 170 },
    /*  6 大坡1           */ {100, 120 },
    /*  7 三级12          */ { 95, 120 },
    /*  8 三级2收回       */ { 40,  10 },
    /*  9 三级3打（复合：70/10 -> 200ms -> 100/50 -> 100ms -> 100/80） */ {100,  80 },
    /* 10 高台            */ {115, 120 },
    /* 11 高台下          */ { 85, 120 },
    /* 12 收回2           */ { 40,   0 },
    /* 13 打下1（另一路） */ { 90,  60 },
    /* 14 打下准备（另一路） */ { 30,  60 },
};

int Servo_ActionCount(void)
{
    return SERVO_ACTION_N;
}

/**
 * @brief 动作名（给舵机测试页的「动作预设」第二行显示；与 k_action_pose 同序）
 *        用户 m13356 要求这一项占两行：上行是"动作预设 n/14"，下行是动作名。
 *        字模里缺的字用 `node tools/font/gen_font.mjs ... "…"` 补（已补过 作/预）。
 */
static const char *const k_action_name[SERVO_ACTION_N] = {
    "收回", "打下准备", "打下1", "打下2", "台阶2收回",
    "大坡1", "三级12", "三级2收回", "三级3打", "高台",
    "高台下", "收回2", "打下1(另一路)", "打下准备(另一路)",
};

const char *Servo_ActionName(int x)
{
    if (x < 1 || x > SERVO_ACTION_N) return "";
    return k_action_name[x - 1];
}

void Servo_ActionPose(int x, int *a1, int *a2)
{
    if (x < 1 || x > SERVO_ACTION_N) {
        if (a1 != 0) *a1 = 0;
        if (a2 != 0) *a2 = 0;
        return;
    }
    if (a1 != 0) *a1 = (int)k_action_pose[x - 1][0];
    if (a2 != 0) *a2 = (int)k_action_pose[x - 1][1];
}

/**
 * @brief 只下发某个动作的终点姿态（不走 5/9 号的中间延时）。
 *        给舵机测试页的"动作预设"行做实时预览用 —— 界面不能被 HAL_Delay 卡住，
 *        而且用户要的就是"切到哪一项、车就摆到那个姿势"。
 */
void Servo_ApplyAction(int x)
{
    int a1, a2;

    if (x < 1 || x > SERVO_ACTION_N) return;
    Servo_ActionPose(x, &a1, &a2);
    Servo_Control(1, (uint8_t)a1);
    Servo_Control(2, (uint8_t)a2);
}

float Servo_GetAngle(int x)
{
    if (x < 1 || x > 2) return 0.0f;
    return s_cur[x];
}

/**
 * @brief 比赛动作表（沿用旧板标定的角度，只把驱动换成新板）
 *        1..14 号的动作终点都在 k_action_pose 里；只有 5/9 号需要中间的
 *        过渡步骤，仍按旧代码原样写在这里（中间延时会阻塞调用者，比赛流程
 *        本来就是顺序执行的，保持原行为不动）。
 */
void Servo_Move(int x)
{
    if (x < 1 || x > SERVO_ACTION_N) return;

    /* 5 号：2 级台阶 2 收回 —— 135/170 停 400ms，再 90，停 100ms，再 80 */
    if (x == 5) {
        Servo_Control(1, 135);
        Servo_Control(2, 170);
        HAL_Delay(400);
        Servo_Control(1, 90);
        HAL_Delay(100);
        Servo_Control(1, 80);
        return;
    }

    /* 9 号：3 级 3 打 —— 70/10 停 200ms，再 100/50，停 100ms，再 100/80 */
    if (x == 9) {
        Servo_Control(1, 70);
        Servo_Control(2, 10);
        HAL_Delay(200);
        Servo_Control(1, 100);
        Servo_Control(2, 50);
        HAL_Delay(100);
        Servo_Control(1, 100);
        Servo_Control(2, 80);
        return;
    }

    Servo_ApplyAction(x);
}
