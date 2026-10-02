/**
 * @file  motor_bench.h
 * @brief 电机 + 编码器上电台架自测（只在 -DMOTOR_BENCH 的构建里编进去）
 *
 * 构建：make MOTOR_BENCH=1   -> build-bench/haixin-bench.elf|.hex|.bin
 * 跑法：上电后自动跑一遍，全程结果走 SEGGER RTT 通道 0。
 */
#ifndef __MOTOR_BENCH_H__
#define __MOTOR_BENCH_H__

void MotorBench_Run(void);

#endif /* __MOTOR_BENCH_H__ */
