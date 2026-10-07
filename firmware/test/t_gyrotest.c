/**
 * @file  t_gyrotest.c   主机侧单元测试：角度环（App/Svc/gyro_control.c）
 *
 * 角度环跑在 TIM6 的 5ms 中断里，是三层串级里最"不能死"的一层：
 * 一旦它在中断里死循环或算出 NaN，车会保持上一拍输出一路冲出去。
 * 这个测试就盯两件事：
 *
 *   1) 角度规范化绝不进死循环、绝不返回 NaN/Inf
 *      原来 NormalizeAngle() 是无界 while 减 360，喂一个 ±Inf 就是**永久死循环**。
 *      现在非法值返回 0、合法值走 fmodf。用例 1 把边界值全钉住。
 *
 *   2) 一整套律在"脏输入"下仍然只输出有限、且在 ±MOTOR_PWM_MAX 内的轮速
 *      用例 6 用 20 万拍随机参数 + 随机（含 NaN/±Inf）陀螺读数做 fuzz；
 *      配合 -fsanitize-undefined-trap-on-error，任何 (int)NaN、越界、溢出
 *      都会变成非法指令让进程直接死 —— "跑完没死"本身就是断言。
 *
 * 另外用一阶假被控对象验证 s_law=3 的闭环语义：能收敛、能锁存、换目标/被推动
 * 会解锁（锁存是"到点停死、不再小幅晃"的实现手段，用户 m10182 直接要求过）。
 */
#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "gyro_control.h"
#include "jy61p.h"
#include "motor.h"
#include "speed_control.h"
#include "host_link.h"
#include "bsp_board.h"
#include "t_common.h"

/* ---- 陀螺读数（真源码里是 jy61p.c 定义的全局量，这里由测试顶替） ---- */
float Yaw = 0.0f;
float Gyro_Z = 0.0f;
float Roll = 0.0f, Pitch = 0.0f;

/* ---- 参数表桩：可注入，0 值也要能注入（用 g_set 标记，别用 0 当"未设置"） ---- */
static float g_p[65536];
static uint8_t g_set[65536];

float host_param_get(uint16_t id, float fallback)
{
    return g_set[id] ? g_p[id] : fallback;
}
void host_param_set_local(uint16_t id, float v)
{
    g_p[id] = v;
    g_set[id] = 1u;
}
static void param_reset(void)
{
    memset(g_set, 0, sizeof(g_set));
}

/* ---- 速度环桩：默认关闭（走 Load 直写 PWM，和没装编码器时一致） ---- */
static uint8_t g_speed_on;
static int     g_tgt_l, g_tgt_r;

uint8_t SpeedControl_IsEnabled(void) { return g_speed_on; }
void    SpeedControl_SetTarget(int l, int r) { g_tgt_l = l; g_tgt_r = r; }

extern int host_load_left(void);        /* host_board.c 里 Load() 的记录 */
extern int host_load_right(void);

static void param_default_angle(void)
{
    param_reset();
    host_param_set_local(HP_GYRO_KP, 150.0f);
    host_param_set_local(HP_GYRO_KI, 0.0f);
    host_param_set_local(HP_GYRO_KD, 15.0f);
    host_param_set_local(HP_GYRO_ILIMIT, 1000.0f);
    host_param_set_local(HP_TURN_TOLERANCE, 2.0f);
    host_param_set_local(HP_TURN_TIMEOUT, 1500.0f);
    host_param_set_local(HP_TURN_ANGLE_OFFSET, 90.0f);
    host_param_set_local(HP_YAW_RATE_DAMP, 1.0f);
    /* 差速限幅/放大参考也钉住出厂值：否则后面几节会读到 fuzz 那段灌进来的
     * 负限幅（param_reset 只是把"已设置"标记清掉，参数表里残留值会当兜底） */
    host_param_set_local(HP_GYRO_CORR_LIMIT, 2000.0f);
    host_param_set_local(HP_GYRO_CORR_REF,   3000.0f);
}

/* ---------------------------------------------------------------
 * 一阶假被控对象：corr -> 角加速度 -> 角速度 -> 偏航角
 * 目的不是复刻真车（真车有强摩擦滞回，那是台架的事），而是给闭环一个
 * 会收敛的、连续的载体，用来验证"能到位、能锁存、能解锁"这些语义。
 * --------------------------------------------------------------- */
static void plant_step(float corr)
{
    float a = corr * 0.6f;

    if (a >  2500.0f) a =  2500.0f;
    if (a < -2500.0f) a = -2500.0f;
    Gyro_Z += a * 0.005f;
    Gyro_Z *= 0.98f;                      /* 一点阻尼，别无限加速 */
    Yaw    += Gyro_Z * 0.005f;
}

int main(void)
{
    /* ---------------------------------------------------------------
     * 1) NormalizeAngle：边界 + 非法值（旧实现遇到 ±Inf 会永久死循环）
     * --------------------------------------------------------------- */
    {
        CHECK(fabsf(NormalizeAngle(0.0f)) < 1e-6f);
        CHECK(fabsf(NormalizeAngle(90.0f) - 90.0f) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(180.0f) - 180.0f) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(181.0f) + 179.0f) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(359.0f) + 1.0f) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(360.0f)) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(-181.0f) - 179.0f) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(720.0f)) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(-720.0f)) < 1e-4f);
        CHECK(fabsf(NormalizeAngle(1.0e6f)) <= 180.0f);      /* 大角度也要在范围内 */
        CHECK(fabsf(NormalizeAngle(-1.0e6f)) <= 180.0f);
        CHECK(fabsf(NormalizeAngle(1.0e9f)) <= 180.0f);
        /* 非法值：返回 0（关键：**不能**死循环、不能返回 NaN） */
        CHECK(fabsf(NormalizeAngle(1.0e30f)) < 1e-6f);
        CHECK(fabsf(NormalizeAngle(INFINITY)) < 1e-6f);
        CHECK(fabsf(NormalizeAngle(-INFINITY)) < 1e-6f);
        CHECK(fabsf(NormalizeAngle(NAN)) < 1e-6f);
    }

    /* ---------------------------------------------------------------
     * 2) CalculateAngleError：跨 ±180 的环绕，并且随机值恒在 ±180 内
     * --------------------------------------------------------------- */
    {
        int i;

        CHECK(fabsf(CalculateAngleError(10.0f, 350.0f) - 20.0f) < 1e-3f);
        CHECK(fabsf(CalculateAngleError(350.0f, 10.0f) + 20.0f) < 1e-3f);
        CHECK(fabsf(CalculateAngleError(170.0f, -170.0f) + 20.0f) < 1e-3f);  /* 跨 ±180 接缝取短边 */
        CHECK(fabsf(CalculateAngleError(0.0f, 180.0f) + 180.0f) < 1e-3f);
        for (i = 0; i < 20000; i++) {
            float t = (float)((i * 7919) % 720) - 360.0f;
            float a = (float)((i * 104729) % 720) - 360.0f;
            float e = CalculateAngleError(t, a);
            float d = t - a;

            while (d >  180.0f) d -= 360.0f;
            while (d < -180.0f) d += 360.0f;
            CHECK(fabsf(e) <= 180.0f + 1e-3f);
            CHECK(fabsf(e - d) < 1e-3f);
        }
    }

    /* ---------------------------------------------------------------
     * 3) s_law=3 闭环：能到位、能量进停车带
     * --------------------------------------------------------------- */
    {
        int i, in_band = 0;

        param_default_angle();
        Yaw = 0.0f; Gyro_Z = 0.0f;
        GyroControl_Init();
        s_law = 3;
        GyroControl_Start();
        GyroControl_SetTargetAngle(60.0f);
        for (i = 0; i < 4000; i++) {
            GyroControl_Task();
            plant_step(-(float)(host_load_left() - host_load_right()) * 0.5f);
            if (fabsf(GyroControl_HeadingError()) < 2.0f) in_band++;
            CHECK(host_load_left()  >= MOTOR_PWM_MIN && host_load_left()  <= MOTOR_PWM_MAX);
            CHECK(host_load_right() >= MOTOR_PWM_MIN && host_load_right() <= MOTOR_PWM_MAX);
        }
        CHECK(in_band > 0);                                    /* 收敛进 ±2° */
        CHECK(fabsf(GyroControl_HeadingError()) < 3.0f);       /* 最后停在带内 */
        CHECK(fabsf(Gyro_Z) < 2.0f);                           /* 而且真的停住了 */

        /* 锁存后输出必须彻底松手（Load 两路都为 0 = 不再小幅晃） */
        for (i = 0; i < 20; i++) { GyroControl_Task(); }
        CHECK_EQ_I(host_load_left(), 0);
        CHECK_EQ_I(host_load_right(), 0);

        /* 4) 被推动（Yaw 漂出 1.5× 带宽）要能解锁重新纠 */
        Yaw += 6.0f;
        for (i = 0; i < 3; i++) { GyroControl_Task(); }
        CHECK(host_load_left() != 0 || host_load_right() != 0);

        /* 5) 换目标要解锁（台架直接写 target_angle 也是这条路） */
        for (i = 0; i < 200; i++) { GyroControl_Task(); }
        GyroControl_SetTargetAngle(0.0f);
        for (i = 0; i < 3; i++) { GyroControl_Task(); }
        CHECK(host_load_left() != 0 || host_load_right() != 0);
    }

    /* ---------------------------------------------------------------
     * 6) 脏输入 fuzz：随机律 + 随机参数（含 0/负/极大/NaN） + 随机陀螺读数
     *    不变量：Load 两路恒在 ±MOTOR_PWM_MAX 内、误差有限。跑完没死就说明
     *    没有 (int)NaN、没有越界/溢出（UBSan trap 会直接终止进程）。
     * --------------------------------------------------------------- */
    {
        uint32_t seed = 20261002u;
        int i;
        int bad = 0;

        param_default_angle();
        GyroControl_Init();
        for (i = 0; i < 200000; i++) {
            float l, r;

            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) & 7u) {
            case 0:  Yaw = 0.0f;                       break;
            case 1:  Yaw = NAN;                        break;
            case 2:  Yaw = INFINITY;                   break;
            case 3:  Yaw = -INFINITY;                  break;
            case 4:  Yaw = 1.0e20f;                    break;
            case 5:  Yaw = -1.0e20f;                   break;
            default: Yaw = (float)((int32_t)(seed % 720u)) - 360.0f; break;
            }
            seed = seed * 1103515245u + 12345u;
            switch ((seed >> 16) & 7u) {
            case 0:  Gyro_Z = NAN;                     break;
            case 1:  Gyro_Z = INFINITY;                break;
            case 2:  Gyro_Z = -INFINITY;               break;
            case 3:  Gyro_Z = 1.0e20f;                 break;
            case 4:  Gyro_Z = 0.0f;                    break;
            default: Gyro_Z = (float)((int32_t)(seed % 2001u)) - 1000.0f; break;
            }

            if ((i % 97) == 0) {
                /* 隔一段换一套参数：0、负数、极大值、NaN 都要走一遍 */
                seed = seed * 1103515245u + 12345u;
                host_param_set_local(HP_GYRO_KP,  (float)((int32_t)(seed % 4001u)) - 2000.0f);
                host_param_set_local(HP_GYRO_KI,  ((i / 97) & 1) ? 0.0f : 5.5f);
                host_param_set_local(HP_GYRO_KD,  (float)((int32_t)(seed % 101u)) - 50.0f);
                s_law        = (uint8_t)((seed >> 8) % 5u);
                s_ang_kp     = (float)((int32_t)(seed % 21u)) - 10.0f;
                s_rate_max   = ((i / 97) & 2) ? 0.0f : 110.0f;
                s_corr_limit = ((i / 97) & 4) ? -2000.0f : 2000.0f;
                s_rate_kp    = (float)((int32_t)(seed % 801u)) - 400.0f;
                s_settle_deg = ((i / 97) & 1) ? 2.0f : 0.0f;
                s_settle_ms  = ((i / 97) & 1) ? 150.0f : 0.0f;
                s_v_floor    = ((i / 97) & 1) ? 35.0f : 0.0f;
                if ((i % 971) == 0) {
                    s_rate_lpf  = NAN;      /* 滤波器系数被写坏 */
                    s_acc_lpf   = INFINITY;
                    s_rate_kd   = 4.0f;
                } else {
                    s_rate_lpf  = 0.35f;
                    s_acc_lpf   = 0.35f;
                    s_rate_kd   = (float)((seed % 9u) == 0u ? 4.0f : 0.0f);
                }
            }
            if ((i % 5000) == 0) {
                GyroControl_Start();
                GyroControl_SetTargetAngle((float)((int32_t)(seed % 721u)) - 360.0f);
            }
            if ((i % 1000) == 0) {
                /* 目标也被写坏 */
                GyroControl_SetTargetAngle(NAN);
                GyroControl_SetTargetAngle(1.0e30f);
            }

            GyroControl_Task();
            plant_step(-(float)(host_load_left() - host_load_right()) * 0.5f);

            l = (float)host_load_left();
            r = (float)host_load_right();
            if (!(l >= (float)MOTOR_PWM_MIN && l <= (float)MOTOR_PWM_MAX) ||
                !(r >= (float)MOTOR_PWM_MIN && r <= (float)MOTOR_PWM_MAX)) bad++;
            {
                float e = GyroControl_HeadingError();
                if (!(e >= -181.0f && e <= 181.0f)) bad++;
            }
        }
        CHECK_EQ_I(bad, 0);
    }

    /* ---------------------------------------------------------------
     * 7) 速度环开着时走 SpeedControl_SetTarget（而不是 Load）
     * --------------------------------------------------------------- */
    {
        int i;

        param_default_angle();
        Yaw = 0.0f; Gyro_Z = 0.0f;
        GyroControl_Init();
        s_law = 3;
        g_speed_on = 1u;
        g_tgt_l = g_tgt_r = 0;
        GyroControl_Start();
        GyroControl_SetTargetAngle(30.0f);
        for (i = 0; i < 50; i++) { GyroControl_Task(); }
        CHECK(g_tgt_l != 0 || g_tgt_r != 0);
        CHECK(g_tgt_l >= MOTOR_PWM_MIN && g_tgt_l <= MOTOR_PWM_MAX);
        CHECK(g_tgt_r >= MOTOR_PWM_MIN && g_tgt_r <= MOTOR_PWM_MAX);
        g_speed_on = 0u;
    }

    /* ---------------------------------------------------------------
     * 8) 未使能时一拍都不动作（不是"写成 0"，而是**根本不碰** LoAd）
     * --------------------------------------------------------------- */
    {
        int before_l, before_r;

        GyroControl_Stop();
        before_l = host_load_left();
        before_r = host_load_right();
        CHECK(before_l != 0 || before_r != 0);   /* 上一段刚写过，先确认有非零值 */
        GyroControl_Task();
        CHECK_EQ_I(host_load_left(),  before_l);
        CHECK_EQ_I(host_load_right(), before_r);
    }

    /* ---------------------------------------------------------------
     * 9) 差速天花板随基准速度放大（用户 m24215：满速 W+A 转不动）
     *    角度环最后把 correction 当**左右轮差速**用，而这个限幅是绝对刻度：
     *    基准 7200 时 Δ/base 只剩 28%，转弯半径随速度线性变大（≈0.2m → 0.47m）。
     *    这里直接钉住那条曲线，不去反推某一拍的输出。
     * --------------------------------------------------------------- */
    {
        s_corr_limit = 2000.0f;
        s_corr_ref   = 3000.0f;

        GyroControl_SetBaseSpeed(0, 0);
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);   /* 原地转不受影响 */

        GyroControl_SetBaseSpeed(1500, 1500);
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);   /* <ref：旧行为 */

        GyroControl_SetBaseSpeed(3000, 3000);
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);   /* =ref：旧行为 */

        GyroControl_SetBaseSpeed(7200, 7200);
        CHECK(fabsf(GyroControl_CorrLimit() - 4800.0f) < 1.0e-3f);   /* 满速：×2.4 */

        /* 一轮正一轮反：取绝对值大的那个 */
        GyroControl_SetBaseSpeed(-7200, 100);
        CHECK(fabsf(GyroControl_CorrLimit() - 4800.0f) < 1.0e-3f);

        /* ref=0 关闭放大，完全退回旧行为（台架 A/B 对照用） */
        s_corr_ref = 0.0f;
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);

        /* 放大之后的上限：**MOTOR_PWM_MAX + |base|**（2026-10-06 放宽，用户 m 放行）。
         * 左轮 = base − corr，旧的"夹在单轮满量程"让 base=7000 时左轮最低只到 −200
         * ⇒ 内侧轮无法反转 ⇒ 直角这种"要先把车头拧过来"的大转弯只有一半差速。
         * 真正的轮量程边界是 gyro_control.c 里 left/right 各自夹到 ±MOTOR_PWM_MAX。 */
        s_corr_ref = 1000.0f;
        CHECK(fabsf(GyroControl_CorrLimit() - 2.0f * (float)MOTOR_PWM_MAX) < 1.0e-3f);

        /* 但原地转（base=0）的硬上限仍是单轮满量程 ⇒ pivot 行为逐位不变 */
        s_corr_limit = 20000.0f;
        GyroControl_SetBaseSpeed(0, 0);
        CHECK(fabsf(GyroControl_CorrLimit() - (float)MOTOR_PWM_MAX) < 1.0e-3f);
        s_corr_limit = 2000.0f;

        /* 参数被写坏：ref 非法 -> 不放大；限幅 NaN -> 0（不转）；负限幅按 |lim| */
        s_corr_ref = NAN;
        GyroControl_SetBaseSpeed(0, 0);
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);
        s_corr_ref   = 3000.0f;
        s_corr_limit = -2000.0f;
        CHECK(fabsf(GyroControl_CorrLimit() - 2000.0f) < 1.0e-3f);
        s_corr_limit = NAN;
        CHECK(fabsf(GyroControl_CorrLimit()) < 1.0e-3f);

        s_corr_limit = 2000.0f;
        s_corr_ref   = 3000.0f;
        GyroControl_SetBaseSpeed(0, 0);
    }

    /* ---------------------------------------------------------------
     * 10) ★2026-10-07 起步助推的两个旋钮进参数表（HP_GYRO_BOOST_GAP_MS 0x0113 /
     *     HP_GYRO_BOOST_RATE 0x0114）—— 加它们的目的就是让"原地转起步一卡一卡"
     *     能像 gyro_corr_limit 一样**在线扫**（J-Link 只读 / 上位机 set + push 改 RAM）。
     *     这里钉两条：
     *       ① 参数表里没有这两个 id 时（台架/老固件）走兜底 = 变量当前值 ⇒ 一个字节都不变；
     *       ② push 进参数表后**下一拍**就按新值跑，而且两个 id 互不串扰。
     *     真车不 push 时读到的就是 SPECS 默认值 250 / 60000（t_hostlink.c 第 15 节钉住），
     *     与加参数表之前的硬编码值逐位相同 ⇒ 默认行为零变化。
     * --------------------------------------------------------------- */
    {
        int i;

        param_default_angle();          /* 故意**不**预置这两个 id ⇒ 先走兜底分支 */
        s_boost_gap  = 250.0f;          /* 加参数表之前的硬编码值 */
        s_boost_rate = 60000.0f;
        Yaw = 0.0f; Gyro_Z = 0.0f;
        GyroControl_Init();
        s_law = 3;
        GyroControl_Start();
        GyroControl_SetTargetAngle(15.0f);
        for (i = 0; i < 8; i++) { GyroControl_Task(); }
        CHECK(s_boost_gap  == 250.0f);          /* 兜底路径不写坏原值 */
        CHECK(s_boost_rate == 60000.0f);

        /* 模拟"车上参数表里就是 SPECS 默认值"：读出来必须还是 250 / 60000 */
        host_param_set_local(HP_GYRO_BOOST_GAP_MS, 250.0f);
        host_param_set_local(HP_GYRO_BOOST_RATE,   60000.0f);
        for (i = 0; i < 8; i++) { GyroControl_Task(); }
        CHECK(s_boost_gap  == 250.0f);
        CHECK(s_boost_rate == 60000.0f);

        /* push 新值 ⇒ 下一拍生效（这就是"能在线扫"的那条路） */
        host_param_set_local(HP_GYRO_BOOST_GAP_MS, 40.0f);
        GyroControl_Task();
        CHECK(s_boost_gap == 40.0f);
        CHECK(s_boost_rate == 60000.0f);        /* 只改了 gap，rate 不受影响 */

        host_param_set_local(HP_GYRO_BOOST_RATE, 120000.0f);
        GyroControl_Task();
        CHECK(s_boost_rate == 120000.0f);
        CHECK(s_boost_gap  == 40.0f);           /* 两个 id 互不串扰 */
    }

    return T_SUMMARY();
}
