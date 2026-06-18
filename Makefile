CC := gcc
LD := ld

CFLAGS := -std=gnu11 -ffreestanding -nostdlib \
          -fno-stack-protector -fno-pic -fno-pie \
          -fno-asynchronous-unwind-tables \
          -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
          -mcmodel=kernel \
          -Wall -Wextra -Iinclude -MMD -MP -g -c

ASFLAGS := -ffreestanding -Iinclude -g -c

LDFLAGS := -nostdlib -z max-page-size=0x1000 --no-warn-rwx-segments -T linker.ld

UCFLAGS := -std=gnu11 -ffreestanding -nostdlib \
           -fno-stack-protector -fno-pic -fno-pie \
           -fno-asynchronous-unwind-tables \
           -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
           -mcmodel=large \
           -Wall -Wextra -Iinclude -Iuser -MMD -MP -g -c

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
                $(UBUILD)/sbrktest.elf $(UBUILD)/fileread.elf \
                $(UBUILD)/pipetest.elf $(UBUILD)/disktest.elf \
                $(UBUILD)/fstest.elf $(UBUILD)/linktest.elf \
                $(UBUILD)/spawntarget.elf $(UBUILD)/exectarget.elf \
                $(UBUILD)/proctest.elf $(UBUILD)/argtest.elf \
                $(UBUILD)/fifomaster.elf $(UBUILD)/fifowriter.elf \
                $(UBUILD)/fiforeader.elf $(UBUILD)/killtest.elf \
                $(UBUILD)/killvictim.elf $(UBUILD)/namedreader.elf \
                $(UBUILD)/namedwriter.elf $(UBUILD)/consolereader1.elf \
                $(UBUILD)/consolereader2.elf $(UBUILD)/sigtest.elf \
                $(UBUILD)/sigtarget.elf $(UBUILD)/sigdefault.elf \
                $(UBUILD)/jobtest.elf $(UBUILD)/jobtarget.elf \
                $(UBUILD)/nesttest.elf $(UBUILD)/nesttarget.elf \
                $(UBUILD)/eintrtest.elf $(UBUILD)/eintrtarget.elf \
                $(UBUILD)/restarttest.elf $(UBUILD)/restarttarget.elf \
                $(UBUILD)/masktest.elf $(UBUILD)/masktarget.elf \
                $(UBUILD)/sigchldtest.elf $(UBUILD)/sigchldchild.elf \
                $(UBUILD)/waitanytest.elf $(UBUILD)/waitanyfast.elf \
                $(UBUILD)/waitanyslow.elf \
                $(UBUILD)/stopchldtest.elf $(UBUILD)/stopchldtarget.elf \
                $(UBUILD)/nodefertest.elf \
                $(UBUILD)/alarmtest.elf $(UBUILD)/alarmdefault.elf \
                $(UBUILD)/siginfotest.elf $(UBUILD)/siginfotarget.elf \
                $(UBUILD)/pgroupfg.elf $(UBUILD)/pgroupbg.elf \
                $(UBUILD)/jobctlfg.elf $(UBUILD)/jobctlbg.elf \
                $(UBUILD)/polltest.elf $(UBUILD)/pollwriter.elf \
                $(UBUILD)/pipesrc.elf $(UBUILD)/pipesink.elf \
                $(UBUILD)/redirwriter.elf $(UBUILD)/redirreader.elf \
                $(UBUILD)/stderrwriter.elf $(UBUILD)/stderrreader.elf \
                $(UBUILD)/selfstoptest.elf $(UBUILD)/selfstoptarget.elf \
                $(UBUILD)/herestest.elf \
                $(UBUILD)/hcat.elf $(UBUILD)/hsink.elf \
                $(UBUILD)/pipecat.elf \
                $(UBUILD)/cwdtest.elf $(UBUILD)/cwdtarget.elf \
                $(UBUILD)/envprint.elf \
                $(UBUILD)/nicetest.elf $(UBUILD)/nicetarget.elf \
                $(UBUILD)/date.elf \
                $(UBUILD)/globcat.elf \
                $(UBUILD)/timetest.elf \
                $(UBUILD)/false.elf \
                $(UBUILD)/argecho.elf \
                $(UBUILD)/rmtest.elf \
                $(UBUILD)/permtest.elf \
                $(UBUILD)/identtest.elf \
                $(UBUILD)/dirpermtest.elf \
                $(UBUILD)/grouptest.elf \
                $(UBUILD)/pidfdtest.elf \
                $(UBUILD)/mmaptest.elf $(UBUILD)/mmapprotvictim.elf \
                $(UBUILD)/mmapexecvictim.elf

FSFILES := fsroot/etc/motd.txt fsroot/etc/readme.txt fsroot/home/notes.txt

KERNEL := $(BUILD)/frostbyte.elf
INITRD := $(BUILD)/initrd.img
MKTAR := $(BUILD)/mktar
ISO := os.iso
DISK := $(BUILD)/disk.img
DISKFLAGS := -drive file=$(DISK),format=raw,if=ide

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

$(UBUILD)/pipetest.elf: $(UBUILD)/pipetest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x80000000 -o $@ $(UBUILD)/pipetest.o $(USERCOMMON)

$(UBUILD)/disktest.elf: $(UBUILD)/disktest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x90000000 -o $@ $(UBUILD)/disktest.o $(USERCOMMON)

$(UBUILD)/fstest.elf: $(UBUILD)/fstest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0xA0000000 -o $@ $(UBUILD)/fstest.o $(USERCOMMON)

$(UBUILD)/linktest.elf: $(UBUILD)/linktest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0xB0000000 -o $@ $(UBUILD)/linktest.o $(USERCOMMON)

#  Note: 0xC0000000 is deliberately skipped -- kmain.c's runPagingTest()
# hardcodes a single supervisor-only diagnostic page at that exact address
# for the whole kernel's lifetime, which would silently collide with a
# user binary's first page (elfLoad's pagingMapUserRange only maps pages
# that are not already present, so it would find that one "already mapped"
# and leave it non-user-accessible, faulting the instant ring 3 tried to
# execute from it).
$(UBUILD)/spawntarget.elf: $(UBUILD)/spawntarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0xD0000000 -o $@ $(UBUILD)/spawntarget.o $(USERCOMMON)

# exectarget is never listed in kmain.c's boot-time spawnUser calls -- it is
# only ever reached via proctest.c's userExec, which derives its stack/brk
# purely from this ELF's own entry point (elfSlotStackTop/elfSlotBrkStart,
# elf.c/elf.h). Its USER_BASE still needs to be reserved here so it never
# collides with another binary's slot.
$(UBUILD)/exectarget.elf: $(UBUILD)/exectarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0xE0000000 -o $@ $(UBUILD)/exectarget.o $(USERCOMMON)

$(UBUILD)/proctest.elf: $(UBUILD)/proctest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0xF0000000 -o $@ $(UBUILD)/proctest.o $(USERCOMMON)

# Like exectarget, argtest is never in kmain.c's boot-time spawnUser list --
# only proctest.c's userSpawn (with a specific argv) ever launches it. Its
# slot picks up right where proctest's leaves off (0xF0000000 + elfSlotSize).
$(UBUILD)/argtest.elf: $(UBUILD)/argtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x100000000 -o $@ $(UBUILD)/argtest.o $(USERCOMMON)

# fifomaster IS in kmain.c's boot-time spawnUser list (it needs no argv of
# its own); fifowriter/fiforeader are only ever spawned by fifomaster, each
# with the shared pipe handle it created as their one argv.
$(UBUILD)/fifomaster.elf: $(UBUILD)/fifomaster.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x110000000 -o $@ $(UBUILD)/fifomaster.o $(USERCOMMON)

$(UBUILD)/fifowriter.elf: $(UBUILD)/fifowriter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x120000000 -o $@ $(UBUILD)/fifowriter.o $(USERCOMMON)

$(UBUILD)/fiforeader.elf: $(UBUILD)/fiforeader.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x130000000 -o $@ $(UBUILD)/fiforeader.o $(USERCOMMON)

# killtest is in kmain.c's boot-time spawnUser list; killvictim is only ever
# spawned by killtest, and is designed to never return on its own (see
# killvictim.c) -- proving threadKill (sched.c) is the only way out.
$(UBUILD)/killtest.elf: $(UBUILD)/killtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x140000000 -o $@ $(UBUILD)/killtest.o $(USERCOMMON)

$(UBUILD)/killvictim.elf: $(UBUILD)/killvictim.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x150000000 -o $@ $(UBUILD)/killvictim.o $(USERCOMMON)

# namedreader/namedwriter are both in kmain.c's boot-time spawnUser list,
# with no parent-child relationship to each other -- they rendezvous purely
# through the vfsMkfifo'd path "/tmp/rendezvous", not a shared argv handle.
$(UBUILD)/namedreader.elf: $(UBUILD)/namedreader.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x160000000 -o $@ $(UBUILD)/namedreader.o $(USERCOMMON)

$(UBUILD)/namedwriter.elf: $(UBUILD)/namedwriter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x170000000 -o $@ $(UBUILD)/namedwriter.o $(USERCOMMON)

# consolereader1/consolereader2 are both in kmain.c's boot-time spawnUser
# list, with no relationship to each other -- each simply reads its own fd 0,
# which keyboard.c's per-reader fan-out delivers a synthetic keystroke to
# independently (consoleInjectorThread, kmain.c).
$(UBUILD)/consolereader1.elf: $(UBUILD)/consolereader1.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x180000000 -o $@ $(UBUILD)/consolereader1.o $(USERCOMMON)

$(UBUILD)/consolereader2.elf: $(UBUILD)/consolereader2.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x190000000 -o $@ $(UBUILD)/consolereader2.o $(USERCOMMON)

# sigtest is spawned directly by kmain.c at boot; sigtarget/sigdefault are
# only ever spawned at runtime by sigtest itself (userSpawn), the same
# relationship killtest.elf/killvictim.elf already have -- each still needs
# its own reserved USER_BASE regardless of who spawns it, since elfLoad
# maps every ELF at whatever fixed address it was linked at and all of
# them share this one address space (see elf.h).
$(UBUILD)/sigtest.elf: $(UBUILD)/sigtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1A0000000 -o $@ $(UBUILD)/sigtest.o $(USERCOMMON)

$(UBUILD)/sigtarget.elf: $(UBUILD)/sigtarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1B0000000 -o $@ $(UBUILD)/sigtarget.o $(USERCOMMON)

$(UBUILD)/sigdefault.elf: $(UBUILD)/sigdefault.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1C0000000 -o $@ $(UBUILD)/sigdefault.o $(USERCOMMON)

# jobtest is spawned directly by kmain.c at boot; jobtarget is only ever
# spawned at runtime by jobtest itself (userSpawn), the same relationship
# sigtest.elf/sigtarget.elf above already have.
$(UBUILD)/jobtest.elf: $(UBUILD)/jobtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1D0000000 -o $@ $(UBUILD)/jobtest.o $(USERCOMMON)

$(UBUILD)/jobtarget.elf: $(UBUILD)/jobtarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1E0000000 -o $@ $(UBUILD)/jobtarget.o $(USERCOMMON)

# nesttest is spawned directly by kmain.c at boot; nesttarget is only ever
# spawned at runtime by nesttest itself (userSpawn), the same relationship
# sigtest.elf/sigtarget.elf and jobtest.elf/jobtarget.elf above already have.
$(UBUILD)/nesttest.elf: $(UBUILD)/nesttest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x1F0000000 -o $@ $(UBUILD)/nesttest.o $(USERCOMMON)

$(UBUILD)/nesttarget.elf: $(UBUILD)/nesttarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x200000000 -o $@ $(UBUILD)/nesttarget.o $(USERCOMMON)

# eintrtest is spawned directly by kmain.c at boot; eintrtarget is only ever
# spawned at runtime by eintrtest itself (userSpawn), the same relationship
# killtest.elf/killvictim.elf above already have.
$(UBUILD)/eintrtest.elf: $(UBUILD)/eintrtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x210000000 -o $@ $(UBUILD)/eintrtest.o $(USERCOMMON)

$(UBUILD)/eintrtarget.elf: $(UBUILD)/eintrtarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x220000000 -o $@ $(UBUILD)/eintrtarget.o $(USERCOMMON)

# restarttest is spawned directly by kmain.c at boot; restarttarget is only
# ever spawned at runtime by restarttest itself (userSpawn), the same
# relationship eintrtest.elf/eintrtarget.elf above already have.
$(UBUILD)/restarttest.elf: $(UBUILD)/restarttest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x230000000 -o $@ $(UBUILD)/restarttest.o $(USERCOMMON)

$(UBUILD)/restarttarget.elf: $(UBUILD)/restarttarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x240000000 -o $@ $(UBUILD)/restarttarget.o $(USERCOMMON)

# masktest is spawned directly by kmain.c at boot; masktarget is only ever
# spawned at runtime by masktest itself (userSpawn), the same relationship
# nesttest.elf/nesttarget.elf above already have.
$(UBUILD)/masktest.elf: $(UBUILD)/masktest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x250000000 -o $@ $(UBUILD)/masktest.o $(USERCOMMON)

$(UBUILD)/masktarget.elf: $(UBUILD)/masktarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x260000000 -o $@ $(UBUILD)/masktarget.o $(USERCOMMON)

# sigchldtest is spawned directly by kmain.c at boot; sigchldchild is only
# ever spawned at runtime by sigchldtest itself (userSpawn), the same
# relationship nesttest.elf/nesttarget.elf above already have.
$(UBUILD)/sigchldtest.elf: $(UBUILD)/sigchldtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x270000000 -o $@ $(UBUILD)/sigchldtest.o $(USERCOMMON)

$(UBUILD)/sigchldchild.elf: $(UBUILD)/sigchldchild.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x280000000 -o $@ $(UBUILD)/sigchldchild.o $(USERCOMMON)

# waitanytest is spawned directly by kmain.c at boot; waitanyfast and
# waitanyslow are only ever spawned at runtime by waitanytest itself
# (userSpawn), the same relationship nesttest.elf/nesttarget.elf above
# already have.
$(UBUILD)/waitanytest.elf: $(UBUILD)/waitanytest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x290000000 -o $@ $(UBUILD)/waitanytest.o $(USERCOMMON)

$(UBUILD)/waitanyfast.elf: $(UBUILD)/waitanyfast.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2A0000000 -o $@ $(UBUILD)/waitanyfast.o $(USERCOMMON)

$(UBUILD)/waitanyslow.elf: $(UBUILD)/waitanyslow.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2B0000000 -o $@ $(UBUILD)/waitanyslow.o $(USERCOMMON)

# stopchldtest is spawned directly by kmain.c at boot; stopchldtarget is
# only ever spawned at runtime by stopchldtest itself (userSpawn), the same
# relationship jobtest.elf/jobtarget.elf above already have.
$(UBUILD)/stopchldtest.elf: $(UBUILD)/stopchldtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2C0000000 -o $@ $(UBUILD)/stopchldtest.o $(USERCOMMON)

$(UBUILD)/stopchldtarget.elf: $(UBUILD)/stopchldtarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2D0000000 -o $@ $(UBUILD)/stopchldtarget.o $(USERCOMMON)

# nodefertest is spawned directly by kmain.c at boot; it is a single,
# self-contained process (self-raises signals against its own pid) with no
# spawned counterpart, unlike every other pair above.
$(UBUILD)/nodefertest.elf: $(UBUILD)/nodefertest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2E0000000 -o $@ $(UBUILD)/nodefertest.o $(USERCOMMON)

$(UBUILD)/alarmtest.elf: $(UBUILD)/alarmtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x2F0000000 -o $@ $(UBUILD)/alarmtest.o $(USERCOMMON)

$(UBUILD)/alarmdefault.elf: $(UBUILD)/alarmdefault.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x300000000 -o $@ $(UBUILD)/alarmdefault.o $(USERCOMMON)

$(UBUILD)/siginfotest.elf: $(UBUILD)/siginfotest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x310000000 -o $@ $(UBUILD)/siginfotest.o $(USERCOMMON)

$(UBUILD)/siginfotarget.elf: $(UBUILD)/siginfotarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x320000000 -o $@ $(UBUILD)/siginfotarget.o $(USERCOMMON)

# pgroupfg/pgroupbg are both spawned directly by kmain.c at boot, with no
# parent-child relationship to each other (unlike every *test/*target pair
# above) -- each simply becomes its own default process group leader
# (sched.c's initialPgid), the same way two independently boot-launched
# programs already do for consolereader1/consolereader2 above.
$(UBUILD)/pgroupfg.elf: $(UBUILD)/pgroupfg.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x330000000 -o $@ $(UBUILD)/pgroupfg.o $(USERCOMMON)

$(UBUILD)/pgroupbg.elf: $(UBUILD)/pgroupbg.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x340000000 -o $@ $(UBUILD)/pgroupbg.o $(USERCOMMON)

# jobctlfg/jobctlbg are never spawned by kmain.c at all -- unlike every
# other program in this list, they are loaded on demand, at runtime, when
# kmain.c's jobctlDriverThread types their names at the real interactive
# shell (shell.c), which resolves them through jobctl.c exactly like a real
# shell's $PATH lookup. They still need their own fixed link slot up front
# like any other binary (elf.h's elfLoad maps a binary at whatever address
# it was linked at, chosen here, not at load time).
$(UBUILD)/jobctlfg.elf: $(UBUILD)/jobctlfg.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x350000000 -o $@ $(UBUILD)/jobctlfg.o $(USERCOMMON)

$(UBUILD)/jobctlbg.elf: $(UBUILD)/jobctlbg.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x360000000 -o $@ $(UBUILD)/jobctlbg.o $(USERCOMMON)

$(UBUILD)/polltest.elf: $(UBUILD)/polltest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x370000000 -o $@ $(UBUILD)/polltest.o $(USERCOMMON)

$(UBUILD)/pollwriter.elf: $(UBUILD)/pollwriter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x380000000 -o $@ $(UBUILD)/pollwriter.o $(USERCOMMON)

# pipesrc/pipesink are, like jobctlfg/jobctlbg above, never spawned by
# kmain.c directly -- loaded on demand when the real interactive shell
# processes a typed "pipesrc | pipesink" line (shell.c's new pipeline
# syntax, kmain.c's pipelineDriverThread).
$(UBUILD)/pipesrc.elf: $(UBUILD)/pipesrc.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x390000000 -o $@ $(UBUILD)/pipesrc.o $(USERCOMMON)

$(UBUILD)/pipesink.elf: $(UBUILD)/pipesink.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3A0000000 -o $@ $(UBUILD)/pipesink.o $(USERCOMMON)

# redirwriter/redirreader are, like pipesrc/pipesink above, never spawned by
# kmain.c directly -- loaded on demand when the real interactive shell
# processes typed "redirwriter > FILE" / "redirwriter >> FILE" /
# "redirreader < FILE" lines (shell.c's new file redirection syntax,
# kmain.c's redirDriverThread).
$(UBUILD)/redirwriter.elf: $(UBUILD)/redirwriter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3B0000000 -o $@ $(UBUILD)/redirwriter.o $(USERCOMMON)

$(UBUILD)/redirreader.elf: $(UBUILD)/redirreader.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3C0000000 -o $@ $(UBUILD)/redirreader.o $(USERCOMMON)

# stderrwriter/stderrreader are, like redirwriter/redirreader above, never
# spawned by kmain.c directly -- loaded on demand when the real interactive
# shell processes typed "stderrwriter 2> FILE" / "stderrwriter 2>> FILE" /
# "stderrreader < FILE" lines (shell.c's new stderr redirection syntax,
# kmain.c's stderrDriverThread).
$(UBUILD)/stderrwriter.elf: $(UBUILD)/stderrwriter.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3D0000000 -o $@ $(UBUILD)/stderrwriter.o $(USERCOMMON)

$(UBUILD)/stderrreader.elf: $(UBUILD)/stderrreader.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3E0000000 -o $@ $(UBUILD)/stderrreader.o $(USERCOMMON)

# selfstoptest is spawned directly by kmain.c at boot; selfstoptarget is
# only ever spawned at runtime by selfstoptest itself (userSpawn), the same
# relationship jobtest.elf/jobtarget.elf above already have.
$(UBUILD)/selfstoptest.elf: $(UBUILD)/selfstoptest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x3F0000000 -o $@ $(UBUILD)/selfstoptest.o $(USERCOMMON)

$(UBUILD)/selfstoptarget.elf: $(UBUILD)/selfstoptarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x400000000 -o $@ $(UBUILD)/selfstoptarget.o $(USERCOMMON)

# herestest is, like redirreader/stderrreader above, never spawned by kmain.c
# directly -- loaded on demand when the real interactive shell processes a
# typed "herestest <<< TEXT" line (shell.c's new here-string redirection
# syntax, kmain.c's hereDriverThread). Unlike redirreader/stderrreader it has
# no writer counterpart binary at all -- the "writer" is the shell's own
# kernel thread, feeding the typed text straight into a pipe (jobctl.c's
# jobctlSpawnHereStringPiped), never another spawned process's stdout.
$(UBUILD)/herestest.elf: $(UBUILD)/herestest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x410000000 -o $@ $(UBUILD)/herestest.o $(USERCOMMON)

# hcat/hsink are, like pipesrc/pipesink above, never spawned by kmain.c
# directly -- loaded on demand when the real interactive shell processes a
# typed "hcat <<< TEXT | hsink" line (shell.c's new here-string-into-
# pipeline syntax, jobctl.c's jobctlSpawnPipedHereString, kmain.c's
# hereStringPipelineDriverThread).
$(UBUILD)/hcat.elf: $(UBUILD)/hcat.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x420000000 -o $@ $(UBUILD)/hcat.o $(USERCOMMON)

$(UBUILD)/hsink.elf: $(UBUILD)/hsink.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x430000000 -o $@ $(UBUILD)/hsink.o $(USERCOMMON)

# pipecat is, like pipesrc/pipesink above, never spawned by kmain.c directly
# -- loaded on demand when the real interactive shell processes a typed
# "pipesrc | pipecat | pipesink" line (shell.c's new N-stage pipeline
# syntax, kmain.c's threeStagePipelineDriverThread). It is the middle stage
# of that pipeline: unlike hcat/hsink above, both its fd 0 and fd 1 are real
# process-to-process pipes, never the console or a here-string.
$(UBUILD)/pipecat.elf: $(UBUILD)/pipecat.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x440000000 -o $@ $(UBUILD)/pipecat.o $(USERCOMMON)

# cwdtest is spawned directly by kmain.c at boot; cwdtarget is only ever
# spawned at runtime by cwdtest itself (userSpawn, no argv), the same
# relationship jobtest.elf/jobtarget.elf above already have -- except this
# pair rendezvous purely through the real per-process cwd the kernel itself
# hands cwdtarget at creation time (sched.c's initialCwd), not a shared
# pipe handle passed as an argv string.
$(UBUILD)/cwdtest.elf: $(UBUILD)/cwdtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x450000000 -o $@ $(UBUILD)/cwdtest.o $(USERCOMMON)

$(UBUILD)/cwdtarget.elf: $(UBUILD)/cwdtarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x460000000 -o $@ $(UBUILD)/cwdtarget.o $(USERCOMMON)

# envprint is, like jobctlfg/jobctlbg/hcat/pipecat above, never spawned by
# kmain.c directly -- loaded on demand when the real interactive shell
# processes a typed "envprint" line (shell.c's fallback /bin/<word> lookup,
# kmain.c's new envDriverThread).
$(UBUILD)/envprint.elf: $(UBUILD)/envprint.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x470000000 -o $@ $(UBUILD)/envprint.o $(USERCOMMON)

# nicetest is spawned directly by kmain.c at boot; nicetarget is only ever
# spawned at runtime by nicetest itself (userSpawn, no argv), the same
# *test / *target relationship cwdtest.elf/cwdtarget.elf above already
# have -- this pair rendezvous purely through sysNice/sysGetPriority
# (syscall.c), never a shared pipe handle or the filesystem.
$(UBUILD)/nicetest.elf: $(UBUILD)/nicetest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x480000000 -o $@ $(UBUILD)/nicetest.o $(USERCOMMON)

$(UBUILD)/nicetarget.elf: $(UBUILD)/nicetarget.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x490000000 -o $@ $(UBUILD)/nicetarget.o $(USERCOMMON)

# date is spawned directly by kmain.c at boot, fully synchronous like
# nicetest.elf above -- no shell rendezvous, no injected keystroke.
$(UBUILD)/date.elf: $(UBUILD)/date.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x4A0000000 -o $@ $(UBUILD)/date.o $(USERCOMMON)

# globcat is, like envprint/hcat/pipecat above, never spawned by kmain.c
# directly -- loaded on demand when the real interactive shell processes a
# typed "globcat /tmp/gd/*.txt" line (shell.c's new wildcard glob
# expansion, kmain.c's new globDriverThread). USER_BASE picks up well past
# the last-used slot (date.elf, plus its own boot-time spawnUser stack
# address) rather than the immediately-following one, to stay clear of any
# address this file's own boot-time spawnUser calls already reference.
$(UBUILD)/globcat.elf: $(UBUILD)/globcat.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x4C0000000 -o $@ $(UBUILD)/globcat.o $(USERCOMMON)

# timetest is spawned directly by kmain.c at boot, fully synchronous like
# nicetest.elf/date.elf above -- no shell rendezvous, no injected keystroke.
# USER_BASE is the slot right after globcat.elf's (the last one reserved);
# nothing else references any address in or past this slot.
$(UBUILD)/timetest.elf: $(UBUILD)/timetest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x4D0000000 -o $@ $(UBUILD)/timetest.o $(USERCOMMON)

# false is, like globcat above, never spawned by kmain.c directly --
# loaded on demand when the real interactive shell processes a typed
# `false && echo X || echo ...` line (shell.c's new `&&`/`||` conditional
# chaining, kmain.c's new andOrDriverThread). USER_BASE picks up the slot
# right after timetest.elf's (the last one reserved).
$(UBUILD)/false.elf: $(UBUILD)/false.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x4E0000000 -o $@ $(UBUILD)/false.o $(USERCOMMON)

# argecho is, like globcat/false above, never spawned by kmain.c directly --
# loaded on demand when the real interactive shell processes a typed
# `argecho $?` line (shell.c's new `$?` expansion, kmain.c's extended
# andOrDriverThread). USER_BASE picks up the slot right after false.elf's
# (the last one reserved).
$(UBUILD)/argecho.elf: $(UBUILD)/argecho.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x4F0000000 -o $@ $(UBUILD)/argecho.o $(USERCOMMON)

# rmtest is spawned directly by kmain.c at boot, fully synchronous like
# timetest.elf/date.elf/nicetest.elf above -- no shell rendezvous, no
# injected keystroke. USER_BASE is the slot right after argecho.elf's (the
# last one reserved); nothing else references any address in or past this
# slot.
$(UBUILD)/rmtest.elf: $(UBUILD)/rmtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x500000000 -o $@ $(UBUILD)/rmtest.o $(USERCOMMON)

# permtest is spawned directly by kmain.c at boot, fully synchronous like
# rmtest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after rmtest.elf's (the last one reserved); nothing
# else references any address in or past this slot.
$(UBUILD)/permtest.elf: $(UBUILD)/permtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x510000000 -o $@ $(UBUILD)/permtest.o $(USERCOMMON)

# identtest is spawned directly by kmain.c at boot, fully synchronous like
# permtest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after permtest.elf's (the last one reserved); nothing
# else references any address in or past this slot.
$(UBUILD)/identtest.elf: $(UBUILD)/identtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x520000000 -o $@ $(UBUILD)/identtest.o $(USERCOMMON)

# dirpermtest is spawned directly by kmain.c at boot, fully synchronous like
# identtest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after identtest.elf's (the last one reserved); nothing
# else references any address in or past this slot.
$(UBUILD)/dirpermtest.elf: $(UBUILD)/dirpermtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x530000000 -o $@ $(UBUILD)/dirpermtest.o $(USERCOMMON)

# grouptest is spawned directly by kmain.c at boot, fully synchronous like
# dirpermtest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after dirpermtest.elf's (the last one reserved); nothing
# else references any address in or past this slot.
$(UBUILD)/grouptest.elf: $(UBUILD)/grouptest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x540000000 -o $@ $(UBUILD)/grouptest.o $(USERCOMMON)

# pidfdtest is spawned directly by kmain.c at boot, fully synchronous like
# grouptest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after grouptest.elf's (the last one reserved, verified by
# scanning every USER_BASE in this Makefile, not just assumed to be "one past
# the highest" -- see kmain.c's own nicetest/date address-space inconsistency,
# left as a warning for exactly this kind of addition); nothing else
# references any address in or past this slot.
$(UBUILD)/pidfdtest.elf: $(UBUILD)/pidfdtest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x550000000 -o $@ $(UBUILD)/pidfdtest.o $(USERCOMMON)

# mmaptest is spawned directly by kmain.c at boot, fully synchronous like
# pidfdtest.elf above -- no shell rendezvous, no injected keystroke. USER_BASE
# is the slot right after pidfdtest.elf's (the last one reserved, verified by
# scanning every USER_BASE in this Makefile); nothing else references any
# address in or past this slot.
$(UBUILD)/mmaptest.elf: $(UBUILD)/mmaptest.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x560000000 -o $@ $(UBUILD)/mmaptest.o $(USERCOMMON)

# mmapprotvictim is only ever spawned at RUNTIME by mmaptest.elf itself
# (userSpawn), never directly by kmain.c -- same relationship killvictim.elf
# already has to killtest.elf. USER_BASE is the slot right after
# mmaptest.elf's own.
$(UBUILD)/mmapprotvictim.elf: $(UBUILD)/mmapprotvictim.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x570000000 -o $@ $(UBUILD)/mmapprotvictim.o $(USERCOMMON)

# mmapexecvictim is only ever spawned at RUNTIME by mmaptest.elf itself
# (userSpawn), exactly like mmapprotvictim.elf above -- never directly by
# kmain.c. USER_BASE is the slot right after mmapprotvictim.elf's own.
$(UBUILD)/mmapexecvictim.elf: $(UBUILD)/mmapexecvictim.o $(USERCOMMON) user/user.ld
	$(LD) $(ULDFLAGS) --defsym USER_BASE=0x580000000 -o $@ $(UBUILD)/mmapexecvictim.o $(USERCOMMON)

$(MKTAR): tools/mktar.c | $(BUILD)
	gcc -O2 -Wall -Wextra -o $@ $<

$(INITRD): $(MKTAR) $(USERPROGRAMS) $(FSFILES)
	$(MKTAR) $@ \
	  --dir bin --dir etc --dir home \
	  --file bin/hello $(UBUILD)/hello.elf \
	  --file bin/counter $(UBUILD)/counter.elf \
	  --file bin/sbrktest $(UBUILD)/sbrktest.elf \
	  --file bin/fileread $(UBUILD)/fileread.elf \
	  --file bin/pipetest $(UBUILD)/pipetest.elf \
	  --file bin/disktest $(UBUILD)/disktest.elf \
	  --file bin/fstest $(UBUILD)/fstest.elf \
	  --file bin/linktest $(UBUILD)/linktest.elf \
	  --file bin/spawntarget $(UBUILD)/spawntarget.elf \
	  --file bin/exectarget $(UBUILD)/exectarget.elf \
	  --file bin/proctest $(UBUILD)/proctest.elf \
	  --file bin/argtest $(UBUILD)/argtest.elf \
	  --file bin/fifomaster $(UBUILD)/fifomaster.elf \
	  --file bin/fifowriter $(UBUILD)/fifowriter.elf \
	  --file bin/fiforeader $(UBUILD)/fiforeader.elf \
	  --file bin/killtest $(UBUILD)/killtest.elf \
	  --file bin/killvictim $(UBUILD)/killvictim.elf \
	  --file bin/namedreader $(UBUILD)/namedreader.elf \
	  --file bin/namedwriter $(UBUILD)/namedwriter.elf \
	  --file bin/consolereader1 $(UBUILD)/consolereader1.elf \
	  --file bin/consolereader2 $(UBUILD)/consolereader2.elf \
	  --file bin/sigtest $(UBUILD)/sigtest.elf \
	  --file bin/sigtarget $(UBUILD)/sigtarget.elf \
	  --file bin/sigdefault $(UBUILD)/sigdefault.elf \
	  --file bin/jobtest $(UBUILD)/jobtest.elf \
	  --file bin/jobtarget $(UBUILD)/jobtarget.elf \
	  --file bin/nesttest $(UBUILD)/nesttest.elf \
	  --file bin/nesttarget $(UBUILD)/nesttarget.elf \
	  --file bin/eintrtest $(UBUILD)/eintrtest.elf \
	  --file bin/eintrtarget $(UBUILD)/eintrtarget.elf \
	  --file bin/restarttest $(UBUILD)/restarttest.elf \
	  --file bin/restarttarget $(UBUILD)/restarttarget.elf \
	  --file bin/masktest $(UBUILD)/masktest.elf \
	  --file bin/masktarget $(UBUILD)/masktarget.elf \
	  --file bin/sigchldtest $(UBUILD)/sigchldtest.elf \
	  --file bin/sigchldchild $(UBUILD)/sigchldchild.elf \
	  --file bin/waitanytest $(UBUILD)/waitanytest.elf \
	  --file bin/waitanyfast $(UBUILD)/waitanyfast.elf \
	  --file bin/waitanyslow $(UBUILD)/waitanyslow.elf \
	  --file bin/stopchldtest $(UBUILD)/stopchldtest.elf \
	  --file bin/stopchldtarget $(UBUILD)/stopchldtarget.elf \
	  --file bin/nodefertest $(UBUILD)/nodefertest.elf \
	  --file bin/alarmtest $(UBUILD)/alarmtest.elf \
	  --file bin/alarmdefault $(UBUILD)/alarmdefault.elf \
	  --file bin/siginfotest $(UBUILD)/siginfotest.elf \
	  --file bin/siginfotarget $(UBUILD)/siginfotarget.elf \
	  --file bin/pgroupfg $(UBUILD)/pgroupfg.elf \
	  --file bin/pgroupbg $(UBUILD)/pgroupbg.elf \
	  --file bin/jobctlfg $(UBUILD)/jobctlfg.elf \
	  --file bin/jobctlbg $(UBUILD)/jobctlbg.elf \
	  --file bin/polltest $(UBUILD)/polltest.elf \
	  --file bin/pollwriter $(UBUILD)/pollwriter.elf \
	  --file bin/pipesrc $(UBUILD)/pipesrc.elf \
	  --file bin/pipesink $(UBUILD)/pipesink.elf \
	  --file bin/redirwriter $(UBUILD)/redirwriter.elf \
	  --file bin/redirreader $(UBUILD)/redirreader.elf \
	  --file bin/stderrwriter $(UBUILD)/stderrwriter.elf \
	  --file bin/stderrreader $(UBUILD)/stderrreader.elf \
	  --file bin/selfstoptest $(UBUILD)/selfstoptest.elf \
	  --file bin/selfstoptarget $(UBUILD)/selfstoptarget.elf \
	  --file bin/herestest $(UBUILD)/herestest.elf \
	  --file bin/hcat $(UBUILD)/hcat.elf \
	  --file bin/hsink $(UBUILD)/hsink.elf \
	  --file bin/pipecat $(UBUILD)/pipecat.elf \
	  --file bin/cwdtest $(UBUILD)/cwdtest.elf \
	  --file bin/cwdtarget $(UBUILD)/cwdtarget.elf \
	  --file bin/envprint $(UBUILD)/envprint.elf \
	  --file bin/nicetest $(UBUILD)/nicetest.elf \
	  --file bin/nicetarget $(UBUILD)/nicetarget.elf \
	  --file bin/date $(UBUILD)/date.elf \
	  --file bin/globcat $(UBUILD)/globcat.elf \
	  --file bin/timetest $(UBUILD)/timetest.elf \
	  --file bin/false $(UBUILD)/false.elf \
	  --file bin/argecho $(UBUILD)/argecho.elf \
	  --file bin/rmtest $(UBUILD)/rmtest.elf \
	  --file bin/permtest $(UBUILD)/permtest.elf \
	  --file bin/identtest $(UBUILD)/identtest.elf \
	  --file bin/dirpermtest $(UBUILD)/dirpermtest.elf \
	  --file bin/grouptest $(UBUILD)/grouptest.elf \
	  --file bin/pidfdtest $(UBUILD)/pidfdtest.elf \
	  --file bin/mmaptest $(UBUILD)/mmaptest.elf \
	  --file bin/mmapprotvictim $(UBUILD)/mmapprotvictim.elf \
	  --file bin/mmapexecvictim $(UBUILD)/mmapexecvictim.elf \
	  --file etc/motd.txt fsroot/etc/motd.txt \
	  --file etc/readme.txt fsroot/etc/readme.txt \
	  --file home/notes.txt fsroot/home/notes.txt

$(ISO): $(KERNEL) $(INITRD) iso/boot/grub/grub.cfg
	@cp $(KERNEL) iso/boot/frostbyte.elf
	@cp $(INITRD) iso/boot/initrd.img
	grub-mkrescue -o $(ISO) iso 2>/dev/null

# Scratch disk backing /dev/disk0 (ata.c / vfs.c blockDev*). Zeroed, not
# committed: a fresh 8 MiB raw image is exactly as good as any other for a
# driver that owns the whole device and formats nothing on it.
$(DISK): | $(BUILD)
	dd if=/dev/zero of=$@ bs=1M count=8 status=none

run: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) $(DISKFLAGS) -serial stdio -m 128M

runserial: $(ISO) $(DISK)
	qemu-system-x86_64 -cdrom $(ISO) $(DISKFLAGS) -serial stdio -display none -m 128M -no-reboot

test: $(ISO) $(DISK)
	sh tools/ci-test.sh

clean:
	rm -rf $(BUILD) $(ISO) iso/boot/frostbyte.elf iso/boot/initrd.img

# Auto-generated header dependencies (-MMD -MP above): without these, editing
# a widely-included header (e.g. sched.h) would not force a rebuild of every
# .c file that includes it, silently linking objects compiled against
# different struct layouts of the same type -- exactly the kind of ABI
# mismatch that corrupts memory at runtime instead of failing to build.
-include $(wildcard $(BUILD)/*.d) $(wildcard $(UBUILD)/*.d)
