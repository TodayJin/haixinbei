/* STM32G474RBT6: 128K Flash / 96K SRAM / 32K CCM SRAM
 * ⚠️ 本板擦除粒度是 **4KB 块**（块号 = PNB = 页号/2，块地址 = 0x08000000 + 块号×4096），
 *    不是 2KB 页 —— 2026-10-04 真板实测，见 App/Svc/task_store.c / nvram.c。
 * 链接器只拿前 116K（0x08000000..0x0801CFFF），最后两个块留给运行期自己擦写的模块：
 *   块 29 = 页 58 = 0x0801D000 -> App/Svc/task_store.c 任务目录页
 *          页 59 = 0x0801D800 -> 任务程序区
 *   块 30 = 页 60 = 0x0801E000 -> 任务程序区
 *          页 61 = 0x0801E800 -> 任务程序区（程序区共 6144 B）
 *   块 31 = 页 62 = 0x0801F000 -> 空着（nvram 擦块 31 时会连带擦掉）
 *          页 63 = 0x0801F800 -> App/Svc/nvram.c 存设置和标定
 * 所以 LENGTH 必须是 116K 而不是 118K/126K：写 118K 时链接器允许把代码放进页 58，
 * 一旦镜像长过 0x0801D000 就会被 TaskStore_Commit 的块擦静默压掉（表现为"烧完固件
 * 任务没了"，或者更糟：擦掉正在执行的代码）。给 116K 才让"放不下"变成链接错误。
 * 页分配的唯一权威是 App/Svc/task_store.h 的头部注释：改了那边就要同步改这里。 */
ENTRY(Reset_Handler)

_estack = ORIGIN(RAM) + LENGTH(RAM);
_Min_Heap_Size  = 0x400;
_Min_Stack_Size = 0x800;

MEMORY
{
  CCMRAM (xrw) : ORIGIN = 0x10000000, LENGTH = 32K
  RAM    (xrw) : ORIGIN = 0x20000000, LENGTH = 96K
  FLASH  (rx)  : ORIGIN = 0x08000000, LENGTH = 116K
}

SECTIONS
{
  .isr_vector :
  {
    . = ALIGN(4);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)
    KEEP (*(.init))
    KEEP (*(.fini))
    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN (__preinit_array_start = .);
    KEEP (*(.preinit_array*))
    PROVIDE_HIDDEN (__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN (__init_array_start = .);
    KEEP (*(SORT(.init_array.*)))
    KEEP (*(.init_array*))
    PROVIDE_HIDDEN (__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN (__fini_array_start = .);
    KEEP (*(SORT(.fini_array.*)))
    KEEP (*(.fini_array*))
    PROVIDE_HIDDEN (__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  . = ALIGN(4);
  .bss :
  {
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE ( end = . );
    PROVIDE ( _end = . );
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    libc.a ( * )
    libm.a ( * )
    libgcc.a ( * )
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
