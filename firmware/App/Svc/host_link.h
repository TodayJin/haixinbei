/**
 * @file  host_link.h
 * @brief 上位机通信链路（LPUART1 <-> 无线下载器）：协议编解码 + 参数表 + 命令分发。
 *
 * 协议全文见 docs/上位机通信协议.md（帧 = A5 5A | type | seq | len | payload | CRC16）。
 */
#ifndef __HOST_LINK_H__
#define __HOST_LINK_H__

#include <stdint.h>

/* ---------------- 参数 id（与上位机 protocol.py 的 PARAMS 表逐项对应） ---------- */
#define HP_LINE_KP           0x0201u
#define HP_LINE_KI           0x0202u
#define HP_LINE_KD           0x0203u
#define HP_LINE_ILIMIT       0x0204u
#define HP_BASE_SPEED        0x0301u
#define HP_BASE_SPEED_SLOW   0x0302u
/* 比赛任务流程（renwu.c）的直行基准速度：renwu.c 从旧板工程照抄来，里面的 900/920/935
 * 是**旧板刻度**（旧板 PWM 满量程 1000），本板满量程 7200。这个参数 = “旧板 900”对应的
 * 本板 PWM，默认 6480（90%）。改它就能整体缩放比赛流程的速度而不动流程内部比例。 */
#define HP_TASK_SPEED        0x0304u
/* 速度环（目标转速 -> PWM）。环已在 speed_control.c 实现，这四项是 PI 增益。 */
#define HP_SPEED_KP          0x0601u
#define HP_SPEED_KI          0x0602u
#define HP_SPEED_KD          0x0603u
#define HP_SPEED_ILIMIT      0x0604u

/* 速度环前馈：out = slope*target ± fric + PI。fric 是摩擦等效压降（PWM 常量），
 * 高速精度主要靠它，PI 只修残差（目标 5000 时 PI 稳态贡献≈0）。
 * 这两个数是 2026-09-30 台架实测拟合出来的（稳态 PWM ≈ 0.662×目标 + 2327），
 * 属于**硬件标定量**：换电机/减速比/电池电压必须重标。
 * 它们同时是 speed_control.c 的兜底值，所以编译期
 * -DSPEED_FF_SLOPE=0.80f -DSPEED_FF_FRIC=0.0f 仍然能精确退化成改动前的老行为（A/B 用）。 */
#ifndef SPEED_FF_SLOPE
#define SPEED_FF_SLOPE       0.662f
#endif
#ifndef SPEED_FF_FRIC
#define SPEED_FF_FRIC        2327.0f
#endif
#define HP_SPEED_FF_SLOPE    0x0605u
#define HP_SPEED_FF_FRIC     0x0606u
#define HP_GYRO_KP           0x0101u
#define HP_GYRO_KI           0x0102u
#define HP_GYRO_KD           0x0103u
#define HP_GYRO_ILIMIT       0x0104u
#define HP_TURN_ANGLE_OFFSET 0x0105u
#define HP_TURN_TOLERANCE    0x0401u
#define HP_TURN_TIMEOUT      0x0402u
#define HP_YAW_RATE_DAMP     0x0403u
#define HP_MARKER_HOLD       0x0404u
#define HP_MARKER_MIN_GAP    0x0405u
#define HP_COUNTS_PER_REV    0x0305u
#define HP_WHEEL_DIA         0x0306u
#define HP_WHEEL_BASE        0x0307u
#define HP_PWM_MAX           0x0303u
#define HP_VBAT_NOMINAL      0x0501u
#define HP_STALL_CURRENT     0x0502u
#define HP_VBAT_LOW          0x0503u

/* ---------------- 生命周期 ---------------- */
void host_link_init(void);                 /* 初始化 LPUART1 + 参数表，发一条启动横幅 */
void host_link_poll(uint32_t loop_us);     /* 主循环里调用：收帧处理 + 按频率发遥测 */

/* ---------------- 给上层用 ---------------- */
float   host_param_get(uint16_t id, float fallback);   /* 控制代码从这里取参数 */
uint8_t host_param_u8(uint16_t id, uint8_t fallback);
uint32_t host_param_u32(uint16_t id, uint32_t fallback);
void    host_param_set_local(uint16_t id, float v);    /* 本地改参（不经串口） */
void    host_link_send_text(const char *s);            /* 调试文本转发到上位机 */
void    host_link_tick_1hz(void);                      /* 每秒报一次链路统计（RTT） */

/* ---------------- 统计（RTT/调试用） ---------------- */
typedef struct {
  uint32_t rx_frames;
  uint32_t tx_frames;
  uint32_t crc_errors;
  uint32_t resyncs;
  uint32_t param_sets;
  uint16_t telem_hz;
} host_link_stats_t;

const host_link_stats_t *host_link_stats(void);

#endif /* __HOST_LINK_H__ */
