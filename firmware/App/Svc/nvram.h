/**
 * @file  nvram.h
 * @brief 设置 + 标定的掉电保存（Flash 最后一页）
 *
 * 用户 m03511："记住了这些所有的设置的内容以及阈值的标定等等关于记忆的
 * 你记得保存flash"。
 *
 * 存在 Flash 最后一页 0x0801F800（2KB，页 63；STM32G474RB 单 bank 128KB
 * 时的最后一页，代码用不满）。记录格式：
 *
 *     magic(4) + version(2) + length(2) + CRC32(4) + payload(160) + pad(4)
 *
 * version 1 -> 2 只往 payload **末尾**追加了速度环前馈两个字段（0.662/2327），
 * v5 -> 6 同样只在末尾追加了角度环 `gyro_corr_ref`（差速天花板随速放大的参考
 * 速度），所以 v1 记录的前缀（含 8 路黑白标定）原样可用，开机时按 length 迁移
 * 过来、只把新的末尾字段填成参数表当前默认值 —— 升级固件不会让用户重标一次。
 * （注意：往末尾加字段必须把 NVRAM_VERSION 加 1，否则长度对不上的老记录会被
 * 当成坏记录整条作废，见 nvram_init()。测试 t_nvram.c ⑦ 盯着这条。）
 *
 * 开机 nvram_init() 校验 magic/version/length/CRC 后把它盖回 g_cfg 和
 * trace_cal；运行中 nvram_poll() 发现"当前值 != 上次存的值"就等 1.5s
 * 防抖（用户可能在菜单里连着改好几项），到点进入 NVRAM_STATE_BUSY：
 * 界面显示"正在保存"并锁住按键，nvram_service() 再真擦写，写完显示
 * "保存成功"（用户 m03944："让屏幕显示'正在保存',并且什么都不让我动,
 * 这样来保证擦血顺利.擦完之后,再显示'保存成功'"）。
 *
 * 写入会把 CPU 停住约 30ms（G4 单 bank 擦写期间取指停摆），所以：
 *   - 只在真的改了东西才写；
 *   - 屏正在搬帧（OLED_IsBusy）时让路；
 *   - 掉电前不保证写完（没有掉电检测），但设置这种场合够用。
 */
#ifndef NVRAM_H
#define NVRAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 上电调一次（app_ui_init 之后：参数表 pull 完再让 Flash 里的值盖上去）。
 * Flash 里没有有效记录就什么都不改（用代码里的默认值）。 */
void nvram_init(void);

/* 主循环每轮调：200ms 检查一次，改了东西 1.5s 之后进入"正在保存"状态 */
void nvram_poll(void);

/* 保存过程的状态：界面据此显示"正在保存/保存成功"并锁住按键 */
#define NVRAM_STATE_IDLE  0U   /* 没在保存 */
#define NVRAM_STATE_BUSY  1U   /* 到点要写了：提示上屏中 / 正在擦写 */
#define NVRAM_STATE_DONE  2U   /* 刚写成功，"保存成功"还挂着 */
#define NVRAM_NOTICE_MS 600U
#define NVRAM_DONE_MS   1200U
/* 提示上屏后最多再等屏把帧搬完多久就照写：显示传输万一被 latch 住，
 * 无限等会让 BUSY 永不到头、按键永久失效（审计 agent 3 项④）。 */
#define NVRAM_DISPLAY_WAIT_MS 1500U

uint8_t nvram_state(void);

/* "现在就存"：跳过 1.5s 防抖，立刻请求一次保存（上位机的一键固化用）。
 * 真正写不写由 nvram_poll() 比对决定（已一致就不写）、车在动仍然不写。 */
void nvram_touch(void);

/* 主循环每轮调，且要放在 app_ui_tick() **之后**：界面先把"正在保存"画
 * 上屏（并吃掉按键），这里才真去擦写 Flash —— 擦写会把 CPU 停住约 30ms，
 * 不能抢在提示上屏之前（用户 m03944）。 */
void nvram_service(void);

/* 1 = 本次开机确实从 Flash 恢复了设置（诊断用） */
uint8_t nvram_loaded(void);

#ifdef __cplusplus
}
#endif
#endif /* NVRAM_H */
