# Name: Makefile
# Author: <insert your name here>
# Copyright: <insert your copyright message here>
# License: <insert your license reference here>

DEVICE     = atmega644p
CLOCK      = 8000000
PROGRAMMER = -c linuxspi -P /dev/spidev0.0
SRCDIR     = src
BIN        = bin
OBJECTS    = $(SRCDIR)/petdisk.o\
$(SRCDIR)/EspConn.o\
$(SRCDIR)/Serial.o\
$(SRCDIR)/EspHttp.o\
$(SRCDIR)/SD_routines.o\
$(SRCDIR)/SPI_routines.o\
$(SRCDIR)/FAT32.o\
$(SRCDIR)/IEEE488.o\
$(SRCDIR)/NetworkDataSource.o

BOOTLOADER_OBJECTS = $(SRCDIR)/bootloader.o\
$(SRCDIR)/SPI_routines.o\
$(SRCDIR)/SD_routines.o\
$(SRCDIR)/FAT32_tiny.o

FUSES 		= -U lfuse:w:0xc2:m -U hfuse:w:0xda:m -U efuse:w:0xff:m -U lock:w:0xEF:m

# For computing fuse byte values for other devices and options see
# the fuse bit calculator at http://www.engbedded.com/fusecalc/


# Tune the lines below only if you know what you are doing:

AVRDUDE = avrdude $(PROGRAMMER) -p $(DEVICE) -V
COMPILE = avr-g++ -Wall -Os -DF_CPU=$(CLOCK) -mmcu=$(DEVICE) -std=gnu99 $(INCLUDE)
COMPILECPP = avr-g++ -Wall -Os -DF_CPU=$(CLOCK) -mmcu=$(DEVICE) -std=c++11 $(INCLUDE)

BOOTLOADER_ADDR_324_H = 0x7000
BOOTLOADER_ADDR_644_H = 0xF000
BOOTLOADER_ADDR_324_D = 28672
BOOTLOADER_ADDR_644_D = 61440

# symbolic targets:
all:	petdisk.hex

.c.o:
	$(COMPILE) -c $< -o $@

.cpp.o:
	$(COMPILECPP) -c $< -o $@

.S.o:
	$(COMPILE) -x assembler-with-cpp -c $< -o $@
# "-x assembler-with-cpp" should not be necessary since this is the default
# file type for the .S (with capital S) extension. However, upper case
# characters are not always preserved on Windows. To ensure WinAVR
# compatibility define the file type manually.

.c.s:
	$(COMPILE) -S $< -o $@

%.flash:
	$(AVRDUDE) -U flash:w:$*.hex

%.flashbin:
	$(AVRDUDE) -U flash:w:$*.bin -V

fuse:
	$(AVRDUDE) -b115200 $(FUSES)

%.program: %.hex
	make progenable
	sudo make $*.install
	make progdisable

%.programbin: %.bin
	make progenable
	sudo make $*.installbin
	make progdisable

%.install: %.hex
	make fuse
	make $*.flash

%.installbin: %.bin
	make fuse
	make $*.flashbin

progenable:
	gpio mode 12 alt0
	gpio mode 13 alt0
	gpio mode 14 alt0
	gpio mode 6 in

progdisable:
	gpio mode 12 in
	gpio mode 13 in
	gpio mode 14 in
	gpio mode 6 out
	gpio write 6 0
	gpio write 6 1

clean:
	rm -f *.hex *.elf *.o *.bin
	rm -f $(SRCDIR)/*.o
	rm -f $(BIN)/*

$(SRCDIR)/githash.h:
	echo const unsigned char _hash[] PROGMEM = \"$(shell git rev-parse --short HEAD | tr [:lower:] [:upper:])\"\; > $@

# file targets:
%.elf: bindir $(SRCDIR)/githash.h $(OBJECTS)
	$(COMPILECPP) -o $*.elf $(OBJECTS)
	rm -f $(SRCDIR)/githash.h

%.hex: bindir %.elf
	rm -f $*.heximage
	avr-objcopy -j .text -j .data -O ihex $*.elf $*.hex
	avr-size --format=avr --mcu=$(DEVICE) $*.elf
# If you have an EEPROM section, you must also create a hex file for the
# EEPROM and add it to the "flassh" target.

%.bin: bindir %.elf
	rm -f $*.bin
	avr-objcopy -j .text -j .data -O binary $*.elf $*.bin
	avr-size --format=avr --mcu=$(DEVICE) $*.elf

# Targets for code debugging and analysis:
disasm:	%.elf
	avr-objdump -d $*.elf

$(BIN)/bootloader.elf: bindir $(BOOTLOADER_OBJECTS)
	$(COMPILECPP) -Ttext=$(BOOTLOADER_ADDR_644_H) -o $(BIN)/bootloader.elf $(BOOTLOADER_OBJECTS)

# pad the end of the main program with zeros, leaving enough room for the bootloader
$(BIN)/petdisk_and_bootloader.bin: bindir $(BIN)/petdisk.bin $(BIN)/bootloader.bin
	dd if=/dev/zero bs=1 count=$(shell expr $(BOOTLOADER_ADDR_644_D) - $(shell stat --format="%s" $(BIN)/petdisk.bin)) >> $(BIN)/petdisk.bin
	cat $(BIN)/petdisk.bin $(BIN)/bootloader.bin > $(BIN)/petdisk_and_bootloader.bin

bindir:
	mkdir -p bin