/**
 * @file  t_nvram.c
 * @brief 真 nvram.c 的主机测试：固化 -> 掉电 -> 重启后参数/标定/动作表回来
 *
 * 为什么值得单独测：nvram.c 是全工程**唯一会擦写 Flash 的模块**，存的是 v5
 * payload（g_cfg + trace_cal 两套标定 + 13 个控制环标定量 + 14 个动作角度）。
 * 既有测试只到"排了一次保存队列"这一层（t_hostext 看假 nvram_touch 计数、
 * t_hostlink:535 看 ACK），真落盘 + 重启恢复这条链一直没人跑过。
 *
 * 本文件只链真 nvram.c + 真 trace_cal.c + 三个共享桩（host_hal/host_board/
 * host_rtt，由 run_tests.ps1 恒定附带），其余外部依赖在本文件内做最小替身，
 * 因此不动任何既有桩文件。
 */
#include "t_common.h"
#include "app_ui.h"      /* app_settings_t g_cfg */
#include "nvram.h"
#include "servo.h"
#include "motion.h"
#include "speed_control.h"
#include "gyro_control.h"
#include "trace_cal.h"
#include "host_link.h"

#include <math.h>

/* 主机桩的入口（照 t_hostext.c / t_tracecal.c 的惯例，用 extern 声明） */
extern void     host_tick_set(uint32_t ms);
extern void     host_flash_reset(void);
extern uint8_t *host_flash_page(uint32_t page);
extern int      host_flash_uses_real_addr(void);

/* nvram 记录头：magic(4) + version(2) + length(2) + crc32(4) */
#define REC_HDR 12

/* ---------------- 被 nvram.c 依赖的最小替身 ---------------- */

app_settings_t g_cfg;

static uint8_t s_gyro_axis = 0u;
static int     s_settings_loaded_calls = 0;

uint8_t app_ui_gyro_axis_mode(void) { return s_gyro_axis; }
void    app_ui_set_gyro_axis_mode(uint8_t mode) { s_gyro_axis = (uint8_t)(mode & 7u); }
void    app_ui_settings_loaded(void) { s_settings_loaded_calls++; }

int trace_read_channel(uint8_t idx) { (void)idx; return 0; }   /* trace_cal 的兜底回落 */

static Motion_Mode s_motion = MOTION_IDLE;
static uint8_t     s_speed_en = 0u;
static uint8_t     s_gyro_en = 0u;

Motion_Mode Motion_GetMode(void) { return s_motion; }
uint8_t     SpeedControl_IsEnabled(void) { return s_speed_en; }
uint8_t     GyroControl_IsEnabled(void) { return s_gyro_en; }

/* 动作表替身：14 项，nvram 只通过 ActionPose/SetActionPose 读写 */
static int s_pose[SERVO_ACTION_N][2];

void Servo_ActionPose(int x, int *a1, int *a2)
{
    if (x < 1 || x > SERVO_ACTION_N) {
        if (a1) *a1 = 0;
        if (a2) *a2 = 0;
        return;
    }
    if (a1) *a1 = s_pose[x - 1][0];
    if (a2) *a2 = s_pose[x - 1][1];
}

int Servo_SetActionPose(int x, int a1, int a2)
{
    if (x < 1 || x > SERVO_ACTION_N) return 0;
    if (a1 < 0) a1 = 0;
    if (a1 > 180) a1 = 180;
    if (a2 < 0) a2 = 0;
    if (a2 > 180) a2 = 180;
    s_pose[x - 1][0] = a1;
    s_pose[x - 1][1] = a2;
    return 1;
}

/* 参数表替身：只放 nvram 快照用到的 14 个 v5/v6 标定量，其余 id 走兜底 */
typedef struct { uint16_t id; float def; float val; } pslot_t;

static pslot_t s_ps[] = {
    { .id = HP_GYRO_ANG_KP,           .def =   3.20f },
    { .id = HP_GYRO_RATE_KP,          .def =   1.80f },
    { .id = HP_GYRO_RATE_KD,          .def =   0.30f },
    { .id = HP_GYRO_RATE_MAX,         .def = 200.0f  },
    { .id = HP_GYRO_CORR_LIMIT,       .def = 900.0f  },
    { .id = HP_GYRO_CORR_SLEW,        .def = 4000.0f },
    { .id = HP_GYRO_SETTLE_DEG,       .def =   4.0f  },
    { .id = HP_GYRO_SETTLE_MS,        .def = 200.0f  },
    { .id = HP_TRACE_ANGLE_PER_UNIT,  .def =   3.50f },
    { .id = HP_TRACE_ANGLE_MAX,       .def =  45.0f  },
    { .id = HP_PIVOT_KP,              .def = 900.0f  },
    { .id = HP_PIVOT_KI,              .def =   0.0f  },
    { .id = HP_PIVOT_KD,              .def =  60.0f  },
    { .id = HP_GYRO_CORR_REF,         .def = 2100.0f },   /* v6 */
};

#define PS_N ((int)(sizeof(s_ps) / sizeof(s_ps[0])))

static int ps_idx(uint16_t id)
{
    int i;
    for (i = 0; i < PS_N; i++) {
        if (s_ps[i].id == id) return i;
    }
    return -1;
}

float host_param_get(uint16_t id, float fallback)
{
    int i = ps_idx(id);
    return (i < 0) ? fallback : s_ps[i].val;
}

void host_param_set_local(uint16_t id, float v)
{
    int i = ps_idx(id);
    if (i >= 0) s_ps[i].val = v;
}

int host_param_bounds(uint16_t id, float *def, float *lo, float *hi)
{
    int i = ps_idx(id);
    if (def) *def = (i < 0) ? 0.0f : s_ps[i].def;
    if (lo)  *lo = -1.0e9f;
    if (hi)  *hi =  1.0e9f;
    return 1;
}

/* ---------------- 测试辅助 ---------------- */

static uint32_t s_now = 100000u;

static void tick_to(uint32_t ms)
{
    s_now = ms;
    host_tick_set(s_now);
}

static void tick_add(uint32_t d)
{
    tick_to(s_now + d);
}

/* 把 RAM 里的活状态清成"刚上电"的样子（g_cfg / 参数表 / 动作表 / 轴映射） */
static void power_off(void)
{
    int i;
    memset(&g_cfg, 0, sizeof(g_cfg));
    for (i = 0; i < PS_N; i++) s_ps[i].val = 0.0f;
    memset(s_pose, 0, sizeof(s_pose));
    s_gyro_axis = 0u;
    s_settings_loaded_calls = 0;
    s_motion = MOTION_IDLE;
}

static void pose_set(int act, int a1, int a2) { (void)Servo_SetActionPose(act, a1, a2); }

/* 与 nvram.c 里那份 static crc32 同一个算法（poly 0xEDB88320、末尾取反）：
 * 测试里要手改记录（改 version/length）就得自己把校验重算回来。 */
static uint32_t t_crc32(const uint8_t *p, uint32_t n)
{
    uint32_t crc = 0xFFFFFFFFUL;
    uint32_t i;

    while (n-- != 0U) {
        crc ^= (uint32_t)(*p++);
        for (i = 0U; i < 8U; i++) {
            crc = (crc >> 1) ^ (0xEDB88320UL & (uint32_t)(-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static int near(float a, float b) { return fabsf(a - b) < 1.0e-3f; }

static int page_all_ff(uint32_t page)
{
    const uint8_t *p = host_flash_page(page);
    uint32_t i;
    if (p == NULL) return 0;
    for (i = 0; i < 2048u; i++) {
        if (p[i] != 0xFFu) return 0;
    }
    return 1;
}

int main(void)
{
    int i;

    host_flash_reset();                 /* 上电是 0x00，先抹成"已擦除"的 0xFF */

    /* nvram 是按地址直读 Flash 的（*(uint32_t*)0x0801F800），主机上必须
     * 真把假 Flash 映射到 0x08000000，否则一读就崩 —— 先自检这条前提。 */
    CHECK_EQ_I(host_flash_uses_real_addr(), 1);

    /* ---------- ① 空 Flash：不加载，值保持现状 ---------- */
    power_off();
    g_cfg.trace_speed = 1000;
    tick_to(100000u);
    nvram_init();
    CHECK_EQ_I(nvram_loaded(), 0);
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);
    CHECK_EQ_I(g_cfg.trace_speed, 1000);
    CHECK_EQ_I(s_settings_loaded_calls, 0);
    CHECK(page_all_ff(63));

    /* ---------- ② 改 -> 固化 -> 掉电 -> 重启，全回来 ---------- */
    g_cfg.trace_speed  = 4321;
    g_cfg.line_kp_x100 = 777;
    g_cfg.speed_kp_x100 = 2500;
    s_gyro_axis = 5u;
    for (i = 0; i < PS_N; i++) s_ps[i].val = s_ps[i].def;
    s_ps[ps_idx(HP_PIVOT_KD)].val       = 123.0f;      /* x1   */
    s_ps[ps_idx(HP_GYRO_ANG_KP)].val    = 4.25f;       /* x100 */
    s_ps[ps_idx(HP_GYRO_SETTLE_DEG)].val = 3.5f;       /* x10  */
    pose_set(3, 170, 12);
    pose_set(14, 30, 60);

    nvram_touch();                     /* 一键固化：跳过 1.5s 防抖 */
    tick_add(200u);
    nvram_poll();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_BUSY);
    tick_add(600u);
    nvram_service();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_DONE);
    CHECK(!page_all_ff(63));           /* 真写进去了 */

    power_off();                       /* 掉电 */
    nvram_init();                      /* 上电，从页 63 读回 */
    CHECK_EQ_I(nvram_loaded(), 1);
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);
    CHECK_EQ_I(g_cfg.trace_speed, 4321);
    CHECK_EQ_I(g_cfg.line_kp_x100, 777);
    CHECK_EQ_I(g_cfg.speed_kp_x100, 2500);
    CHECK_EQ_I(s_gyro_axis, 5);
    CHECK(near(host_param_get(HP_PIVOT_KD, -1.0f), 123.0f));
    CHECK(near(host_param_get(HP_GYRO_ANG_KP, -1.0f), 4.25f));
    CHECK(near(host_param_get(HP_GYRO_SETTLE_DEG, -1.0f), 3.5f));
    CHECK(near(host_param_get(HP_PIVOT_KP, -1.0f), 900.0f));   /* 没动过的也回来 */
    {
        int a1 = -1, a2 = -1;
        Servo_ActionPose(3, &a1, &a2);
        CHECK_EQ_I(a1, 170);
        CHECK_EQ_I(a2, 12);
        Servo_ActionPose(14, &a1, &a2);
        CHECK_EQ_I(a1, 30);
        CHECK_EQ_I(a2, 60);
        Servo_ActionPose(2, &a1, &a2);
        CHECK_EQ_I(a1, 0);
        CHECK_EQ_I(a2, 0);
    }
    CHECK_EQ_I(s_settings_loaded_calls, 1);   /* 盖完之后通知 UI 重刷 */

    /* ---------- ③ 状态没变就不擦页 ---------- */
    {
        uint8_t *pg = host_flash_page(63);
        pg[1900] = 0x5Au;              /* 记录之外的哨兵，页被擦就会没 */
        tick_add(1000u);
        nvram_poll();
        tick_add(700u);
        nvram_poll();
        nvram_service();
        CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);
        CHECK_EQ_I(pg[1900], 0x5A);
    }

    /* ---------- ④ 防抖：改完 1.5s 内不写 ---------- */
    g_cfg.trace_speed = 5555;
    tick_add(200u);
    nvram_poll();                      /* 首次发现变化 -> 重新计时 */
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);
    tick_add(1400u);
    nvram_poll();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);   /* 累计 1.4s，还不到 */
    tick_add(200u);
    nvram_poll();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_BUSY);   /* 累计 1.6s，排上 */
    tick_add(600u);
    nvram_service();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_DONE);
    tick_add(1200u);                                  /* "保存成功"挂 1.2s 后自己消失 */
    nvram_service();
    CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);

    /* ---------- ⑤ 运动中绝不擦写 ---------- */
    {
        uint8_t *pg = host_flash_page(63);
        s_motion = MOTION_GYRO;
        g_cfg.trace_speed = 6666;
        tick_add(200u);
        nvram_poll();
        tick_add(2000u);
        nvram_poll();
        CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);   /* 早就过了 1.5s，仍不排 */

        s_motion = MOTION_IDLE;                        /* 停车后才排上 */
        tick_add(200u);
        nvram_poll();
        tick_add(2000u);
        nvram_poll();
        CHECK_EQ_I(nvram_state(), NVRAM_STATE_BUSY);

        pg[1950] = 0x3Cu;                              /* 新哨兵 */
        s_motion = MOTION_TRACE;                       /* 排队之后又动起来 */
        tick_add(600u);
        nvram_service();
        CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);   /* 撤回，不写 */
        CHECK_EQ_I(pg[1950], 0x3C);
        s_motion = MOTION_IDLE;

        pg[1951] = 0x5Au;
        g_cfg.trace_speed = 5555;                      /* 改回"上次存过的值"：不算变化 */
        tick_add(200u);
        nvram_poll();
        tick_add(2000u);
        nvram_poll();
        nvram_service();
        CHECK_EQ_I(nvram_state(), NVRAM_STATE_IDLE);   /* 一致就不写 */
        CHECK_EQ_I(pg[1951], 0x5A);                    /* 页没被擦 */
    }

    /* ---------- ⑥ 记录被改坏 -> CRC 兜底，按"没存过"处理 ---------- */
    {
        uint8_t *pg = host_flash_page(63);
        pg[REC_HDR + 5] ^= 0xFFu;      /* payload 里翻一位 */
        power_off();
        g_cfg.trace_speed = 1;
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);
        CHECK_EQ_I(g_cfg.trace_speed, 1);              /* 没被坏记录盖 */
        CHECK_EQ_I(s_settings_loaded_calls, 0);

        /* 版本号越界也算坏记录 */
        pg[REC_HDR + 5] ^= 0xFFu;      /* 先还原 */
        pg[4] = 9u;                    /* version = 9 > NVRAM_VERSION(6) */
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);
    }

    /* ---------- ⑦ 老版本(v5)记录升级：新加的 gyro_corr_ref 不许被打回 0 ----------
     * v6 在 payload 末尾追加了 gyro_corr_ref（差速随速放大参考）。老记录的 length
     * 比现在短 8 字节 ⇒ 走 nvram_init() 里"先按当前值 snapshot、再 memcpy 前缀"那一支：
     * 记录里有的字段原样保留，记录里没有的新字段保持参数表当前值（出厂 3000）。
     * 这里把页里那条记录改扮成 v5 再读回来验证这条路径 —— 要是哪天有人把新字段塞进
     * 老的 pad 区（pad 是 0），升级后放大就被静默关成 0，用户满速又转不动了。 */
    {
        uint8_t *pg = host_flash_page(63);
        uint16_t cur_len = (uint16_t)((uint16_t)pg[6] | ((uint16_t)pg[7] << 8));
        uint16_t v5_len  = (uint16_t)(cur_len - 8U);
        uint32_t c;

        pg[4] = 5u;                                    /* version = 5 (< NVRAM_VERSION) */
        pg[5] = 0u;
        pg[6] = (uint8_t)(v5_len & 0xFFu);
        pg[7] = (uint8_t)((v5_len >> 8) & 0xFFu);
        c = t_crc32(&pg[REC_HDR], v5_len);
        pg[8]  = (uint8_t)(c & 0xFFu);
        pg[9]  = (uint8_t)((c >> 8) & 0xFFu);
        pg[10] = (uint8_t)((c >> 16) & 0xFFu);
        pg[11] = (uint8_t)((c >> 24) & 0xFFu);

        /* 同一份短 length 如果标成 v6：长度和结构对不上 -> 必须判坏。
         * （这正是"往 payload 末尾加字段就必须升 NVRAM_VERSION"的原因） */
        pg[4] = 6u;
        power_off();
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);

        /* 改回 v5：老记录，前缀读。参数表里先放一个可辨认的当前值 */
        pg[4] = 5u;
        host_param_set_local(HP_GYRO_CORR_REF, 9999.0f);
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 1);
        CHECK_EQ_I(s_settings_loaded_calls, 1);        /* 确实 apply() 进去了 */
        CHECK(near(host_param_get(HP_GYRO_CORR_REF, -1.0f), 9999.0f));
    }

    return T_SUMMARY();
}
