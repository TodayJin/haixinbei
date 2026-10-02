/**
 * @file  t_common.h   （firmware/test 共用的小测试框架）
 *
 * 每个测试都是一个独立可执行文件：main() 里跑用例，末尾 T_SUMMARY() 返回
 * 失败个数（0 = 通过）。编译时带 UBSan trap 模式，越界/溢出会直接把进程打死
 * （Windows 上是 0xC000001D），也算失败。
 */
#ifndef __T_COMMON_H
#define __T_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int t_checks;
static int t_fails;

#define CHECK(cond)                                                             \
    do {                                                                        \
        t_checks++;                                                             \
        if (!(cond)) {                                                          \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);            \
            t_fails++;                                                          \
        }                                                                       \
    } while (0)

#define CHECK_EQ_I(a, b)                                                        \
    do {                                                                        \
        long long va_ = (long long)(a), vb_ = (long long)(b);                   \
        t_checks++;                                                             \
        if (va_ != vb_) {                                                       \
            printf("  FAIL %s:%d  %s == %s  (%lld != %lld)\n", __FILE__,        \
                   __LINE__, #a, #b, va_, vb_);                                 \
            t_fails++;                                                          \
        }                                                                       \
    } while (0)

#define T_SUMMARY()                                                             \
    (printf("  %s: %d 项检查, %d 处失败\n", __FILE__, t_checks, t_fails),        \
     t_fails ? 1 : 0)

/* 每个测试文件自己实现这个（run_tests.ps1 只调 main） */
#endif /* __T_COMMON_H */
