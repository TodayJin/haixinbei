/**
 * @file  pipe_hostlink.c  把主机上的「真固件协议层」变成一个字节管道进程
 *
 * 用途：给上位机（tools/console）做**真源码字节级对拍**。
 *   python 组帧 → 本进程（真 host_link.c + 真 host_ext.c）→ 真固件的回帧 → python 解码
 *
 * 行为：stdin 每收到一个字节就
 *   host_dl_feed(1 字节) → 反复 host_link_poll(0)（其间推进 5ms 假时钟，让
 *   telem_hz 默认 200 的遥测照常发）→ 把 host_dl_tx() 的全部字节写 stdout →
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
 *
 * 可选 `-DHOST_PIPE_REAL_NVRAM`：再链真 App/Svc/nvram.c + App/Svc/trace_cal.c，
 * 于是 T_PARAM_SAVE 走真固化路径（host_ext.c → nvram_touch → nvram_poll/service →
 * 假 Flash 页 63）。这时支持两个命令行开关做"掉电重启"：
 *   --flash <文件>        开机前把页 63 的 2048 B 从文件载入（模拟"上次存过的 Flash"）
 *   --dump-flash <文件>   退出前把页 63 的 2048 B 写出来（给下一个进程当 --flash）
 * 注意顺序：host_flash_reset()（映射出来的内存是 0，不是 0xFF）→ 载入镜像 →
 * host_link_init() → nvram_init()（真固件就是"参数表 pull 完再让 Flash 盖上去"）。
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#endif

#include "host_link.h"

#ifdef HOST_PIPE_REAL_NVRAM
#include "nvram.h"
#endif

/* 桩函数（firmware/test/stub/host_board.c 与 host_hal.c 提供） */
void     host_dl_feed(const uint8_t *data, uint16_t len);
void     host_dl_reset(void);
uint16_t host_dl_tx_len(void);
const uint8_t *host_dl_tx(void);
void     host_dl_tx_clear(void);
void     host_tick_add(uint32_t ms);
void     host_flash_reset(void);
uint8_t *host_flash_page(uint32_t page);

#define PIPE_POLLS_PER_BYTE  4u     /* 每字节拍几次：够把分帧的参数 dump 抽干 */
#define PIPE_TICK_MS         5u     /* 每拍假时钟步长：50Hz ⇒ 约每 4 字节一帧遥测 */
#define PIPE_FLASH_PAGE      63u    /* nvram 的记录页：0x0801F800 = 页 63 */
#define PIPE_FLASH_BYTES     2048u
#define PIPE_TAIL_TICKS      300u   /* EOF 后空转上限：只用来把已排队的固化跑完 */

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

#ifdef HOST_PIPE_REAL_NVRAM
/* ---- 页 63 镜像 <-> 文件（模拟"掉电但不丢 Flash"） ---- */
static int load_flash(const char *path)
{
    uint8_t *pg = host_flash_page(PIPE_FLASH_PAGE);
    FILE *f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        fprintf(stderr, "pipe_hostlink: 打不开 --flash 文件 %s\n", path);
        return -1;
    }
    if (pg == NULL) {
        fprintf(stderr, "pipe_hostlink: 假 Flash 里没有页 %u\n", (unsigned)PIPE_FLASH_PAGE);
        fclose(f);
        return -1;
    }
    n = fread(pg, 1u, PIPE_FLASH_BYTES, f);
    fclose(f);
    if (n != PIPE_FLASH_BYTES) {
        fprintf(stderr, "pipe_hostlink: --flash 文件 %s 不是 %u 字节（读到 %u）\n",
                path, (unsigned)PIPE_FLASH_BYTES, (unsigned)n);
        return -1;
    }
    return 0;
}

static int dump_flash(const char *path)
{
    const uint8_t *pg = host_flash_page(PIPE_FLASH_PAGE);
    FILE *f;

    if (pg == NULL) {
        fprintf(stderr, "pipe_hostlink: 假 Flash 里没有页 %u\n", (unsigned)PIPE_FLASH_PAGE);
        return -1;
    }
    f = fopen(path, "wb");
    if (f == NULL) {
        fprintf(stderr, "pipe_hostlink: 打不开 --dump-flash 文件 %s\n", path);
        return -1;
    }
    if (fwrite(pg, 1u, PIPE_FLASH_BYTES, f) != PIPE_FLASH_BYTES) {
        fprintf(stderr, "pipe_hostlink: 写 --dump-flash 文件失败\n");
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}
#endif /* HOST_PIPE_REAL_NVRAM */

int main(int argc, char **argv)
{
    int c;
    uint32_t i;
    const char *flash_in = NULL;
    const char *flash_out = NULL;
    int k;

    for (k = 1; k < argc; k++) {
        if (strcmp(argv[k], "--flash") == 0 && (k + 1) < argc) {
            flash_in = argv[++k];
        } else if (strcmp(argv[k], "--dump-flash") == 0 && (k + 1) < argc) {
            flash_out = argv[++k];
        } else {
            fprintf(stderr, "pipe_hostlink: 用法: pipe_hostlink [--flash 文件] [--dump-flash 文件]\n");
            return 2;
        }
    }

#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_TEXT);
#endif
    setvbuf(stdout, NULL, _IONBF, 0);

    host_flash_reset();     /* 上电：假 Flash 抹成 0xFF（映射出来的内存本来是 0x00） */

#ifdef HOST_PIPE_REAL_NVRAM
    if (flash_in != NULL && load_flash(flash_in) != 0) {
        return 2;
    }
#endif

    host_link_init();
#ifdef HOST_PIPE_REAL_NVRAM
    nvram_init();           /* 真固件：参数表 pull 完，再让 Flash 里的值盖上去 */
#endif

    while ((c = fgetc(stdin)) != EOF) {
        uint8_t b = (uint8_t)c;

        host_dl_feed(&b, 1u);

        for (i = 0u; i < PIPE_POLLS_PER_BYTE; i++) {
            host_link_poll(0u);
#ifdef HOST_PIPE_REAL_NVRAM
            nvram_poll();       /* 真主循环每轮都做：比对 + 1.5s 防抖 + 排固化 */
            nvram_service();    /* UI tick 之后才真擦写 */
#endif
            flush_tx();
            host_tick_add(PIPE_TICK_MS);
        }

        host_dl_reset();
    }

#ifdef HOST_PIPE_REAL_NVRAM
    /* 输入没了（就像车断电前停了手）。只把**已经在排队**的固化跑完，绝不再空转到
     * 1.5s 自动防抖到期 —— 否则测试就分不清"点了固化"和"没点固化但等够了"。 */
    for (i = 0u; i < PIPE_TAIL_TICKS; i++) {
        uint8_t st;
        nvram_poll();
        nvram_service();
        host_tick_add(PIPE_TICK_MS);
        st = nvram_state();
        if (st == NVRAM_STATE_DONE || (st == NVRAM_STATE_IDLE && i >= 1u)) {
            break;      /* 固化已写完，或压根没排队 */
        }
    }

    if (flash_out != NULL && dump_flash(flash_out) != 0) {
        return 2;
    }
#endif

    flush_tx();
    return 0;
}
