/**
 * @file  trace_cal.h
 * @brief 8 路灰度模块：模拟量采集 + 黑白两点标定
 *
 * 模块接口（出处：官方 STM32 例程，已解包到 out/gray_sdk/8路数字量STM32程序/）：
 *
 *   IIC（System/IIC/iic.c + Hardware/LINE/line.c，从地址默认 0x01）
 *     0xA0 -> 数字量位图（1 字节，bit0 = 第 1 路）
 *     0xA1 -> 位置 + 偏移量（3 字节：位置, 偏移量 Hi, 偏移量 Lo；语义见下面串口那段）
 *     0xA2 -> 模拟量（8 路 × 2 字节，**大端：高字节在前**）
 *   ⚠ 官方《关于IIC通信注意事项》原文："IIC默认是数字量输出……如果想输出偏移量
 *     或者模拟量，需要通过串口修改为对应的输出模式"。所以上电先走串口广播
 *     0x4C + ID(0x00) + 0x01(输出模式) + 0x03(全部)，再去读 0xA2。
 *
 *   串口（System/USART/usart.c；本板走 UART4 = PC10/PC11，AF5，座子 H2 / U15）
 *     发 0x57 + 地址 -> 模块回一帧（帧头 0x75，帧尾区分内容）：
 *        3 字节 {0x75, 数字量, 0x02}
 *        5 字节 {0x75, 位置, BE16 偏移量, 0x04}
 *        18 字节 {0x75, 8×BE16 模拟量, 0x11}
 *        22 字节 {0x75, 数字量, 位置, 9×BE16(偏移量 + 8 路模拟量), 0x15}
 *     「位置」1 字节 = 状态字节：**低 5 位 = 压线通道数**（0 = 出线、8 = 8 路全压线；
 *      厂家例程拿 >= 4 判"路口"），bit5 = 偏左(0)/偏右(1)，bit6 = 左出线(0)/右出线(1)。
 *     「偏移量」16 位 = 横向偏差的**大小**（无符号），方向在「位置」的 bit5 里 ——
 *      厂家 track_PID1 直接用 error = ±偏移量 喂 P 环（P 取 0.01~0.1）。
 *      ⚠ 它和 line.c 里 track_PID2 的 H_SETPOINT=4500 不是一回事：4500 那套是
 *      "只取模拟量、自己在 MCU 里加权"（Σvᵢ·(i+1)/Σvᵢ×1000，权重 1..8）——
 *      它用的是**未归一化的原值**当权重（等于把"每路零点差"也算进权重里）。
 *      本固件取 8 路模拟量后按**自己的标定**归一化（见文件末「判线口径」）。
 *
 *   数字口（备用）：本板 PA0–PA7 = huidu1–8，只给 0/1。
 *
 * 本文件只做"采集 + 标定 + 归一化"，不碰电机；循迹环从 trace_read_channel() 取值。
 */
#ifndef TRACE_CAL_H
#define TRACE_CAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TRACE_CAL_CH       8U
#define TRACE_SENSOR_ADDR  0x01U   /* 模块 IIC 出厂地址（可用寄存器 0x10 / 串口改） */

/* 标定采样：按一下中键就**一口气连采**这么多帧（不再等 20ms 的采集周期），
 * 去掉最高/最低各 TRACE_CAL_TRIM 帧再平均（抗单帧毛刺、抗手抖）。
 * 16 字节 IIC 读 ≈ 0.6ms/帧 → 32 帧 ≈ 20ms 一个点（用户 m03536：
 * "每个点不用 640ms 吧感觉几十ms就够了"）。 */
#define TRACE_CAL_SAMPLE_N 32U
#define TRACE_CAL_TRIM      4U

/* IIC 寄存器 */
#define TRACE_REG_DIGITAL  0xA0U
#define TRACE_REG_POSITION 0xA1U
#define TRACE_REG_ANALOG   0xA2U

/* 串口设置命令：0x4C + ID + 寄存器 + 数据 */
#define TRACE_CMD_SOF      0x4CU
#define TRACE_REG_OUTPUT   0x01U
#define TRACE_OUT_ALL      0x03U   /* 00 数字量 / 01 偏移量 / 02 模拟量 / 03 全部 */

/* 数据通道（trace_cal_src）：串口不通自动退到 IIC，都不通才回落数字口 */
#define TRACE_SRC_NONE     0U      /* 用 PA0–PA7 数字口，只有 0/1 */
#define TRACE_SRC_I2C      1U      /* IIC 0xA2 模拟量（主用） */
#define TRACE_SRC_UART     2U      /* UART4 0x57 读取帧（不占 I2C 总线） */

/* 自动模式（TRACE_MODE_AUTO）先试哪条：**串口优先**，不通再退 IIC
 * （用户 m03944："这个灰度接口默认,比如说我开自动的话,它是优先串口,
 * 不要让它优先 i2c,之后才是最后是数字"）。用户也可以在设置菜单里把
 * 通讯方式定死成某一条（见 trace_cal_mode）。 */

/* 通讯方式（菜单「灰度接口」项 / 存 Flash）：
 *   AUTO    先试串口，连续失败 5 帧就换 IIC，两条都不通 -> 数字口；
 *           （换完两条还是不通会退避 2s 再从头试）
 *   I2C     只用 IIC 0x01/0xA2，不通就回落数字口，不偷偷换通道；
 *   UART    只用 UART4 0x57 读取帧，同上；
 *   DIGITAL 完全不碰模拟量，直接用 PA0–PA7 数字口。
 * 固定通道的价值：串口读一帧要几毫秒且要等模块应答，IIC 快得多；
 * 反过来模块只插在串口座上时，IIC 每次探测都要等超时。 */
#define TRACE_MODE_AUTO    0U
#define TRACE_MODE_I2C     1U
#define TRACE_MODE_UART    2U
#define TRACE_MODE_DIGITAL 3U
#define TRACE_MODE_COUNT   4U

/* 标定数据（存 Flash 用）：黑白原值 + 阈值 + 抖动 + 极性 + 有效性。
 * 界面/上位机不用直接碰，走 trace_cal_get_data()/set_data() 原样搬运。 */
typedef struct {
    uint8_t  valid;                       /* 1 = 这份标定做过黑白两点 */
    uint8_t  pol[TRACE_CAL_CH];           /* 1 = 黑比白原值大 */
    uint8_t  bad[TRACE_CAL_CH];           /* 1 = 这一路标定无效 */
    uint16_t blk[TRACE_CAL_CH];           /* 全黑时的原值 */
    uint16_t wht[TRACE_CAL_CH];           /* 全白时的原值 */
    uint16_t thr[TRACE_CAL_CH];           /* 0/1 分界（原值刻度） */
    uint16_t jit[TRACE_CAL_CH];           /* 采样抖动 */
} trace_cal_data_t;

/* 标定状态。没有"采样中"这一态：按下中键后 32 帧是在 wizard_press() 里
 * 一口气采完的（≈20ms），用户看到的只有"等我按黑" -> "等我按白" -> "完成"。 */
typedef enum {
    TRACE_CAL_IDLE = 0,      /* 没在标定 */
    TRACE_CAL_WAIT_BLACK,    /* 等用户把 8 路全按在黑上，按中键 */
    TRACE_CAL_WAIT_WHITE,    /* 等用户全按在白上，按中键 */
    TRACE_CAL_DONE,          /* 标定完成（1.5s 后自动回 IDLE） */
    TRACE_CAL_FAILED         /* 没有模拟量通路，标不了 */
} trace_cal_state_t;

extern uint16_t trace_cal_raw[TRACE_CAL_CH];  /* 最近一帧模拟量原值（未标定的绝对值） */
extern uint8_t  trace_cal_analog_ok;          /* 1 = 某条模拟量通路活着（IIC 或串口） */
extern uint8_t  trace_cal_valid;              /* 1 = 已完成黑白标定 */
extern uint8_t  trace_cal_src;                /* TRACE_SRC_xxx：**当前真的在用**哪条 */
extern uint8_t  trace_cal_mode;               /* TRACE_MODE_xxx：用户设的通讯方式 */

void trace_cal_init(void);            /* 上电调一次：切"输出全部" + 选通道 */
void trace_cal_poll(void);            /* 主循环调：20ms 一帧（IIC 时自己躲 OLED 传输） */
int  trace_cal_channel(uint8_t idx);  /* 1 = 压黑线 / 0 = 没压 / **-1 = 请回落数字口（只有用户选了"数字"模式）** */
uint8_t trace_cal_level(uint8_t idx); /* 0..255 柱高（255 = 压黑线）；模拟量断了先给"按住值"（见下） */
int16_t trace_cal_thr_level(uint8_t idx); /* 0/1 分界线的柱高（-1 = 这一路没有有效阈值） */
uint16_t trace_cal_threshold(uint8_t idx);/* 该路原始阈值（调试/上位机看） */

/* ==================== 丢线时"按住上一帧"（用户 m26712 第 3 条） ====================
 * 场景：车在跑，模拟量通路断了（I2C 被 OLED 抢/线松/模块掉电，AUTO 两条都不通 ⇒
 * trace_cal_analog_ok = 0），或者这一帧对比度不够（TRACE_FRAME_WEAK，真丢线）。
 * 老行为是 `trace_cal_level()` 回落到 PA0..PA7 数字口 —— 但这台车上那 8 根脚配的是
 * **上拉输入**（bsp_board.c 的 io.Pull = GPIO_PULLUP）且线没被驱动 ⇒ 8 路全读高。
 * 于是模拟量一断，整个屏立刻变成"8 个 1、柱子全满"，看着像卡住；更糟的是**任务块的
 * 「探头全白」判据也走同一个 trace_cal_level()** ⇒ 永远不成立 ⇒ 任务停在第一个
 * 等全白的块上再也不动（用户看到的就是"跑任务时屏幕暂停刷新、都显示一"）。
 *
 * 现在的规矩：
 *   ① 只有用户**显式**把「灰度接口」设成 TRACE_MODE_DIGITAL，才拿数字口当真值；
 *   ② 其余情况（模拟量断了 / 这帧 WEAK）判定上按"丢线"走（level = 0），
 *      **界面**则按住最后一帧好帧（柱高 + 0/1）最多 TRACE_HOLD_MS，并打「丢线」标记，
 *      超时才归零 —— 这样"丢线之后还保持着之前的状态"（用户原话），又不骗控制逻辑。
 * ③ 断链（analog_ok = 0）时的判定也按住 TRACE_HOLD_MS：链路抖一下不该让车当场
 *    跳到任务的"探头全白"分支上去，真断了才报丢线。 */
#define TRACE_HOLD_MS 500U  /* 按住上一帧好帧的上限（ms）；超时按丢线算（柱高 0） */

uint8_t trace_cal_disp_level(uint8_t idx); /* 0..255 界面柱高：本帧真值；丢线/断链时 = 按住值，超时 = 0 */
uint8_t trace_cal_disp_bit(uint8_t idx);   /* 界面底部那个 0/1：同上（阈值同 trace_cal_channel：半高 128） */
uint8_t trace_cal_held(void);              /* 1 = 界面现在显示的不是本帧真值（该在旁边打「丢线」） */

/* ==================== 判线口径（2026-10-05 用户 m31902 整改后） ====================
 * 用户原话：「你把这个"自适应"还是删除了吧，我感觉没有用…你还不如去优化一下之前说的那个
 * 阈值，可以把它设为一个区间之类的…如果非要细分,也可以根据它离阈值的距离来确定中间的位置」
 *
 * 所以自适应（每路白底基线 + 每帧整体偏置 ŝ + 中位数/jolt/最小对比度那一整套）已经
 * **整个删掉**。现在每帧只有一件事：**标定阈值 + 离阈值的距离**。
 *
 *   标定给的是每路的白值 / 黑值 / 阈值（= (白+黑)/2）和极性；
 *   每帧对每一路算 d = 原值 - 阈值（按极性取符号，d > 0 = 越过阈值 = 黑侧），然后
 *       黑度 = 128 ± (|d| / 半个跨度) × 127
 *   —— d = 0 时黑度正好 128（0/1 的判定线），|d| 涨到半个跨度（= 标定黑值/白值）时
 *   饱和到 255/0。也就是说"阈值"不是一根线而是一段区间：白侧 [白值, 阈值]、黑侧
 *   [阈值, 黑值]，中间的位置由**离阈值的距离**插值出来（用户要的"中间的位置"）。
 *
 *   判定：有一路越过阈值 ⇒ 本帧 TRACE_FRAME_OK；一路都没越过 ⇒ TRACE_FRAME_WEAK（丢线）。
 *   "探头底下整片一个样"（离地 / 全黑地面 / 整片脏）由**循迹层** PID_tracing.c 的
 *   TRACE_UNIFORM_MIN 判（本帧最亮/最黑之比在 0.75 以内 = 没结构 = 丢线），这一层不再自己判
 *   —— 两层分工明确，就不会再出现"车明明在线上、这一层先把自己判死"（用户 m31580/m31902）。
 *
 * 两个口径（设置菜单「连续判线」，变量 trace_adapt_enable）：
 *   1（默认）= 上面的连续插值，柱高能看出"压了多少"；
 *   0        = 纯 0/1：越过阈值就是 255、否则 0（老的中点阈值二值法）。
 *
 * 实时性（用户 m07689）：8 路"距离 -> 黑度"全用标定时算好的定点倒数（每帧只剩乘 + 移位），
 * 实测开销见 trace_frame_cycles / trace_frame_cycles_max（DWT 周期数，170MHz 下 /170 = µs）。
 * =============================================================================== */

typedef enum {
    TRACE_FRAME_FALLBACK = 0,  /* 没标定 / 没有模拟量：走老的固定阈值 + 按住逻辑 */
    TRACE_FRAME_OK       = 1,  /* 本帧可用：给 8 路权重 */
    TRACE_FRAME_WEAK     = 2   /* 一路都没越过标定阈值：当丢线（沿用上一帧重心） */
} trace_frame_t;

extern uint8_t trace_adapt_enable;   /* 1 = 连续（默认）；0 = 纯 0/1 */

/* 设置菜单「连续判线」用它切换（app_ui 每拍只在变化时调一次）*/
void trace_cal_set_adapt(uint8_t on);

/* 界面上那几个数就从这里取（draw_trace_verify 用）。
 * 自适应删掉（用户 m31902）后 shift/shift_raw/jolt/ped 恒为 0 —— 字段保留，
 * 界面与 J-Link 的布局/地址都不动。 */
typedef struct {
    int16_t shift;      /* 恒 0（原：本帧用的整体偏置 ŝ） */
    int16_t shift_raw;  /* 恒 0（原：本帧观测到的中位数） */
    uint8_t lvl_max;    /* 本帧最黑那一路的黑度 0..255（= 还剩多少对比度） */
    uint8_t noise;      /* 有效路里抖动折算成黑度的最大值：信噪比 = lvl_max/noise */
    uint8_t whiteish;   /* 本帧"没越过阈值"的路数（8 = 一路都没压线） */
    uint8_t jolt;       /* 恒 0（原：1 = 本帧被判扰动） */
    uint8_t state;      /* trace_frame_t */
    uint8_t ped;        /* 恒 0（原：本帧被减掉的零点） */
    uint8_t black_thr;  /* 本帧有几路越过了**标定阈值**（= 真黑）；0 = 这帧没有线 */
    uint8_t white_ref;  /* 本帧有几路还稳稳读在**标定白值**上（带容差）；只看调试 */
} trace_cal_frame_dbg_t;
extern trace_cal_frame_dbg_t trace_cal_frame_dbg;

/* 标定推出来的几何量（用户 m07689：参数靠标定算）。
 * 标定一变（向导收尾 / Flash 恢复）就重算一次；只读调试口，out/_base_read.jlink 会 dump。 */
typedef struct {
    int32_t sp_med;   /* 有效路跨度的中位数（原值计数，0 = 没有有效路） */
    int32_t jit_med;  /* 有效路标定抖动（中间 24 帧 max-min）的中位数 */
    int32_t rq_shift; /* 定点倒数用的移位：黑度 = (距离 × (255<<rq_shift)/跨度) >> rq_shift */
} trace_geom_t;
extern trace_geom_t trace_geom;

/* frame_compute() 的实测开销（DWT 周期数；170MHz 下 /170 = µs）。只增不减的是峰值。 */
extern uint32_t trace_frame_cycles;
extern uint32_t trace_frame_cycles_max;

/* 取本帧 8 路重心权重（0..1）= Σwᵢxᵢ/Σwᵢ 里的 wᵢ。
 * **权重 = 越过标定阈值多少**：0 = 没越过阈值（白侧一律 0，所以整片变亮/变暗不会把重心
 * 往中间拽），1 = 到了标定黑值；0.5 = 越过半个跨度。柱子本身仍是对称的 0..255
 * （128 = 正好压在阈值上，见 trace_cal_disp_level）。
 * 值是 trace_cal_poll() 每 20ms 算好缓存的，和 trace_cal_raw[] 同一时刻，不额外占时间。 */
trace_frame_t trace_cal_frame(float w[TRACE_CAL_CH]);

/* 标定质量（界面/调试用） */
uint8_t  trace_cal_ch_ok(uint8_t idx);      /* 1 = 这一路标定有效（跨度够、采样不抖） */
int32_t  trace_cal_span(uint8_t idx);       /* 黑原值 - 白原值（可负，幅度 = 对比度） */
uint16_t trace_cal_jitter(uint8_t idx);     /* 两点里较差的那个采样抖动幅度 */

/* 「这份标定能不能信」——逐路原因码 + 整体有效路数（用户 m05894：标定流程要能检验）。
 * 按下标定键不再是"按键就一定收下"：单点抖动超 TRACE_POINT_JIT_MAX 就整点作废、
 * 状态留在原档等重按；两点都收下后每路自己给一个原因码，界面按 '!' 或质量分数显示。 */
#define TRACE_BAD_SPAN      1U   /* 这一路黑白跨度太小：这一点压根没跨过黑线 */
#define TRACE_BAD_JITTER    2U   /* 这一路采样在抖（抖动与跨度同量级）：阈值不可信 */
#define TRACE_BAD_NOSAMPLE  3U   /* 这一路没标定 / 没有模拟量 */
#define TRACE_QUALITY_MIN   6U   /* 8 路里至少这么好才算"能拿去循迹"（少 2 路还能跑） */
#define TRACE_POINT_JIT_MAX 80U  /* 单点采样抖动上限（原值刻度）：超了请用户重按 */

uint8_t  trace_cal_bad_reason(uint8_t idx); /* 0 = 好；否则 TRACE_BAD_xxx */
uint8_t  trace_cal_quality(void);           /* 有效路数 0..8（未标定 = 0） */
uint16_t trace_cal_min_span(void);          /* 有效路里最小的对比度（0 = 没有有效路） */
uint8_t  trace_cal_retry_hint(void);        /* 1 = 刚那一点被抖动判掉，提示"重按" */

void trace_cal_wizard_start(void);
void trace_cal_wizard_press(void);
void trace_cal_wizard_cancel(void);
trace_cal_state_t trace_cal_state(void);

void trace_cal_request_analog_mode(void);  /* 走 UART4 发 0x4C 00 01 03（广播：输出全部） */

/* 通讯方式切换（设置页改完调一次；同值调用无副作用） */
void trace_cal_set_mode(uint8_t mode);
/* 标定数据搬运（Flash 持久化：nvram.c 开机 restore、改完 get 出来存） */
void trace_cal_get_data(trace_cal_data_t *out);
void trace_cal_set_data(const trace_cal_data_t *in);

/* ---- 两套标定（用户 m07888：普通路面和亚克力路段各标一套，跑任务时切换）----
 * 0 = 普通，1 = 亚克力。存 Flash 用 trace_cal_export()/import()：
 *   export() 永远把第 0 套放进 *set0、第 1 套放进 *set1（和当前用哪套无关），
 *   所以记录里 cal=第0套、cal_b=第1套，切换不会把某套弄丢；
 *   import() 反着来，最后把 active 那套设为当前。
 * 切换（含 import）会把白底基线/公共偏移清零重新起头：两套白值不同，老基线不能接着用。 */
#define TRACE_CAL_SET_COUNT  2U
uint8_t trace_cal_active_set(void);
void    trace_cal_select_set(uint8_t set);
void    trace_cal_export(trace_cal_data_t *set0, trace_cal_data_t *set1, uint8_t *active);
void    trace_cal_import(const trace_cal_data_t *set0, const trace_cal_data_t *set1,
                         uint8_t active);

#ifdef __cplusplus
}
#endif
#endif /* TRACE_CAL_H */
