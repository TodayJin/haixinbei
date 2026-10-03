/**
 * @file  task_store.h
 * @brief 上位机烧进 Flash 的任务程序：3 个槽位（页 60 / 61 / 62）
 *
 * 用户 m01443：「固件不需要有现在的任务流程，只要能识别上位机存 flash 的能执行就行」
 * 设计文档 `docs/任务编程.md`。车只是执行机构：上位机把任务拼成 task_prog.h 那段
 * 二进制、烧进这里，车离线自主执行（比赛不能遥控）。
 *
 * 布局（本板实测 2 bank × 32 页 × 2KB，页 63 被 nvram 占了）：
 *
 *     页 60 = 0x0801E000 ─┐
 *     页 61 = 0x0801E800  ├─ 一个槽位一页，槽与槽互不影响
 *     页 62 = 0x0801F000 ─┘
 *     页 63 = 0x0801F800    设置/标定（nvram.c）
 *
 * 每页开头 40 字节是记录头，后面紧跟程序镜像：
 *
 *     magic(2) ver(1) id(1) len(2) crc16(2) flags(1) name_len(1) name(30) | 程序镜像…
 *
 * 为什么头不单独放一个"目录页"：目录页写坏一次三个任务就全找不着了；一槽一页时
 * 一次掉电最多毁掉正在写的那一个，而镜像自己带 CRC16 + 结构校验，坏了就是"空槽"。
 *
 * 擦写沿用 nvram.c 那套**手写 FLASH->CR**（BKER=1 + PNB=页号−32）：本板 FLASH_OPTR
 * 的 DBANK 位读出来是 0，用 HAL_FLASHEx_Erase 会用 PNB=63 越界并静默失败。
 * 判据（nvram.c:291-300）里那两行 J-Link 实测结论同样适用。
 *
 * 车在动（或任务在跑）时**绝不擦写**：擦一页会把 CPU 停住几十毫秒，正在跑的控制环
 * 会直接失控 —— 这是 nvram.c 的老铁律，这里一字不改地沿用。
 */
#ifndef __TASK_STORE_H__
#define __TASK_STORE_H__

#include <stdint.h>

#define TASK_STORE_SLOTS       3u
#define TASK_STORE_PAGE_SIZE   2048u
#define TASK_STORE_HDR_LEN     40u
#define TASK_STORE_NAME_MAX    30u
/* 单个任务的程序镜像上限 = 页大小 − 头。上位机 task.ts 那边按 6144 画进度条只是显示，
 * 真正能被车接收的上限是这里（T_TASK_COMMIT 超出就回 ST_OUT_OF_RANGE）。 */
#define TASK_STORE_CAP         (TASK_STORE_PAGE_SIZE - TASK_STORE_HDR_LEN)   /* 2008 */
#define TASK_STORE_MAGIC       0x5453u    /* 小端存下来是 53 54 = 'S' 'T' */
#define TASK_STORE_VER         1u
#define TASK_STORE_FLAG_AUTO   0x01u      /* bit0：开机自启（离线自主跑，比赛用） */
#define TASK_STORE_NO_SLOT     0xFFu

/* 记录头（40 B —— 必须是 8 的整数倍：按双字编程） */
typedef struct {
    uint16_t magic;                       /* TASK_STORE_MAGIC */
    uint8_t  ver;                         /* TASK_STORE_VER */
    uint8_t  id;                          /* 槽号 0..TASK_STORE_SLOTS-1 */
    uint16_t len;                         /* 程序镜像字节数 */
    uint16_t crc16;                       /* 程序自己的 CRC16（覆盖镜像 [12,len)） */
    uint8_t  flags;                       /* TASK_STORE_FLAG_* */
    uint8_t  name_len;                    /* 0..TASK_STORE_NAME_MAX */
    uint8_t  name[TASK_STORE_NAME_MAX];   /* 定长区，用 name_len 截断；不保证 NUL */
} task_store_hdr_t;

/* 返回码：0 = 成功，负数为失败原因 */
enum {
    TASK_STORE_OK       =  0,
    TASK_STORE_E_PARAM  = -1,   /* 槽号/长度/名字不合法，或分片没传够 */
    TASK_STORE_E_BUSY   = -2,   /* 车在动或任务在跑：拒绝擦写 */
    TASK_STORE_E_CRC    = -3,   /* 提交的 CRC 与暂存镜像对不上 */
    TASK_STORE_E_FORMAT = -4,   /* 镜像格式校验不过（TaskProg_Validate） */
    TASK_STORE_E_FLASH  = -5,   /* 擦除或编程失败（页坏/写保护） */
    TASK_STORE_E_EMPTY  = -6,   /* 该槽位没有合法任务 */
};

/* 上电调一次（app 初始化、nvram_init 之后）：扫三个槽位、把有效的缓存起来 */
void TaskStore_Init(void);

/* ---- 只读查询（都是缓存值，不碰 Flash） ---- */
uint8_t  TaskStore_SlotCount(void);
uint8_t  TaskStore_Present(uint8_t slot);                       /* 1 = 该槽有合法任务 */
uint16_t TaskStore_Len(uint8_t slot);
uint16_t TaskStore_Crc(uint8_t slot);
uint8_t  TaskStore_Flags(uint8_t slot);
/* 名字拷进 out（必带结尾 0），返回实际字节数；out 为 NULL 只问长度 */
uint8_t  TaskStore_Name(uint8_t slot, char *out, uint8_t cap);
/* 程序镜像首地址：**直接指向 Flash**（XIP 读），无任务返回 NULL。
 * 只有 TaskStore_Commit/Erase 会改 Flash，两者写完都会刷新缓存并回读校验。 */
const uint8_t *TaskStore_Image(uint8_t slot);
/* flags 里带开机自启、且 id 最小的那个槽；没有 → TASK_STORE_NO_SLOT */
uint8_t  TaskStore_AutoSlot(void);
/* 把槽位里的程序载入解释器（TaskRun_Load），返回 0 或 -TASK_E_xxx / TASK_STORE_E_* */
int      TaskStore_Load(uint8_t slot);

/* ---- 烧写：先分片暂存，再一次性提交 ---- */
/* 清暂存（T_TASK_PUT 的第一片之前调；换任务/换槽也调） */
void     TaskStore_StageReset(void);
/* 写一片到暂存区（off + n 必须落在 [0, TASK_STORE_CAP] 内） */
int      TaskStore_Stage(uint16_t off, const uint8_t *d, uint16_t n);
uint16_t TaskStore_StageLen(void);     /* 暂存区已覆盖到的长度（最高偏移+1） */
/* 校验（CRC16 + 格式）通过才擦写 Flash；name 可以为 NULL（name_len 必须为 0） */
int      TaskStore_Commit(uint8_t slot, uint16_t len, uint16_t crc,
                          uint8_t flags, const char *name, uint8_t name_len);
/* 删掉一个槽位（擦成 0xFF） */
int      TaskStore_Erase(uint8_t slot);

/* 主机测试用：槽位 → 绝对页号 / bank 内页号 */
uint8_t  TaskStore_Page(uint8_t slot);
uint8_t  TaskStore_BankPage(uint8_t slot);

#endif /* __TASK_STORE_H__ */
