# ============================================================
#  主控板固件 —— GNU Make 构建
#
#    make                -> 主程序（App + KK_UI，默认目标）
#    make SELFTEST=1     -> 上电自检固件（旧默认）
#    make SELFTEST=1 RESCUE=1     -> 救援固件（只跑时序/RTT）
#    make SELFTEST=1 PA15TEST=2|3|4 -> PA15 判决实验固件
#    make SELFTEST=1 LOOPBACK=1   -> USART3 自发自收固件
#    make flash / make clean / make size
#    make MOTOR_BENCH=1  -> 电机 + 编码器台架自测固件
#    台架自测在 Encoder_Init() 之后、周期任务起来之前跑，结果走 RTT
#
#  依赖：arm-none-eabi-gcc 在 PATH；本地 STM32Cube G4 包（只引用不复制）
# ============================================================

# ---- 本地 STM32Cube G4 包路径 ----
CUBE_G4 ?= C:/Users/Today/STM32Cube/Repository/STM32Cube_FW_G4_V1.6.3
HAL_DIR   = $(CUBE_G4)/Drivers/STM32G4xx_HAL_Driver
CMSIS_DEV = $(CUBE_G4)/Drivers/CMSIS/Device/ST/STM32G4xx
CMSIS     = $(CUBE_G4)/Drivers/CMSIS

JLINK  = C:/Program Files (x86)/SEGGER/JLink/JLink.exe
DEVICE = STM32G474RE

# -- local make shell: the sh.exe shipped with Git for Windows --
#    (POSIX recipe syntax does not work under cmd.exe); override with: make SHELL=/bin/sh
SHELL = C:/Program Files/Git/bin/sh.exe

# ---- 目标/输出目录（变体）----
ifdef SELFTEST
  TARGET = haixin-selftest
  BUILD  = build
  ifdef RESCUE
    TARGET = haixin-rescue
    BUILD  = build-rescue
  endif
  ifdef PA15TEST
    TARGET = haixin-pa15
    BUILD  = build-pa15
  endif
  ifdef LOOPBACK
    TARGET = haixin-loop
    BUILD  = build-loop
  endif
else
  TARGET = haixin-app
  BUILD  = build-app
  ifdef DIAG
    TARGET = haixin-diag
    BUILD  = build-diag
  endif
  ifdef MOTOR_BENCH
    TARGET = haixin-bench
    BUILD  = build-bench
  endif
endif

# ---- 应用公共源码（两个目标都要）----
CORE_COMMON = Core/Src/board.c Core/Src/stm32g4xx_it.c Core/Src/syscalls.c
RTT_SRC     = Middlewares/SEGGER_RTT/SEGGER_RTT.c \
              Middlewares/SEGGER_RTT/SEGGER_RTT_printf.c

ifdef SELFTEST
  VARIANT_SRC = Core/Src/main.c Core/Src/selftest.c Core/Src/ssd1315.c \
                Core/Src/pa15test.c Core/Src/probe_jy61p.c
else
  VARIANT_SRC = Core/Src/main_app.c \
                $(wildcard App/Src/*.c) \
                $(wildcard App/Bsp/*.c) \
                $(wildcard App/Svc/*.c) \
                $(wildcard App/Fonts/*.c) \
                $(wildcard Middlewares/kk_ui/src/*.c) \
                $(wildcard Middlewares/kk_oled/graphics/*.c) \
                Middlewares/kk_oled/driver/kk_oled_driver.c
endif

HAL_SRC = $(addprefix $(HAL_DIR)/Src/,\
  stm32g4xx_hal.c stm32g4xx_hal_cortex.c stm32g4xx_hal_rcc.c stm32g4xx_hal_rcc_ex.c \
  stm32g4xx_hal_gpio.c stm32g4xx_hal_exti.c stm32g4xx_hal_dma.c stm32g4xx_hal_dma_ex.c \
  stm32g4xx_hal_pwr.c stm32g4xx_hal_pwr_ex.c stm32g4xx_hal_flash.c stm32g4xx_hal_flash_ex.c \
  stm32g4xx_hal_i2c.c stm32g4xx_hal_i2c_ex.c stm32g4xx_hal_uart.c stm32g4xx_hal_uart_ex.c \
  stm32g4xx_hal_tim.c stm32g4xx_hal_tim_ex.c)

SYS_SRC = $(CMSIS_DEV)/Source/Templates/system_stm32g4xx.c
ASM_SRC = $(CMSIS_DEV)/Source/Templates/gcc/startup_stm32g474xx.s

C_SOURCES = $(CORE_COMMON) $(VARIANT_SRC) $(RTT_SRC) $(HAL_SRC) $(SYS_SRC)

C_INCLUDES = \
 -ICore/Inc \
 -IApp/Inc \
 -IApp/Bsp \
 -IApp/Svc \
 -IApp/Fonts \
 -IMiddlewares/SEGGER_RTT \
 -IMiddlewares/kk_oled/include \
 -IMiddlewares/kk_oled/graphics \
 -IMiddlewares/kk_oled/driver \
 -IMiddlewares/kk_ui/include \
 -I$(HAL_DIR)/Inc \
 -I$(HAL_DIR)/Inc/Legacy \
 -I$(CMSIS_DEV)/Include \
 -I$(CMSIS)/Include

C_DEFS = -DUSE_HAL_DRIVER -DSTM32G474xx

ifdef SELFTEST
  ifdef RESCUE
    C_DEFS += -DRESCUE_MODE
  endif
  ifdef PA15TEST
    C_DEFS += -DPA15_TEST=$(PA15TEST)
  endif
  ifdef LOOPBACK
    C_DEFS += -DUART_LOOPBACK
  endif
else
  # 刷新方式：
  #   IT      — 页命令和页数据都走 I2C1 中断（默认，实测最稳）
  #   DMA     — 页数据走 DMA1_Channel1；实测 400kHz 下偶发黑屏（中断->DMA 交接竞争），
  #              同速下 IT 正常，故默认不用
  #   BLOCKING — 同步刷新，最保守，最快退回基线

  ifdef DIAG
    C_DEFS += -DAPP_OLED_DIAG
  endif
  ifdef MOTOR_BENCH
    C_DEFS += -DMOTOR_BENCH
  endif
endif

MCU     = -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
OPT     = -Og -g3
CFLAGS  = -MMD -MP $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -Wall -Wno-unused-parameter \
          -fdata-sections -ffunction-sections
LDFLAGS = $(MCU) -T STM32G474RBTx_FLASH.ld --specs=nano.specs \
          -Wl,-Map=$(BUILD)/$(TARGET).map,--cref -Wl,--gc-sections

CC = arm-none-eabi-gcc
SZ = arm-none-eabi-size
OD = arm-none-eabi-objcopy

OBJECTS  = $(addprefix $(BUILD)/,$(notdir $(C_SOURCES:.c=.o)))
OBJECTS += $(addprefix $(BUILD)/,$(notdir $(ASM_SRC:.s=.o)))

vpath %.c $(sort $(dir $(C_SOURCES)))
vpath %.s $(sort $(dir $(ASM_SRC)))

.PHONY: all clean flash erase size

all: $(BUILD)/$(TARGET).elf $(BUILD)/$(TARGET).hex $(BUILD)/$(TARGET).bin

$(BUILD)/%.o: %.c Makefile | $(BUILD)
	$(CC) -c $(CFLAGS) $< -o $@

$(BUILD)/%.o: %.s Makefile | $(BUILD)
	$(CC) -c $(CFLAGS) -x assembler-with-cpp $< -o $@

$(BUILD)/$(TARGET).elf: $(OBJECTS) Makefile
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	$(SZ) $@

$(BUILD)/%.hex: $(BUILD)/%.elf
	$(OD) -O ihex $< $@

$(BUILD)/%.bin: $(BUILD)/%.elf
	$(OD) -O binary -S $< $@

$(BUILD):
	@mkdir -p "$@"

size: $(BUILD)/$(TARGET).elf
	$(SZ) $<

clean:
	-rm -rf -- build build-app build-diag build-rescue build-pa15 build-loop "-p"

# 烧录优先用 pwsh 7（按 UTF-8 读脚本；5.1 会把无 BOM 的 UTF-8 脚本当 GBK 读，中文注释就报 ParserError）
PSFLASH := $(if $(shell where pwsh),pwsh,powershell)

flash: $(BUILD)/$(TARGET).bin
	@$(PSFLASH) -NoProfile -ExecutionPolicy Bypass -File ../tools/flash.ps1 -Bin $(CURDIR)/$(BUILD)/$(TARGET).bin -JLink "$(JLINK)" -Device $(DEVICE)

# 整片擦除：J-Link 的 erase 是整片（带不带地址范围都一样），擦完必须重新 make flash
erase:

	@(echo si SWD&echo speed 4000&echo connect&echo erase&echo qc) > $(BUILD)/erase.jlink
	"$(JLINK)" -device $(DEVICE) -if SWD -speed 4000 -autoconnect 0 -CommanderScript $(BUILD)/erase.jlink

-include $(wildcard $(BUILD)/*.d)
