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
 * ★ 模块的输出模式会"丢"：它自己上电/复位后回到默认的"数字量"，必须再收到一次
 *   0x4C 00 01 03 才给带模拟量的"全部"。开机那一次广播可能撞在模块启动过程上、
 *   跑动中模块也可能被颠掉电重启 —— 所以只要读不成帧就按 TRACE_MODE_REQ_MS 节流
 *   重播一次（maybe_request_analog_mode），不必等 MCU 复位（用户 m29611）。
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

/* ★2026-10-13：采集周期**按当前用的是哪条**分别定。
 *
 *  I2C 那条：一次读 16 字节 @400kHz ≈ 0.45ms ⇒ 5ms 一个周期只占约 9% 总线；
 *            主循环实测中位耗时 8us/5ms ⇒ 余量很大，故提到 **5ms（200Hz）**。
 *  串口那条：一次是"发 2 字节请求 + 逐字节等回复"的**往返**（uart_read_analog），
 *            缺一个字节就要等一个 TRACE_UART_BYTE_MS ⇒ 几毫秒到几十毫秒（见 poll 里那句
 *            "UART 那条每次要干等数十毫秒，把主循环拖慢"）。
 *            ⇒ **绝不能**跟着 I2C 一起提到 5ms，否则主循环被阻塞读拖垮，控制环跟着抖。
 *            所以串口保持原来的 20ms。
 *  源还没定下来（TRACE_SRC_NONE）时用保守的那个（20ms）。
 */
#define TRACE_READ_PERIOD_MS       20U   /* 串口 / 源未定：原值不动 */
#define TRACE_READ_PERIOD_I2C_MS    5U   /* I2C：0.45ms/次，5ms 很安全 */
#define TRACE_FAIL_LIMIT        5U    /* 连续失败这么多帧：按下面的"粘性"规则处理 */
#define TRACE_SRC_STICK_MS    800U    /* 这个源 800ms 内还读到过帧 ⇒ 只重试，不换源/不判死（m31580） */
#define TRACE_OFF_RETRY_MS    300U    /* 两条都不通：多久后再从头试一遍（原 2000 ⇒ 黑屏 2~3s，用户 m31580） */
#define TRACE_MODE_REQ_MS    3000U    /* 读不通时重播"输出=全部"的最小间隔（用户 m29611） */
#define TRACE_MIN_SPAN         32U    /* 黑白原值差小于这个 = 这一路没数据 */
#define TRACE_UART_BYTE_MS      2U    /* 每字节超时：115200 下一字节 87us，2ms 很宽裕 */
/* ★2026-10-15：找帧头时最多丢弃多少个字节就放弃这一帧。
 * 没有这个上限的话，总线持续来乱码时 uart_read_analog() 会**无限循环**
 * （每轮还等 2ms）⇒ 主循环被 trace_cal_poll() 反复饿死（实测遥测卡 195ms）。
 * 8 个字节 × 2ms = 上限 16ms，足够正常情况（正常时下一字节 87us 内就来）。 */
#define TRACE_UART_MAX_DROP      8U

#define TRACE_UART_SOF       0x75U    /* 模块回的帧头 */
#define TRACE_UART_READ      0x57U    /* 读命令（+ 1 字节地址） */
#define TRACE_UART_TAIL_ANA  0x11U    /* 18 字节模拟量帧帧尾 */
#define TRACE_UART_TAIL_ALL  0x15U    /* 22 字节"全部"帧帧尾 */

uint16_t trace_cal_raw[TRACE_CAL_CH];
uint8_t  trace_cal_analog_ok;
uint8_t  trace_cal_valid;
uint8_t  trace_cal_src;
uint8_t  trace_cal_mode = TRACE_MODE_AUTO;
uint8_t  trace_adapt_enable = 1U;         /* 口径：1 = 连续（离阈值插值，默认）/ 0 = 纯 0/1 */
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

/* 本帧每路的黑度（0..255，128 = 正好压在标定阈值上 = 0/1 分界；用户 m31902 起不再归一化）
 * + 本帧判定结果。在 trace_cal_poll() 里跟着模拟量一起算，取值口直接读缓存
 * （和 trace_cal_raw[] 同时刻）。 */
static uint8_t s_fl_lvl[TRACE_CAL_CH];
static uint8_t s_fl_state;
/* ★2026-10-13：5 次多数表决 —— 每路一个位历史（bit0 = 最近一帧，越往高位越早）+ 表决结果 */
static uint8_t s_vote_hist[TRACE_CAL_CH];
static uint8_t s_vote_lvl[TRACE_CAL_CH];

/* 最后一帧"好帧"（TRACE_FRAME_OK）的柱高快照 + 当时时刻（用户 m26712 第 3 条：
 * 丢线之后界面要还能保持住之前的状态，见 trace_cal.h 的「丢线时按住上一帧」）。 */
static uint8_t  s_hold_lvl[TRACE_CAL_CH];
static uint32_t s_hold_ms;
static uint8_t  s_hold_valid;

/* ★★ 单帧自适应已按用户 m31902 删除（2026-10-05）★★
 * 用户原话：「你把这个"自适应"还是删除了吧，我感觉没有用…你还不如去优化一下之前说的那个
 * 阈值，可以把它设为一个区间之类的…如果非要细分,也可以根据它离阈值的距离来确定中间的位置」
 *
 * 删掉的是：每路白底基线 s_base（慢学 + 硬夹子）、每帧整体偏置 ŝ（s_shift + 中位数 +
 * 限速 + jolt 判扰动）、漂移半径 s_dr、饱和黑/白底计数兜底、最小对比度 s_minlvl。
 * 它们原本是为了跟"白值随路面/高度/灯光漂几十个计数"，但代价是判据一多就在现场互相打架：
 * 2026-10-05 实测（用户 m31580）车明明白白摆在线上，帧却在 OK/WEAK 之间跳 —— WEAK 那一拍
 * 全是 white_ref == 0 触发的（本车标定白值当时漂了 ~50 个计数），WEAK 一按住 500ms 就到
 * 期，界面 8 根柱子整个塌成 0 = 用户看到的"丢线/全是零"。
 *
 * 现在只有一条路：**标定阈值 + 离阈值的距离**（见下面 frame_compute_body 的注释）。
 * 白值漂移由"距离"天然吸收：白地比标定白值亮/暗几十个计数，只是柱高在 0 附近动几十个数，
 * 判定线（阈值 = 128）不会跟着乱跳。
 * trace_adapt_enable（设置页「连续判线」，2026-10-05 用户 m32216 前叫「自动判线」）现在只区分两种显示/权重口径：
 *   1 = 连续（默认）：阈值上 = 128，按离阈值的距离插值到 0/255；
 *   0 = 纯 0/1：越过阈值就是 255、没越过就是 0（老的中点阈值二值法）。
 */

/* ---------------------------------------------------------------- 几何缓存
 * 用户 m07689：「自适应里它实际上也有一些参数，这个参数可以靠我标定的来计算」。
 * frame_compute() 每 20ms 跑一次、8 路各要一次"原值 -> 0..255 黑度"的换算。整数除法在
 * Cortex-M4 上 2~12 个周期一条，8 路×2 处 = 16 条，比这个函数其它部分加起来还贵。所以
 * 标定一变（向导收尾 / Flash 恢复）就把每路的几何量算一次，之后每帧只剩乘 + 移位：
 *   s_sp[i] = |跨度|（原值计数）
 *   s_rq[i] = (255 << TRACE_LVL_SHIFT) / s_sp[i]
 *             ->  离阈值的距离折算成黑度 = (距离 * s_rq[i]) >> TRACE_LVL_SHIFT
 *   s_an[i] = "还压在标定白值上"的容差 = max(跨度/8, max(16, 3×这一路标定抖动))
 *             （只喂调试口 white_ref，判据已经不用它了） */
#define TRACE_LVL_SHIFT 12         /* 定点倒数的移位：12 位足够，距离夹 ±2047 不会溢出 */
static int32_t s_sp[TRACE_CAL_CH];
static int32_t s_rq[TRACE_CAL_CH];
static int32_t s_an[TRACE_CAL_CH];
trace_geom_t trace_geom;           /* 只读调试口，见 trace_cal.h */
uint32_t trace_frame_cycles;       /* 上一帧 frame_compute() 的 DWT 周期数 */
uint32_t trace_frame_cycles_max;   /* 上电以来最坏的一次（实时性证据，只增不减） */

static void frame_compute(void);   /* 定义在下面「取值」段，poll 里先调 */
static void frame_compute_body(void);
static void s_recalc_geom(void);   /* 标定变了就重算上面那块缓存 */
static int32_t s_median(int32_t *v, uint8_t n);

static uint16_t s_buf[TRACE_CAL_CH][TRACE_CAL_SAMPLE_N];  /* 标定采样缓冲：8×32×2 = 512B */
static uint8_t  s_n;                      /* 本次标定点实际采到的帧数 */
/* ★2026-10-15（审计 #16）：标定采样从"一口气采完"改成"每轮主循环采一帧"。
 * s_samp_k = 本轮已采到第几帧；s_sampling = 1 表示正在采（press 发起、poll 推进）。 */
static uint8_t  s_samp_k;
static uint8_t  s_sampling;
static uint8_t  s_fail;
static uint8_t  s_tried;                  /* 1 = 已经为本次探测换过通道了（只换一次） */
static uint8_t  s_first;                  /* 首选通道（自动模式：串口优先） */
static uint32_t s_last_ms;
static uint32_t s_off_until_ms;
static uint32_t s_ok_ms;                  /* 上一次读成帧的时刻（AUTO 换源的"粘性"判据，m31580） */
static uint32_t s_done_ms;
static uint32_t s_retry_ms;               /* 上一次"整点被抖动判掉"的时刻（0 = 没有） */
static uint32_t s_req_ms;                 /* 上一次重播"输出=全部"的时刻 */
static uint8_t  s_req_sent;               /* 1 = 播过至少一次（配合 TRACE_MODE_REQ_MS 节流） */
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

/* 读不通时把"输出=全部"再播一遍（按 TRACE_MODE_REQ_MS 节流）。
 *
 * 为什么必须重播（用户 m29611 报"卡住了，复位一下就好"，2026-10-05）：
 * 模块**自己上电/复位后会回到默认的"数字量"输出**（见文件头引的官方《IIC通信注意事项》），
 * 只有收到一次 0x4C 00 01 03 才改成带模拟量的"全部"。而本文件以前只在
 * trace_cal_init()（=MCU 复位）和标定开始时播这一次 —— 于是：
 *   ① 车一上电 MCU 先起来、模块还在启动，那一次广播丢掉，就再也读不到模拟量；
 *   ② 跑动中模块被颠得掉电重启（同一根线/同一个插头），同样只认 MCU 复位。
 * 两种现象都是"模块自己的灯还亮着、数据也对，就是主机读不到"。
 * 现在只要读不成帧（IIC 或串口都算，模拟量输出本来就是靠这条串口广播开的），
 * 就隔 500ms 再播一次，最坏 TRACE_OFF_RETRY_MS 一个周期内自己恢复，不用复位。 */
static void maybe_request_analog_mode(uint32_t now)
{
    if (s_req_sent && (uint32_t)(now - s_req_ms) < TRACE_MODE_REQ_MS) return;
    s_req_sent = 1U;
    s_req_ms   = now;
    trace_cal_request_analog_mode();
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
    s_sampling = 0U;      /* ★审计 #16：跨轮采样的状态，上电必须归零 */
    s_samp_k   = 0U;
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
    uint8_t drop = 0U;          /* ★已丢弃的"非帧头"字节数（见下面的上限说明） */

    /* 先冲刷掉可能残留的字节：读取帧是"你问我才答"，正常不会有。
     * ★2026-10-15：这里同样要有上限 —— 超时是 0（不等），但若总线**持续**来字节
     *  （线悬空/波特率不符时会被误当连续字节），这个 while 也会一直转。
     *  与下面找帧头处的 TRACE_UART_MAX_DROP 同一个原因：**不允许无界循环**。 */
    {
        uint8_t flush = 0U;
        while (board_uart4_read_byte(&b, 0U)) {
            if (++flush > 32U) break;
        }
    }

    board_uart4_write(req, sizeof(req));

    /* ★2026-10-15（实测抓到）：丢字节**必须有上限**。
     *  原来 `if (n == 0U && b != TRACE_UART_SOF) continue;` 里 `n` 不自增
     *  ⇒ 只要总线上持续来"非帧头"字节（传感器没插/线悬空/乱码时就是这样），
     *    这个循环**永远不会结束**，而每轮还要等最多 TRACE_UART_BYTE_MS(2ms)
     *  ⇒ **无界阻塞**。trace_cal_poll() 每 20ms 就来调一次 ⇒ 主循环被反复饿死。
     *  实测症状：逐帧采集发现遥测**周期性卡 195ms**（帧间隔中位 5ms、最大 195ms），
     *  电机电流稳定在 2.1~2.8A 而编码器读数一半为 0 ⇒ 之前所有速度环数据都被它污染。
     *  修法：最多丢弃 TRACE_UART_MAX_DROP 个字节就认定"这一帧没齐"，交给上层退避逻辑。 */
    while (n < (uint8_t)sizeof(buf)) {
        if (!board_uart4_read_byte(&b, TRACE_UART_BYTE_MS)) break;  /* 字节没来 = 帧没齐 */
        if (n == 0U && b != TRACE_UART_SOF) {
            if (++drop > (uint8_t)TRACE_UART_MAX_DROP) break;       /* ★丢够了就放弃这一帧 */
            continue;                                               /* 帧头前全丢 */
        }
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

/* 采**一帧**标定样本，推进 s_samp_k；返回 1 = 这一帧读失败。
 *
 * ★2026-10-15（审计 #16）：这里原来是 `sample_point()`，**一口气连采 32 帧**。
 *  它的注释按 **I2C** 估（一帧 ≈0.6ms ⇒ 32 帧 ≈20ms），但 `TRACE_SRC_AUTO` 是
 *  **串口优先**：串口一帧 = 发 2 字节 + 每字节最多等 `TRACE_UART_BYTE_MS`(2ms)
 *  ⇒ 健康链路 ≈1.8~2.5ms/帧 ⇒ 32 帧 ≈ **60~80ms**；链路半死时最坏 22×2ms×32 ≈ **1.4s**。
 *  这段时间全在**主循环里同步阻塞**：屏幕不刷、按键不响应；任务在跑时车还会带着
 *  最后一条指令继续动。现在拆成"每次 poll 采一帧"，单次阻塞降到**一帧**。
 *
 * I2C 与 OLED 共用总线：每帧前等屏空闲，别插进它的搬运中间。 */
static uint8_t sample_one(void)
{
    uint8_t b16[TRACE_CAL_CH * 2U];
    uint8_t i;

    if (trace_cal_src == TRACE_SRC_I2C) {
        uint16_t guard = 0U;
        while (OLED_IsBusy()) {
            if (++guard > 200U) break;   /* 200ms 还不空闲：屏那边有问题，别死等 */
            HAL_Delay(1);
        }
    }
    if (!source_read(b16)) return 1U;
    for (i = 0; i < TRACE_CAL_CH; i++) {
        uint16_t v = (uint16_t)(((uint16_t)b16[i * 2U] << 8) |
                                (uint16_t)b16[i * 2U + 1U]);
        s_buf[i][s_samp_k] = v;
        if (v < s_rmin[i]) s_rmin[i] = v;
        if (v > s_rmax[i]) s_rmax[i] = v;
        trace_cal_raw[i] = v;
    }
    s_samp_k++;
    return 0U;
}

/* 采样完算平均并推进状态。进入时 s_state 还是 WAIT_BLACK / WAIT_WHITE。 */
/* ★2026-10-13（审计 #13）：进标定前那份 trace_cal_valid。声明放这里 —— `sample_finish()`
 *  （在下面）失败时就要用它还原，不能等向导那一段才声明。 */
static uint8_t s_valid_before;   /* 进标定前的 trace_cal_valid：中途取消要还回去 */
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
    if (trace_cal_valid == 0U) {
        /* ★2026-10-13（审计 #13）：这次采下来一路都不能用 ⇒ 应当**保留进标定之前那份旧标定**，
         *  而不是把 valid 清成 0。清成 0 的后果不是"这一次会话里没标定"，而是**旧的整套标定
         *  静默作废**（消费侧 `trace_cal_channel` 见到 valid=0 就直接回 -1 ⇒ 循迹永远判丢线），
         *  而且 ~1.5s 后 nvram 会自动把 valid=0 写进 Flash ⇒ **掉电也回不来**，必须重做完整两点标定。
         *  用户"退出了标定向导"≠"我要废掉旧标定"。 */
        trace_cal_valid = s_valid_before;
        s_state         = TRACE_CAL_FAILED;
    } else {
        s_state         = TRACE_CAL_DONE;
    }

    /* 刚标完：几何缓存按新标定重算（用户 m07689：参数靠标定算）。
     * 自适应删掉后没有"白底基线要重新起头"这回事了：阈值/极性就在新标定里，下一帧直接生效。 */
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

    /* ★2026-10-15（审计 #16）：标定采样推进 —— `trace_cal_wizard_press()` 只**发起**
     *  （s_sampling=1, s_samp_k=0），真正的取帧在这里**一轮一帧**地做。
     * 放在周期闸门**之前**：采样本事有它自己的节奏（一帧就是一次总线/串口往返），
     * 不该再被 20ms/5ms 的常规读取周期拖慢。
     * 这几十轮里会跳过常规周期采样与 `frame_compute()` —— 循迹的线状态最多陈旧
     * 这么多轮；用户按标定键时车是静止的，无影响。 */
    if (s_sampling) {
        if (sample_one()) {
            /* 这一帧读失败 ⇒ **停止采样**（与原 `sample_point()` 里 `break` 的语义一致），
             *  但已经采到的帧照样用：够 8 帧就照常算，不够才当"采样根本没做成"。
             *  两种出口都与审计 #13 的口径一致：采样没做成时**保留进标定前那份旧标定**。 */
            s_sampling = 0U;
            if (s_samp_k >= 8U) {
                s_n = s_samp_k;
                sample_finish();
            } else {
                trace_cal_valid = s_valid_before;
                s_state         = TRACE_CAL_FAILED;
            }
            return;
        }
        if (s_samp_k >= (uint8_t)TRACE_CAL_SAMPLE_N) {
            s_sampling = 0U;
            s_n        = s_samp_k;
            sample_finish();
        }
        return;   /* 还在采、或刚采完这一轮：本轮都不做常规采样 */
    }

    {
        /* ★按当前源选周期（见上面两个常量的说明）：I2C 提到 5ms，串口保持 20ms。 */
        uint32_t period = (trace_cal_src == TRACE_SRC_I2C) ? TRACE_READ_PERIOD_I2C_MS
                                                          : TRACE_READ_PERIOD_MS;
        if ((now - s_last_ms) < period) return;
    }
    s_last_ms = now;

    /* I2C 总线看门狗：OLED 空闲时才有机会真正执行恢复（恢复要手动打时钟，
     * 会把正在 DMA 搬运的屏打断）。这里只是"报个空闲"，不满足条件时它自己什么都不做。 */
    (void)board_i2c_watchdog((uint8_t)(OLED_IsBusy() ? 0U : 1U));

    /* ★2026-10-13（审计 #14，用户："你先检查代码再改"）：`frame_compute()` **原来放在这里**
     *  —— 也就是"过了周期闸门、但在取数之前"。问题是下面每一条提前 return（退避 / 等 OLED /
     *  读失败）都**不产生新样本**，可 frame_compute() 已经跑过了：它会拿**没变过的**
     *  `trace_cal_raw[]` 重算一遍，只要仍判成好帧就再 `vote_push()` 一次 ⇒
     *  【5 深度多数表决是按"调用次数"计票，而不是按"样本数"】—— 单帧毛刺能被同一份陈旧数据
     *  自己灌成"连续 5 次确认"，`s_hold_ms` 也被反复续期。这与 vote_push 自己的注释
     *  （"连续两帧以上的干扰直接不认"）直接矛盾。
     *
     *  改法：挪到**真正取到新样本之后**（本函数末尾那段缓存更新之后）。周期闸门仍在原处
     *  （所以"每 5ms/20ms 一帧"的节奏不变），只是不再用陈旧帧充数。 */

    /* 当前这条判死了（AUTO 两条都试过 / 固定通道失败）：退避 TRACE_OFF_RETRY_MS 再按用户
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
        /* 一帧都读不成，先把模块的"输出=全部"再播一遍（节流）：模块自己复位过、
         * 或开机那一次广播撞在它启动过程上时，只有重播才能把模拟量要回来（用户 m29611）。 */
        /* 只在**连续**失败(>= TRACE_FAIL_LIMIT)时才允许重发配置：偶发一帧收错不动它。
         * 用户反馈 2026-10-17：一直发配置占用 UART4、给循迹数据加延迟，反而更差。 */
        if (s_fail >= TRACE_FAIL_LIMIT) maybe_request_analog_mode(now);
        if (s_fail >= TRACE_FAIL_LIMIT) {
            s_fail = 0U;
            if ((uint32_t)(now - s_ok_ms) < TRACE_SRC_STICK_MS) {
                /* 这个源刚刚（< TRACE_SRC_STICK_MS）还读到过帧：当成一次瞬时丢帧，原地继续重试。
                 * 用户 m31580 现场："摆在线上有时候显示丢线、有时候全是零" —— 进循迹模式那几拍
                 * 会连丢几帧，老代码据此判"串口死了"，换到本车不存在的 I2C，再把模拟量整条关掉
                 * 退避 2s ⇒ 界面空 2~3 秒（LED 柱全 0、丢线标亮）。这类抖动现在只重试。 */
            } else if (trace_cal_mode == TRACE_MODE_AUTO && !s_tried) {
                /* AUTO：确实一直读不到才换另一条试（IIC <-> 串口）。**只换一次**，
                 * 否则两条都不通时会 IIC/UART 来回弹，UART 那条每次要干等
                 * 几十毫秒，把主循环拖慢。 */
                s_tried       = 1U;
                trace_cal_src = other_source(trace_cal_src);
            } else {
                /* 固定通道失败，或 AUTO 两条都试过了：判"没有模拟量"，
                 * 上位层回落 PA0–PA7 数字口；TRACE_OFF_RETRY_MS 后再按设置重试。
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
    s_ok_ms = now;                       /* 这一拍读到了：AUTO 的"粘性"从此算起（m31580） */
    trace_cal_analog_ok = 1U;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        uint16_t v = (uint16_t)(((uint16_t)b16[i * 2U] << 8) | (uint16_t)b16[i * 2U + 1U]);
        trace_cal_raw[i] = v;
        if (v < s_rmin[i]) s_rmin[i] = v;
        if (v > s_rmax[i]) s_rmax[i] = v;
    }

    /* ★2026-10-13（审计 #14）：**到这里才算是"这一帧真的取到了新样本"**。
     *  黑度/判定与 5 次多数表决都只在这里算一次 ⇒ 表决窗按**样本数**计票（不是调用次数），
     *  `s_hold_ms` 也只在真实新帧刷新。上面任何一条提前 return 都不会再动它们。 */
    frame_compute();
}

/* ------------------------------------------------- 取值 */

/* 每 20ms 跟着一帧模拟量算一次，结果放 s_fl_lvl[]/s_fl_state 给取值口读
 * （算法说明见 trace_cal.h「判线口径」）。 */
/* ------------------------------------------------- 标定 -> 几何缓存 / 计时壳 */

/* ★2026-10-13（用户："采集 5 次数据之后，把占比多的那个当成新的数据"）：
 *  **每路的 0/1 判定走一道 5 深度滑窗多数表决**（≥3/5 才算压黑）。
 *
 *  为什么加在"0/1"这一层，而不是原始值/柱高上：
 *    · 原始 16 位值几乎不重复、"柱高"是连续量 —— 连续量取"众数"没有意义；
 *    · 而"压黑 / 没压黑"是**量化量**，5 个里出现最多的那个就是它的众数，
 *      等价于"连续两帧以上的干扰直接不认"，单帧毛刺天然被 3/5 盖掉。
 *  为什么用**滑窗**而不是"攒够 5 帧再更新"：攒够会多出 5×采集周期 的延迟
 *      （I2C 5ms ⇒ 25ms），滑窗每来一帧就更新，延迟只有约 2 个周期。
 *  只在**好帧**（TRACE_FRAME_OK）入窗：弱帧/丢线帧本身没信息，入了会把窗口带歪；
 *      那两种情况的语义仍由原来的 s_fl_state / 按住值 决定，一行没动。
 *  想关掉：把 TRACE_VOTE_N 改成 1 即可（1 深度 = 直接透传本帧，逐位退回旧行为）。 */
#define TRACE_VOTE_N   5U     /* 表决窗深度：奇数，取"过半"即多数 */

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

/* 标定一变就重算几何缓存（用户 m07689："参数靠我标定的来计算"）。
 * 除法/排序只在这里做一次，之后每帧只剩乘 + 移位。
 * 自适应（2026-10-05 用户 m31902 删）走了以后这里只剩：每路跨度 s_sp、定点倒数 s_rq、
 * 白值容差 s_an，外加两个只给调试口看的中位数（trace_geom，out/_base_read.jlink dump）。 */
static void s_recalc_geom(void)
{
    int32_t sp[TRACE_CAL_CH], jt[TRACE_CAL_CH];
    uint8_t i, n = 0U;

    trace_geom.rq_shift = TRACE_LVL_SHIFT;
    trace_geom.sp_med   = 0;
    trace_geom.jit_med  = 0;

    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t sp_i = (int32_t)s_blk[i] - (int32_t)s_wht[i];
        int32_t jt_i = (int32_t)s_jit[i];
        int32_t floor_an;

        if (sp_i < 0) sp_i = -sp_i;
        s_sp[i] = sp_i;

        floor_an = jt_i * 3;  if (floor_an < 16) floor_an = 16;  /* 噪声底：3 倍抖动，至少 16 个计数 */

        if (sp_i > 0) {
            int32_t dv = (sp_i < 8) ? 8 : sp_i;   /* 跨度特别小时防溢出：距离夹在 ±2047 */
            s_rq[i] = ((int32_t)255 << TRACE_LVL_SHIFT) / dv;
        } else {
            s_rq[i] = 0;                          /* 0 = 这一路没法换算 */
        }
        s_an[i] = (sp_i / 8 > floor_an) ? (sp_i / 8) : floor_an;

        if (trace_cal_valid && !s_bad[i] && sp_i > 0) {
            sp[n] = sp_i;
            jt[n] = jt_i;
            n++;
        }
    }

    if (n > 0U) {
        trace_geom.sp_med  = s_median(sp, n);
        trace_geom.jit_med = s_median(jt, n);
    }
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

/* 每帧判定：**标定阈值 + 离阈值的距离**（用户 m31902 整改后的唯一一条路）。
 *
 *   d = 原值 - 阈值（按标定学出的极性取符号）：d > 0 = 已经越过阈值 = 黑侧。
 *   黑度 = 128 ± |d| / 半个跨度 × 127     → d = 0 时正好 128（0/1 的判定线），
 *                                          |d| = 半个跨度（= 标定黑值/白值）时饱和到 255/0。
 *
 * 所以"阈值"不是一根线而是一段区间：白侧 [标定白值, 阈值]、黑侧 [阈值, 标定黑值]，
 * 中间的柱高由**离阈值的距离**插值出来（用户 m31902 原话：「可以根据它离阈值的距离
 * 来确定中间的位置」）。没有白底基线、没有整体偏置 ŝ、没有中位数/jolt/最小对比度那一套。
 *
 * 判据只有一条：**有一路越过标定阈值 ⇒ 这一帧认到线（TRACE_FRAME_OK）**；一路都没越过 ⇒ WEAK。
 * "探头底下整片一个样"（离地 / 全黑地面 / 整片脏）不在这层判：那是循迹层
 * PID_tracing.c 的 TRACE_UNIFORM_MIN（本帧最亮与最黑之比在 0.75 以内 = 没结构 = 丢线）的活。
 * 两层分工明确，就不会再出现"车明明在线上、这一层先把自己判死"（用户 m31580 / m31902）。
 *
 * 没有模拟量（数字口 / 读不通）或者没标定：保持 TRACE_FRAME_FALLBACK —— 界面按住上一帧，
 * 循迹口按丢线走（数字口模式另有回落，见 trace_cal_channel）。
 */
/**
 * @brief 把本帧每路的 0/1 推进滑窗，重新表决出 s_vote_lvl[]
 *
 * 只由 frame_compute_body() 在**好帧**分支里调用（见 TRACE_VOTE_N 的说明）。
 * 窗口是 5 位左移：新的一帧进 bit0，最老的从 bit5 溢出丢掉。
 */
static void vote_push(void)
{
  uint8_t i;

#if (TRACE_VOTE_N <= 1U)
  /* 关掉表决：直接透传本帧（逐位等于旧行为） */
  for (i = 0U; i < TRACE_CAL_CH; i++) {
    s_vote_lvl[i] = (uint8_t)((s_fl_lvl[i] >= 128U) ? 1U : 0U);
  }
#else
  for (i = 0U; i < TRACE_CAL_CH; i++) {
    uint8_t k;
    uint8_t bit = (uint8_t)((s_fl_lvl[i] >= 128U) ? 1U : 0U);
    uint8_t h   = (uint8_t)(((uint8_t)(s_vote_hist[i] << 1) | bit) & 0x1FU);
    uint8_t cnt = 0U;

    s_vote_hist[i] = h;
    for (k = 0U; k < TRACE_VOTE_N; k++) {
      if ((h & (uint8_t)(1U << k)) != 0U) cnt++;
    }
    /* 过半即多数。注意初始窗口是全 0：刚上电若本帧压黑，要连续 3 帧黑才成立 ——
     * 这正是"表决"该有的行为（宁可晚 1~2 帧，也不要被单帧骗）。 */
    s_vote_lvl[i] = (uint8_t)(((uint16_t)cnt * 2U > (uint16_t)TRACE_VOTE_N) ? 1U : 0U);
  }
#endif
}

static void frame_compute_body(void)
{
    uint8_t i;
    int32_t lvl_max   = 0;
    int32_t noise     = 0;
    int32_t black_thr = 0;   /* 越过标定阈值的路数（= 黑） */
    int32_t white_ref = 0;   /* 还稳稳压在标定白值上的路数（只在调试口里看） */
    uint8_t whiteish  = 0U;  /* 没过阈值的路数（= 白） */

    s_fl_state = TRACE_FRAME_FALLBACK;
    for (i = 0; i < TRACE_CAL_CH; i++) s_fl_lvl[i] = 0U;
    trace_cal_frame_dbg.shift     = 0;   /* 自适应删了：这两个字段保留但恒 0 */
    trace_cal_frame_dbg.shift_raw = 0;
    trace_cal_frame_dbg.lvl_max   = 0U;
    trace_cal_frame_dbg.noise     = 0U;
    trace_cal_frame_dbg.whiteish  = 0U;
    trace_cal_frame_dbg.jolt      = 0U;
    trace_cal_frame_dbg.state     = TRACE_FRAME_FALLBACK;
    trace_cal_frame_dbg.ped       = 0U;
    trace_cal_frame_dbg.black_thr = 0U;
    trace_cal_frame_dbg.white_ref = 0U;

    if (!trace_cal_analog_ok) return;   /* 没有模拟量：界面按住上一帧、循迹走回落 */
    if (!trace_cal_valid)     return;   /* 没标定：没有阈值可用 */

    for (i = 0; i < TRACE_CAL_CH; i++) {
        int32_t raw, d, mag, off, lvl, nz;

        if (s_bad[i]) continue;                        /* 这一路标定无效：当白 */
        if (s_sp[i] <= 0 || s_rq[i] <= 0) continue;    /* 跨度太小：没法换算 */

        raw = (int32_t)trace_cal_raw[i];
        /* 带符号的距离：> 0 = 越过阈值（黑侧）。极性是标定时学的（黑值比白值大或小都行）。 */
        d   = s_pol[i] ? (raw - (int32_t)s_thr[i]) : ((int32_t)s_thr[i] - raw);
        mag = (d < 0) ? -d : d;
        /* 距离夹 ±2047：模块吐野值（raw 最大 65535）时 mag × s_rq 不能溢出 32 位。
         * 本车一路跨度只有 85~300，夹到 2047 远在正常读数之外，判定不受影响。 */
        if (mag > 2047) mag = 2047;
        off = (int32_t)(((uint32_t)mag * (uint32_t)s_rq[i]) >> TRACE_LVL_SHIFT);  /* 255 × 距离/跨度 */

        if (!trace_adapt_enable) {
            lvl = (d >= 0) ? 255 : 0;                  /* 关掉插值：退回纯 0/1 */
        } else {
            lvl = (d >= 0) ? (128 + off) : (128 - off);   /* 128 = 正好压在阈值上 */
            if (lvl < 0)   lvl = 0;
            if (lvl > 255) lvl = 255;
        }
        s_fl_lvl[i] = (uint8_t)lvl;

        if (lvl > lvl_max) lvl_max = lvl;
        if (d >= 0) black_thr++; else whiteish++;

        /* 调试口：还压在标定白值 ±s_an 里的路数（m31122 那条判据已经不用它判了） */
        nz = (int32_t)s_wht[i] - raw;
        if (nz < 0) nz = -nz;
        if (nz <= s_an[i]) white_ref++;

        /* 这一路的标定抖动折算成黑度：调试口看信噪比（noise 取最差的那路） */
        nz = (int32_t)(((uint32_t)s_jit[i] * (uint32_t)s_rq[i]) >> TRACE_LVL_SHIFT);
        if (nz > noise) noise = nz;
    }

    if (black_thr == 0) {
        /* 一路都没越过阈值 = 这帧没看到线（白地 / 车被拿起来 / 线在探头外面） */
        for (i = 0; i < TRACE_CAL_CH; i++) s_fl_lvl[i] = 0U;
        s_fl_state = TRACE_FRAME_WEAK;
    } else {
        s_fl_state = TRACE_FRAME_OK;
        vote_push();                    /* ★好帧才入表决窗（见 TRACE_VOTE_N 的说明）*/
        /* 好帧留一份快照，给「丢线时按住上一帧」用（trace_cal.h 那条规矩）。
         * 只存柱高：界面那个 0/1 位本来就是"柱高 >= 半高 128"，不另存一份、免得两处口径。 */
        for (i = 0; i < TRACE_CAL_CH; i++) s_hold_lvl[i] = s_fl_lvl[i];
        s_hold_ms    = HAL_GetTick();
        s_hold_valid = 1U;
    }

    trace_cal_frame_dbg.lvl_max   = (uint8_t)((lvl_max > 255) ? 255 : lvl_max);
    trace_cal_frame_dbg.noise     = (uint8_t)((noise > 255) ? 255 : noise);
    trace_cal_frame_dbg.whiteish  = whiteish;
    trace_cal_frame_dbg.state     = s_fl_state;
    trace_cal_frame_dbg.black_thr = (uint8_t)((black_thr > 255) ? 255 : black_thr);
    trace_cal_frame_dbg.white_ref = (uint8_t)((white_ref > 255) ? 255 : white_ref);
}

trace_frame_t trace_cal_frame(float w[TRACE_CAL_CH])
{
    uint8_t i;

    if (s_fl_state != TRACE_FRAME_OK) return (trace_frame_t)s_fl_state;

    /* 重心权重 = **越过标定阈值多少**（0 = 没越过；1 = 到标定黑值），用户 m31902：
     * 「这个循迹模块输出那几个值就可以了…如果非要细分,也可以根据它离阈值的距离来确定中间的位置」。
     * 白侧（黑度 ≤ 128，也就是没越过阈值）一律给 0 —— 这样整片路面变亮/变暗（标定漂了）
     * 只是让白路的黑度在 0..128 之间动，**不会**给它们一份非零权重把重心往中间拽
     * （那正是"明明偏了却直着走"的老毛病）。柱高（界面 OSD 那个 0..255）仍是对称的，
     * 128 = 正好压在阈值上，好让操作者看出"压了多少"。 */
    for (i = 0; i < TRACE_CAL_CH; i++) {
        w[i] = (s_fl_lvl[i] > 128U) ? ((float)(s_fl_lvl[i] - 128U) / 127.0f) : 0.0f;
    }
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

    /* 这一帧算过了：0/1 就是"本帧黑度过不过半高（= 标定阈值）"，和重心用的是同一份权重 */
    /* ★2026-10-13：好帧的 0/1 走 5 次多数表决的结果（s_vote_lvl），不再直接看本帧柱高。
     * 弱帧/丢线帧的语义一字未动（下面两行）。 */
    if (s_fl_state == TRACE_FRAME_OK)   return (s_vote_lvl[idx] != 0U) ? 1 : 0;
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
    /* 这一帧算过了：柱高 = 离标定阈值的距离（128 = 正好在阈值上），
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
    if (!trace_cal_valid) return 128;         /* 没标定（只有观察量程）：先画中点当临时分界 */
    if (s_bad[idx]) return -1;

    /* 这一帧算过了：判定线就是标定阈值，黑度在阈值处正好 128，画在半高 */
    if (s_fl_state == TRACE_FRAME_OK)   return 128;
    if (s_fl_state == TRACE_FRAME_WEAK) return -1;

    /* 落到这儿说明这帧根本没算（没模拟量之外的 FALLBACK）：映射是两点线性的，
     * 阈值一定落在柱高的一半（±1 的取整误差），直接算一遍而不是写死 128。 */
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
        /* ★2026-10-13（审计 #13）：这两条都属于"**采样根本没做成**"（读不到模拟量 / 连 8 帧
         *  都没采到）—— 一个采样点都没写进 s_blk/s_wht。旧标定必须原样保留，否则用户只是
         *  "进了标定页又退出来"，整套旧标定就被清掉、并在 ~1.5s 后被 nvram 固化进 Flash（掉电也丢）。 */
        trace_cal_valid = s_valid_before;
        s_state = TRACE_CAL_FAILED;
        return;
    }

    /* ★2026-10-15（审计 #16）：这里原来**一口气采完 32 帧** —— 注释按 I2C 估（≈20ms），
     *  但默认走的是**串口**：32 帧 ≈60~80ms，链路半死时最坏 ≈1.4s，全在**主循环里阻塞**
     *  （屏幕不刷、按键不响应；任务在跑时车还会带着最后一条指令继续动）。
     *  现在这里只**发起**采样，取帧交给 `trace_cal_poll()` 一轮一帧做完
     *  ⇒ 单次阻塞从"32 帧"降到"一帧"。
     *  失败分支（读失败且采到的帧不够 8）已随采样一起搬到 poll 里，口径不变
     *  （都属于"采样根本没做成"⇒ 保留进标定前那份旧标定，见审计 #13）。 */
    s_samp_k   = 0U;
    s_sampling = 1U;
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
    /* ★标定换了（切套/重标/开机从 Flash 恢复）：清空 5 次表决窗 ——
     *  旧的 0/1 是按旧阈值判的，留着会拿旧结论顶新标定的前两帧。 */
    {
        uint8_t vk;
        for (vk = 0U; vk < TRACE_CAL_CH; vk++) { s_vote_hist[vk] = 0U; s_vote_lvl[vk] = 0U; }
    }
    memcpy(s_pol, in->pol, sizeof(s_pol));
    memcpy(s_bad, in->bad, sizeof(s_bad));
    memcpy(s_blk, in->blk, sizeof(s_blk));
    memcpy(s_wht, in->wht, sizeof(s_wht));
    memcpy(s_thr, in->thr, sizeof(s_thr));
    memcpy(s_jit, in->jit, sizeof(s_jit));

    /* 换了标定（或者从 Flash 恢复了另一份）：几何缓存按新标定重算（用户 m07689：参数靠标定算）。
     * 自适应删掉后没有基线要起头；注意这也在 nvram 开机恢复时走一遍 —— 所以上电那一刻
     * 阈值就已经是这台车的数了。 */
    s_recalc_geom();
}

void trace_cal_set_adapt(uint8_t on)
{
    uint8_t v = (on != 0U) ? 1U : 0U;

    if (v == trace_adapt_enable) return;
    trace_adapt_enable = v;
    /* 自适应已经删了（用户 m31902）：这里只是"连续插值 / 纯 0/1"两种口径的切换，
     * 没有任何残留状态要清，下一帧 frame_compute_body 直接按新口径算。 */
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
    /* ★2026-10-15（审计 #16）：采样现在是跨轮做的 ⇒ 取消时必须把它停掉，
     *  否则退出向导后 poll 还会继续往 s_buf 里塞帧、甚至调 sample_finish()
     *  把 s_blk/s_wht 改掉（那时 s_state 已经不是 WAIT_*，会写错分支）。 */
    s_sampling = 0U;
    s_samp_k   = 0U;
}

trace_cal_state_t trace_cal_state(void)
{
    return s_state;
}
