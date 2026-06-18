# frostbyte

A small hobby operating system kernel for x86_64, written in C and GAS assembly.
It boots via Multiboot2/GRUB, switches from 32-bit protected mode into 64-bit long
mode with its own page tables, runs a preemptive round-robin scheduler, and loads
static ELF programs from a ramdisk into ring 3 that talk back through system calls.

Everything below is verifiable headlessly in QEMU.

## What it does

- Boots as a Multiboot2 kernel loaded by GRUB at 1 MiB, then builds its own paging
  structures and jumps into a higher-half 64-bit kernel at `0xFFFFFFFF80000000`.
- Talks over serial COM1 and the VGA text buffer through a shared `kprintf`.
- Installs a full IDT (32 CPU exception handlers, 16 remapped hardware IRQs, and a
  ring-3-callable `int 0x80` syscall gate).
- Drives the PIT at 100 Hz and preempts between kernel and user threads on every tick.
- Reads the PS/2 keyboard and feeds an in-kernel shell.
- Manages physical RAM with a bitmap frame allocator built from the Multiboot2
  memory map, maps pages on demand, and serves a real free-list heap with `kfree`
  and coalescing.
- Loads a Multiboot2 module ramdisk in USTAR (tar) format, parses static x86_64 ELF
  executables from it, maps their segments as user pages, and runs them in ring 3
  with their own kernel and user stacks via a TSS.
- Mounts a VFS: the USTAR ramdisk read-only at `/` (directories and files) and a
  writable in-RAM tmpfs at `/tmp`, with per-process file descriptor tables.
- Serves system calls: `write`, `read`, `exit`, `getpid`, `yield`, `sbrk`, `sleep`,
  plus `open`, `close`, `lseek`, `stat`, `getdents`, `mkdir`, `unlink`.

## Layout

```
src/
  boot.S        multiboot2 header, 32-bit entry, long mode + paging bring-up
  isrStubs.S    per-vector interrupt stubs and the common save/restore path
  kmain.c       bring-up sequence and the demo threads
  serial.c      COM1 driver
  vga.c         80x25 VGA text driver with scrolling and hardware cursor
  console.c     kprintf over serial + VGA
  string.c      freestanding memset/memcpy/memmove/strcmp/...
  gdt.c         flat 64-bit GDT
  idt.c         IDT setup
  isr.c         exception panic + IRQ dispatch
  pic.c         8259 PIC remap, EOI, masking
  pit.c         programmable interval timer
  keyboard.c    PS/2 scancode set 1 to ASCII, ring buffer
  pmm.c         bitmap physical frame allocator
  paging.c      4-level page table walk, on-demand tables, user-range mapping
  heap.c        free-list heap allocator: kmalloc / kfree / krealloc, coalescing
  sched.c       preemptive scheduler, kernel + ring-3 threads, sleep/exit/sbrk
  gdt.c         kernel + user segments and the 64-bit TSS descriptor
  tss.c         task state segment, per-thread rsp0 for ring 3 -> ring 0
  multiboot.c   Multiboot2 module (ramdisk) discovery
  initrd.c      USTAR (tar) ramdisk header/content/directory reader
  vfs.c         path resolution, per-process descriptors, tmpfs, fd syscalls
  elf.c         static ELF64 loader (maps PT_LOAD segments as user pages)
  syscall.c     int 0x80 dispatch and the system calls
  shell.c       help / echo / ls / cat / mkdir / cd / pwd / mem / uptime / ps / clear
include/        headers (shared syscall.h is included by the kernel and userland)
user/
  crt.S         _start: call main, then exit
  ulib.c        userland syscall wrappers and print helpers
  hello.c       writes from ring 3, yields, exits
  counter.c     counts with sleep between steps
  sbrktest.c    grows its heap with sbrk and verifies the page
  fileread.c    reads a ramdisk file and round-trips a tmpfs file over syscalls
  user.ld       links each program above 1 GiB with headers in the first segment
fsroot/         sample text files packed into the ramdisk (etc/, home/)
tools/mktar.c   host tool that packs the user ELFs and text files into a USTAR image
tools/ci-test.sh  headless QEMU boot gate: greps serial for the filesystem proof
linker.ld       higher-half layout, low boot segment
Makefile        build + run + test targets
iso/boot/grub/grub.cfg
```

## Requirements

System GCC 13 with freestanding flags (no cross compiler needed), GNU ld, plus
`grub-mkrescue`, `xorriso`, `mtools` and `qemu-system-x86_64`.

## Build

```
make
```

This compiles every `src/*.c` and `src/*.S` and links the kernel with `linker.ld`
into `build/frostbyte.elf`. It also builds the userland programs under `user/`
as standalone static ELF64 executables, packs them together with the `fsroot/`
text files into a USTAR `build/initrd.img` with the host `mktar` tool, and wraps
kernel plus ramdisk into a bootable `os.iso` with `grub-mkrescue`. GRUB hands the
ramdisk to the kernel as a Multiboot2 module.

`make test` boots the ISO headless in QEMU and greps the serial log for the proof
that a ring-3 program read a ramdisk file and round-tripped a tmpfs file.

## Run

Windowed (see the VGA text screen and type into the shell):

```
make run
```

Headless with the kernel log on your terminal over serial:

```
make runserial
```

To capture the serial log to a file:

```
qemu-system-x86_64 -cdrom os.iso -serial file:boot.log -display none -m 128M -no-reboot
```

## Proof

`docs/boot-serial.log` is a captured headless boot. Abridged:

```
========================================
  frostbyte kernel  x86_64  long mode
  multiboot2 / grub / freestanding gcc
========================================

[boot] multiboot2 magic verified (36d76289)
[gdt]  loaded flat 64-bit segments
[idt]  256 vectors installed (32 exceptions, 16 irqs)
[pic]  remapped to 0x20-0x2f, irq0 and irq1 unmasked
[pmm]  130559 KiB usable, 32736 frames, 32463 free after kernel
[heap] bump allocator at 0x0000000000111000, 8192 KiB
[paging] mapped 0x0000000040000000 -> phys 911000, readback OK
[pit]  timer at 100 Hz
[kbd]  ps/2 keyboard driver ready
[sched] 4 threads: idle, shell, workerA, workerB

[boot] enabling interrupts, starting preemptive scheduler
frostbyte> [workerA] tick=50 iterations=8650613
[workerB] tick=91 iterations=15943273
[workerA] tick=150 iterations=29911570
[workerB] tick=191 iterations=34964279
```

`workerA` and `workerB` never yield; they run tight loops on separate kernel
stacks. Both advancing while the timer ticks climb is the proof that the timer
interrupt preempts and the scheduler round-robins between them.

`docs/userspace-serial.log` is a captured headless boot of the userspace layer.
The kernel mounts the ramdisk, loads three ELF programs into ring 3, and they
answer over the syscall interface. Abridged:

```
[heap]   coalesce PASS, 8191 KiB free of 8192 KiB, blocks=1
[initrd] fbar mounted, 3 files
[user] loaded 'hello' pid=4 entry=0x00000000400000b7 stackTop=0x0000000048000000
frostbyte> hello: greetings from ring 3, pid=4
counter: pid=5 step=0
sbrktest: pid=6 requesting 8192 bytes via sbrk
hello: resumed after yield, exiting cleanly
[user] pid=4 'hello' exited with code 7
sbrktest: heap page write and read-back verified OK
counter: finished counting
[user] pid=5 'counter' exited with code 0
```

The `hello:` line only reaches the serial port through the `write` syscall from
ring 3, so its presence is the proof that the ring 0 -> ring 3 transition, the
`int 0x80` gate, and the dispatcher all work. As a counterfactual, routing `int
0x80` to a vector the dispatcher never sees erases every user line and leaves a
ring-3 page fault (the fault frame reports `cs=1b`, confirming the code really ran
unprivileged).

`docs/boot-serial.log` and `docs/shell-serial.log` show the v1 kernel path: the
two never-yielding worker threads advancing on separate stacks (scheduler proof)
and the shell answering keystrokes injected over the QEMU monitor.

`docs/screenshot.png` is a QEMU framebuffer dump, showing the VGA text mode output
that the serial log cannot display.

## Design notes

- Higher-half kernel: the boot segment is identity mapped low so GRUB can enter
  it in 32-bit, and the rest of the kernel is linked at `-2 GiB`
  (`-mcmodel=kernel`). `boot.S` maps the first 1 GiB both identity and at the
  higher half, then far-jumps into 64-bit code.
- Context switching happens inside the timer IRQ. The common interrupt stub saves
  all registers to the interrupted thread's stack, hands the stack pointer to the
  scheduler, and reloads the stack pointer the scheduler returns. A fresh thread
  starts from a hand-crafted interrupt frame, so `iretq` drops straight into its
  entry point with interrupts enabled.
- Userspace runs in the kernel's address space but under ring 3. Each user program
  is linked at its own base above 1 GiB (so it sits outside the identity-mapped
  low region) and its segments are mapped as user pages. The `int 0x80` frame is a
  normal interrupt frame, so the same save/restore stub serves both IRQs and
  syscalls; `iretq` returns to ring 3 because the saved `cs` has RPL 3.
- Every thread that can enter ring 3 owns a separate kernel stack. The scheduler
  writes that stack's top into the TSS `rsp0` on each switch, so a trap taken in
  ring 3 lands on the right kernel stack.
- The heap is an address-ordered free list with boundary coalescing and block
  splitting, which is what makes `kfree` and reuse under fragmentation work.

## Not in this version

Per-process address spaces (all user programs currently share one page table and
are separated by link base), a persistent on-disk filesystem, networking, and SMP
are the next steps. The current VFS reads a USTAR ramdisk and a RAM-only tmpfs;
`fork`/`exec`, an ATA/AHCI block driver behind the VFS, and a TCP/IP stack come
after that.
