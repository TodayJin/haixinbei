/**
 * @file  task_store.h
 * @brief 上位机烧进车上 Flash 的任务程序：目录页 + 程序区（持久化层）
 *
 * 页分配（用户已拍板口径：共 4 页 = 8KB，最多 8 个任务）：
 *
 *   页 59 = 0x0801D800                     目录页（magic/ver/count + 8 条记录）
 *   页 60..62 = 0x0801E000..0x0801F7FF     程序区 6144 B（任务镜像紧凑摆放）
 *   页 63 = 0x0801F800                     nvram（既有，本模块绝不碰）
 *
 * 链接脚本 `firmware/STM32G474RBTx_FLASH.ld` 的 FLASH 收到 **118K**（= 页 59 起点
 * 0x1D800）：固件镜像长进任务区必须是链接错误，而不是静默把车上的任务压掉。
 *
 * 目录页字节布局（小端；主机测试按这些偏移直接查页内容）：
 *
 *   记录头 8 B   0 u16 magic = 0x5453
 *                2 u8  ver   = 2
 *                3 u8  count (0..8)
 *                4 u16 seq   （每次成功落盘 +1，只用于诊断）
 *                6 u8  reserved[2] = 0xFF
 *   第 i 条记录（40 B，i < count）
 *                0 u16 id
 *                2 u16 off   （程序区内偏移，**8 字节对齐**：双字编程的硬要求）
 *                4 u16 len
 *                6 u16 crc16 （TaskProg_Crc16，覆盖镜像 [12,len)）
 *                8 u8  flags （bit0 = TASK_STORE_FLAG_AUTO）
 *                9 u8  name_len (0..30)
 *               10 char name[30]
 *
 * 写入语义（与 nvram.c 同一条铁律：**运动中绝不擦写**）：
 *   - 写一个任务 = 擦目录页 + 擦程序区 3 页 + 整份重写（约 4 页擦 + 8KB 编程）。
 *   - **先擦目录页**：掉电中断时车上看到的是"没有任务"，而不是"半个新程序配着旧偏移"
 *     ——只有前者是安全的（跑错轨迹比不跑更糟）。
 *   - 每条记录独立校验（结构 + CRC16），坏记录 = 空槽；目录 magic/ver 坏 = 整片空
 *     （自愈：下次 Commit 会把目录整页写回去）。
 */
#ifndef TASK_STORE_H
#define TASK_STORE_H

#include <stdint.h>

/* ---- 页分配 ---- */
#define TASK_STORE_PAGE_SIZE   2048u
#define TASK_STORE_DIR_PAGE    59u
#define TASK_STORE_PROG_PAGE0  60u
#define TASK_STORE_PROG_PAGES  3u
#define TASK_STORE_PROG_BYTES  6144u    /* 3 × 2048，程序区总字节 */

/* ---- 容量 ---- */
#define TASK_STORE_SLOTS       8u       /* 目录最多 8 条记录（用户口径：最多 8 个任务） */
#define TASK_STORE_NO_SLOT     0xFFu
#define TASK_STORE_NAME_MAX    30u
/* 单个任务上限 = 解释器的 RAM 上限（firmware/App/Svc/task_run.c 的 TASK_RUN_MAX_PROG）：
 * 更长的程序装了也跑不起来，所以由存储层先挡掉。程序区总共 6144 B，8 个任务要装得下
 * 得看实际体积（典型 8 状态 × 12 块 ≈ 1.1KB）。 */
#define TASK_STORE_CAP         4096u

/* ---- 目录页布局常量（测试直接按这些偏移查 Flash） ---- */
#define TASK_STORE_DIR_MAGIC   0x5453u
#define TASK_STORE_VER         2u
#define TASK_STORE_DIR_HDR_LEN 8u
#define TASK_STORE_ENTRY_LEN   40u

/* ---- 记录 flags ---- */
#define TASK_STORE_FLAG_AUTO   0x01u    /* 开机自启：上电自动载入并启动这个任务 */

/* ---- 返回码 ---- */
enum {
    TASK_STORE_OK       = 0,
    TASK_STORE_E_PARAM  = -1,   /* 参数/容量不对（槽号、len、分片没收全、装不下） */
    TASK_STORE_E_BUSY   = -2,   /* 车在动 / 解释器在跑：擦写会把控制环按住几十毫秒 */
    TASK_STORE_E_CRC    = -3,   /* 镜像 CRC 与上位机给的对不上 */
    TASK_STORE_E_FORMAT = -4,   /* 镜像能过 CRC 但结构不合格（TaskProg_Validate） */
    TASK_STORE_E_FLASH  = -5,   /* 擦/写/回读失败 */
    TASK_STORE_E_EMPTY  = -6    /* 空槽（Load 时） */
};

/* ---- 上电扫描 / 查询（host_link 的 T_TASK_* 帧都走这几个） ---- */
void     TaskStore_Init(void);
uint8_t  TaskStore_SlotCount(void);
uint8_t  TaskStore_Present(uint8_t slot);
uint16_t TaskStore_Len(uint8_t slot);
uint16_t TaskStore_Crc(uint8_t slot);
uint8_t  TaskStore_Flags(uint8_t slot);
/* 返回名字真实长度（out 一定补 '\0'）；空槽返回 0 */
uint8_t  TaskStore_Name(uint8_t slot, char *out, uint8_t cap);
/* 直接指 Flash 里的镜像（XIP，无拷贝）；空槽返回 NULL */
const uint8_t *TaskStore_Image(uint8_t slot);
/* 镜像所在绝对页号 / bank 内页号（诊断用）；空槽返回 0xFF */
uint8_t  TaskStore_Page(uint8_t slot);
uint8_t  TaskStore_BankPage(uint8_t slot);
/* 带开机自启标记（flags.bit0）里 id 最小的槽；没有则 TASK_STORE_NO_SLOT */
uint8_t  TaskStore_AutoSlot(void);
/* 校验过的镜像载入解释器（空槽回 TASK_STORE_E_EMPTY，其余透传 TaskRun_Load 的返回值） */
int      TaskStore_Load(uint8_t slot);

/* ---- 分片上传（T_TASK_PUT 只写 RAM 暂存，车在动也允许） ---- */
void     TaskStore_StageReset(void);
int      TaskStore_Stage(uint16_t off, const uint8_t *d, uint16_t n);
uint16_t TaskStore_StageLen(void);

/* ---- 落盘 / 删除（运动中一律 TASK_STORE_E_BUSY） ---- */
int      TaskStore_Commit(uint8_t slot, uint16_t len, uint16_t crc, uint8_t flags,
                          const char *name, uint8_t name_len);
int      TaskStore_Erase(uint8_t slot);

#endif /* TASK_STORE_H */
