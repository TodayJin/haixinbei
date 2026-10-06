/**
 * @file  nvram.c
 * @brief 设置 + 标定的掉电保存（实现）
 *
 * 见 nvram.h 的格式说明。这里只依赖 app_ui（g_cfg / 陀螺仪档位）和
 * trace_cal（标定数据），不碰别的业务。
 */
#include "nvram.h"

#include "app_ui.h"
#include "board.h"
#include "gyro_control.h"   /* GyroControl_IsEnabled：运动中就别动 Flash */
#include "host_link.h"      /* host_param_* + HP_* 参数 id（v5：控制环标定量进参数表） */
#include "kk_oled.h"
#include "motion.h"         /* Motion_GetMode / MOTION_IDLE */
#include "servo.h"          /* v5：舵机动作表（14 个动作的终点角度） */
#include "speed_control.h"  /* SpeedControl_IsEnabled */
#include "trace_cal.h"

#include <string.h>
#include <stddef.h>

#define NVRAM_MAGIC      0x31425848UL   /* "HXB1"（小端存下来是 48 58 42 31） */
#define NVRAM_VERSION    8U             /* v8：舵机动作表 14 → **20** 个（v7：直角转弯开关；v6：角度环差速天花板参考；
                                         * v5：控制环标定量 + 舵机动作表）。
                                         * ★铁律：**payload 长大就必须升这个版本号** —— 判据是
                                         *   `version == NVRAM_VERSION` 时要求 `length == sizeof(payload)`，
                                         *   只加字段不升版本 ⇒ 旧存档被判不合格 ⇒ **用户所有设置回默认**。 */
#define NVRAM_VERSION_MIN 1U            /* 能读的最老版本（靠"字段只往末尾追加"兼容升级：
                                         * 比当前版本短的记录，缺的那些字段取当前默认值）*/

#define NVRAM_FLASH_BASE 0x08000000UL
#define NVRAM_PAGE_INDEX 63UL                      /* 128KB / 2KB - 1 = 最后一页 */
#define NVRAM_PAGE_ADDR  (NVRAM_FLASH_BASE + NVRAM_PAGE_INDEX * 0x800UL)
/* 本板 Flash 实际组织（2026-10-04 真板实测，块模型）：单 bank、32 个 **4KB 擦除块**，
 * 块号 = PNB，块地址 = 0x08000000 + PNB×0x1000；PNB 0..31 全部有效，BKER 被硬件忽略。
 * NVRAM 页 0x0801F800（绝对页 63）落在块 31 里 ⇒ 块 31 = 页 62 + 页 63。
 * ⚠️ 所以每次存参数都会**连带擦掉页 62**：页 62 必须保持"无人使用"（任务存储在页 58..61，
 *    见 App/Svc/task_store.h 的页分配注释）。 */
#define NVRAM_BANK_PAGE   31UL                     /* = 块号：块 31 = 页 62+63 = 0x0801F800 */

#define NVRAM_CHECK_PERIOD_MS  200U    /* 多久对比一次"当前值 vs 已存值" */
#define NVRAM_SAVE_DELAY_MS   1500U    /* 最后一次改动之后静这么久才动 Flash */
#define NVRAM_FAIL_BACKOFF_MS 5000U    /* 写失败（页坏了/RDP 保护）：5s 后再试 */
#define NVRAM_FAIL_BACKOFF_MAX_MS 60000U  /* 连续失败时退避翻倍到此封顶 */
#define NVRAM_ERASE_TIMEOUT_MS  50U    /* 一页擦除典型 <5ms，超时就不死等 */

/* 存进去的内容：菜单里所有可改的东西 + 标定数据。
 * 字段顺序不要动（旧记录靠 version + length 判定，改了要升 version）。 */
typedef struct {
    int32_t  trace_speed;
    int32_t  speed_kp_x100, speed_ki_x100, speed_kd_x100, speed_ilim;
    int32_t  gyro_kp_x100,  gyro_ki_x100,  gyro_kd_x100,  gyro_ilim;
    int32_t  line_kp_x100,  line_ki_x100,  line_kd_x100,  line_ilim;
    uint8_t  use_gyro, beep_key, beep_marker, trace_invert;
    uint8_t  gray_mode;          /* TRACE_MODE_*：灰度接口 */
    uint8_t  gyro_axis_mode;     /* 陀螺仪方框轴映射档 0..7（屏上 M1..M8） */
    uint8_t  pad[2];
    trace_cal_data_t cal;        /* 8 路黑白标定 */
    /* ---- v2 新增，只能往末尾加 ---- */
    int32_t  speed_ff_slope_x1000;   /* 速度环前馈斜率 ×1000 */
    int32_t  speed_ff_fric;          /* 速度环前馈摩擦项 */
    /* ---- v3 新增 ---- */
    uint8_t  trace_adapt;            /* 「连续判线」0 = 纯 0/1，1 = 连续插值 */
    uint8_t  pad3[3];
    /* ---- v4 新增：两套标定（0=普通 1=亚克力，用户 m07888）----
     * cal 里**永远**是第 0 套、cal_b 里永远是第 1 套（与"当前在用哪套"无关），
     * 这样切来切去也不会把某一套覆盖掉；cal_set 只说现在用哪套。 */
    trace_cal_data_t cal_b;          /* 第 1 套（亚克力路段） */
    uint8_t  cal_set;                /* 当前用第几套 0/1 */
    uint8_t  pad4[3];
    /* ---- v5 新增：控制环标定量 + 舵机动作表（用户 m13466 要求上位机改完能固化）----
     * 控制环那 13 个量**同时活在参数表 s_param[] 里**（控制环每拍从参数表读），
     * Flash 里这份只是参数表的镜像 ⇒ snapshot/apply 全部走 host_param_*，
     * 不另开一套真值来源（否则两边会漂）。缩放与已有 *_x100 一致：
     * 增益 ×100，速度/角度 ×10，限幅/斜率/毫秒 ×1。 */
    int32_t  gyro_ang_kp_x100;
    int32_t  gyro_rate_kp_x100;
    int32_t  gyro_rate_kd_x100;
    int32_t  gyro_rate_max_x10;
    int32_t  gyro_corr_limit;
    int32_t  gyro_corr_slew;
    int32_t  gyro_settle_deg_x10;
    int32_t  gyro_settle_ms;
    int32_t  trace_per_unit_x100;
    int32_t  trace_aim_max_x10;
    int32_t  pivot_kp;
    int32_t  pivot_ki_x100;
    int32_t  pivot_kd;
    uint8_t  action_pose[SERVO_ACTION_N][2];   /* 14 → **20** 个动作（用户 m 2026-10-07）：+12 字节，
                                                * 靠下面的 pad 凑成 +16 ⇒ 记录仍是 8 的整数倍。 */
    uint8_t  pad_action[4];                    /* ★补齐：动作表涨 12 字节，这里补 4 ⇒ 总涨 16（8 对齐） */
    uint8_t  pad5[10];                         /* 补到 8 的整数倍（见下面的编译期断言） */
    /* ---- v6 新增：角度环差速天花板随速放大的参考刻度（用户 m24215）----
     * 只往末尾加、并且**多留 4 字节 pad6**：payload 必须比 v5 大，
     * 这样旧记录（length 更短）走的是 nvram_init 里"先 snapshot 铺默认值再 memcpy 前缀"
     * 那一支，新字段拿到的是当前默认值（3000），而不是原 pad 区的 0（= 把放大功能关掉）。 */
    int32_t  gyro_corr_ref;
    uint8_t  pad6[4];
    /* ---- v7 新增：「直角转弯」开关（用户 m 2026-10-07：屏幕里能设置的都要保存）----
     * 与 `trace_adapt` 同一套路：payload 里开专用字段、snapshot/apply 直接读写 `g_cfg`。
     * 同样只往末尾加 + 留 pad：旧记录（length 更短）走 nvram_init 里"先 snapshot 铺默认值
     * 再 memcpy 前缀"那一支 ⇒ 新字段拿到的是**当前默认值**（0 = 关），不是 pad 区的 0。 */
    uint8_t  trace_corner;
    uint8_t  pad7[7];                          /* ★必须凑满 8 字节：下面有编译期断言
                                                *   `nvram_record_size_must_be_8x`，加 4 字节会当场编不过。 */
} nvram_payload_t;

typedef struct {
    uint32_t        magic;
    uint16_t        version;
    uint16_t        length;      /* = sizeof(nvram_payload_t) */
    uint32_t        crc;         /* payload 的 CRC32 */
    nvram_payload_t data;
    uint8_t         pad[4];      /* 补齐成 8 的整数倍，好按双字写 */
} nvram_record_t;

/* 编译期断言：整个记录必须是 8 字节的整数倍。s_stage 按 64 位（双字）写 Flash，
 * `words[sizeof(...) / 8U]` 一旦除不尽，尾部那几个字节就永远写不进去 ——
 * 加字段（尤其奇数长度的 uint8_t 数组）时必须同步调 pad，这里替我们挡住。 */
typedef char nvram_record_size_must_be_8x[(sizeof(nvram_record_t) % 8U == 0U) ? 1 : -1];

/* 按 64 位写 Flash，所以暂存要 8 字节对齐 */
static union {
    nvram_record_t rec;
    uint64_t       words[sizeof(nvram_record_t) / 8U];
} s_stage;

static nvram_payload_t s_live;      /* 最近一次看到的现场值 */
static nvram_payload_t s_saved;     /* 已经躺在 Flash 里的那份 */
static uint32_t s_change_ms;        /* 现场值最后一次变化的时刻 */
static uint32_t s_check_ms;
static uint32_t s_fail_until_ms;
static uint32_t s_fail_backoff_ms = NVRAM_FAIL_BACKOFF_MS;   /* 连续失败时翻倍 */
static uint8_t  s_loaded;
static uint8_t  s_state;            /* NVRAM_STATE_*：给界面看的保存状态 */
static uint32_t s_state_ms;         /* 进入当前状态的时刻 */

/* ------------------------------------------------------------------ CRC */

static uint32_t crc32(const uint8_t *p, uint32_t n)
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

/* -------------------------------------------------------------- 现场快照 */

/**
 * @brief 参数表的值 ×scale 存成整数 / 反变换
 *
 * 兜底值取**参数表自己的出厂默认**（host_param_bounds），这样 Flash 里那份和
 * 参数表不会出现两套默认值；缩放约定与已有字段一致：增益 ×100、速度/角度 ×10、
 * 限幅/斜率/毫秒 ×1。正数加 0.5 取整（这些量实际都是非负的）。
 */
static int32_t snap_param_scaled(uint16_t id, float scale)
{
    float def = 0.0f;
    float v;

    (void)host_param_bounds(id, &def, NULL, NULL);
    v = host_param_get(id, def) * scale;
    return (int32_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

static void apply_param_scaled(uint16_t id, int32_t raw, float scale)
{
    host_param_set_local(id, (float)raw / scale);
}

static void snapshot(nvram_payload_t *p)
{
    int i;

    memset(p, 0, sizeof(*p));

    p->trace_speed   = g_cfg.trace_speed;
    p->speed_kp_x100 = g_cfg.speed_kp_x100;
    p->speed_ki_x100 = g_cfg.speed_ki_x100;
    p->speed_kd_x100 = g_cfg.speed_kd_x100;
    p->speed_ilim    = g_cfg.speed_ilim;
    p->gyro_kp_x100  = g_cfg.gyro_kp_x100;
    p->gyro_ki_x100  = g_cfg.gyro_ki_x100;
    p->gyro_kd_x100  = g_cfg.gyro_kd_x100;
    p->gyro_ilim     = g_cfg.gyro_ilim;
    p->line_kp_x100  = g_cfg.line_kp_x100;
    p->line_ki_x100  = g_cfg.line_ki_x100;
    p->line_kd_x100  = g_cfg.line_kd_x100;
    p->line_ilim     = g_cfg.line_ilim;

    p->use_gyro     = (uint8_t)(g_cfg.use_gyro ? 1U : 0U);
    p->beep_key     = (uint8_t)(g_cfg.beep_key ? 1U : 0U);
    p->beep_marker  = (uint8_t)(g_cfg.beep_marker ? 1U : 0U);
    p->trace_invert = (uint8_t)(g_cfg.trace_invert ? 1U : 0U);

    /* 存"当前真的在用"的通讯方式（app_ui_tick 会把菜单里那一项同步过来），
     * 而不是 g_cfg.gray_mode 那个镜像，免得存出一个还没生效的值。 */
    p->gray_mode      = (uint8_t)(trace_cal_mode % TRACE_MODE_COUNT);
    p->gyro_axis_mode = (uint8_t)(app_ui_gyro_axis_mode() & 7U);

    /* cal/cal_b 两套永远是"0 号套 -> cal、1 号套 -> cal_b"，跟屏上正在看哪一套无关
     * （trace_cal_export 自己按 s_set 摆位，所以预览来预览去也不会把两套写乱）。
     * `cal_set` 这个字段从用户 m09558 起不再表达"用户选了哪套"：循迹页的右键只是
     * 预览、不落盘，所以这里固定写 0 —— 开机一定回到普通那套，而亚克力那套原样
     * 存在 cal_b 里（预览要用、任务程序 trace_cal_select_set() 也要用）。
     * 这一点很重要：如果这里跟着 s_set 写，预览一下就会让 payload 变化，
     * nvram_poll() 就会白写一次 Flash + 弹「保存成功」。 */
    trace_cal_export(&p->cal, &p->cal_b, NULL);
    p->cal_set = 0U;

    p->speed_ff_slope_x1000 = g_cfg.speed_ff_slope_x1000;
    p->speed_ff_fric        = g_cfg.speed_ff_fric;

    /* v3：「连续判线」口径（0 = 纯 0/1，1 = 连续插值）。存的是 g_cfg 里那一项（菜单直接改它） */
    p->trace_adapt = (uint8_t)(g_cfg.trace_adapt ? 1U : 0U);
    p->trace_corner = (uint8_t)(g_cfg.trace_corner ? 1U : 0U);   /* ★v7：直角转弯开关 */

    /* v5：控制环标定量（参数表镜像）+ 舵机动作表 */
    p->gyro_ang_kp_x100    = snap_param_scaled(HP_GYRO_ANG_KP,        100.0f);
    p->gyro_rate_kp_x100   = snap_param_scaled(HP_GYRO_RATE_KP,       100.0f);
    p->gyro_rate_kd_x100   = snap_param_scaled(HP_GYRO_RATE_KD,       100.0f);
    p->gyro_rate_max_x10   = snap_param_scaled(HP_GYRO_RATE_MAX,       10.0f);
    p->gyro_corr_limit     = snap_param_scaled(HP_GYRO_CORR_LIMIT,      1.0f);
    p->gyro_corr_slew      = snap_param_scaled(HP_GYRO_CORR_SLEW,       1.0f);
    p->gyro_settle_deg_x10 = snap_param_scaled(HP_GYRO_SETTLE_DEG,     10.0f);
    p->gyro_settle_ms      = snap_param_scaled(HP_GYRO_SETTLE_MS,       1.0f);
    p->trace_per_unit_x100 = snap_param_scaled(HP_TRACE_ANGLE_PER_UNIT, 100.0f);
    p->trace_aim_max_x10   = snap_param_scaled(HP_TRACE_ANGLE_MAX,      10.0f);
    p->pivot_kp            = snap_param_scaled(HP_PIVOT_KP,              1.0f);
    p->pivot_ki_x100       = snap_param_scaled(HP_PIVOT_KI,            100.0f);
    p->pivot_kd            = snap_param_scaled(HP_PIVOT_KD,              1.0f);

    /* v6：差速天花板随速放大的参考刻度（0 = 关闭放大，退回旧行为） */
    p->gyro_corr_ref       = snap_param_scaled(HP_GYRO_CORR_REF,         1.0f);

    for (i = 0; i < SERVO_ACTION_N; i++) {
        int a1 = 0;
        int a2 = 0;
        Servo_ActionPose(i + 1, &a1, &a2);
        p->action_pose[i][0] = (uint8_t)a1;
        p->action_pose[i][1] = (uint8_t)a2;
    }
}

static void apply(const nvram_payload_t *p)
{
    int i;
    g_cfg.trace_speed   = p->trace_speed;
    g_cfg.speed_kp_x100 = p->speed_kp_x100;
    g_cfg.speed_ki_x100 = p->speed_ki_x100;
    g_cfg.speed_kd_x100 = p->speed_kd_x100;
    g_cfg.speed_ilim    = p->speed_ilim;
    g_cfg.gyro_kp_x100  = p->gyro_kp_x100;
    g_cfg.gyro_ki_x100  = p->gyro_ki_x100;
    g_cfg.gyro_kd_x100  = p->gyro_kd_x100;
    g_cfg.gyro_ilim     = p->gyro_ilim;
    g_cfg.line_kp_x100  = p->line_kp_x100;
    g_cfg.line_ki_x100  = p->line_ki_x100;
    g_cfg.line_kd_x100  = p->line_kd_x100;
    g_cfg.line_ilim     = p->line_ilim;

    g_cfg.speed_ff_slope_x1000 = p->speed_ff_slope_x1000;
    g_cfg.speed_ff_fric        = p->speed_ff_fric;

    g_cfg.use_gyro     = (p->use_gyro != 0U);
    g_cfg.beep_key     = (p->beep_key != 0U);
    g_cfg.beep_marker  = (p->beep_marker != 0U);
    g_cfg.trace_invert = (p->trace_invert != 0U);

    /* v3：「连续判线」口径（0 = 纯 0/1，1 = 连续插值）。老记录里没有这一项时，nvram_init 已经把
     * 默认值先填进 up 了，所以这里无条件盖是安全的。 */
    g_cfg.trace_adapt  = (p->trace_adapt != 0U);
    g_cfg.trace_corner = (p->trace_corner != 0U);   /* ★v7：直角转弯开关 */

    g_cfg.gray_mode = (int32_t)(p->gray_mode % TRACE_MODE_COUNT);

    app_ui_set_gyro_axis_mode((uint8_t)(p->gyro_axis_mode & 7U));
    /* v4：两套标定一起恢复（cal=第0套 / cal_b=第1套），并把 active 那套设为当前 */
    g_cfg.trace_cal_set = (int32_t)(p->cal_set & 1U);
    trace_cal_import(&p->cal, &p->cal_b, (uint8_t)(p->cal_set & 1U));

    /* v5：控制环标定量（写回参数表 —— 控制环每拍从参数表读，所以这里写完就生效）
     * + 舵机动作表（写进 servo.c 的 RAM 表；只改表、不掰舵机，避免载入时甩一下） */
    apply_param_scaled(HP_GYRO_ANG_KP,         p->gyro_ang_kp_x100,    100.0f);
    apply_param_scaled(HP_GYRO_RATE_KP,        p->gyro_rate_kp_x100,   100.0f);
    apply_param_scaled(HP_GYRO_RATE_KD,        p->gyro_rate_kd_x100,   100.0f);
    apply_param_scaled(HP_GYRO_RATE_MAX,       p->gyro_rate_max_x10,    10.0f);
    apply_param_scaled(HP_GYRO_CORR_LIMIT,     p->gyro_corr_limit,       1.0f);
    apply_param_scaled(HP_GYRO_CORR_SLEW,      p->gyro_corr_slew,        1.0f);
    apply_param_scaled(HP_GYRO_SETTLE_DEG,     p->gyro_settle_deg_x10,  10.0f);
    apply_param_scaled(HP_GYRO_SETTLE_MS,      p->gyro_settle_ms,        1.0f);
    apply_param_scaled(HP_TRACE_ANGLE_PER_UNIT, p->trace_per_unit_x100, 100.0f);
    apply_param_scaled(HP_TRACE_ANGLE_MAX,     p->trace_aim_max_x10,    10.0f);
    apply_param_scaled(HP_PIVOT_KP,            p->pivot_kp,              1.0f);
    apply_param_scaled(HP_PIVOT_KI,            p->pivot_ki_x100,       100.0f);
    apply_param_scaled(HP_PIVOT_KD,            p->pivot_kd,              1.0f);

    /* v6：差速天花板随速放大的参考刻度 */
    apply_param_scaled(HP_GYRO_CORR_REF,       p->gyro_corr_ref,         1.0f);

    for (i = 0; i < SERVO_ACTION_N; i++) {
        (void)Servo_SetActionPose(i + 1, (int)p->action_pose[i][0], (int)p->action_pose[i][1]);
    }

    /* 让应用把刚恢复的东西推给控制环/循迹层（PID 参数表、灰度通讯方式） */
    app_ui_settings_loaded();
}

/* ------------------------------------------------------------------ 写 */

/* ------------------------------------------------- 页擦除（自己写寄存器） */

/* 本板 Flash 组织（2026-10-04 真板实测，块模型）：单 bank、32 个 4KB 擦除块，
 * **块号 = PNB，块地址 = 0x08000000 + PNB×4096，BKER 被硬件忽略**。页 0x0801F800 是
 * 块 31 里的第二页 ⇒ 用 PNB=31 擦块 31（连带页 62，正是没人用的那一页）。
 *
 * **不要改回 HAL_FLASHEx_Erase()**：它按 FLASH_OPTR 的 DBANK 位决定 BKER，而这颗料的
 * DBANK 位读出来是 0（与硬件实际行为不符）⇒ FLASH_PageErase() 会清掉 BKER 再用
 * PNB=63 去擦；PNB 越界（>31）时硬件**静默忽略**（SR 不置错、页内容不变）⇒ 擦除永远无效、
 * 接着在还没擦的地方编程得到 PROGERR ⇒ 设置永远存不进 Flash、每 5s 重试一次并反复弹
 * "正在保存"（用户 m03645 报的 bug 就是这么来的）。
 * 判据：J-Link 手写 FLASH->CR = BKER|PNB=31|PER|STRT 后页 63 立刻变成 0xFF；
 *       而 PNB=63（越界）擦三次都是 SR 干净且页不变；
 *       探针实测 PNB 29/30/31 分别命中块 0x0801D000 / 0x0801E000 / 0x0801F000。 */
static uint8_t nvram_page_erase(void)
{
    uint8_t  dcache = (READ_BIT(FLASH->ACR, FLASH_ACR_DCEN) != 0U) ? 1U : 0U;
    uint32_t t0;

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_DISABLE();      /* 擦写期间关数据 cache（HAL 也这么做） */
    }

    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
    CLEAR_BIT(FLASH->CR, FLASH_CR_PG | FLASH_CR_PER | FLASH_CR_PNB |
                         FLASH_CR_BKER | FLASH_CR_MER1 | FLASH_CR_MER2);
    SET_BIT(FLASH->CR, FLASH_CR_BKER);                          /* bank2 */
    MODIFY_REG(FLASH->CR, FLASH_CR_PNB, (NVRAM_BANK_PAGE << FLASH_CR_PNB_Pos));
    SET_BIT(FLASH->CR, FLASH_CR_PER);
    SET_BIT(FLASH->CR, FLASH_CR_STRT);

    t0 = HAL_GetTick();
    while ((FLASH->SR & FLASH_SR_BSY) != 0U) {
        if ((uint32_t)(HAL_GetTick() - t0) > NVRAM_ERASE_TIMEOUT_MS) {
            break;                              /* 别死等 */
        }
    }
    CLEAR_BIT(FLASH->CR, FLASH_CR_PER | FLASH_CR_PNB | FLASH_CR_BKER);

    if (dcache != 0U) {
        __HAL_FLASH_DATA_CACHE_RESET();         /* 不复位的话下面的回读可能命中旧 cache */
        __HAL_FLASH_DATA_CACHE_ENABLE();
    }

    if ((FLASH->SR & (FLASH_FLAG_PGSERR | FLASH_FLAG_PGAERR | FLASH_FLAG_WRPERR |
                      FLASH_FLAG_PROGERR | FLASH_FLAG_OPERR)) != 0U) {
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        return 0U;
    }
    return 1U;
}

/* ------------------------------------------------------------------ 写 */

static uint8_t write_record(const nvram_payload_t *p)
{
    uint32_t addr;
    uint32_t i;
    uint8_t  ok;

    /* 擦除态是 0xFF：先把整个暂存填成 0xFF，再盖上有意义的部分 */
    memset(&s_stage, 0xFF, sizeof(s_stage));
    s_stage.rec.magic   = NVRAM_MAGIC;
    s_stage.rec.version = NVRAM_VERSION;
    s_stage.rec.length  = (uint16_t)sizeof(nvram_payload_t);
    s_stage.rec.crc     = crc32((const uint8_t *)p, (uint32_t)sizeof(*p));
    memcpy(&s_stage.rec.data, p, sizeof(*p));

    if (HAL_FLASH_Unlock() != HAL_OK) {
        return 0U;
    }

    ok = nvram_page_erase();
    if (ok == 0U) {
        /* 兜底：清一次标志再擦一遍（还没写数据，重来是安全的） */
        __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);
        ok = nvram_page_erase();
    }
    if (ok != 0U) {
        /* 擦除静默失败只剩这一道防线：页首还是 0xFFFFFFFF 才算真擦干净 */
        ok = (*(volatile uint32_t *)NVRAM_PAGE_ADDR == 0xFFFFFFFFUL) ? 1U : 0U;
    }

    if (ok != 0U) {
        for (i = 0U, addr = (uint32_t)NVRAM_PAGE_ADDR;
             i < (uint32_t)(sizeof(s_stage) / 8U); i++, addr += 8U) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, addr,
                                  s_stage.words[i]) != HAL_OK) {
                ok = 0U;
                break;
            }
        }
    }
    (void)HAL_FLASH_Lock();

    /* 写完读回来对一遍：擦写失败（页坏了/写保护）能立刻发现，别以为存上了 */
    if (ok != 0U) {
        ok = (uint8_t)(memcmp((const void *)NVRAM_PAGE_ADDR, &s_stage,
                              sizeof(s_stage)) == 0);
    }
    return ok;
}

/* ------------------------------------------------------------- 对外接口 */

void nvram_init(void)
{
    const nvram_record_t *rec = (const nvram_record_t *)NVRAM_PAGE_ADDR;
    nvram_payload_t cur;

    s_loaded = 0U;

    if (rec->magic == NVRAM_MAGIC &&
        rec->version >= NVRAM_VERSION_MIN &&
        rec->version <= NVRAM_VERSION &&
        (((rec->version == NVRAM_VERSION) &&
          rec->length == (uint16_t)sizeof(nvram_payload_t)) ||
         ((rec->version < NVRAM_VERSION) &&
          rec->length > 0U &&
          rec->length <= (uint16_t)sizeof(nvram_payload_t))) &&
        rec->crc == crc32((const uint8_t *)&rec->data,
                          (uint32_t)rec->length)) {
        /* 老记录（v1..v4）：字段全在当前结构的前缀里，末尾只多加了字段。
         * 做法：先按**当前默认值**铺一份 `up`，再把记录里那 `length` 个字节盖上去 ——
         * 记录里有的字段（含用户标定好的 8 路黑白阈值、调好的前馈）原样保留，
         * 记录里没有的字段就保持当前默认值。以后再加字段这里一行都不用动。
         * 不升 version 就地读，是为了不因为加了参数就让用户重标一次。 */
        nvram_payload_t up;
        snapshot(&up);
        (void)memcpy(&up, &rec->data, (size_t)rec->length);

        /* v4：老记录只有一套标定 —— 把它同时当成第 0/第 1 套（用户切到"亚克力"
         * 也不会切到一份空标定而让 span=0），当前套默认 0（普通）。 */
        if ((uint32_t)rec->length <
            (uint32_t)offsetof(nvram_payload_t, cal_b) + (uint32_t)sizeof(up.cal_b)) {
            up.cal_b = up.cal;
        }
        if ((uint32_t)rec->length < (uint32_t)offsetof(nvram_payload_t, cal_set) + 1U) {
            up.cal_set = 0U;
        }

        apply(&up);
        s_saved  = up;             /* 内容与当前生效值一致，不必立刻重写 Flash */
        s_loaded = 1U;
    } else {
        /* Flash 里没东西（新烧的板子/校验不过）：拿当前默认值当基线 */
        snapshot(&s_saved);
    }

    snapshot(&cur);
    s_live      = cur;
    s_change_ms = HAL_GetTick();
    s_state     = NVRAM_STATE_IDLE;
    s_state_ms  = 0U;
}

/**
 * @brief 现在车是不是"在动"（运动调度接管了电机 / 速度环或角度环使能）
 *
 * 用途只有一个：运动中绝对不擦写 Flash（理由见 nvram_poll 里的注释）。
 * 判据取"任一层使能"就够了 —— 三层串级里下层都是上级使能的，宁可多等一会儿。
 */
static int nvram_motion_active(void)
{
    if (Motion_GetMode() != MOTION_IDLE) {
        return 1;
    }
    if (SpeedControl_IsEnabled() != 0U) {
        return 1;
    }
    if (GyroControl_IsEnabled() != 0U) {
        return 1;
    }
    return 0;
}

void nvram_poll(void)
{
    nvram_payload_t cur;
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - s_check_ms) < NVRAM_CHECK_PERIOD_MS) {
        return;
    }
    s_check_ms = now;

    /* 正在保存 / 刚保存完：这一轮不比对也不动 Flash，等 nvram_service() 推完 */
    if (s_state != NVRAM_STATE_IDLE) {
        return;
    }

    if ((int32_t)(now - s_fail_until_ms) < 0) {
        return;
    }

    snapshot(&cur);
    if (memcmp(&cur, &s_live, sizeof(cur)) != 0) {
        s_live      = cur;         /* 还在改：重新计时 */
        s_change_ms = now;
        return;
    }
    if (memcmp(&s_live, &s_saved, sizeof(s_live)) == 0) {
        return;                    /* 和 Flash 里那份一样，什么都不用做 */
    }
    if ((int32_t)(now - s_change_ms) < (int32_t)NVRAM_SAVE_DELAY_MS) {
        return;                    /* 刚改完，等用户停手 */
    }
    if (OLED_IsBusy()) {
        return;                    /* 屏在搬帧：擦写要停 CPU，别插进去 */
    }
    if (nvram_motion_active()) {
        /* 车在动（循迹/角度环/速度环任一使能，或运动调度不是 IDLE）：绝不擦写。
         * 本板 128KB Flash 只有 2 个 bank，代码 106KB 已经跨到 bank2，而记录页
         * 0x0801F800 也在 bank2 —— 擦/写 bank2 期间 CPU 从 bank2 取指会被 stall，
         * TIM6 的 5ms 运动控制中断会被拖住（擦一页最坏几十毫秒，就是"车顿一下
         * 冲出去"）。存档晚点没关系：s_live 和 s_saved 还是不一样，等车停下来
         * 下一拍照样会存。 */
        return;
    }

    /* 到点了，但**先不写**：进入"正在保存"，让界面把提示画上屏并吃掉
     * 按键；真正的擦写交给 nvram_service()（用户 m03944）。 */
    s_state    = NVRAM_STATE_BUSY;
    s_state_ms = now;
}

uint8_t nvram_state(void)
{
    return s_state;
}

/**
 * @brief "现在就存"（不走 1.5s 防抖）
 *
 * 给上位机的 /api/params/save 与 CMD_SAVE_PARAMS 用（用户 m13466：改完动作
 * 角度/PID 要能一键固化到小车 Flash）。做法只是把"最后改动时刻"往前推、
 * 刷新一次现场快照 —— **真正写不写仍由 nvram_poll() 用 snapshot() 比对决定**：
 * 和 Flash 里那份已经一样就什么都不做（调用方照样收到 OK），
 * 而且"车在动绝不擦写"这条铁律仍然由 nvram_poll() 把关（返回 OK ≠ 已写完）。
 */
void nvram_touch(void)
{
    uint32_t now = HAL_GetTick();

    snapshot(&s_live);
    s_change_ms = now - NVRAM_SAVE_DELAY_MS;
    s_check_ms  = now - NVRAM_CHECK_PERIOD_MS;
}

void nvram_service(void)
{
    uint32_t now = HAL_GetTick();

    if (s_state == NVRAM_STATE_BUSY) {
        /* 等提示真的上屏：一是让用户看得见"正在保存"，二是屏还在搬帧时
         * 绝不能插擦写（会把 I2C 时序打断，花屏/黑屏）。 */
        if ((int32_t)(now - s_state_ms) < (int32_t)NVRAM_NOTICE_MS) {
            return;
        }
        if (OLED_IsBusy() &&
            (int32_t)(now - s_state_ms) <
                (int32_t)(NVRAM_NOTICE_MS + NVRAM_DISPLAY_WAIT_MS)) {
            return;                 /* 屏还在搬帧，等它；等过头就照写（见 nvram.h） */
        }

        if (nvram_motion_active()) {
            /* 真写之前再看一眼：从 nvram_poll() 置 BUSY 到这里隔了 NVRAM_NOTICE_MS
             * （几十毫秒），用户完全可能在这段时间里把车发起来。车一动就撤回这次
             * 保存 —— s_live 和 s_saved 还是不一样，停下车下一拍会自动重来。 */
            s_state = NVRAM_STATE_IDLE;
            return;
        }

        if (write_record(&s_live) != 0U) {
            s_saved           = s_live;
            s_fail_backoff_ms = NVRAM_FAIL_BACKOFF_MS;   /* 通了，退避复位 */
            s_state           = NVRAM_STATE_DONE;        /* 界面接着显示"保存成功" */
            s_state_ms        = now;
        } else {
            /* 写失败（页坏了/RDP 保护）：别骗用户说成功，悄悄回空闲，之后再试。
             * 退避逐次翻倍（5s→10s→20s→40s→60s 封顶）——一直失败时提示不会连着弹
             * （用户 m03645 报的"过一会儿弹一个"就是这个循环，根因见 nvram_page_erase()）。 */
            s_fail_until_ms = now + s_fail_backoff_ms;
            if (s_fail_backoff_ms < NVRAM_FAIL_BACKOFF_MAX_MS) {
                s_fail_backoff_ms *= 2U;
                if (s_fail_backoff_ms > NVRAM_FAIL_BACKOFF_MAX_MS) {
                    s_fail_backoff_ms = NVRAM_FAIL_BACKOFF_MAX_MS;
                }
            }
            s_state = NVRAM_STATE_IDLE;
        }
        return;
    }

    if (s_state == NVRAM_STATE_DONE &&
        (int32_t)(now - s_state_ms) >= (int32_t)NVRAM_DONE_MS) {
        s_state = NVRAM_STATE_IDLE;      /* "保存成功"到点自己退场 */
    }
}

uint8_t nvram_loaded(void)
{
    return s_loaded;
}
