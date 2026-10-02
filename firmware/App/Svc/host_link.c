/**
 * @file  host_link.c
 * @brief 上位机通信链路：帧编解码 + 参数表 + 命令分发 + 遥测打包。
 *
 * 为什么不用文本协议：50Hz × 32 字节遥测在 115200 下只占 17%，
 * 而文本要 3~5 倍带宽、丢一个字节就整行错乱。详见 docs/上位机通信协议.md。
 *
 * 数据来源（都是**真实读数**，不编）：
 *   yaw/pitch/roll   <- jy61p.c（USART3 收的 JY61P）
 *   pwm_l/pwm_r      <- TIM8 CH1..CH4 的 CCR（AT8236 两根输入的实际占空比）
 *   servo1/servo2    <- TIM1 CH1/CH2 的 CCR（实际脉宽 us）
 *   vbat/ibat        <- INA226（只在 OLED 不占 I2C1 时读，避免抢总线）
 *   line_mask        <- 8 路灰度原始电平（1 = 压线）
 *   state_id         <- renwu.c 的 black_num
 */

#include "host_link.h"

#include <string.h>

#include "main.h"
#include "board.h"
#include "bsp_board.h"
#include "jy61p.h"
#include "kk_oled.h"
#include "tracing.h"
#include "SEGGER_RTT.h"

/* 实际 PWM / 舵机脉宽要从定时器比较寄存器读 */
extern TIM_HandleTypeDef htim1;
extern TIM_HandleTypeDef htim8;
extern uint8_t black_num;              /* renwu.c：任务步号 */
extern uint8_t renwu_flag;             /* renwu.c：任务选择（0 停 / 1 A / 2 B / 3 C） */

/* =====================================================================
 *  CRC16/CCITT-FALSE  (poly 0x1021, init 0xFFFF, 不反转)
 *  标准校验值："123456789" -> 0x29B1（上位机单测用同一个值钉住）
 * ===================================================================== */
static uint16_t crc16_ccitt(const uint8_t *d, uint16_t n)
{
  uint16_t crc = 0xFFFFu;
  uint8_t  b;
  while (n--) {
    crc ^= (uint16_t)(*d++) << 8;
    for (b = 0; b < 8u; b++) {
      crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                            : (uint16_t)(crc << 1);
    }
  }
  return crc;
}

/* =====================================================================
 *  参数表
 * ===================================================================== */
typedef struct {
  uint16_t id;
  float    def;
  float    min;
  float    max;
} param_spec_t;

static const param_spec_t SPECS[] = {
  /* 循迹/角度环增益 = 旧板硬编码值 ×7.2（旧板 PWM 满量程 1000 → 本板 7200；这两个环的输出
   * 是直接加在左右轮上的 PWM 偏置，所以照抄旧值等效只有 1/7.2 力度）。积分限幅不折算
   * （量纲是误差本身）。见 app_ui.c 的 g_cfg 默认值注释（2026-09-30 m02112 同步）。 */
  { HP_LINE_KP,          2700.0f,     0.0f,     5000.0f },
  { HP_LINE_KI,            10.8f,     0.0f,      200.0f },
  { HP_LINE_KD,           360.0f,     0.0f,     1000.0f },
  { HP_LINE_ILIMIT,        50.0f,     0.0f,    10000.0f },
  /* 循迹基准速度：默认与 app_ui.c 的 g_cfg.trace_speed 默认值一致（6480 = 旧 900 刻度），
   * 开机时 app_ui 会把菜单里的值 push 进来覆盖它。
   * 慢速档 2500 = 满量程 35%，是静摩擦起步死区（~2500~2900）的下沿；旧板写的 200
   * 是旧刻度（旧板满量程 1000），×7.2 只有 1440 —— 仍在死区以下，所以这里**故意不折算**，
   * 保持 2500（m02112 同步里的例外，理由=硬件死区）。 */
  { HP_BASE_SPEED,       6480.0f,     0.0f,     7200.0f },
  { HP_BASE_SPEED_SLOW,  2500.0f,     0.0f,     7200.0f },
  { HP_TASK_SPEED,       6480.0f,     0.0f,     7200.0f },
  /* 速度环 PI 增益 */
  { HP_SPEED_KP,           20.0f,     0.0f,     2000.0f },
  { HP_SPEED_KI,            2.0f,     0.0f,      200.0f },
  { HP_SPEED_KD,            0.0f,     0.0f,      500.0f },
  { HP_SPEED_ILIMIT,     3000.0f,     0.0f,    20000.0f },
  /* 速度环前馈（台架实测拟合，硬件标定量；默认值来自 host_link.h 的宏） */
  { HP_SPEED_FF_SLOPE, SPEED_FF_SLOPE, 0.0f,       2.0f },
  { HP_SPEED_FF_FRIC,  SPEED_FF_FRIC,  0.0f,    5000.0f },
  /* 角度环增益：**不按 ×7.2**。2026-10-01 原地台架实测（只改 RAM，见
   * out/_angletest.ps1）：由旧刻度换算来的 720/72 在本板必自激（Δ=3°..20° 全振，
   * 尾晃 20~38°、峰值 400~600°/s）；150/15 稳定（3° 阶跃误差 0.6°、20° 过冲 4~10°、
   * 90° 分六次增量转累计误差 1.1°）。改这里要同步 app_ui.c 的 g_cfg 默认值
   * 与开机迁移判断（CFG_GYRO_*_OLD_X100）。 */
  { HP_GYRO_KP,           150.0f,     0.0f,     2000.0f },   /* 实测选定（旧 720 自激） */
  { HP_GYRO_KI,             0.0f,     0.0f,      200.0f },   /* 默认 0：积分被静摩擦死区"喂"成常驻推力，车会停在停车带外一直抽 */
  { HP_GYRO_KD,            15.0f,     0.0f,      500.0f },   /* ≈Kp/10，压过冲 */
  { HP_GYRO_ILIMIT,      1000.0f,     0.0f,    20000.0f },
  { HP_TURN_ANGLE_OFFSET,  90.0f,     1.0f,      180.0f },
  { HP_TURN_TOLERANCE,      2.0f,     0.1f,       20.0f },
  { HP_TURN_TIMEOUT,     1500.0f,   100.0f,    10000.0f },
  { HP_YAW_RATE_DAMP,       1.0f,     0.0f,       20.0f },
  { HP_MARKER_HOLD,         3.0f,     1.0f,       50.0f },
  { HP_MARKER_MIN_GAP,     50.0f,     0.0f,     5000.0f },
  { HP_COUNTS_PER_REV,   1980.0f,     1.0f,   100000.0f },
  { HP_WHEEL_DIA,          65.0f,    10.0f,      300.0f },
  { HP_WHEEL_BASE,        150.0f,    10.0f,      500.0f },
  { HP_PWM_MAX,          7200.0f,   100.0f,     7200.0f },
  { HP_VBAT_NOMINAL,    12000.0f,  3000.0f,    30000.0f },
  { HP_STALL_CURRENT,    2500.0f,   100.0f,    20000.0f },
  { HP_VBAT_LOW,        10500.0f,  3000.0f,    30000.0f },
};
#define NSPEC (sizeof(SPECS) / sizeof(SPECS[0]))

static float s_param[NSPEC];
static host_link_stats_t s_stats;

/* 全表参数 dump 的分帧发送状态（见 handle_frame 的 T_PARAM_GET）：
 * 一次 dump 有 NSPEC 帧，不一次性发完，由 host_link_poll 每拍发 DUMP_PER_POLL 帧。 */
static uint16_t s_dump_i;
static uint8_t  s_dump_seq;
static uint8_t  s_dump_busy;
#define DUMP_PER_POLL 4u

static int spec_index(uint16_t id)
{
  uint16_t i;
  for (i = 0; i < NSPEC; i++) {
    if (SPECS[i].id == id) return (int)i;
  }
  return -1;
}

static uint32_t param_table_crc(void)
{
  /* 与上位机 protocol.py 的 param_table_crc 同构：把所有 (id, def) 打包后算 CRC */
  uint8_t buf[NSPEC * 6];
  uint16_t n = 0, i;
  for (i = 0; i < NSPEC; i++) {
    uint32_t bits;
    buf[n++] = (uint8_t)(SPECS[i].id & 0xFFu);
    buf[n++] = (uint8_t)(SPECS[i].id >> 8);
    memcpy(&bits, &SPECS[i].def, 4);
    buf[n++] = (uint8_t)(bits & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 8) & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 16) & 0xFFu);
    buf[n++] = (uint8_t)((bits >> 24) & 0xFFu);
  }
  return (uint32_t)crc16_ccitt(buf, n);
}

/* 参数值必须是有限数：NaN 会让范围检查失效（NaN 与任何数比较都是 false），
 * 一旦混进 Kp/Ki 就一直传到 (int) 强转 —— 那是未定义行为，输出会变成野值。
 * |v| > 1e9 把 ±Inf 也挡掉（Inf != Inf 不成立，只能靠范围判）。 */
static int param_finite(float v)
{
  return (v == v) && (v >= -1.0e9f) && (v <= 1.0e9f);
}

float host_param_get(uint16_t id, float fallback)
{
  int i = spec_index(id);
  if (i < 0) return fallback;
  return param_finite(s_param[i]) ? s_param[i] : fallback;
}

uint8_t host_param_u8(uint16_t id, uint8_t fallback)
{
  float v = host_param_get(id, (float)fallback);
  if (v < 0.0f) v = 0.0f;
  if (v > 255.0f) v = 255.0f;
  return (uint8_t)(v + 0.5f);
}

uint32_t host_param_u32(uint16_t id, uint32_t fallback)
{
  float v = host_param_get(id, (float)fallback);
  return (v <= 0.0f) ? 0u : (uint32_t)(v + 0.5f);
}

void host_param_set_local(uint16_t id, float v)
{
  int i = spec_index(id);
  if (i >= 0 && param_finite(v)) s_param[i] = v;   /* 非有限值直接丢掉，不污染参数表 */
}

/* =====================================================================
 *  帧的收发
 * ===================================================================== */
#define SOF0 0xA5u
#define SOF1 0x5Au
#define MAX_PAYLOAD 128u

enum { T_TELEM = 0x01, T_PARAM_GET = 0x10, T_PARAM_SET = 0x11, T_PARAM_REPORT = 0x12,
       T_PARAM_SAVE = 0x13, T_CMD = 0x20, T_ACK = 0x21, T_HELLO = 0x40,
       T_HELLO_REQ = 0x41, T_TEXT = 0x50 };

enum { ST_OK = 0x00, ST_UNKNOWN_TYPE = 0x02, ST_BAD_PARAM = 0x03,
       ST_OUT_OF_RANGE = 0x04, ST_NOT_READY = 0x05, ST_BAD_CMD = 0x06 };

enum { CMD_STOP = 0x01, CMD_START_A = 0x02, CMD_START_B = 0x03, CMD_START_C = 0x04,
       CMD_CALIB_GYRO = 0x05, CMD_SAVE_PARAMS = 0x06, CMD_CLEAR_FAULT = 0x07,
       CMD_SET_TELEM_HZ = 0x08, CMD_RESET = 0x09, CMD_SET_LOGGING = 0x0A };

#define TELEM_LEN 32u

static uint8_t tx_buf[7u + MAX_PAYLOAD];

static void frame_send(uint8_t type, uint8_t seq, const uint8_t *payload, uint8_t len)
{
  uint16_t crc;
  if (len > MAX_PAYLOAD) return;
  tx_buf[0] = SOF0;
  tx_buf[1] = SOF1;
  tx_buf[2] = type;
  tx_buf[3] = seq;
  tx_buf[4] = len;
  if (len && payload) memcpy(&tx_buf[5], payload, len);
  crc = crc16_ccitt(&tx_buf[2], (uint16_t)(3u + len));
  tx_buf[5u + len] = (uint8_t)(crc & 0xFFu);
  tx_buf[6u + len] = (uint8_t)(crc >> 8);
  /* 主循环里阻塞发一帧：50Hz 下 39 字节 ≈ 3.4ms @115200，可接受。
     将来要提到 200Hz 再改 DMA / 环形发送。 */
  (void)board_dl_send(tx_buf, (uint16_t)(7u + len));
  s_stats.tx_frames++;
}

void host_link_send_text(const char *s)
{
  size_t n = strlen(s);
  if (n > MAX_PAYLOAD) n = MAX_PAYLOAD;
  frame_send(T_TEXT, 0, (const uint8_t *)s, (uint8_t)n);
}

static void send_ack(uint8_t seq, uint8_t status)
{
  uint8_t p[2];
  p[0] = seq;
  p[1] = status;
  frame_send(T_ACK, seq, p, 2);
}

static void send_param_report(uint8_t seq, uint16_t id, float v)
{
  uint8_t p[6];
  uint32_t bits;
  p[0] = (uint8_t)(id & 0xFFu);
  p[1] = (uint8_t)(id >> 8);
  memcpy(&bits, &v, 4);
  p[2] = (uint8_t)(bits & 0xFFu);
  p[3] = (uint8_t)((bits >> 8) & 0xFFu);
  p[4] = (uint8_t)((bits >> 16) & 0xFFu);
  p[5] = (uint8_t)((bits >> 24) & 0xFFu);
  frame_send(T_PARAM_REPORT, seq, p, 6);
}

static void send_hello(uint8_t seq)
{
  uint8_t p[20];
  const char *name = "quandi-car";
  uint32_t crc = param_table_crc();
  uint16_t hz = s_stats.telem_hz;
  memset(p, 0, sizeof(p));
  p[0] = 0x01; p[1] = 0x00;              /* fw version 0x0001 */
  p[2] = 0x01;                           /* proto version */
  p[3] = 0x00;                           /* reserved */
  p[4] = (uint8_t)(crc & 0xFFu);
  p[5] = (uint8_t)(crc >> 8);
  p[6] = (uint8_t)(hz & 0xFFu);
  p[7] = (uint8_t)(hz >> 8);
  memcpy(&p[8], name, strlen(name) > 12u ? 12u : strlen(name));
  frame_send(T_HELLO, seq, p, 20);
}

/* =====================================================================
 *  遥测：把车上的真实读数打包
 * ===================================================================== */
static int16_t clamp_i16(float v)
{
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)v;
}

/* TIM8 CH1/CH2 = 电机A 两根输入，CH3/CH4 = 电机B。正速度走 IN1，负速度走 IN2。 */
static int16_t motor_pwm(uint32_t ch_fwd, uint32_t ch_rev)
{
  uint32_t a = __HAL_TIM_GET_COMPARE(&htim8, ch_fwd);
  uint32_t b = __HAL_TIM_GET_COMPARE(&htim8, ch_rev);
  if (a >= b) return (int16_t)(a > 32767u ? 32767u : a);
  return (int16_t)(-(int32_t)(b > 32767u ? 32767u : b));
}

static uint8_t line_mask(void)
{
  uint8_t m = 0, i;
  for (i = 0; i < 8u; i++) {
    if (bsp_trace_bit(i) == GPIO_PIN_SET) m |= (uint8_t)(1u << i);   /* 1 = 压线 */
  }
  return m;
}

static void put_u16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put_u32(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_i16(uint8_t *p, int16_t v) { put_u16(p, (uint16_t)v); }

static void send_telemetry(uint32_t loop_us)
{
  uint8_t p[TELEM_LEN];
  static int32_t vbus_mv, ibat_ma;
  static uint32_t last_ina;
  uint8_t  lm = line_mask();
  uint8_t  flags = 0;
  uint16_t v1, v2;
  uint32_t now = HAL_GetTick();

  /* INA226 也在 I2C1 上，而 OLED 走异步中断/DMA 刷屏 —— 只在屏不忙时读，
     免得抢总线把显示刷挂。100ms 读一次就够。 */
  if (!OLED_IsBusy() && (now - last_ina >= 100u)) {
    int32_t t;
    last_ina = now;
    if (ina226_read_bus_voltage_mv(&t) == HAL_OK) vbus_mv = t;
    if (ina226_read_current_ma(&t) == HAL_OK)     ibat_ma = t;
  }

  if (lm == 0u) flags |= 0x02u;                       /* 循迹丢线 */
  if (Pitch > 8.0f || Pitch < -8.0f) flags |= 0x08u;  /* 坡道（用 pitch 粗判） */
  /* TODO(编码器/陀螺在线检测): bit0 陀螺有效、bit5 车上记录 —— 等编码器与
     黑匣子做完再补，现在不谎报。 */

  memset(p, 0, sizeof(p));
  put_u32(&p[0], now);
  put_i16(&p[4], clamp_i16(Yaw * 10.0f));
  put_i16(&p[6], clamp_i16(Pitch * 10.0f));
  put_i16(&p[8], clamp_i16(Roll * 10.0f));
  /* v_l/v_r 暂为 0：编码器还没做（硬件在 PB4..PB7，固件待写）。
     不拿 PWM 假装速度 —— 那会让上位机的波形骗人。 */
  put_i16(&p[10], 0);
  put_i16(&p[12], 0);
  put_i16(&p[14], motor_pwm(TIM_CHANNEL_1, TIM_CHANNEL_2));
  put_i16(&p[16], motor_pwm(TIM_CHANNEL_3, TIM_CHANNEL_4));
  put_u16(&p[18], (uint16_t)(vbus_mv < 0 ? 0 : (vbus_mv > 65535 ? 65535 : vbus_mv)));
  put_i16(&p[20], (int16_t)(ibat_ma > 32767 ? 32767 : (ibat_ma < -32768 ? -32768 : ibat_ma)));
  v1 = (uint16_t)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_1);
  v2 = (uint16_t)__HAL_TIM_GET_COMPARE(&htim1, TIM_CHANNEL_2);
  put_u16(&p[22], v1);
  put_u16(&p[24], v2);
  p[26] = lm;
  p[27] = black_num;                                  /* 任务步号 */
  put_u16(&p[28], (uint16_t)(loop_us > 65535u ? 65535u : loop_us));
  p[30] = flags;
  p[31] = 0;
  frame_send(T_TELEM, 0, p, TELEM_LEN);
}

/* =====================================================================
 *  收帧状态机（CRC 错只前移 1 字节重同步，上电乱码能自愈）
 * ===================================================================== */
static uint8_t  rx_buf[5u + MAX_PAYLOAD + 2u];
static uint16_t rx_len;
static uint8_t  rx_expect;

static void handle_frame(uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len);

static void rx_reset(void)
{
  rx_len = 0;
  rx_expect = 2;      /* 先要 SOF0 SOF1 */
}

static void rx_byte(uint8_t b)
{
  if (rx_len < 2u) {
    if (rx_len == 0u) {
      if (b == SOF0) rx_buf[rx_len++] = b;
      else s_stats.resyncs++;
    } else {
      if (b == SOF1) rx_buf[rx_len++] = b;
      else { s_stats.resyncs++; rx_len = (b == SOF0) ? 1u : 0u; if (rx_len) rx_buf[0] = SOF0; }
    }
    if (rx_len == 2u) rx_expect = 5u;   /* 再收 type/seq/len */
    return;
  }
  if (rx_len < 5u) {
    rx_buf[rx_len++] = b;
    if (rx_len == 5u) {
      uint8_t plen = rx_buf[4];
      if (plen > MAX_PAYLOAD) {         /* 长度不合理 -> 丢掉这个假头 */
        s_stats.resyncs++;
        rx_len = (b == SOF0) ? 1u : 0u;
        if (rx_len) rx_buf[0] = SOF0;
        rx_expect = 2u;
        return;
      }
      rx_expect = (uint8_t)(5u + plen + 2u);
    }
    return;
  }
  rx_buf[rx_len++] = b;
  if (rx_len >= rx_expect) {
    uint8_t plen = rx_buf[4];
    uint16_t crc_rx = (uint16_t)rx_buf[5u + plen] | ((uint16_t)rx_buf[6u + plen] << 8);
    uint16_t crc_calc = crc16_ccitt(&rx_buf[2], (uint16_t)(3u + plen));
    if (crc_rx == crc_calc) {
      s_stats.rx_frames++;
      handle_frame(rx_buf[2], rx_buf[3], &rx_buf[5], plen);
    } else {
      s_stats.crc_errors++;
    }
    rx_reset();
  }
}

static void handle_frame(uint8_t type, uint8_t seq, const uint8_t *p, uint8_t len)
{
  switch (type) {
  case T_HELLO_REQ:
    send_hello(seq);
    break;

  case T_PARAM_GET: {
    uint16_t id;
    if (len < 2u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    if (id == 0xFFFFu) {
      /* 全表 dump 不在这里连发：原来循环 31 次 send_param_report，每帧
       * board_dl_send 都是同步阻塞的（115200 下一帧 ≈1.1ms）⇒ 主循环停约 35ms，
       * 期间 UI 不刷、遥测不发。改成挂个"待发"标记，由 host_link_poll 每拍发几帧。 */
      s_dump_seq  = seq;
      s_dump_i    = 0;
      s_dump_busy = 1u;
    } else {
      int i = spec_index(id);
      if (i < 0) send_ack(seq, ST_BAD_PARAM);
      else send_param_report(seq, id, s_param[i]);
    }
    break;
  }

  case T_PARAM_SET: {
    uint16_t id;
    uint32_t bits;
    float v;
    int i;
    if (len < 6u) { send_ack(seq, ST_BAD_PARAM); break; }
    id = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    bits = (uint32_t)p[2] | ((uint32_t)p[3] << 8) | ((uint32_t)p[4] << 16)
         | ((uint32_t)p[5] << 24);
    memcpy(&v, &bits, 4);
    i = spec_index(id);
    if (i < 0) { send_ack(seq, ST_BAD_PARAM); break; }
    /* 用取反形式：NaN 与任何数比较都是 false，写成 !(v >= min && v <= max) 才能
     * 把 NaN/±Inf 挡住（原来的 v < min || v > max 会让 NaN 通过范围检查）。 */
    if (!(v >= SPECS[i].min && v <= SPECS[i].max)) { send_ack(seq, ST_OUT_OF_RANGE); break; }
    s_param[i] = v;
    s_stats.param_sets++;
    send_ack(seq, ST_OK);
    send_param_report(seq, id, v);       /* 回读，上位机立刻看到生效值 */
    break;
  }

  case T_PARAM_SAVE:
    /* TODO(掉电保存): 需要占用最后一个 Flash 页做参数区。先明确回"未就绪"，
       不假装写成功 —— 否则上位机以为存住了。 */
    send_ack(seq, ST_NOT_READY);
    host_link_send_text("[fw] param save to flash: not implemented yet\r\n");
    break;

  case T_CMD: {
    uint8_t cmd = (len >= 1u) ? p[0] : 0u;
    int32_t arg = 0;
    if (len >= 5u) {
      arg = (int32_t)((uint32_t)p[1] | ((uint32_t)p[2] << 8)
                    | ((uint32_t)p[3] << 16) | ((uint32_t)p[4] << 24));
    }
    switch (cmd) {
    case CMD_STOP:        renwu_flag = 0u; send_ack(seq, ST_OK); break;
    case CMD_START_A:     renwu_flag = 1u; send_ack(seq, ST_OK); break;
    case CMD_START_B:     renwu_flag = 2u; send_ack(seq, ST_OK); break;
    case CMD_START_C:     renwu_flag = 3u; send_ack(seq, ST_OK); break;
    case CMD_CLEAR_FAULT: send_ack(seq, ST_OK); break;
    case CMD_SET_TELEM_HZ:
      if (arg < 1) arg = 1;
      if (arg > 200) arg = 200;
      s_stats.telem_hz = (uint16_t)arg;
      send_ack(seq, ST_OK);
      break;
    case CMD_RESET:       send_ack(seq, ST_OK); HAL_Delay(5); NVIC_SystemReset(); break;
    /* 下面几个还没实现，明确回"未就绪"，不假装成功 */
    case CMD_CALIB_GYRO:
    case CMD_SAVE_PARAMS:
    case CMD_SET_LOGGING:
    default:
      send_ack(seq, (cmd == CMD_CALIB_GYRO || cmd == CMD_SAVE_PARAMS
                     || cmd == CMD_SET_LOGGING) ? ST_NOT_READY : ST_BAD_CMD);
      break;
    }
    break;
  }

  default:
    send_ack(seq, ST_UNKNOWN_TYPE);
    break;
  }
}

/* =====================================================================
 *  公共接口
 * ===================================================================== */
void host_link_init(void)
{
  uint16_t i;
  for (i = 0; i < NSPEC; i++) s_param[i] = SPECS[i].def;
  memset(&s_stats, 0, sizeof(s_stats));
  s_stats.telem_hz = 50u;

  rx_reset();
  (void)board_dl_init();
  board_dl_start_rx();
  (void)ina226_init();
}

void host_link_poll(uint32_t loop_us)
{
  uint8_t b;
  static uint32_t next_tx;
  uint32_t now = HAL_GetTick();

  while (board_dl_get_byte(&b)) rx_byte(b);

  if (s_stats.telem_hz && (int32_t)(now - next_tx) >= 0) {
    next_tx = now + (1000u / s_stats.telem_hz);
    send_telemetry(loop_us);
  }

  /* 全表参数 dump 的剩余部分：每拍发几帧，避免一次性阻塞主循环几十毫秒 */
  if (s_dump_busy) {
    uint8_t n = 0u;
    while (s_dump_busy && n < DUMP_PER_POLL) {
      send_param_report(s_dump_seq, SPECS[s_dump_i].id, s_param[s_dump_i]);
      s_dump_i++;
      n++;
      if (s_dump_i >= NSPEC) s_dump_busy = 0u;
    }
  }
}

void host_link_tick_1hz(void)
{
  static uint32_t last;
  uint32_t now = HAL_GetTick();
  if (now - last < 1000u) return;
  last = now;
  SEGGER_RTT_printf(0, "HOST: rx=%lu tx=%lu crc=%lu resync=%lu sets=%lu hz=%u\r\n",
                    (unsigned long)s_stats.rx_frames, (unsigned long)s_stats.tx_frames,
                    (unsigned long)s_stats.crc_errors, (unsigned long)s_stats.resyncs,
                    (unsigned long)s_stats.param_sets, (unsigned)s_stats.telem_hz);
}

const host_link_stats_t *host_link_stats(void) { return &s_stats; }
