/**
 * @file  t_nvram.c
 * @brief 真 nvram.c 的主机测试：固化 -> 掉电 -> 重启后参数/标定/动作表回来
 *
 * 为什么值得单独测：nvram.c 是全工程**唯一会擦写 Flash 的模块**，存的是当前 payload
 * （g_cfg + trace_cal 两套标定 + 13 个控制环标定量 + SERVO_ACTION_N 个动作角度）。
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

/* ⑦ 用的"老记录"长度：只保留 payload 开头 4 字节（第一个字段 trace_speed）。
 * ★故意不用 `当前长度 - N` 那种差值写法：N = "自那个老版本以来往末尾又追加了多少字节"，
 *   每加一批字段就变一次（v6 +8、v7/v8 又 +8），而且加到后来差值里已经把新字段包进去了，
 *   "末尾新字段保持当前值"就再也测不到。取一个比任何真实历史记录都短的固定前缀，
 *   判据与真实升级路径逐字一致，以后再追加字段也不用回来改这里。 */
#define OLD_REC_LEN 4u

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

/* 动作表替身：SERVO_ACTION_N 项，nvram 只通过 ActionPose/SetActionPose 读写 */
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
    /* 页里那条记录自称的版本号 / payload 长度（= 固件写下来的，见 ⑥ 开头）。
     * ⑥⑦ 两块共用 —— 不写死数字，否则每次往 payload 末尾加字段都要回来改测试。 */
    uint16_t fw_ver = 0u;
    uint16_t fw_len = 0u;

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
        uint16_t bad_ver;

        /* ★版本号 / payload 长度都从**固件刚写下的那条记录**里读回来（写端是唯一真值
         * 来源），⑥⑦ 两块共用。这里不写死数字：往 payload 末尾加字段就必须升
         * NVRAM_VERSION、长度也跟着变，写死的话"改一次固件 = 坏一次测试"，而且会
         * **静默失效** —— 写死的那个数一旦不再等于当前版本，下面"短记录标成当前版本
         * 必须判坏"就变成在测一条无关的分支了。 */
        CHECK_EQ_I(pg[0], 0x48);       /* magic "HXB1"：先确认页里真有一条记录 */
        CHECK_EQ_I(pg[1], 0x58);
        CHECK_EQ_I(pg[2], 0x42);
        CHECK_EQ_I(pg[3], 0x31);
        fw_ver = (uint16_t)((uint16_t)pg[4] | ((uint16_t)pg[5] << 8));
        fw_len = (uint16_t)((uint16_t)pg[6] | ((uint16_t)pg[7] << 8));
        CHECK(fw_ver >= 2u);           /* ⑦ 要拿 fw_ver-1 当"老版本"，至少得减出 1 */
        CHECK(fw_len > OLD_REC_LEN);   /* 当前 payload 必须比 ⑦ 那条短记录长 */

        pg[REC_HDR + 5] ^= 0xFFu;      /* payload 里翻一位 */
        power_off();
        g_cfg.trace_speed = 1;
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);
        CHECK_EQ_I(g_cfg.trace_speed, 1);              /* 没被坏记录盖 */
        CHECK_EQ_I(s_settings_loaded_calls, 0);

        /* 版本号越界也算坏记录（fw_ver + 1 = 比当前版本还新） */
        pg[REC_HDR + 5] ^= 0xFFu;      /* 先还原 */
        bad_ver = (uint16_t)(fw_ver + 1u);
        pg[4] = (uint8_t)(bad_ver & 0xFFu);
        pg[5] = (uint8_t)(bad_ver >> 8);
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);
    }

    /* ---------- ⑦ 老记录的短 length：必须能升级读回，末尾新字段不许被打回 0 ----------
     * payload 每往末尾追加字段（v6 的 gyro_corr_ref、v7/v8 的 trace_corner…），老记录的
     * length 就更短 ⇒ nvram_init() 走"先按当前值 snapshot、再 memcpy 记录前缀"那一支：
     * **记录里有的字段原样保留，记录里没有的字段保持参数表当前值**。
     * 这条判据不能写成 `fw_len - 8`（见 OLD_REC_LEN 的注释），所以这里用一条"只保留开头
     * 4 字节"的老记录：既验证前缀被原样读回，又验证记录里没有的末尾字段（gyro_corr_ref）
     * 保持参数表当前值，而不是被旧存档里的字节/pad 区的 0 静默关掉。 */
    {
        uint8_t *pg = host_flash_page(63);
        uint16_t old_ver = (uint16_t)(fw_ver - 1u);   /* 比当前版本老 ⇒ 走升级那一支 */
        uint32_t c;

        pg[REC_HDR + 0] = 0x11u;                       /* trace_speed = 0x44332211（小端）*/
        pg[REC_HDR + 1] = 0x22u;
        pg[REC_HDR + 2] = 0x33u;
        pg[REC_HDR + 3] = 0x44u;
        pg[4] = (uint8_t)(old_ver & 0xFFu);
        pg[5] = (uint8_t)(old_ver >> 8);
        pg[6] = (uint8_t)(OLD_REC_LEN & 0xFFu);
        pg[7] = (uint8_t)((OLD_REC_LEN >> 8) & 0xFFu);
        c = t_crc32(&pg[REC_HDR], OLD_REC_LEN);
        pg[8]  = (uint8_t)(c & 0xFFu);
        pg[9]  = (uint8_t)((c >> 8) & 0xFFu);
        pg[10] = (uint8_t)((c >> 16) & 0xFFu);
        pg[11] = (uint8_t)((c >> 24) & 0xFFu);

        power_off();                                   /* 掉电：RAM 全清 */
        host_param_set_local(HP_GYRO_CORR_REF, 9999.0f);  /* 参数表里放一个可辨认的当前值 */
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 1);                 /* 老记录（短 length）要能升级读回 */
        CHECK_EQ_I(s_settings_loaded_calls, 1);        /* 确实 apply() 进去了 */
        CHECK_EQ_I(g_cfg.trace_speed, 0x44332211);     /* 记录里有的字段原样保留 */
        CHECK(near(host_param_get(HP_GYRO_CORR_REF, -1.0f), 9999.0f));  /* 没有的保持当前值 */

        /* 同一条短 length 如果标成**当前版本**：长度和结构对不上 -> 必须判坏。
         * （这正是"往 payload 末尾加字段就必须升 NVRAM_VERSION"的守门人） */
        pg[4] = (uint8_t)(fw_ver & 0xFFu);
        pg[5] = (uint8_t)(fw_ver >> 8);
        power_off();
        nvram_init();
        CHECK_EQ_I(nvram_loaded(), 0);
        CHECK_EQ_I(s_settings_loaded_calls, 0);        /* 坏记录不许 apply() */
    }

    return T_SUMMARY();
}
