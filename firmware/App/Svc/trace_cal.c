/**
 * @file  trace_cal.c
 * @brief 8 路灰度模块：模拟量采集 + 黑白两点标定（实现）
 *
 * 数据通道自动选（谁通就用谁，见 trace_cal_src）：
 *   ① IIC1：从地址 0x01 / 寄存器 0xA2，8 路 × 2 字节大端 = 16 字节。
 *      官方《关于IIC通信注意事项》：IIC 默认输出数字量，读 0xA2 前必须先用串口
 *      广播 0x4C 00 01 03 把输出模式改成"全部" -> trace_cal_init() 里就这么干。
 *   ② UART4：发 0x57 + 地址，模块回 18 字节 {0x75, 8×BE16, 0x11}
 *      （兼容 22 字节"全部"帧 {0x75, 数字量, 位置, 9×BE16, 0x15}）。
 *      模块只插在 H2/U15 串口座时走这条；它完全不占 I2C，OLED 独享总线。
 *   ③ 两条都不通：回落 PA0–PA7 数字口（bsp_trace_bit，只有 0/1）。
 *
 * 标定：每个点连采 TRACE_CAL_SAMPLE_N(=32) 帧，**去掉最高/最低各 TRACE_CAL_TRIM(=4)
 * 帧再求平均**（抗单帧毛刺 / 抗手抖），然后每路自己算阈值 + 极性：
 *   阈值 = (黑 + 白)/2，极性 = 黑的原值比白大还是小（自动学，不用管模块正相/反相）
 *   无效判据两条：黑白跨度 < TRACE_MIN_SPAN；或采样抖动与跨度同量级（值本身不稳）。
 *
 * 可检验性（用户 m05894 要求"标定流程要能检验"，改在 2026-10-01）：
 *   ① 单点抖动闸门：那一点 8 路里最差抖动 > TRACE_POINT_JIT_MAX 就**整点作废**，
 *      状态留在原档等重按（trace_cal_retry_hint），不把晃动的数静默收进阈值；
 *   ② 逐路**原因码** s_bad[]（SPAN / JITTER / NOSAMPLE），界面按码显示、检验视图给数；
 *   ③ 整体质量 trace_cal_quality()（有效路数）与最差对比度 trace_cal_min_span()，
 *      一路都不好就不置 valid（循迹回落数字口，界面报"标定失败"）。
 */
#include "trace_cal.h"

#include "board.h"
#include "kk_oled.h"
#include "tracing.h"

#include <string.h>

#define TRACE_READ_PERIOD_MS   20U    /* 采集周期：16 字节 @400kHz ≈ 0.45ms */
#define TRACE_FAIL_LIMIT        5U    /* 连续失败这么多帧就换通道 / 判"没有模拟量" */
#define TRACE_OFF_RETRY_MS   2000U    /* 两条都不通：多久后再从头试一遍 */
#define TRACE_MIN_SPAN         32U    /* 黑白原值差小于这个 = 这一路没数据 */
#define TRACE_UART_BYTE_MS      2U    /* 每字节超时：115200 下一字节 87us，2ms 很宽裕 */

#define TRACE_UART_SOF       0x75U    /* 模块回的帧头 */
#define TRACE_UART_READ      0x57U    /* 读命令（+ 1 字节地址） */
#define TRACE_UART_TAIL_ANA  0x11U    /* 18 字节模拟量帧帧尾 */
#define TRACE_UART_TAIL_ALL  0x15U    /* 22 字节"全部"帧帧尾 */

uint16_t trace_cal_raw[TRACE_CAL_CH];
uint8_t  trace_cal_analog_ok;
uint8_t  trace_cal_valid;
uint8_t  trace_cal_src;
uint8_t  trace_cal_mode = TRACE_MODE_AUTO;
uint8_t  trace_adapt_enable = 1U;         /* 单帧自适应（见 trace_cal.h 末） */
trace_cal_frame_dbg_t trace_cal_frame_dbg;

static uint16_t s_blk[TRACE_CAL_CH];      /* 全黑时各路的原值 */
static uint16_t s_wht[TRACE_CAL_CH];      /* 全白时各路的原值 */
static uint16_t s_thr[TRACE_CAL_CH];      /* 0/1 分界（原值刻度） */
static uint16_t s_jit[TRACE_CAL_CH];      /* 采样抖动（去掉极值后剩下那段的幅度） */
static uint8_t  s_pol[TRACE_CAL_CH];      /* 1 = 黑比白原值大 */
static uint8_t  s_bad[TRACE_CAL_CH];      /* 1 = 这一路标定无效 */
static uint16_t s_rmin[TRACE_CAL_CH];     /* 未标定时的自适应量程 */
static uint16_t s_rmax[TRACE_CAL_CH];

/* ---- 两套标定（用户 m07888：普通路面 / 亚克力路段各标一套，跑任务时切换）----
 * 上面那几个数组（s_blk/s_wht/...）永远装"当前这一套"，第二套放在 s_other 里。
 * 这样 frame_compute() 一行都不用改（它只认当前套），切换时把两套对调即可。
 * s_other_ok = 0 表示**还没有过第二套**（没从 Flash 恢复过、用户也没标过）：
 * 这时切过去先把当前这套复制一份，绝不会把当前标定弄丢。 */
static trace_cal_data_t s_other;
static uint8_t s_other_ok;
static uint8_t s_set;                     /* 当前用第几套：0=普通 1=亚克力 */

/* 单帧自适应：本帧每路的归一化黑度（0..255，最黑那一路 = 255）+ 本帧判定结果。
 * 在 trace_cal_poll() 里跟着模拟量一起算，取值口直接读缓存（和 trace_cal_raw[] 同时刻）。 */
static uint8_t s_fl_lvl[TRACE_CAL_CH];
static uint8_t s_fl_state;

/* 最后一帧"好帧"（TRACE_FRAME_OK）的柱高快照 + 当时时刻（用户 m26712 第 3 条：
 * 丢线之后界面要还能保持住之前的状态，见 trace_cal.h 的「丢线时按住上一帧」）。 */
static uint8_t  s_hold_lvl[TRACE_CAL_CH];
static uint32_t s_hold_ms;
static uint8_t  s_hold_valid;

/* 每路"白底基线"：标定时的白值和现在跑的时候可以差几十个计数（换路面、换高度、
 * 白天晚上灯不一样），而且**每路差得不一样**（实测同一帧里 ch1 比它的标定白值高 22、
 * ch3 低 52）。一个全局偏置补不了这种每路不同的漂移，光用标定白值会让没压线的路
 * 带上半格权重（会看到 ch3=126/255、ch6=107/255 这种假黑度）。
 * 所以白底参考 = 标定白值起头 + 每帧极慢地跟着这段路的观测白值走。
 *
 * ⚠ 用户 m07626：「两三个传感器扫到的都是黑的了，但它现在识别的只是一个」。
 * 原因是**半压线**的路也在学：老判据只看"黑度不到本帧最黑的一半"，而黑线裙边上的路
 * 黑度正好是 0.3~0.5 倍（实测停在线上的那一帧：最黑 224/255，两侧裙边 92/255 和
 * 69/255，两条都过了老闸门）→ 基线每帧往下挪一点 → 黑度更小 → 继续学，几十秒就把
 * 基线学到黑线上（实测 ch3 的基线被学到 198，而它的标定白值是 336 = 学掉了 86% 跨度）。
 * 于是"压线的路"全被学成白底，只剩最黑那一路还认得出黑。
 * 现在三道闸门 + 一道硬夹子，缺一不可：
 *   a) 本路黑度 < 本帧最黑的 1/4（lvl*4 < lvl_max）—— 直接排除裙边上的邻路；
 *   b) 相对**标定白值 + ŝ** 的黑度 < 本路跨度的 1/8 —— 这一条不跟着基线一起漂，
 *      所以不会出现"越学越黑"的正反馈；长期停在裙边的路永远进不来；
 *   c) 整帧必须有一个真白点（whiteish > 0，8 路全压线的路口全部 lvl≈满格 → 不学）；
 *   d) 硬夹子：s_base 只允许在**标定白值 ± span/4** 之间微调。黑线离白底整整一个
 *      span，所以"把白底学到黑线上"在原理上不可能，最坏也只偏掉对比度的 1/4。
 * 每帧只动 1/32（残差累加器，不是直接除：小残差也要能一点点学到）。
 * 改标定（向导/Flash 恢复）时 s_base_ok 清零，重新用新白值起头；硬夹子每帧都会
 * 把已经学歪的基线拉回来（所以即使烧录前基线已经学到黑上，这一版一上电就自愈）。 */
static uint16_t s_base[TRACE_CAL_CH];
static int32_t  s_base_acc[TRACE_CAL_CH];
static uint8_t  s_base_ok;
#define TRACE_BASE_LEARN_DEN 32    /* 每帧学 1/32：20ms 一帧 -> 时间常数 ≈0.64s */
#define TRACE_BASE_DRIFT_DEN  4    /* 硬夹子：允许漂移 = span/4 */
#define TRACE_BASE_DRIFT_MIN 24    /* span 很小的那几路至少给 24 个计数 */

/* 整体偏置 ŝ：本帧观测到的中位数先放 s_shift_raw，**限速**后才进 s_shift 给本帧用。
 * 限速 + "跳变超 TRACE_ADAPT_JOLT 就判扰动帧" 是用户 m07343 报"轻轻一晃就都变了、
 * 要等好久才恢复"的解法：单路尖峰被中位数扔掉，整片一起跳也只准一帧挪 16 个计数，
 * 还会先丢线几帧（循迹层沿用上一帧重心），绝不把跳变直接喂进 PID。 */
static int32_t s_shift;        /* 当前生效的 ŝ */
static uint8_t s_shift_ok;     /* 0 = 还没有 ŝ（第一次算出来直接采用，不限速） */

/* ---------------------------------------------------------------- 几何缓存
 * 用户 m07689：「自适应里它实际上也有一些参数，这个参数可以靠我标定的来计算」。
 * frame_compute() 每 20ms 跑一次、8 路各要一次"原值 -> 0..255 黑度"的换算。整数除法在
 * Cortex-M4 上 2~12 个周期一条，8 路×2 处 = 16 条，比这个函数其它部分加起来还贵。所以
 * 标定一变（向导收尾 / Flash 恢复）就把每路的几何量算一次，之后每帧只剩乘 + 移位：
 *   s_sp[i] = |跨度|（原值计数）
 *   s_rq[i] = (255 << TRACE_LVL_SHIFT) / s_sp[i]  ->  黑度 = (b * s_rq[i]) >> TRACE_LVL_SHIFT
 *   s_dr[i] = 基线允许漂移的半径 = max(跨度/4, max(24, 3×这一路标定抖动))
 *   s_an[i] = 学基线时"相对标定白值"的容差 = max(跨度/8, max(16, 3×这一路抖动))
 * 后面两个里的抖动项就是"参数靠标定算"：s_jit[] 是标定时测出来的这套传感器自己的噪声底，
 * 用它当地板比写死 24/16 更贴这台车（手特别稳时也不会紧到没法用）。三条全局阈值同理，
 * 取有效路的中位数（抗单路离群），标定时算一次：
 *   s_jolt   = 夹(max(4×抖动中位数, 跨度中位数/4), 24, 255)  一帧里 ŝ 观测值跳这么多 = 晃/离地
 *   s_step   = 夹(跨度中位数/8, 6, 40)                         ŝ 每帧最多挪多少（追踪速度上限）
 *   s_minlvl = max(60, 6×抖动折算的中位黑度)                  本帧最黑低于它 = 这帧没信息
 * 没标定（跨度=0）时全部保持"保守默认值"（老常量），行为与不标定时一致。 */
#define TRACE_LVL_SHIFT 12         /* 定点倒数的移位：12 位足够，b 夹 ±2047 不会溢出 */
static int32_t s_sp[TRACE_CAL_CH];
static int32_t s_rq[TRACE_CAL_CH];
static int32_t s_dr[TRACE_CAL_CH];
static int32_t s_an[TRACE_CAL_CH];
static int32_t s_jolt   = (int32_t)TRACE_ADAPT_JOLT;
static int32_t s_step   = (int32_t)TRACE_ADAPT_SHIFT_STEP;
static int32_t s_minlvl = (int32_t)TRACE_ADAPT_MIN_LEVEL;
trace_geom_t trace_geom;           /* 只读调试口，见 trace_cal.h */
uint32_t trace_frame_cycles;       /* 上一帧 frame_compute() 的 DWT 周期数 */
uint32_t trace_frame_cycles_max;   /* 上电以来最坏的一次（实时性证据，只增不减） */

static void frame_compute(void);   /* 定义在下面「取值」段，poll 里先调 */
static void frame_compute_body(void);
static void s_recalc_geom(void);   /* 标定变了就重算上面那块缓存 */
static int32_t s_median(int32_t *v, uint8_t n);

static uint16_t s_buf[TRACE_CAL_CH][TRACE_CAL_SAMPLE_N];  /* 标定采样缓冲：8×32×2 = 512B */
static uint8_t  s_n;                      /* 本次标定点实际采到的帧数 */
static uint8_t  s_fail;
static uint8_t  s_tried;                  /* 1 = 已经为本次探测换过通道了（只换一次） */
static uint8_t  s_first;                  /* 首选通道（自动模式：串口优先） */
static uint32_t s_last_ms;
static uint32_t s_off_until_ms;
static uint32_t s_done_ms;
static uint32_t s_retry_ms;               /* 上一次"整点被抖动判掉"的时刻（0 = 没有） */
static trace_cal_state_t s_state;

/* ---------------------------------------------------------------- 工具 */

void trace_cal_request_analog_mode(void)
{
    /* 0x4C + 广播地址 + 输出模式寄存器 + "全部"。
     * 出处：官方例程 usart.c 的 Set_Data1(add,Par1,Par2)；
     * 例程注释里 `Set_Data1(0x01,0x01,0x03)` 就是把输出模式设成"全部"。 */
    static const uint8_t cmd[4] = { TRACE_CMD_SOF, 0x00U, TRACE_REG_OUTPUT, TRACE_OUT_ALL };
    board_uart4_write(cmd, 4U);
}

static uint8_t other_source(uint8_t s)
{
    return (uint8_t)((s == TRACE_SRC_I2C) ? TRACE_SRC_UART : TRACE_SRC_I2C);
}

/* 用户设的"通讯方式"对应哪条通道（AUTO 时是首选那条） */
static uint8_t desired_source(void)
{
    if (trace_cal_mode == TRACE_MODE_I2C)  return TRACE_SRC_I2C;
    if (trace_cal_mode == TRACE_MODE_UART) return TRACE_SRC_UART;
    return s_first;
}

void trace_cal_set_mode(uint8_t mode)
{
    if (mode >= TRACE_MODE_COUNT) mode = TRACE_MODE_AUTO;
    if (mode == trace_cal_mode) return;

    trace_cal_mode  = mode;
    s_fail          = 0U;
    s_tried         = 0U;
    s_off_until_ms  = 0U;
    s_last_ms       = 0U;      /* 下一轮 poll 立刻按新方式试 */
    trace_cal_src   = (mode == TRACE_MODE_DIGITAL) ? TRACE_SRC_NONE
                                                   : desired_source();
    trace_cal_analog_ok = 0U;  /* 换方式后等第一帧成功再报"模拟量活着" */
}

/* 中间那段（去掉两端极值）求平均；jit 回传这段的幅度，用来判断采样稳不稳 */
static uint16_t robust_mean(uint16_t *v, uint8_t n, uint16_t *jit)
{
    uint8_t i, j, lo, hi;
    uint32_t sum = 0U;

    /* 插入排序：n = 32、8 路一共 256 次比较，且只在按键标定那 0.6s 里跑，无所谓 */
    for (i = 1U; i < n; i++) {
        uint16_t k = v[i];
        j = i;
        while (j > 0U && v[j - 1U] > k) {
            v[j] = v[j - 1U];
            j--;
        }
        v[j] = k;
    }

    lo = TRACE_CAL_TRIM;
    hi = (uint8_t)(n - TRACE_CAL_TRIM);
    if (hi <= lo) { lo = 0U; hi = n; }        /* n 太小就全用上，别算出 0 个数 */
    for (i = lo; i < hi; i++) sum += v[i];
    if (jit) *jit = (uint16_t)(v[hi - 1U] - v[lo]);

    return (uint16_t)(sum / (uint32_t)(hi - lo));
}

void trace_cal_init(void)
{
    uint8_t i;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        s_rmin[i] = 0xFFFFU;
        s_rmax[i] = 0U;
        s_thr[i]  = 0U;
        s_jit[i]  = 0U;
        s_bad[i]  = 0U;
    }
    s_state = TRACE_CAL_IDLE;
    trace_cal_analog_ok = 0U;
    trace_cal_valid     = 0U;
    s_off_until_ms      = 0U;
    s_fail              = 0U;
    s_tried             = 0U;
    s_last_ms           = 0U;
    /* 自动模式：优先串口，其次 I2C，两条都不通才回落数字口（用户 m03944）。 */
    s_first = TRACE_SRC_UART;
    /* trace_cal_mode 是用户设的（可能刚被 nvram 从 Flash 里恢复过），
     * 所以这里不能把它清成 AUTO，只按它挑一条通道。 */
    trace_cal_src = (trace_cal_mode == TRACE_MODE_DIGITAL) ? TRACE_SRC_NONE
                                                           : desired_source();

    trace_cal_request_analog_mode();

    /* 几何缓存先按当前标定算一遍（这里通常还是空的，valid=0 时用不上）；
     * 真正那份标定由 nvram 开机恢复走 trace_cal_set_data() 再重算一次。 */
    s_recalc_geom();
}

/* 原始值 -> 0..255 柱高（255 = 压黑线）。
 * 已标定：两点线性映射，黑 -> 255、白 -> 0，中间线性；
 *         极性自动处理（反相模块 s_blk < s_wht，(raw-b)/(a-b) 两个负号抵消）。
 * 未标定：按观察到的极小/极大自适应铺满，方向假设"原值越大越黑"
 *         （方向对不对由用户标定一次即可定死，未标定只求能看见变化）。 */
static uint8_t level_of(uint8_t i, uint16_t raw)
{
    int32_t v;

    if (trace_cal_valid && !s_bad[i]) {
        int32_t a = (int32_t)s_blk[i];
        int32_t b = (int32_t)s_wht[i];
        if (a == b) {
            v = 128;
        } else {
            v = ((int32_t)raw - b) * 255 / (a - b);
        }
    } else {
        int32_t lo = (int32_t)s_rmin[i];
        int32_t hi = (int32_t)s_rmax[i];
        if (s_rmin[i] == 0xFFFFU || hi <= lo) return 0U;
        v = ((int32_t)raw - lo) * 255 / (hi - lo);
    }
    if (v < 0)   v = 0;
    if (v > 255) v = 255;
    return (uint8_t)v;
}

/* ---------------------------------------------------------------- 采集 */

/* 备用通道：0x57 + 地址 -> 模块回一帧，只取里面那 16 字节模拟量（大端原样）。
 *   18 字节 {0x75, 8×BE16, 0x11}          -> 模拟量在 [1..16]
 *   22 字节 {0x75, 数字量, 位置, 9×BE16, 0x15} -> 模拟量在 [5..20]
 * 凑够一帧就返回，不等后面没用的字节（省总线时间）。 */
static uint8_t uart_read_analog(uint8_t *b16)
{
    static const uint8_t req[2] = { TRACE_UART_READ, TRACE_SENSOR_ADDR };
    uint8_t buf[22];
    uint8_t n = 0U;
    uint8_t b;

    /* 先冲刷掉可能残留的字节：读取帧是"你问我才答"，正常不会有 */
    while (board_uart4_read_byte(&b, 0U)) { /* 丢掉 */ }

    board_uart4_write(req, sizeof(req));

    while (n < (uint8_t)sizeof(buf)) {
        if (!board_uart4_read_byte(&b, TRACE_UART_BYTE_MS)) break;  /* 字节没来 = 帧没齐 */
        if (n == 0U && b != TRACE_UART_SOF) continue;               /* 帧头前全丢 */
        buf[n++] = b;
        if (n == 18U && buf[17] == TRACE_UART_TAIL_ANA) {
            memcpy(b16, &buf[1], TRACE_CAL_CH * 2U);
            return 1U;
        }
        if (n == 22U && buf[21] == TRACE_UART_TAIL_ALL) {
            memcpy(b16, &buf[5], TRACE_CAL_CH * 2U);
            return 1U;
        }
    }
    return 0U;
}

static uint8_t source_read(uint8_t *b16)
{
    if (trace_cal_src == TRACE_SRC_I2C) {
        HAL_StatusTypeDef st = i2c_read_reg(TRACE_SENSOR_ADDR, TRACE_REG_ANALOG,
                                            b16, TRACE_CAL_CH * 2U);
        board_i2c_report((uint8_t)((st == HAL_OK) ? 1U : 0U));   /* 喂总线看门狗 */
        return (uint8_t)((st == HAL_OK) ? 1U : 0U);
    }
    if (trace_cal_src == TRACE_SRC_UART) {
        return uart_read_analog(b16);
    }
    return 0U;
}

/* 一口气连采 TRACE_CAL_SAMPLE_N 帧（IIC 一帧 ≈0.6ms → 32 帧 ≈20ms）。
 * 返回 1 = 采够了。IIC 与 OLED 共用总线：每帧前等屏空闲，别插进它的搬运中间。 */
static uint8_t sample_point(void)
{
    uint8_t b16[TRACE_CAL_CH * 2U];
    uint8_t k, i;

    for (k = 0U; k < (uint8_t)TRACE_CAL_SAMPLE_N; k++) {
        if (trace_cal_src == TRACE_SRC_I2C) {
            uint16_t guard = 0U;
            while (OLED_IsBusy()) {
                if (++guard > 200U) break;   /* 200ms 还不空闲：屏那边有问题，别死等 */
                HAL_Delay(1);
            }
        }
        if (!source_read(b16)) break;
        for (i = 0; i < TRACE_CAL_CH; i++) {
            uint16_t v = (uint16_t)(((uint16_t)b16[i * 2U] << 8) |
                                    (uint16_t)b16[i * 2U + 1U]);
            s_buf[i][k] = v;
            if (v < s_rmin[i]) s_rmin[i] = v;
            if (v > s_rmax[i]) s_rmax[i] = v;
            trace_cal_raw[i] = v;
        }
    }

    if (k < 8U) return 0U;    /* 连 8 帧都没采到：这次算失败，别拿半份数据去算平均 */
    s_n = k;
    return 1U;
}

/* 采样完算平均并推进状态。进入时 s_state 还是 WAIT_BLACK / WAIT_WHITE。 */
static void sample_finish(void)
{
    uint8_t i;
    uint8_t ok = 0U;
    uint16_t avg[TRACE_CAL_CH];
    uint16_t jit[TRACE_CAL_CH];
    uint16_t worst = 0U;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        avg[i] = robust_mean(s_buf[i], s_n, &jit[i]);
        if (jit[i] > worst) worst = jit[i];
    }

    /* 这一点整体都在晃（手没拿稳 / 车还在动）：**整点作废**，状态留在同一档等重按。
     * 不能"按下就收下"——晃动的数收进阈值里，后面循迹就会莫名其妙地漂，
     * 而用户完全看不出来（用户 m05894：标定流程要能检验；第一步就是别静默吃坏数据）。 */
    if (worst > (uint16_t)TRACE_POINT_JIT_MAX) {
        s_retry_ms = HAL_GetTick();
        s_n = 0U;
        return;
    }

    for (i = 0; i < TRACE_CAL_CH; i++) {
        if (jit[i] > s_jit[i]) s_jit[i] = jit[i];    /* 两点里记较差的那个 */
        if (s_state == TRACE_CAL_WAIT_BLACK) {
            s_blk[i] = avg[i];
        } else {
            s_wht[i] = avg[i];
        }
    }
    s_n = 0U;

    if (s_state == TRACE_CAL_WAIT_BLACK) {
        s_state = TRACE_CAL_WAIT_WHITE;
        return;
    }

    /* 两个点都有了：每路自己算阈值 + 极性 + 有效性（不有效的那路给出**原因码**，
     * 界面逐路画 '!'，检验视图里还能看到跨度/抖动的具体数） */
    for (i = 0; i < TRACE_CAL_CH; i++) {
        uint32_t a = s_blk[i], b = s_wht[i];
        uint32_t d = (a > b) ? (a - b) : (b - a);
        s_pol[i] = (a > b) ? 1U : 0U;
        s_thr[i] = (uint16_t)((a + b) / 2U);
        if (d < TRACE_MIN_SPAN) {
            s_bad[i] = TRACE_BAD_SPAN;      /* 黑白几乎一样：这一点没跨过黑线 */
        } else if ((uint32_t)s_jit[i] * 3U >= d) {
            s_bad[i] = TRACE_BAD_JITTER;    /* 抖动与跨度同量级：阈值不可信 */
        } else {
            s_bad[i] = 0U;
            ok++;
        }
    }
    /* 一路都不好 = 这份标定彻底没用：不置 valid（循迹自动回落数字口），界面报失败。
     * 只要还有一路能用就照常置 valid——少几路的场合用户自己看 8 路的 '!' 和质量分数。 */
    trace_cal_valid = (uint8_t)((ok > 0U) ? 1U : 0U);
    s_done_ms = HAL_GetTick();
    s_state   = (trace_cal_valid != 0U) ? TRACE_CAL_DONE : TRACE_CAL_FAILED;

    /* 刚标完：白底基线/整体偏置全部重新起头（用新的白值），否则旧基线要靠
     * "每帧 1/32" 慢慢爬过来 —— 那要几十秒，用户会觉得"标了不管用"（m07343）。
     * 几何缓存/运行时阈值也一起按新标定重算（用户 m07689：参数靠标定算）。 */
    s_base_ok  = 0U;
    s_shift_ok = 0U;
    s_recalc_geom();
}

void trace_cal_poll(void)
{
    uint8_t b16[TRACE_CAL_CH * 2U];
    uint8_t i;
    uint32_t now = HAL_GetTick();

    /* 标定完成提示自己退场，用户不用再按键。给 3s：提示里带"8/8"或"7/8!"的质量
     * 摘要，1.5s 只够瞟一眼；失败态（TRACE_CAL_FAILED）则一直留着，用户不重来就一直
     * 看着原因码对应的那行字。 */
    if (s_state == TRACE_CAL_DONE && (now - s_done_ms) > 3000U) {
        s_state = TRACE_CAL_IDLE;
    }

    /* 用户把「灰度接口」设成"数字"：压根不碰模拟量通路，直接用 PA0–PA7 */
    if (trace_cal_mode == TRACE_MODE_DIGITAL) {
        trace_cal_src       = TRACE_SRC_NONE;
        trace_cal_analog_ok = 0U;
        return;
    }

    if ((now - s_last_ms) < TRACE_READ_PERIOD_MS) return;
    s_last_ms = now;

    /* I2C 总线看门狗：OLED 空闲时才有机会真正执行恢复（恢复要手动打时钟，
     * 会把正在 DMA 搬运的屏打断）。这里只是"报个空闲"，不满足条件时它自己什么都不做。 */
    (void)board_i2c_watchdog((uint8_t)(OLED_IsBusy() ? 0U : 1U));

    /* 单帧自适应：跟着采集周期算一遍（读值口直接读这份缓存）。
     * 必须放在这个 20ms 闸门**之后**：poll 是主循环每圈都调的（~1ms 一圈），
     * 放前面会让基线跟踪"每帧 1/32"变成 1ms 学一次，几十毫秒就把黑线学成白底。
     * 放在这里，下面每条提前 return（退避/等 OLED/读失败）都不再算 —— 那几种情况
     * 本来也没有新模拟量，用上一帧的缓存和 trace_cal_raw[] 同样是旧的，节奏一致。 */
    frame_compute();

    /* 当前这条判死了（AUTO 两条都试过 / 固定通道失败）：退避 2s 再按用户
     * 设的方式重试，别一直拿超时堵着总线。 */
    if (trace_cal_src == TRACE_SRC_NONE) {
        if ((int32_t)(now - s_off_until_ms) < 0) return;
        trace_cal_src = desired_source();
        s_fail  = 0U;
        s_tried = 0U;
    }

    /* I2C1 和 OLED 共用：OLED 正在（中断/DMA）搬页数据时绝对不能插一次阻塞读，
     * 会把它的时序打断（现象是花屏/黑屏）。串口那条线不占 I2C，不用等。 */
    if (trace_cal_src == TRACE_SRC_I2C && OLED_IsBusy()) return;

    if (!source_read(b16)) {
        if (s_fail < 255U) s_fail++;
        if (s_fail >= TRACE_FAIL_LIMIT) {
            s_fail = 0U;
            if (trace_cal_mode == TRACE_MODE_AUTO && !s_tried) {
                /* AUTO：当前通道判死，换另一条试（IIC <-> 串口）。**只换一次**，
                 * 否则两条都不通时会 IIC/UART 来回弹，UART 那条每次要干等
                 * 几十毫秒，把主循环拖慢。 */
                s_tried       = 1U;
                trace_cal_src = other_source(trace_cal_src);
            } else {
                /* 固定通道失败，或 AUTO 两条都试过了：判"没有模拟量"，
                 * 上位层回落 PA0–PA7 数字口；2s 后再按设置重试。
                 * 固定通道时把 src 留着（界面显示"用户选的是哪条"）。 */
                if (trace_cal_mode == TRACE_MODE_AUTO) {
                    trace_cal_src = TRACE_SRC_NONE;
                }
                trace_cal_analog_ok = 0U;
                s_off_until_ms = now + TRACE_OFF_RETRY_MS;
            }
        }
        return;
    }

    s_fail = 0U;
    s_tried = 0U;
    s_off_until_ms = 0U;
    trace_cal_analog_ok = 1U;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        uint16_t v = (uint16_t)(((uint16_t)b16[i * 2U] << 8) | (uint16_t)b16[i * 2U + 1U]);
        trace_cal_raw[i] = v;
        if (v < s_rmin[i]) s_rmin[i] = v;
        if (v > s_rmax[i]) s_rmax[i] = v;
    }
}

/* ------------------------------------------------- 取值 */

/* 单帧自适应（算法说明见 trace_cal.h 末「单帧自适应」）。
 * 每 20ms 跟着一帧模拟量算一次，结果放 s_fl_lvl[]/s_fl_state 给取值口读。 */
/* ------------------------------------------------- 标定 -> 几何缓存 / 计时壳 */

/* 就地插入排序 + 取上中位数（n ≤ 8，比调 qsort 便宜得多；调用者给的是可改的副本） */
static int32_t s_median(int32_t *v, uint8_t n)
{
    uint8_t a, c;

    for (a = 1U; a < n; a++) {
        int32_t key = v[a];
        c = a;
        while (c > 0U && v[c - 1U] > key) { v[c] = v[c - 1U]; c--; }
        v[c] = key;
    }
    return v[n / 2U];
}

/* 标定一变就重算几何缓存 + 三条阈值（用户 m07689："参数靠我标定的来计算"）。
 * 除法/排序只在这里做一次，之后每帧只剩乘 + 移位。 */
static void s_recalc_geom(void)
{
    int32_t sp[TRACE_CAL_CH], jt[TRACE_CAL_CH], sh[TRACE_CAL_CH];
    uint8_t i, n = 0U;

    trace_geom.rq_shift = TRACE_LVL_SHIFT;
    trace_geom.sp_med   = 0;
    trace_geom.jit_med  = 0;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t sp_i = (int32_t)s_blk[i] - (int32_t)s_wht[i];
        int32_t jt_i = (int32_t)s_jit[i];
        int32_t floor_dr, floor_an;

        if (sp_i < 0) sp_i = -sp_i;
        s_sp[i] = sp_i;

        /* 地板用"噪声底"：3 倍标定抖动，但至少给 24（漂移半径）/16（学习容差）个计数 */
        floor_dr = jt_i * 3;  if (floor_dr < 24) floor_dr = 24;
        floor_an = jt_i * 3;  if (floor_an < 16) floor_an = 16;

        if (sp_i > 0) {
            int32_t dv = (sp_i < 8) ? 8 : sp_i;   /* 跨度特别小时防溢出：b 夹在 ±4095 */
            s_rq[i] = ((int32_t)255 << TRACE_LVL_SHIFT) / dv;
        } else {
            s_rq[i] = 0;                          /* 0 = 这一路没法换算 */
        }
        s_dr[i] = (sp_i / (int32_t)TRACE_BASE_DRIFT_DEN > floor_dr)
                  ? (sp_i / (int32_t)TRACE_BASE_DRIFT_DEN) : floor_dr;
        s_an[i] = (sp_i / 8 > floor_an) ? (sp_i / 8) : floor_an;

        if (trace_cal_valid && !s_bad[i] && sp_i > 0) {
            int32_t lvl = (int32_t)s_jit[i] * 255 / sp_i;   /* 抖动折算成黑度 */
            sp[n] = sp_i;
            jt[n] = jt_i;
            sh[n] = (lvl > 255) ? 255 : lvl;
            n++;
        }
    }

    /* 没标定 / 一路都不好：保持保守默认值（行为跟不标定时一模一样） */
    if (n == 0U) {
        s_jolt   = (int32_t)TRACE_ADAPT_JOLT;
        s_step   = (int32_t)TRACE_ADAPT_SHIFT_STEP;
        s_minlvl = (int32_t)TRACE_ADAPT_MIN_LEVEL;
        trace_geom.jolt   = s_jolt;
        trace_geom.step   = s_step;
        trace_geom.minlvl = s_minlvl;
        return;
    }

    trace_geom.sp_med  = s_median(sp, n);
    trace_geom.jit_med = s_median(jt, n);

    /* ŝ 的扰动判定：一帧里观测中位数离当前 ŝ 跳这么多 = 晃/离地。
     * 取"4 倍抖动"与"1/4 个对比度"里大的那个：前者是噪声底，后者是"路面真变了"的门槛。 */
    s_jolt = 4 * trace_geom.jit_med;
    if (s_jolt < trace_geom.sp_med / 4) s_jolt = trace_geom.sp_med / 4;
    if (s_jolt < 24)  s_jolt = 24;
    if (s_jolt > 255) s_jolt = 255;

    /* ŝ 每帧限速 = 跨度中位数/8（本车约 18 个计数/帧 = 900 计数/s） */
    s_step = trace_geom.sp_med / 8;
    if (s_step < 6)  s_step = 6;
    if (s_step > 40) s_step = 40;

    /* "这帧有信息"的最低对比度：60（无量纲，约 1/4 格）与 6 倍噪声里大的那个 */
    s_minlvl = 6 * s_median(sh, n);
    if (s_minlvl < (int32_t)TRACE_ADAPT_MIN_LEVEL) s_minlvl = (int32_t)TRACE_ADAPT_MIN_LEVEL;

    trace_geom.jolt   = s_jolt;
    trace_geom.step   = s_step;
    trace_geom.minlvl = s_minlvl;
}

/* 计时壳：frame_compute() 的真实开销用 DWT 周期数记账（用户 m07689 要的实时性证据）。
 * DWT 在 dwt_init()（main_app.c）里已使能，CYCCNT 是自由跑的 32 位周期计数；差一次减法
 * 就够（170MHz 下回绕要 25s，本函数是 100µs 级，不用管回绕）。 */
static void frame_compute(void)
{
    uint32_t t0 = DWT->CYCCNT;

    frame_compute_body();

    trace_frame_cycles = DWT->CYCCNT - t0;
    if (trace_frame_cycles > trace_frame_cycles_max) trace_frame_cycles_max = trace_frame_cycles;
}

static void frame_compute_body(void)
{
    uint8_t i;
    int32_t span, b, lvl, lvl_max = 0, noise = 0, shift = 0, shift_raw = 0, d;
    int32_t ped = 0;
    int32_t rs[TRACE_CAL_CH];
    uint8_t whiteish = 0;
    uint8_t n = 0;
    uint8_t jolt = 0U;

    s_fl_state = TRACE_FRAME_FALLBACK;
    for (i = 0; i < TRACE_CAL_CH; i++) s_fl_lvl[i] = 0U;
    trace_cal_frame_dbg.shift     = 0;
    trace_cal_frame_dbg.shift_raw = 0;
    trace_cal_frame_dbg.lvl_max   = 0U;
    trace_cal_frame_dbg.noise     = 0U;
    trace_cal_frame_dbg.whiteish  = 0U;
    trace_cal_frame_dbg.jolt      = 0U;
    trace_cal_frame_dbg.state     = TRACE_FRAME_FALLBACK;

    /* 没标定 / 没有模拟量 / 用户关掉自适应：一律回落老的中点阈值 */
    if (!trace_adapt_enable) return;
    if (!trace_cal_analog_ok) return;
    if (!trace_cal_valid)     return;

    /* 白底基线第一次用：从标定白值起头（之后每帧极慢地跟着观测白值走，见 s_base 注释） */
    if (!s_base_ok) {
        for (i = 0; i < TRACE_CAL_CH; i++) {
            s_base[i]     = s_wht[i];
            s_base_acc[i] = 0;
        }
        s_base_ok  = 1U;
        s_shift_ok = 0U;    /* 基线换了：老 ŝ 对比新基线没意义，下一帧直接采用新中位数 */
    }

    /* ① 本帧整体偏置 ŝ：在"自己看着像白底"（黑度不到半跨）的路里取残差 (原值-基线)
     * 的**中位数**。取中位数而不是最大值：某一路抖出尖峰时（轻轻一晃/传感器离地）
     * 最大值会被它拉走、8 路黑度一起跳，中位数把这种离群点直接扔掉。 */
    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t raw, ref0;

        if (s_bad[i]) continue;
        span = s_sp[i];                     /* 标定时算好的 |跨度|（几何缓存，不再每次调） */
        if (span == 0) continue;

        raw  = (int32_t)trace_cal_raw[i];
        ref0 = (int32_t)s_base[i];          /* 本路"白底"当前值（标定起头 + 慢跟踪） */
        b = s_pol[i] ? (raw - ref0) : (ref0 - raw);
        if (b * 2 < span) {                       /* 这一路自己认为没压线 */
            rs[n++] = raw - ref0;
        }
    }
    whiteish = n;

    /* 8 路全压线（n=0，路口/大黑块）时没有"白"可参考：ŝ 保持不变，也不判扰动。 */
    if (n > 0U) {
        shift_raw = s_median(rs, n);              /* 上中位数：偏向"更亮"的那一侧 */
        if (shift_raw >  2047) shift_raw =  2047;
        if (shift_raw < -2047) shift_raw = -2047;

        if (!s_shift_ok) {
            s_shift    = shift_raw;               /* 第一次算出来：直接采用 */
            s_shift_ok = 1U;
        } else {
            d = shift_raw - s_shift;
            if (d > s_jolt || d < -s_jolt) {
                jolt = 1U;                        /* 一帧跳这么多 = 车在晃/离地，判扰动 */
            }
            if (d > s_step) {
                s_shift += s_step;
            } else if (d < -s_step) {
                s_shift -= s_step;
            } else {
                s_shift = shift_raw;
            }
        }
    }
    shift = s_shift;                              /* 本帧真正用的 ŝ（限速后） */

    /* ①.5 8 路全压线（whiteish==0：大黑块 / 路口 / 用户把车整个摆在全黑上）时 ŝ 没有白参考，
     * 这时候 s_shift 是"车刚压上黑线那几帧"（还有一两路是白的）冻结下来的值 —— 拿它去减
     * (基线-原值)，等于按**原值计数**整条扣掉一截，而黑度是按**各路自己的跨度**归一化的：
     * 跨度越窄的边路被扣得越狠。2026-10-02 用户 m08812 报的"全黑时两头的 1 和 8 明显比中间低、
     * 第 8 路有时掉成 0"就是这个：实测 ŝ=-58、第 8 路跨度只有 85 ⇒ 58×255/85 ≈ 174 个黑度
     * 白扣（同一帧 8 路的实际黑度其实都在 90~98%）。
     * 全黑帧里"谁更黑"本来就是相对比较，用中性 0；白底回来（whiteish>0）立刻按新中位数收敛。 */
    if (whiteish == 0U) {
        shift = 0;
    }

    /* ② 每路黑度：相对"基线 + ŝ"、按**自己**的黑白跨度归一化到 0..255。
     * 换算用标定时算好的定点倒数 s_rq[]：只剩乘 + 移位（去掉每帧 8+8 条整数除法，
     * 用户 m07689 要的实时性）。b 最大 ±4095、s_rq 最大 255<<12/8 → 32 位内不会溢出。 */
    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t ref;

        if (s_bad[i]) continue;
        if (s_sp[i] == 0 || s_rq[i] == 0) continue;

        ref = (int32_t)s_base[i] + shift;
        b = s_pol[i] ? ((int32_t)trace_cal_raw[i] - ref)
                     : (ref - (int32_t)trace_cal_raw[i]);
        lvl = (b * s_rq[i]) >> TRACE_LVL_SHIFT;
        if (lvl < 0)   lvl = 0;
        if (lvl > 255) lvl = 255;
        s_fl_lvl[i] = (uint8_t)lvl;
        if (lvl > lvl_max) lvl_max = lvl;

        /* 这一路的采样抖动折算成黑度：信噪比 = lvl_max / noise（noise 取最差的那路） */
        lvl = ((int32_t)s_jit[i] * s_rq[i]) >> TRACE_LVL_SHIFT;
        if (lvl > noise) noise = lvl;
    }

    /* ③ 本帧没信息（对比度塌到接近噪声）或本帧是扰动帧（ŝ 一帧跳太多）：判 WEAK 当丢线，
     * 绝不瞎判压线。扰动帧只丢几帧：ŝ 每帧能挪 s_step 个计数，追上以后自动回 OK。 */
    if (jolt || lvl_max < s_minlvl || lvl_max < noise * 2) {
        for (i = 0; i < TRACE_CAL_CH; i++) s_fl_lvl[i] = 0U;
        s_fl_state = TRACE_FRAME_WEAK;
    } else {
        /* ③.4 硬夹子：不管本帧学不学，先把每条基线拉回"标定白值 ± s_dr"（=max(跨度/4, 3×抖动)）。
         * 既防学歪，也让上一版/上一块板已经学到黑线上的基线在一帧内自愈（见 s_base 注释）。 */
        for (i = 0; i < TRACE_CAL_CH; i++) {
            int32_t lo, hi;

            if (s_bad[i]) continue;
            if (s_sp[i] == 0) continue;

            lo = (int32_t)s_wht[i] - s_dr[i];  if (lo < 0) lo = 0;
            hi = (int32_t)s_wht[i] + s_dr[i];  if (hi > 4095) hi = 4095;
            if ((int32_t)s_base[i] < lo)      s_base[i] = (uint16_t)lo;
            else if ((int32_t)s_base[i] > hi) s_base[i] = (uint16_t)hi;
        }

        /* ③.5 学白底基线（闸门/夹子的来由见 s_base 上面的注释，用户 m07626）：
         * 只有"本帧相对地白"**且**"相对标定白值+ŝ 也确实白"的路才学 —— 半压线、黑线裙边上
         * 的路一个计数都学不到，所以不会再出现"两三个传感器压在黑上只认得出一个"。 */
        if (whiteish > 0U) {
            for (i = 0; i < TRACE_CAL_CH; i++) {
                int32_t raw, lo, hi, anchor;
                uint16_t wht;

                if (s_bad[i]) continue;
                if (s_sp[i] == 0) continue;

                raw = (int32_t)trace_cal_raw[i];
                wht = s_wht[i];

                if ((int32_t)s_fl_lvl[i] * 4 >= lvl_max) continue;  /* a) 本帧最黑的 1/4 以内 */

                /* b) 相对"标定白值 + ŝ"的黑度：用它当锚，不跟着基线漂，堵掉"越学越黑"。
                 *    容差 s_an = max(跨度/8, max(16, 3×抖动))：抖得厉害的路放宽一点。 */
                anchor = s_pol[i] ? (raw - ((int32_t)wht + shift))
                                  : (((int32_t)wht + shift) - raw);
                if (anchor >= s_an[i] || anchor <= -s_an[i]) continue;

                lo = (int32_t)wht - s_dr[i];  if (lo < 0) lo = 0;
                hi = (int32_t)wht + s_dr[i];  if (hi > 4095) hi = 4095;

                s_base_acc[i] += raw - (int32_t)s_base[i];
                if (s_base_acc[i] >= (int32_t)TRACE_BASE_LEARN_DEN) {
                    s_base_acc[i] -= (int32_t)TRACE_BASE_LEARN_DEN;
                    if ((int32_t)s_base[i] < hi) s_base[i]++;
                } else if (s_base_acc[i] <= -(int32_t)TRACE_BASE_LEARN_DEN) {
                    s_base_acc[i] += (int32_t)TRACE_BASE_LEARN_DEN;
                    if ((int32_t)s_base[i] > lo) s_base[i]--;
                }
            }
        }

        /* ④ 去零偏 + 归一化成本帧的相对黑度（用户 m07888 第 2 条）：
         * 先减掉**零点**（本帧采样抖动折算出来的黑度 noise —— 低于它的黑度跟 0 没法区分，
         * 它们加进重心就是白路上的假权重），再归一化：最黑那一路 = 255 -> 0/1 分界永远是
         * 半高(128)，对比度整体变小也照样成立（这就是"亚克力也能判"的来源）。
         * 一条除法算好整帧的倒数，8 路只做一次减法 + 乘 + 移位。 */
        {
            int32_t  off = noise;                       /* 零点：本帧的噪声地板 */
            uint32_t nrq = ((uint32_t)255U << TRACE_LVL_SHIFT) / (uint32_t)lvl_max;

            if (off > lvl_max) off = lvl_max;
            for (i = 0; i < TRACE_CAL_CH; i++) {
                int32_t v = (int32_t)s_fl_lvl[i] - off;

                if (v < 0) v = 0;
                s_fl_lvl[i] = (uint8_t)(((uint32_t)v * nrq) >> TRACE_LVL_SHIFT);
            }
            ped = off;
        }
        s_fl_state = TRACE_FRAME_OK;

        /* 好帧留一份快照，给「丢线时按住上一帧」用（trace_cal.h 那条规矩）。
         * 只存柱高：界面那个 0/1 位本来就是"柱高 >= 半高 128"，不另存一份、免得两处口径。 */
        for (i = 0; i < TRACE_CAL_CH; i++) s_hold_lvl[i] = s_fl_lvl[i];
        s_hold_ms    = HAL_GetTick();
        s_hold_valid = 1U;
    }

    trace_cal_frame_dbg.shift     = (int16_t)shift;
    trace_cal_frame_dbg.shift_raw = (int16_t)shift_raw;
    trace_cal_frame_dbg.lvl_max   = (uint8_t)lvl_max;
    trace_cal_frame_dbg.noise     = (uint8_t)((noise > 255) ? 255 : noise);
    trace_cal_frame_dbg.whiteish  = whiteish;
    trace_cal_frame_dbg.jolt      = jolt;
    trace_cal_frame_dbg.state     = s_fl_state;
    trace_cal_frame_dbg.ped       = (uint8_t)((ped < 0) ? 0 : ((ped > 255) ? 255 : ped));
}

trace_frame_t trace_cal_frame(float w[TRACE_CAL_CH])
{
    uint8_t i;

    if (s_fl_state != TRACE_FRAME_OK) return (trace_frame_t)s_fl_state;

    for (i = 0; i < TRACE_CAL_CH; i++) w[i] = (float)s_fl_lvl[i] / 255.0f;
    return TRACE_FRAME_OK;
}

/* 手上有没有"可以按住"的上一帧好帧：有，而且还没超 TRACE_HOLD_MS。
 * （用户 m26712 第 3 条：丢线/断链之后先保持之前的状态，超时才认丢线） */
static uint8_t hold_ready(void)
{
    if (!s_hold_valid) return 0U;
    return (uint8_t)(((uint32_t)(HAL_GetTick() - s_hold_ms) <= TRACE_HOLD_MS) ? 1U : 0U);
}

int trace_cal_channel(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return -1;
    if (!trace_cal_analog_ok) {
        /* 没有模拟量：只有用户**显式**选了「灰度接口 = 数字」才回落 PA0..PA7（那本来就是
         * 他要的源）；其余情况按"丢线"走 —— 先用按住值顶 TRACE_HOLD_MS，超时算一路都没压线。
         * 不再无条件回落数字口：这台车上那 8 根脚配的是上拉输入、线没被驱动（bsp_board.c），
         * 回落过去 8 路全高，任务块的「探头全白」判据就永远不成立（用户 m26712 第 2 条）。 */
        if (trace_cal_mode == TRACE_MODE_DIGITAL) return -1;   /* 调用者回落数字口 */
        return (hold_ready() && s_hold_lvl[idx] >= 128U) ? 1 : 0;
    }
    if (!trace_cal_valid || s_bad[idx]) return -1;    /* 没标定 / 这一路标定无效 */

    /* 单帧自适应在跑：0/1 就是"本帧黑度过不过半高"，和重心用的是同一份权重 */
    if (s_fl_state == TRACE_FRAME_OK)   return (s_fl_lvl[idx] >= 128U) ? 1 : 0;
    if (s_fl_state == TRACE_FRAME_WEAK) return 0;     /* 这帧没信息：一路都不算压线 */

    if (s_pol[idx]) {
        return (trace_cal_raw[idx] >= s_thr[idx]) ? 1 : 0;
    }
    return (trace_cal_raw[idx] <= s_thr[idx]) ? 1 : 0;
}

uint8_t trace_cal_level(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return 0U;

    if (!trace_cal_analog_ok) {
        /* 没有模拟量：数字模式才拿数字口当满/空；其余情况先给按住值、超时判丢线（0）。
         * 判定口（任务块/循迹）要的是真值，所以按"丢线"走；界面要的是"别突然全塌"，
         * 走 trace_cal_disp_level()（同一份按住值，另外还会打「丢线」标记）。 */
        if (trace_cal_mode == TRACE_MODE_DIGITAL) return trace_read_channel(idx) ? 255U : 0U;
        return hold_ready() ? s_hold_lvl[idx] : 0U;
    }
    /* 单帧自适应在跑：柱高 = 本帧相对黑度（最黑那一路满格），
     * 于是"半高的那条分界线"就是真正的判定线（对比度变小也对得上） */
    if (s_fl_state == TRACE_FRAME_OK)   return s_fl_lvl[idx];
    if (s_fl_state == TRACE_FRAME_WEAK) return 0U;

    return level_of(idx, trace_cal_raw[idx]);
}

uint8_t trace_cal_disp_level(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return 0U;

    /* 本帧是好的：直接给真值（和 trace_cal_level 同源） */
    if (trace_cal_analog_ok && s_fl_state == TRACE_FRAME_OK) return s_fl_lvl[idx];

    /* 用户显式选了数字口：那 0/255 就是他要看的东西 */
    if (!trace_cal_analog_ok && trace_cal_mode == TRACE_MODE_DIGITAL) {
        return trace_read_channel(idx) ? 255U : 0U;
    }

    /* 丢线（本帧 WEAK）或者模拟量断了：按住最后一帧好帧，超时归零。
     * 配合 trace_cal_held() 打「丢线」，用户一眼能看出这几个柱子是旧的。 */
    return hold_ready() ? s_hold_lvl[idx] : 0U;
}

uint8_t trace_cal_disp_bit(uint8_t idx)
{
    /* 界面底部那个 0/1 和柱高同一份数据（半高就是判定线），不另存一份免得两处口径 */
    return (uint8_t)((trace_cal_disp_level(idx) >= 128U) ? 1U : 0U);
}

uint8_t trace_cal_held(void)
{
    if (trace_cal_mode == TRACE_MODE_DIGITAL) return 0U;                    /* 数字口就是真值 */
    if (trace_cal_analog_ok && s_fl_state == TRACE_FRAME_OK) return 0U;     /* 本帧真值 */
    return 1U;   /* 丢线 / 断链 / 没标定：界面显示的是按住值（或空） */
}

int16_t trace_cal_thr_level(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return -1;
    if (!trace_cal_analog_ok) return -1;      /* 没有模拟量：数字口那个比较器不是这根线 */
    if (!trace_cal_valid) return 128;         /* 只有自适应量程：先画中点当临时分界 */
    if (s_bad[idx]) return -1;

    /* 自适应在跑：判定线就是"本帧最黑那一路的一半"，画在半高 */
    if (s_fl_state == TRACE_FRAME_OK)   return 128;
    if (s_fl_state == TRACE_FRAME_WEAK) return -1;

    /* 映射是两点线性的，阈值一定落在柱高的一半（±1 的取整误差），
     * 直接算一遍而不是写死 128，将来换映射公式这里不用跟着改。 */
    return (int16_t)level_of(idx, s_thr[idx]);
}

uint16_t trace_cal_threshold(uint8_t idx)
{
    return (idx < TRACE_CAL_CH) ? s_thr[idx] : 0U;
}

/* ------------------------------------------------- 标定质量（界面/调试） */

uint8_t trace_cal_ch_ok(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return 0U;
    return (uint8_t)((trace_cal_valid && !s_bad[idx]) ? 1U : 0U);
}

int32_t trace_cal_span(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return 0;
    return (int32_t)s_blk[idx] - (int32_t)s_wht[idx];
}

uint16_t trace_cal_jitter(uint8_t idx)
{
    return (idx < TRACE_CAL_CH) ? s_jit[idx] : 0U;
}

/* 「这份标定能不能信」的查询口（界面/上位机/台架脚本都用它检验）：
 *   bad_reason = 逐路原因码，quality = 有效路数，min_span = 有效路里最差的对比度。
 * 抖动那一路的原因码只有在**两点都采到**之后才有意义；没标定时统一报 NOSAMPLE。 */
uint8_t trace_cal_bad_reason(uint8_t idx)
{
    if (idx >= TRACE_CAL_CH) return TRACE_BAD_NOSAMPLE;
    if (!trace_cal_valid)    return TRACE_BAD_NOSAMPLE;
    return s_bad[idx];
}

uint8_t trace_cal_quality(void)
{
    uint8_t i, n = 0U;

    if (!trace_cal_valid) return 0U;
    for (i = 0; i < TRACE_CAL_CH; i++) {
        if (s_bad[i] == 0U) n++;
    }
    return n;
}

uint16_t trace_cal_min_span(void)
{
    uint8_t i;
    uint32_t best = 0U;

    if (!trace_cal_valid) return 0U;
    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t sp;
        uint32_t a;

        if (s_bad[i] != 0U) continue;      /* 无效路不参与"最差对比度" */
        sp = trace_cal_span(i);
        a  = (uint32_t)((sp < 0) ? -sp : sp);
        if (best == 0U || a < best) best = a;
    }
    return (uint16_t)best;
}

uint8_t trace_cal_retry_hint(void)
{
    if (s_retry_ms == 0U) return 0U;
    if (s_state != TRACE_CAL_WAIT_BLACK && s_state != TRACE_CAL_WAIT_WHITE) return 0U;
    /* 只提示 1.5s：用户重按了、或本来就采得好，这一行自己消失 */
    return (uint8_t)(((HAL_GetTick() - s_retry_ms) > 1500U) ? 0U : 1U);
}

/* ---------------------------------------------------------------- 标定向导 */

static uint8_t s_valid_before;   /* 进标定前的 trace_cal_valid：中途取消要还回去 */

void trace_cal_wizard_start(void)
{
    uint8_t i;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        s_rmin[i] = 0xFFFFU;
        s_rmax[i] = 0U;
        s_jit[i]  = 0U;
        s_bad[i]  = 0U;
    }
    s_n = 0U;
    s_retry_ms = 0U;
    s_valid_before = trace_cal_valid;   /* 半路取消别把旧标定一起废掉 */
    trace_cal_valid = 0U;         /* 重标期间先不吃旧阈值 */
    s_state = TRACE_CAL_WAIT_BLACK;

    /* 顺手再喊一次"输出全部"：用户可能就是刚插上模块才开始标定 */
    trace_cal_request_analog_mode();
}

void trace_cal_wizard_press(void)
{
    if (s_state != TRACE_CAL_WAIT_BLACK && s_state != TRACE_CAL_WAIT_WHITE) {
        if (s_state == TRACE_CAL_DONE || s_state == TRACE_CAL_FAILED) {
            s_state = TRACE_CAL_IDLE;
        }
        return;
    }

    if (!trace_cal_analog_ok) {
        /* 读不到模拟量就标不出来，别让用户对着"没反应"发呆 */
        s_state = TRACE_CAL_FAILED;
        return;
    }

    /* 一口气采完这一个点（≈20ms），采完直接推进状态/算阈值。
     * 这么短就不需要"0/32"的采样进度画面了（用户 m03536：几十毫秒就够）。 */
    s_n = 0U;
    if (!sample_point()) {
        s_state = TRACE_CAL_FAILED;
        return;
    }
    sample_finish();
}

/* ------------------------------------------- 标定数据（Flash 持久化搬运） */

void trace_cal_get_data(trace_cal_data_t *out)
{
    if (out == NULL) return;
    /* 先清零：`bad[8]` 与 `blk[8]` 之间编译器插了一个填充字节，不写它的话
     * 调用方（trace_cal_export 的栈上局部量）会把这个字节的栈垃圾一起
     * 赋给 payload ⇒ nvram 的 `memcmp(&cur, &s_live)` 每轮都不等、
     * 自动保存永远不触发（2026-10-01 实测到的"换套不落盘"真因）。 */
    memset(out, 0, sizeof(*out));
    out->valid = trace_cal_valid;
    memcpy(out->pol, s_pol, sizeof(out->pol));
    memcpy(out->bad, s_bad, sizeof(out->bad));
    memcpy(out->blk, s_blk, sizeof(out->blk));
    memcpy(out->wht, s_wht, sizeof(out->wht));
    memcpy(out->thr, s_thr, sizeof(out->thr));
    memcpy(out->jit, s_jit, sizeof(out->jit));
}

void trace_cal_set_data(const trace_cal_data_t *in)
{
    if (in == NULL) return;
    trace_cal_valid = (uint8_t)((in->valid != 0U) ? 1U : 0U);
    memcpy(s_pol, in->pol, sizeof(s_pol));
    memcpy(s_bad, in->bad, sizeof(s_bad));
    memcpy(s_blk, in->blk, sizeof(s_blk));
    memcpy(s_wht, in->wht, sizeof(s_wht));
    memcpy(s_thr, in->thr, sizeof(s_thr));
    memcpy(s_jit, in->jit, sizeof(s_jit));

    /* 换了标定（或者从 Flash 恢复了另一份）：白底基线/整体偏置重新起头，
     * 几何缓存/运行时阈值按新标定重算（用户 m07689：参数靠标定算）。
     * 注意这也在 nvram 开机恢复时走一遍 —— 所以上电那一刻阈值就已经是这台车的数了。 */
    s_base_ok  = 0U;
    s_shift_ok = 0U;
    s_recalc_geom();
}

void trace_cal_set_adapt(uint8_t on)
{
    uint8_t v = (on != 0U) ? 1U : 0U;

    if (v == trace_adapt_enable) return;
    trace_adapt_enable = v;
    /* 重新打开时从当前标定重新起头（关了这段时间里路面可能已经变了） */
    s_base_ok  = 0U;
    s_shift_ok = 0U;
}

/* ------------------------------------------------ 两套标定（普通 / 亚克力） */

uint8_t trace_cal_active_set(void)
{
    return s_set;
}

void trace_cal_select_set(uint8_t set)
{
    trace_cal_data_t cur;
    trace_cal_data_t next;
    uint8_t v = (set != 0U) ? 1U : 0U;

    if (v == s_set) return;

    trace_cal_get_data(&cur);          /* 把当前这套收好 */
    if (!s_other_ok) {
        /* 还没有第二套（刚烧完/没恢复过 Flash）：复制当前这套 —— 切过去至少能用，
         * 绝不会出现"切到空标定"导致 span=0 的野值。 */
        next = cur;
        s_other_ok = 1U;
    } else {
        next = s_other;
    }
    s_other = cur;
    trace_cal_set_data(&next);         /* 换当前套：清白底基线 + 按新标定重算几何 */
    s_set = v;
}

void trace_cal_export(trace_cal_data_t *set0, trace_cal_data_t *set1, uint8_t *active)
{
    trace_cal_data_t cur;

    trace_cal_get_data(&cur);
    if (s_set == 0U) {
        if (set0 != NULL) *set0 = cur;
        if (set1 != NULL) *set1 = s_other_ok ? s_other : cur;
    } else {
        if (set0 != NULL) *set0 = s_other_ok ? s_other : cur;
        if (set1 != NULL) *set1 = cur;
    }
    if (active != NULL) *active = s_set;
}

void trace_cal_import(const trace_cal_data_t *set0, const trace_cal_data_t *set1, uint8_t active)
{
    uint8_t v = (active != 0U) ? 1U : 0U;

    if (set0 == NULL || set1 == NULL) return;

    /* 非当前那套先摆好，当前套走 trace_cal_set_data()（它会清基线、重算几何） */
    s_other    = (v == 0U) ? *set1 : *set0;
    s_other_ok = 1U;
    trace_cal_set_data((v == 0U) ? set0 : set1);
    s_set = v;
}

void trace_cal_wizard_cancel(void)
{
    /* 黑点还没采（WAIT_BLACK）时什么都没写进 s_blk/s_wht：把旧标定还回去，
     * 否则"点进来又退出"会把车上一直在用的那份标定废掉（trace_cal_valid=0，
     * 判线退回数字口）。采过黑点之后就不能还了——s_blk 已经是新旧混合。 */
    if (s_state == TRACE_CAL_WAIT_BLACK) {
        trace_cal_valid = s_valid_before;
    }
    s_state = TRACE_CAL_IDLE;
    s_n = 0U;
}

trace_cal_state_t trace_cal_state(void)
{
    return s_state;
}
