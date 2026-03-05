# Makefile for Daikin BRP069A42 reversed firmware
#
# Cross-compile for ARM Cortex-M3 (STM32F2xx)
# Requires: arm-none-eabi-gcc toolchain

CC      = arm-none-eabi-gcc
OBJCOPY = arm-none-eabi-objcopy
OBJDUMP = arm-none-eabi-objdump
SIZE    = arm-none-eabi-size

# STM32F207 target
MCU     = -mcpu=cortex-m3 -mthumb -mfloat-abi=soft

CFLAGS  = $(MCU) -O2 -Wall -Wextra \
          -ffunction-sections -fdata-sections \
          -DSTM32F2XX \
          -I.

LDFLAGS = $(MCU) \
          -Wl,--gc-sections \
          -Wl,-Map=$(TARGET).map \
          -T stm32f207.ld \
          --specs=nosys.specs

TARGET  = daikin_brp069a42

SRCS    = startup.c \
          main.c \
          uart.c \
          network.c \
          http_server.c \
          aircon_api.c \
          common_api.c \
          echonet.c \
          cloud.c \
          util.c

OBJS    = $(SRCS:.c=.o)

all: $(TARGET).elf $(TARGET).bin

$(TARGET).elf: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^
	$(SIZE) $@

$(TARGET).bin: $(TARGET).elf
	$(OBJCOPY) -O binary $< $@

$(TARGET).lst: $(TARGET).elf
	$(OBJDUMP) -d -S $< > $@

%.o: %.c daikin.h
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) $(TARGET).elf $(TARGET).bin $(TARGET).map $(TARGET).lst

flash: $(TARGET).bin
	# Flash via OpenOCD: openocd -f board/stm32f2x.cfg \
	#   -c "program $(TARGET).bin 0x08000000 verify reset exit"

disasm: $(TARGET).elf
	$(OBJDUMP) -d $(TARGET).elf > $(TARGET).lst

.PHONY: all clean flash disasm
