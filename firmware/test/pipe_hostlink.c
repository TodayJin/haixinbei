/**
 * @file  pipe_hostlink.c  把主机上的「真固件协议层」变成一个字节管道进程
 *
 * 用途：给上位机（tools/console）做**真源码字节级对拍**。
 *   python 组帧 → 本进程（真 host_link.c + 真 host_ext.c）→ 真固件的回帧 → python 解码
 *
 * 行为：stdin 每收到一个字节就
 *   host_dl_feed(1 字节) → 反复 host_link_poll(0)（其间推进 5ms 假时钟，让
 *   telem_hz 默认 50 的遥测照常发）→ 把 host_dl_tx() 的全部字节写 stdout →
 *   host_dl_reset()（rx 已抽干、tx 已送出，把环形缓冲归零，避免 4096 上限丢字节）
 * EOF 退出。stdout 只出协议字节（二进制、无缓冲），诊断一律走 stderr。
 *
 * 编译（`-DHOST_PIPE_REAL_SERVO` 是关键）：源文件 =
 *   test/pipe_hostlink.c + App/Svc/host_link.c + App/Svc/host_ext.c
 *   + App/Bsp/servo.c（真动作表/中位偏差，所以关掉桩里的假舵机）
 *   + test/stub/host_appstub.c + test/stub/host_pipestub.c
 *   + test/stub/{host_hal,host_board,host_rtt}.c
 * 完整命令行见 tools/console/tests/test_firmware_pipe.py 的 FLAGS/SOURCES
 * （它会用 gcc 编出来、喂帧、解回帧；本机没 gcc 就 skip）。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "host_link.h"

/* 桩函数（firmware/test/stub/host_board.c 与 host_hal.c 提供） */
void     host_dl_feed(const uint8_t *data, uint16_t len);
void     host_dl_reset(void);
uint16_t host_dl_tx_len(void);
const uint8_t *host_dl_tx(void);
void     host_dl_tx_clear(void);
void     host_tick_add(uint32_t ms);

#define PIPE_POLLS_PER_BYTE  4u     /* 每字节拍几次：够把分帧的参数 dump 抽干 */
#define PIPE_TICK_MS         5u     /* 每拍假时钟步长：50Hz ⇒ 约每 4 字节一帧遥测 */

static void flush_tx(void)
{
    uint16_t n = host_dl_tx_len();
    if (n == 0u) {
        return;
    }
    if (fwrite(host_dl_tx(), 1u, (size_t)n, stdout) != (size_t)n) {
        fprintf(stderr, "pipe_hostlink: stdout 写失败\n");
        exit(2);
    }
    fflush(stdout);
    host_dl_tx_clear();
}

int main(void)
{
    int c;

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_TEXT);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);

    host_link_init();

    while ((c = fgetc(stdin)) != EOF) {
        uint8_t b = (uint8_t)c;
        uint32_t i;

        host_dl_feed(&b, 1u);

        for (i = 0u; i < PIPE_POLLS_PER_BYTE; i++) {
            host_link_poll(0u);
            flush_tx();
            host_tick_add(PIPE_TICK_MS);
        }

        host_dl_reset();
    }

    flush_tx();
    return 0;
}
