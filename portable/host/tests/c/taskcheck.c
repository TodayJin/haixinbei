/**
 * @file  tools/console/tests/c/taskcheck.c
 * @brief 让**真固件的校验器**去读上位机（TS 编码器）拼出来的任务镜像。
 *
 * 链接的是 firmware/App/Svc/task_prog.c（不是复制品），所以这个程序说的"合法"就是车上说的"合法"。
 * 用法：taskcheck.exe <prog.bin>
 * 输出（每行一个字段，方便测试程序按正则抓）：
 *   VALIDATE <code> <errstr>
 *   HDR n_states=<n> main=<m> len=<l> crc=0x<hex>
 *   STATE <i> name="<utf8>" n_enter=<a> n_body=<b>
 *   BLOCK <i> <seq> <j> op=0x<op> flags=<f> p0=<p0> p1=<p1> p2=<p2> p3=<p3>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "task_prog.h"

static uint8_t *read_file(const char *path, uint32_t *n_out)
{
    FILE *f = fopen(path, "rb");
    long  n;
    uint8_t *buf;

    if (f == NULL) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    n = ftell(f);
    if (n < 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);
    buf = (uint8_t *)malloc((size_t)n + 1u);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    buf[n] = 0u;
    *n_out = (uint32_t)n;
    return buf;
}

static void dump_seq(const uint8_t *prog, uint8_t st, int seq, uint16_t off, uint8_t nblocks)
{
    uint8_t j;

    for (j = 0u; j < nblocks; j++) {
        task_block_t b;
        TaskProg_ReadBlock(prog, (uint32_t)off + (uint32_t)j * TASK_BLOCK_LEN, &b);
        printf("BLOCK %u %s %u op=0x%02X flags=%u p0=%u p1=%d p2=%u p3=%u\n",
               (unsigned)st, seq == 0 ? "enter" : "body", (unsigned)j,
               (unsigned)b.op, (unsigned)b.flags, (unsigned)b.p0,
               (int)b.p1, (unsigned)b.p2, (unsigned)b.p3);
    }
}

/**
 * 打印一个状态名。
 *
 * ⚠️ 格式里**没有长度字段、也没有 '\0'**（见 docs/任务编程.md §4）：名字区是若干个名字首尾相接，
 * 定界只能靠"下一个偏移"（和 TS 侧 `decodeTask` 的 `nameEnd()` 同一套规则）。
 * 这里用 `%s` 直接打印是**错的** —— 会把下一个名字一起读出来。
 */
static void dump_name(const uint8_t *prog, uint32_t len, uint8_t n_states, uint8_t idx, uint16_t noff)
{
    uint32_t end = len;
    uint8_t  k;

    if (noff == 0u) {
        printf(" name=\"\"");
        return;
    }
    for (k = 0u; k < n_states; k++) {
        task_state_t o;
        uint16_t cand[3];
        int      c;
        TaskProg_ReadState(prog, k, &o);
        cand[0] = o.name_off;
        cand[1] = o.enter_off;
        cand[2] = o.body_off;
        for (c = 0; c < 3; c++) {
            if (cand[c] > noff && (uint32_t)cand[c] < end) {
                end = cand[c];
            }
        }
    }
    (void)idx;
    printf(" name=\"");
    fwrite(&prog[noff], 1u, (size_t)(end - noff), stdout);
    printf("\"");
}

int main(int argc, char **argv)
{
    uint8_t *prog;
    uint32_t len = 0u;
    int      e;
    uint8_t  n_states;
    uint8_t  i;

    if (argc < 2) {
        fprintf(stderr, "用法：taskcheck <prog.bin>\n");
        return 2;
    }
    prog = read_file(argv[1], &len);
    if (prog == NULL) {
        fprintf(stderr, "读不了 %s\n", argv[1]);
        return 2;
    }

    e = TaskProg_Validate(prog, len);
    printf("VALIDATE %d %s\n", e, TaskProg_ErrStr(e));
    if (e != TASK_OK) {
        free(prog);
        return 0; /* 校验失败不是本程序的错误，交给调用方判断 */
    }

    n_states = prog[3];
    printf("HDR n_states=%u main=%u len=%lu crc=0x%04X\n",
           (unsigned)n_states, (unsigned)prog[4], (unsigned long)len,
           (unsigned)task_rd_u16(&prog[6]));

    for (i = 0u; i < n_states; i++) {
        task_state_t st;
        TaskProg_ReadState(prog, i, &st);
        printf("STATE %u", (unsigned)i);
        dump_name(prog, len, n_states, i, st.name_off);
        printf(" n_enter=%u n_body=%u\n", (unsigned)st.n_enter, (unsigned)st.n_body);
        dump_seq(prog, i, 0, st.enter_off, st.n_enter);
        dump_seq(prog, i, 1, st.body_off, st.n_body);
    }

    free(prog);
    return 0;
}
