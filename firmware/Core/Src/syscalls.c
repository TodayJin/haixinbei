/**
 * @file  syscalls.c
 * @brief newlib-nano 需要的裸机系统调用桩
 *
 * 本工程不用 libc 的 stdio（观测走 SEGGER RTT，显示走 KK_OLED），
 * 这些桩只是为了满足 newlib 的 malloc/_write 等符号引用，不产生实际 I/O。
 */
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

extern uint8_t _end;        /* 链接脚本里的堆起点 */
extern uint8_t _estack;     /* 栈顶（= RAM 末尾） */
extern uint8_t _Min_Stack_Size;

/**
 * @brief 极简堆：从 _end 向栈顶方向增长，撞到栈就返回失败。
 *        KK_UI / KK_OLED 自身不使用动态内存；这里只为 libc 兜底。
 */
void *_sbrk(ptrdiff_t incr)
{
    static uint8_t *heap_end = NULL;
    uint8_t *prev;
    uint8_t *limit;

    if (heap_end == NULL) {
        heap_end = &_end;
    }
    prev  = heap_end;
    limit = (uint8_t *)((uintptr_t)&_estack - 0x800U);   /* 留 2KB 给栈 */
    if (heap_end + incr > limit) {
        errno = ENOMEM;
        return (void *)-1;
    }
    heap_end += incr;
    return prev;
}

int _write(int file, char *ptr, int len) { (void)file; (void)ptr; return len; }
int _read(int file, char *ptr, int len)  { (void)file; (void)ptr; (void)len; return 0; }
int _close(int file)                     { (void)file; return -1; }
int _lseek(int file, int ptr, int dir)   { (void)file; (void)ptr; (void)dir; return 0; }

int _fstat(int file, struct stat *st)
{
    (void)file;
    st->st_mode = S_IFCHR;
    return 0;
}

int _isatty(int file) { (void)file; return 1; }
int _getpid(void)     { return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
void _exit(int status) { (void)status; while (1) { } }
