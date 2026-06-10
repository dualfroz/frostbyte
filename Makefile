CC := gcc
LD := ld

CFLAGS := -std=gnu11 -ffreestanding -nostdlib \
          -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel \
          -Wall -Wextra -Iinclude -g -c

ASFLAGS := -ffreestanding -Iinclude -g -c

LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -T linker.ld

UCFLAGS := -std=gnu11 -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -fno-asynchronous-unwind-tables \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -Wall -Wextra -Iinclude -Iuser -g -c

UASFLAGS := -ffreestanding -Iinclude -Iuser -g -c

ULDFLAGS := -z max-page-size=0x1000 --no-warn-rwx-segments -e _start -T user/user.ld

BUILD := build
SRC := src
USER := user
UBUILD := $(BUILD)/user

COBJS := $(patsubst $(SRC)/%.c,$(BUILD)/%.o,$(wildcard $(SRC)/*.c))
SOBJS := $(patsubst $(SRC)/%.S,$(BUILD)/%.o,$(wildcard $(SRC)/*.S))
OBJS := $(SOBJS) $(COBJS)

USERCOMMON := $(UBUILD)/crt.o $(UBUILD)/ulib.o
USERPROGRAMS := $(UBUILD)/hello.elf $(UBUILD)/counter.elf \
                $(UBUILD)/sbrktest.elf $(UBUILD)/fileread.elf

FSFILES := fsroot/etc/motd.txt fsroot/etc/readme.txt fsroot/home/notes.txt

KERNEL := $(BUILD)/frostbyte.elf
INITRD := $(BUILD)/initrd.img
MKTAR := $(BUILD)/mktar
ISO := os.iso

.PHONY: all clean run runserial test

all: $(ISO)

$(BUILD):
	@mkdir -p $(BUILD)

$(UBUILD):
	@mkdir -p $(UBUILD)

$(BUILD)/%.o: $(SRC)/%.c | $(BUILD)
	$(CC) $(CFLAGS) $< -o $@

$(BUILD)/%.o: $(SRC)/%.S | $(BUILD)
	$(CC) $(ASFLAGS) $< -o $@

$(KERNEL): $(OBJS) linker.ld
	$(LD) $(LDFLAGS) -o $@ $(OBJS)

$(UBUILD)/%.o: $(USER)/%.c | $(UBUILD)
	$(CC) $(UCFLAGS) $< -o $@

$(UBUILD)/%.o: $(USER)/%.S | $(UBUILD)
	$(CC) $(UASFLAGS) $< -o $@

$(UBUILD)/hello.elf: $(UBUILD)/hello.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x40000000 -o $@ $(UBUILD)/hello.o $(USERCOMMON)

$(UBUILD)/counter.elf: $(UBUILD)/counter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x50000000 -o $@ $(UBUILD)/counter.o $(USERCOMMON)

$(UBUILD)/sbrktest.elf: $(UBUILD)/sbrktest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x60000000 -o $@ $(UBUILD)/sbrktest.o $(USERCOMMON)

$(UBUILD)/fileread.elf: $(UBUILD)/fileread.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x70000000 -o $@ $(UBUILD)/fileread.o $(USERCOMMON)

$(MKTAR): tools/mktar.c | $(BUILD)
	gcc -O2 -Wall -Wextra -o $@ $<

$(INITRD): $(MKTAR) $(USERPROGRAMS) $(FSFILES)
	$(MKTAR) $@ \
	  --dir bin --dir etc --dir home \
	  --file bin/hello $(UBUILD)/hello.elf \
	  --file bin/counter $(UBUILD)/counter.elf \
	  --file bin/sbrktest $(UBUILD)/sbrktest.elf \
	  --file bin/fileread $(UBUILD)/fileread.elf \
	  --file etc/motd.txt fsroot/etc/motd.txt \
	  --file etc/readme.txt fsroot/etc/readme.txt \
	  --file home/notes.txt fsroot/home/notes.txt

$(ISO): $(KERNEL) $(INITRD) iso/boot/grub/grub.cfg
	@cp $(KERNEL) iso/boot/frostbyte.elf
	@cp $(INITRD) iso/boot/initrd.img
	grub-mkrescue -o $(ISO) iso 2>/dev/null

run: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -m 128M

runserial: $(ISO)
	qemu-system-x86_64 -cdrom $(ISO) -serial stdio -display none -m 128M -no-reboot

test: $(ISO)
	sh tools/ci-test.sh

clean:
	rm -rf $(BUILD) $(ISO) iso/boot/frostbyte.elf iso/boot/initrd.img
