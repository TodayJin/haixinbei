/**
 * @file  speed_control.c
 * @brief 速度环：编码器测速 + 双轮 PI（带前馈、积分限幅、输出限幅、反饱和、死区补偿）
 *
 * 为什么用"前馈 + PI"而不是纯 PID：
 *   上层给的目标本身就是 PWM 刻度，而 PWM->转速的关系大致线性，
 *   所以直接拿目标当基础输出（前馈），PI 只修负载/电压/坡道带来的偏差，
 *   这样参数好调，也不会因为积分太慢而在坡上掉速。
 *
 * 标定方法（RTT 的 FRAME 行里有 ENC 字段）：
 *   给一个恒定目标（比如 3000），看 counts 稳定在多少；
 *   12V 空载 150rpm -> 4950 计数/s -> 每 5ms 约 25 个计数（SPEED_FULL_COUNTS 就是按这个来的）。
 *   实测对不上就改 SPEED_FULL_COUNTS，或把 SPEED_FF_SLOPE 调小让 PI 多干活。
 */
#include "speed_control.h"
#include <math.h>           /* fabsf */
#include "encoder.h"
#include "motor.h"
#include "bsp_board.h"      /* MOTOR_PWM_MAX / MOTOR_PWM_MIN */
#include "host_link.h"

#define SPEED_PERIOD_MS       5.0f
#define SPEED_FULL_COUNTS     25.0f    /* 满 PWM(7200) 时每 5ms 的期望计数（12V 空载标定值） */
/* 测速滑动平均窗口（拍）：4 x 5ms = 20ms。
 * 单次读数只有 ±0.5 计数的量化误差，目标 1500（5.2 计数/5ms）时相对误差 ±9.6%；
 * 取最近 4 拍求和再除以拍数，等效分辨率提高 4 倍（±2.4%）。反馈值仍保持
 * "计数/5ms"的量纲，所以 kp/ki/ilim 的整定值一个都不用动。
 * 注意：这**只是把平均从积分器搬到测量端**，环路本身照旧跑 5ms。
 * 千万不要反过来去改 TIM6 的周期（bsp_board.c 的 BSP_TRACE_PERIOD_MS）—— 那一拍里
 * 同时跑循迹环和角度环，而且 gyro_control.c 的 GYRO_DT 是写死的 0.005f。 */
#ifndef SPEED_AVG_TICKS
#define SPEED_AVG_TICKS       4
#endif
/* ---- 前馈：把"稳态到底需要多少 PWM"直接算给电机，PI 只修残差 ----
 *
 * 台架实测（firmware/App/Bsp/motor_bench.c 的 OPEN/CLOSE/STAT 行）拟合出来的稳态关系：
 *     稳态所需 PWM ≈ 2327 + 0.662 × 目标        （目标 = PWM 刻度 0..7200）
 *   验算：目标 1000 -> 拟合 2989 / 闭环实测均值 2999~3293；1250 -> 3155 / 3123~3297；
 *         3000 -> 4313 / 4101~4186；5000 -> 5637 / 5672~5710。
 *
 * 2327 这一项就是**摩擦等效压降**，0.662 是"转速/PWM"的斜率。原来的 SPEED_FF_GAIN = 0.80
 * 只对了斜率、把 2327 漏了，后果在低速最明显：目标 1000 稳态需要 ~3000 PWM，前馈却只给
 * 800，剩下 2200 全靠积分器一拍 +2×err 地慢慢爬 —— 爬升期间轮子不动（白等），爬过头又
 * 冲太快，误差变负再把积分退回去。**低速误差就是这么来的。**
 *
 * 为什么摩擦项是 2327 而不是直接取量到的起步阈值：左右两个轮子的起转阈值本来就不同
 * （R 轮比 L 轮高 300~500 PWM），而前馈只有一组常数，只能取两轮稳态需求的中间值，
 * 差由各自的积分器吸收。开环静特性（1.3s 稳态均值，单位计数/秒）：
 *     pwm   3000  3500  3800  4000  4200  4500  5000  6000
 *     L      836  1456  1865  2083  2254  2522  3012  3653
 *     R        0  1074  1698  1900  2110  2488  2953  3691   <- 3000 上 R 轮完全不动
 * 3500 以上两轮都平滑单调（停滞计数 st=0），所以低速段 STAT 里的 st>0 是环路输出瞬时
 * 落到起转阈值以下的**死区边界**，不是电机在抖。
 *
 * 两个常数现在是**参数表里的参数**（host_link.h 的 HP_SPEED_FF_SLOPE / HP_SPEED_FF_FRIC，
 * 出厂默认值就是上面这两个数），所以菜单「PID -> 速度环 -> 前馈斜率/前馈摩擦」和上位机
 * 都能在线改、都能存 Flash，不用重烧。SpeedControl_SetFeedforward() / _GetFeedforward()
 * 也走同一个参数表，台架一次烧录照样能扫一整张参数表。
 * 编译期 -DSPEED_FF_SLOPE=0.80f -DSPEED_FF_FRIC=0.0f 改的是参数表的出厂默认值，
 * 仍然能精确退化成改动前的老行为（A/B 用）。 */
#define SPEED_FF_FRIC_MIN_TGT 0.5f     /* 目标小于这个就不加摩擦前馈，防止零目标时电机自走 */
#define SPEED_MIN_ACTIVE      500      /* 死区补偿：有目标时最小有效 PWM，克服静摩擦 */
#define SPEED_RAMP_DEFAULT    20000.0f /* 目标斜坡，PWM/s（20000 -> 每 5ms 变 100） */
/* 目标"往 0 或往反号"方向的斜坡，PWM/s。角度环反打时要的是**立刻换向**：
 * 全局斜坡 20000 时 ±2000 的差速换向要 200ms，这 200ms 里轮子还按旧方向使劲，
 * 车身就先倒转一下再回来（用户说的"在目标点左右晃动两下"，实测往回转 4~9°）。
 * 加速方向仍用 SPEED_RAMP_DEFAULT 慢慢爬（防打滑/防电流冲击），只有减速换向快。 */
#define SPEED_RAMP_BRAKE_MAX  300000.0f

typedef struct {
    float integral;
    float last_err;
    float target;    /* 本周期实际使用的目标（带斜坡，PWM 刻度） */
    int   out;       /* 本周期写下去的 PWM */
} speed_pi_t;

static speed_pi_t s_pi[2];
static float      s_req[2];        /* 上层请求的目标 */
static float      s_ramp = SPEED_RAMP_DEFAULT;
volatile float    s_ramp_brake = SPEED_RAMP_BRAKE_MAX;  /* 减速/换向方向用的斜坡（台架可 poke） */
static int        s_counts[2];     /* 本拍原始增量（计数/5ms），对外报数用 */
static int        s_win[2][SPEED_AVG_TICKS];  /* 滑动窗口：最近几拍的原始增量 */
static int        s_win_sum[2];    /* 窗口内增量之和 */
static int        s_win_n;         /* 窗口已填拍数（1..SPEED_AVG_TICKS），启动时不足 4 */
static uint8_t    s_win_pos;
static float      s_mmps[2];
static uint8_t    s_enabled;

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float approach(float cur, float req, float step)
{
    if (cur < req) { cur += step; if (cur > req) cur = req; }
    else if (cur > req) { cur -= step; if (cur < req) cur = req; }
    return cur;
}

/* 清空测速滑动窗口（初始化 / 使能边沿 / 停止时调，避免把上一次的残留算进去） */
static void win_reset(void)
{
    int i, k;
    for (i = 0; i < 2; i++) {
        for (k = 0; k < SPEED_AVG_TICKS; k++) s_win[i][k] = 0;
        s_win_sum[i] = 0;
    }
    s_win_n   = 0;
    s_win_pos = 0U;
}

void SpeedControl_Init(void)
{
    int i;
    for (i = 0; i < 2; i++) {
        s_pi[i].integral = 0.0f;
        s_pi[i].last_err = 0.0f;
        s_pi[i].target   = 0.0f;
        s_pi[i].out      = 0;
        s_req[i]         = 0.0f;
        s_counts[i]      = 0;
        s_mmps[i]        = 0.0f;
    }
    win_reset();
    s_ramp    = SPEED_RAMP_DEFAULT;
    s_ramp_brake = SPEED_RAMP_BRAKE_MAX;
    s_enabled = 0U;
}

void SpeedControl_Enable(uint8_t on)
{
    if (on) {
        if (!s_enabled) {
            /* 刚使能：从 0 开始爬（软启动），积分清零，避免上次的残留 */
            s_pi[0].integral = 0.0f; s_pi[1].integral = 0.0f;
            s_pi[0].last_err = 0.0f; s_pi[1].last_err = 0.0f;
            s_pi[0].target   = 0.0f; s_pi[1].target   = 0.0f;
            win_reset();
        }
        s_enabled = 1U;
    } else {
        s_enabled = 0U;
    }
}

uint8_t SpeedControl_IsEnabled(void)
{
    return s_enabled;
}

void SpeedControl_SetTarget(int left, int right)
{
    s_req[0] = (float)left;
    s_req[1] = (float)right;
}

void SpeedControl_SetRamp(float ramp_pwm_per_s)
{
    s_ramp = ramp_pwm_per_s;
}

/* 前馈参数在线改（台架扫表 / 上位机整定用）—— 直接写参数表，和菜单改的是同一份值，
 * 所以改完下一拍生效、也能被 Flash 存档保住。传 SPEED_FF_SLOPE / SPEED_FF_FRIC 即恢复出厂。 */
void SpeedControl_SetFeedforward(float slope, float fric)
{
    host_param_set_local(HP_SPEED_FF_SLOPE, slope);
    host_param_set_local(HP_SPEED_FF_FRIC,  fric);
}

void SpeedControl_GetFeedforward(float *slope, float *fric)
{
    if (slope) *slope = host_param_get(HP_SPEED_FF_SLOPE, SPEED_FF_SLOPE);
    if (fric)  *fric  = host_param_get(HP_SPEED_FF_FRIC,  SPEED_FF_FRIC);
}

void SpeedControl_SetTargetRamp(int left, int right, float ramp_pwm_per_s)
{
    SpeedControl_SetTarget(left, right);
    s_ramp = ramp_pwm_per_s;
}

void SpeedControl_Stop(void)
{
    int i;
    s_enabled = 0U;
    for (i = 0; i < 2; i++) {
        s_req[i]         = 0.0f;
        s_pi[i].target   = 0.0f;
        s_pi[i].integral = 0.0f;
        s_pi[i].last_err = 0.0f;
        s_pi[i].out      = 0;
    }
    win_reset();
    Load(0, 0);
}

/* 单轮 PI：目标(PWM刻度) -> 期望计数 -> 误差 -> 输出
 * counts 是滑动平均后的转速（仍是"计数/5ms"的量纲，但带小数）—— 不要取整，
 * 取整就把 20ms 平均换来的亚计数分辨率又丢掉了。 */
static int speed_pi_step(speed_pi_t *s, float counts, float kp, float ki, float kd, float ilim,
                         float ff_slope, float ff_fric, float full_counts)
{
    /* ★2026-10-14：刻度从参数表取（HP_SPEED_FULL_COUNTS），不再用编译期常数。
     *  原来写死 `SPEED_FULL_COUNTS`(25) 是旧电机的标定值，换电机后目标够不到 ⇒ 环饱和。
     *  调用方传进来而不是在这里 host_param_get，是为了让本函数保持"纯函数、可单测" */
    float target_counts = s->target * (full_counts / (float)MOTOR_PWM_MAX);
    float err = target_counts - counts;
    float u_ff, out;

    s->integral += err;
    s->integral = clampf(s->integral, -ilim, ilim);

    /* 前馈 = 斜率项 + 摩擦项。摩擦项按目标符号取正负，目标为 0 就不给
     * （不然零目标时轮子会自己走）。目标 1500 时它直接给出 1870+1125 = 2995 PWM，
     * 落点就在稳态工作点上，PI 只剩几十个 PWM 的活要干。 */
    u_ff = ff_slope * s->target;
    if (s->target > SPEED_FF_FRIC_MIN_TGT) {
        u_ff += ff_fric;
    } else if (s->target < -SPEED_FF_FRIC_MIN_TGT) {
        u_ff -= ff_fric;
    }

    out = u_ff
        + kp * err
        + ki * s->integral
        + kd * (err - s->last_err);
    s->last_err = err;

    /* 输出限幅 + 反饱和（只回退本次加进去的那份误差，不清零积分） */
    if (out > (float)MOTOR_PWM_MAX) {
        out = (float)MOTOR_PWM_MAX;
        if (err > 0.0f) s->integral -= err;
    } else if (out < (float)MOTOR_PWM_MIN) {
        out = (float)MOTOR_PWM_MIN;
        if (err < 0.0f) s->integral -= err;
    }

    /* 死区补偿：有目标但算出来的 PWM 太小，电机根本不转，补到最小有效值 */
    if (s->target > 0.5f && out > 0.0f && out < (float)SPEED_MIN_ACTIVE) {
        out = (float)SPEED_MIN_ACTIVE;
    } else if (s->target < -0.5f && out < 0.0f && out > -(float)SPEED_MIN_ACTIVE) {
        out = -(float)SPEED_MIN_ACTIVE;
    }

    s->out = (int)(out >= 0.0f ? (out + 0.5f) : (out - 0.5f));
    return s->out;
}

void SpeedControl_Task(void)
{
    int   dl = 0, dr = 0;
    float kp, ki, kd, ilim, cpr, dia;
    float ff_slope, ff_fric;
    float full_counts;              /* ★满 PWM 时每 5ms 的期望计数（HP_SPEED_FULL_COUNTS）*/
    float avg_l, avg_r;
    int   i;

    /* 1) 测速：读增量、清零（这一步不管使能与否都做，方便随时观测） */
    Encoder_ReadDelta(&dl, &dr);
    s_counts[0] = dl;
    s_counts[1] = dr;

    /* 1b) 20ms 滑动平均：窗口里放着最近 SPEED_AVG_TICKS 拍的原始增量。
     * 反馈值取"和 / 有效拍数"而不是裸的"和"，是为了让它继续保持"计数/5ms"的量纲
     * —— 这样 kp/ki/ilim 一个都不用重调，台架上整定好的值继续有效。
     * 启动时窗口没填满，按实际拍数除（s_win_n 至少为 1），避免头几拍把反馈压得过低。 */
    s_win_sum[0] -= s_win[0][s_win_pos];
    s_win[0][s_win_pos] = dl;
    s_win_sum[0] += dl;
    s_win_sum[1] -= s_win[1][s_win_pos];
    s_win[1][s_win_pos] = dr;
    s_win_sum[1] += dr;
    s_win_pos = (uint8_t)((s_win_pos + 1) % SPEED_AVG_TICKS);
    if (s_win_n < SPEED_AVG_TICKS) s_win_n++;

    avg_l = (float)s_win_sum[0] / (float)s_win_n;
    avg_r = (float)s_win_sum[1] / (float)s_win_n;

    cpr = host_param_get(HP_COUNTS_PER_REV, 1980.0f);
    dia = host_param_get(HP_WHEEL_DIA, 65.0f);
    if (cpr < 1.0f) cpr = 1.0f;
    /* mm/s = 计数/周期 * (1000/周期ms) / 每圈计数 * 周长（用平滑值，低速读数不再跳） */
    s_mmps[0] = avg_l * (1000.0f / SPEED_PERIOD_MS) / cpr * 3.14159265f * dia;
    s_mmps[1] = avg_r * (1000.0f / SPEED_PERIOD_MS) / cpr * 3.14159265f * dia;

    /* 2) 未使能：不碰 PWM（模块测试页/标定会直接调 Load） */
    if (!s_enabled) return;

    /* 3) 参数每拍实时取：菜单/上位机改完立刻生效 */
    kp   = host_param_get(HP_SPEED_KP, 20.0f);
    ki   = host_param_get(HP_SPEED_KI, 2.0f);
    kd   = host_param_get(HP_SPEED_KD, 0.0f);
    ilim = host_param_get(HP_SPEED_ILIMIT, 3000.0f);
    /* 前馈也从参数表取：菜单「前馈斜率/前馈摩擦」、上位机、台架改完都是下一拍生效 */
    ff_slope = host_param_get(HP_SPEED_FF_SLOPE, SPEED_FF_SLOPE);
    ff_fric  = host_param_get(HP_SPEED_FF_FRIC,  SPEED_FF_FRIC);
    /* ★速度环的"PWM 刻度 → 期望计数"刻度（换电机后要重标的那个）。兜底 25 = 旧硬编码值 */
    full_counts = host_param_get(HP_SPEED_FULL_COUNTS, SPEED_FULL_COUNTS);
    if (!(full_counts >= 1.0f)) full_counts = SPEED_FULL_COUNTS;   /* NaN/0 兜底 */

    /* 4) 目标斜坡（软启动/防打滑）：
     *    幅值变大（起步/加速）用 s_ramp 慢慢爬，防打滑、防电流冲击；
     *    幅值变小或反号（刹车/换向）用 s_ramp_brake 立刻给 —— 角度环反打时
     *    若还按旧方向转 200ms，车身就会先倒转一下再回来（"在目标点左右晃两下"）。
     *
     *    注意这里判据必须是 cur * req < 0 而不是 <= 0：
     *    停车后目标恰好是 0，此时 cur == 0，用 <= 会把"从静止起步"也当成换向，
     *    于是起步那一下直接跳到满目标 —— 慢斜坡的软启动整个失效（打滑/电流冲击）。
     *    改成 < 之后：cur == 0 且 req != 0 走慢斜坡（起步），真反号才走快斜坡。 */
    for (i = 0; i < 2; i++) {
        float cur = s_pi[i].target;
        float req = s_req[i];
        float st;
        if (s_ramp <= 0.0f) { s_pi[i].target = req; continue; }
        st = ((cur * req < 0.0f) || (fabsf(req) < fabsf(cur))) ? s_ramp_brake : s_ramp;
        s_pi[i].target = approach(cur, req, st * (SPEED_PERIOD_MS / 1000.0f));
    }

    /* 5) 双轮 PI -> PWM（反馈用 20ms 平滑值，控制节拍仍是 5ms） */
    s_pi[0].out = speed_pi_step(&s_pi[0], avg_l, kp, ki, kd, ilim, ff_slope, ff_fric, full_counts);
    s_pi[1].out = speed_pi_step(&s_pi[1], avg_r, kp, ki, kd, ilim, ff_slope, ff_fric, full_counts);
    Load(s_pi[0].out, s_pi[1].out);
}

int SpeedControl_Counts(int wheel)
{
    return s_counts[wheel & 1];
}

/* 最近一个测速窗（SPEED_AVG_TICKS × 5ms = 20ms）里的**计数增量之和**，
 * 给屏上「电机测试」页当"实时速度"用：满速 7200 -> 4950 计数/s -> 一个 20ms 窗
 * 刚好约 100，也就是 0..100 的刻度里 100 = 满速（倒转就是负的）。
 * 界面不能自己调 Encoder_ReadDelta() 去数：那个函数读完立刻把 TIM 计数器清零，
 * 界面拿走这一拍，速度环就少一拍。窗口没填满（刚清过/刚使能）时和值偏小，
 * 20ms 之后就是满窗。 */
int SpeedControl_WindowCounts(int wheel)
{
    return s_win_sum[wheel & 1];
}

int SpeedControl_SpeedMmps(int wheel)
{
    return (int)s_mmps[wheel & 1];
}

int SpeedControl_Output(int wheel)
{
    return s_pi[wheel & 1].out;
}

int SpeedControl_Target(int wheel)
{
    return (int)s_pi[wheel & 1].target;
}
