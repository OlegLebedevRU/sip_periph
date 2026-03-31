##########################################################################################################################
# Makefile for sip_periph — Release build (STM32F411CEUx)
# Mirrors the Release configuration defined in .cproject / STM32CubeIDE.
##########################################################################################################################

TARGET = sip_periph
BUILD_DIR = Release

#######################################
# Toolchain
#######################################
PREFIX = arm-none-eabi-
CC      = $(PREFIX)gcc
AS      = $(PREFIX)gcc -x assembler-with-cpp
CP      = $(PREFIX)objcopy
SZ      = $(PREFIX)size

HEX = $(CP) -O ihex
BIN = $(CP) -O binary -S

#######################################
# MCU flags (Cortex-M4, FPv4-SP, hard ABI)
#######################################
CPU   = -mcpu=cortex-m4
FPU   = -mfpu=fpv4-sp-d16
FLOAT = -mfloat-abi=hard
MCU   = $(CPU) -mthumb $(FPU) $(FLOAT)

#######################################
# Compiler / assembler defines
#######################################
C_DEFS = -DUSE_HAL_DRIVER -DSTM32F411xE

#######################################
# Include paths
#######################################
C_INCLUDES = \
  -ICore/Inc \
  -IDrivers/STM32F4xx_HAL_Driver/Inc \
  -IDrivers/STM32F4xx_HAL_Driver/Inc/Legacy \
  -IDrivers/CMSIS/Device/ST/STM32F4xx/Include \
  -IDrivers/CMSIS/Include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/include \
  -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS \
  -IMiddlewares/Third_Party/FreeRTOS/Source/CMSIS_RTOS_V2 \
  -IMiddlewares/Third_Party/FreeRTOS/Source/portable/GCC/ARM_CM4F \
  -IMiddlewares/ST/STM32_USB_Device_Library/Core/Inc \
  -IMiddlewares/ST/STM32_USB_Device_Library/Class/CDC/Inc \
  -IUSB_DEVICE/App \
  -IUSB_DEVICE/Target

#######################################
# Source files
#######################################
# Recursive wildcard helper: $(call rwildcard,<dir>,<pattern>)
rwildcard = $(foreach d,$(wildcard $(1:=/*)),$(call rwildcard,$d,$2) $(filter $(subst *,%,$2),$d))

# Discover all .c files from source trees (excludes docs/)
C_SOURCES = $(call rwildcard,Core,*.c) \
            $(call rwildcard,Drivers,*.c) \
            $(call rwildcard,Middlewares,*.c) \
            $(call rwildcard,USB_DEVICE,*.c)

# Startup assembly
ASM_SOURCES = Core/Startup/startup_stm32f411ceux.s

#######################################
# Build flags
#######################################
OPT = -Os

ASFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -g0 -Wall \
          -fdata-sections -ffunction-sections

CFLAGS = $(MCU) $(C_DEFS) $(C_INCLUDES) $(OPT) -g0 -Wall \
         -fdata-sections -ffunction-sections \
         -std=gnu11 \
         -MMD -MP -MF"$(@:%.o=%.d)"

LDSCRIPT = STM32F411CEUX_FLASH.ld

LDFLAGS = $(MCU) -specs=nano.specs \
          -T$(LDSCRIPT) \
          -Wl,-Map=$(BUILD_DIR)/$(TARGET).map,--cref \
          -Wl,--gc-sections \
          -lc -lm -lnosys

#######################################
# Build rules
#######################################
OBJECTS  = $(addprefix $(BUILD_DIR)/,$(C_SOURCES:.c=.o))
OBJECTS += $(addprefix $(BUILD_DIR)/,$(ASM_SOURCES:.s=.o))

.PHONY: all clean

all: $(BUILD_DIR)/$(TARGET).elf $(BUILD_DIR)/$(TARGET).hex $(BUILD_DIR)/$(TARGET).bin
	$(SZ) $<

$(BUILD_DIR)/$(TARGET).elf: $(OBJECTS) $(LDSCRIPT)
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@

$(BUILD_DIR)/$(TARGET).hex: $(BUILD_DIR)/$(TARGET).elf
	$(HEX) $< $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	$(BIN) $< $@

# Compile C sources
$(BUILD_DIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) -c $(CFLAGS) -Wa,-a,-ad,-alms=$(BUILD_DIR)/$(notdir $(<:.c=.lst)) $< -o $@

# Assemble startup
$(BUILD_DIR)/%.o: %.s
	@mkdir -p $(dir $@)
	$(AS) -c $(ASFLAGS) $< -o $@

clean:
	rm -rf $(BUILD_DIR)

# Include auto-generated dependency files
-include $(OBJECTS:.o=.d)
