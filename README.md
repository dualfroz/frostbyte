# frostbyte

frostbyte is a hobby x86_64 kernel written in freestanding C and GNU assembler. GRUB
loads it as a Multiboot2 kernel; it switches itself into 64-bit long mode, runs in
the higher half, schedules kernel threads and ring-3 processes with a weighted
fair scheduler, and loads static ELF programs from a USTAR ramdisk. Processes talk
to the kernel through 56 system calls covering files, pipes and FIFOs, process
spawning and exec, POSIX-style signals, job control, `poll`, `mmap`, pidfds and a
Unix permission model. Storage comes from a ramdisk, an in-memory tmpfs and a
persistent inode filesystem on an ATA disk. An in-kernel shell supports pipelines,
redirection, background jobs and environment variables. Every boot runs an
in-kernel test suite and a set of ring-3 test programs, and a headless QEMU script
checks their output.

## Features

### Boot

- Multiboot2 header and 32-bit entry point (`src/boot.S`). The entry code checks
  for long mode, maps the first 1 GiB with 2 MiB pages both identity-mapped and at
  `0xFFFFFFFF80000000`, turns on PAE, `EFER.LME` and paging, and jumps into the
  64-bit higher-half kernel. When CPUID reports NX, `EFER.NXE` is enabled too.
- `kmain` sets up the GDT (kernel and user segments plus a 64-bit TSS), the IDT
  (32 exception vectors, 16 remapped IRQs, an `int 0x80` gate callable from
  ring 3), the 8259 PIC, the PIT at 100 Hz and the PS/2 keyboard. It then finds the
  ramdisk module, mounts the filesystems, runs the self-tests, starts the boot
  demo threads and user programs, and enables preemption.

### Memory

- Bitmap physical frame allocator built from the Multiboot2 memory map
  (`pmm.c`).
- 4-level paging with page tables allocated on demand, user-range mapping and
  unmapping, address resolution and NX support (`paging.c`). ELF segments are
  mapped W^X based on their `p_flags`.
- 48 MiB kernel heap: a free-list allocator with `kmalloc`, `kfree`, `krealloc`
  and coalescing of neighbouring free blocks (`heap.c`).
- Frame-backed slab caches for fixed-size objects (`slab.c`).
- One address space shared by the whole system. Each user binary is linked at its
  own 256 MiB slot: code and data at the base, `sbrk` heap from +64 MiB, the stack
  ending at +128 MiB, and an `mmap` area from +128 MiB to the end of the slot.
  When a process is reaped, its whole slot is unmapped and the frames are freed.
- `mmap`/`munmap`: anonymous or file-backed mappings with read, write and exec
  protection. Pages are filled lazily from the page fault handler, and an access
  that breaks the protection delivers `SIGSEGV`.

### Scheduler and processes

- Preemptive scheduler driven by the 100 Hz timer (`sched.c`). Each runnable
  thread has a virtual runtime that grows in inverse proportion to its weight. The
  thread with the smallest virtual runtime runs next, taken from a priority queue
  built on an intrusive red-black tree (`prioq.c`, `rbt.c`). New and newly woken
  threads start no lower than the current minimum, so they cannot starve threads
  that have already run.
- Kernel threads and ring-3 processes share one thread table (288 slots).
  Free slots are reused round-robin, and every slot carries a generation counter.
  A process has its own kernel stack, file descriptor table (16 entries), working
  directory, environment (8 variables), uid/gid, process group and signal state.
- There is no `fork`. `spawn` loads a binary into a new process (like
  `posix_spawn`) and `exec` replaces the caller's image in place, keeping its pid
  and open descriptors. Both pass `argv` on the new stack using the SysV layout.
- `wait` for one pid or for any child, `kill` for a pid or a process group,
  sleeping, blocking on wait channels, and `nice`/`getpriority` to change the
  scheduling weight.
- pidfds (`pidfd.c`): a handle tied to one particular process. Once that process
  is reaped, the pidfd reports it as gone, even if a new process has been given
  the same pid.

### Signals and job control

- 32 signals with default actions (terminate, ignore, stop, continue) and user
  handlers that run through a shared trampoline page and return with `sigreturn`
  (`signal.c`, `sigTrampoline.S`).
- Per-process signal masks (block, unblock, set) and per-handler masks. Handler
  flags `sigFlagRestart` (restart the interrupted system call instead of failing
  with EINTR), `sigFlagNoDefer` and `sigFlagSigInfo` (pass a siginfo record with
  the sender pid and code). Nested delivery is supported.
- `SIGCHLD` is sent when a child exits, is killed, stops or continues.
  `SIGSTOP`/`SIGCONT` work, and `alarm` delivers `SIGALRM`. A process ended by a
  signal exits with code 128 + signal number.
- Process groups and a foreground group. The keyboard driver sends Ctrl-C as
  `SIGINT` and Ctrl-Z as `SIGTSTP` to the foreground group only.

### Filesystems

- VFS (`vfs.c`) with paths relative to the working directory and per-process
  file descriptors. Mounts:
  - `/`: the USTAR ramdisk (read-only), loaded as a Multiboot2 module.
  - `/tmp`: a writable tmpfs held in kernel memory.
  - `/mnt`: SFS, a persistent inode filesystem on the ATA disk (`sfs.c`). It has
    a superblock, inode and block bitmaps, 512 inodes, and direct plus single,
    double and triple indirect blocks. Inodes store the size, link count, mode,
    uid, gid, mtime and ctime. An unformatted disk is formatted on mount.
  - `/dev/disk0`: the raw ATA disk as a seekable byte device.
- Files, directories, `unlink`, `rmdir`, `getdents`, `stat`, `lseek`, append
  and truncate. Symbolic links (followed for up to 8 hops) and hard links are
  available on `/mnt`.
- Unix permissions: owner/group/other rwx bits, checked on open, exec, directory
  search, directory listing and directory writes. `chmod`, `chown` and `chgrp`
  follow owner and root rules, and a process can drop root with
  `setuid`/`setgid`.
- Timestamps come from the CMOS real-time clock.

### Pipes, FIFOs and poll

- Anonymous pipes (`pipe`), plus pipes created by handle that any number of
  processes can attach to as readers or writers (`pipeCreate`/`pipeAttach`).
- Named FIFOs (`mkfifo`) on tmpfs and on SFS. Opening a FIFO for writing blocks
  until a reader is present.
- Blocking reads and writes, EOF once the last writer closes, and a write error
  once the last reader closes. Each pipe has a 256-byte buffer, and 32 pipes can
  exist at once.
- `poll` over pipes and the console, with an optional timeout.

### Drivers

- Serial COM1 and the 80x25 VGA text console. Both are written by one
  interrupt-safe `kprintf`.
- PS/2 keyboard (scancode set 1, Shift and left Ctrl). Several processes can read the
  console at once and each one gets every key.
- PIT timer, 8259 PIC, CMOS RTC.
- ATA PIO: primary master, LBA28, polling with bounded timeouts.

### Kernel library

Freestanding `string`/`klib` routines (`snprintf`, `strtol`, `qsort`, `bsearch`,
`strTokR` and more), an intrusive doubly linked list, a ring buffer, a red-black
tree and a priority queue. The kernel itself uses all of these. The repository
also has a growable vector, an FNV-1a hash map, a bitmap and an interrupt-safe
spinlock; for now only the self-tests use those.

### Shell

`src/shell.c` runs as a kernel thread. It reads input from the PS/2 keyboard and
writes to VGA and serial.

- Built-in commands: `help`, `echo`, `ls [-l]`, `cat`, `mkdir`, `rm`, `rmdir`,
  `chmod`, `chown`, `chgrp`, `cd`, `pwd`, `whoami`, `mem`, `uptime`, `ps`,
  `jobs`, `export`, `unset`, `env`, `clear`.
- Any other word runs `/bin/NAME` in a new process group, in the foreground or in
  the background with `&`.
- Pipelines (`a | b | c`), redirections (`<`, `>`, `>>`, `2>`, `2>>`),
  here-strings (`<<<`), `&&` and `||` chains, `$NAME` and `$?` expansion, and
  `*` globs.

## System calls

User programs make system calls with `int 0x80`. The call number goes in `rax`
and the arguments in `rdi`, `rsi`, `rdx`, `r10`, `r8`, `r9`. Numbers are defined
in `include/syscall.h`, and the wrappers in `user/ulib.c`.

| No. | Name | Purpose |
|---|---|---|
| 0 | exit | end the calling process with an exit code |
| 1 | write | write to a file descriptor |
| 2 | read | read from a file descriptor (blocks on pipes and the console) |
| 3 | yield | give up the rest of the time slice |
| 4 | getpid | return the caller's pid |
| 5 | sbrk | grow or shrink the process heap |
| 6 | sleep | sleep for a number of milliseconds |
| 7 | open | open a file, directory, FIFO or `/dev/disk0` |
| 8 | close | close a file descriptor |
| 9 | lseek | move the file offset |
| 10 | stat | type, size, mode, owner, group and mtime of a path |
| 11 | getdents | read directory entries |
| 12 | mkdir | create a directory |
| 13 | unlink | remove a file or link |
| 14 | pipe | create an anonymous pipe pair |
| 15 | symlink | create a symbolic link |
| 16 | readlink | read a symbolic link target |
| 17 | link | create a hard link |
| 18 | exec | replace the current image with another binary (`spawnSlotBusy` if another process holds it) |
| 19 | spawn | start a binary as a new child process (`spawnSlotBusy` if one is alive) |
| 20 | wait | wait for a child (a pid, or any child) and reap it |
| 21 | pipeCreate | create a pipe identified by a handle number |
| 22 | pipeAttach | attach to a handle pipe as reader or writer |
| 23 | kill | kill a pid or a process group |
| 24 | mkfifo | create a named FIFO |
| 25 | sigSend | send a signal to a pid or a process group |
| 26 | sigAction | install a signal handler with mask and flags |
| 27 | sigProcMask | block, unblock or set the signal mask |
| 28 | sigReturn | return from a signal handler |
| 29 | alarm | schedule `SIGALRM` after a number of seconds |
| 30 | setpgid | set a process group |
| 31 | getpgid | read a process group |
| 32 | setFgPgrp | set the foreground process group |
| 33 | getFgPgrp | read the foreground process group |
| 34 | poll | wait for readiness on several descriptors |
| 35 | chdir | change the working directory |
| 36 | getcwd | read the working directory |
| 37 | getEnv | read an environment variable |
| 38 | setEnv | set an environment variable |
| 39 | nice | set the scheduling weight of a process |
| 40 | getPriority | read the scheduling weight of a process |
| 41 | time | seconds since the Unix epoch, from the RTC |
| 42 | unsetEnv | remove an environment variable |
| 43 | rmdir | remove an empty directory |
| 44 | chmod | change permission bits |
| 45 | getuid | read the user id |
| 46 | setuid | set the user id (drop from root) |
| 47 | chown | change the owner |
| 48 | getgid | read the group id |
| 49 | setgid | set the group id (drop from root) |
| 50 | chgrp | change the group |
| 51 | pidfdOpen | open a pidfd on a process |
| 52 | pidfdSend | send a signal through a pidfd |
| 53 | pidfdWait | wait for and reap a process through a pidfd |
| 54 | mmap | map anonymous memory or a file |
| 55 | munmap | remove a mapping |

## User programs

`user/` holds 84 programs and a small runtime: `crt.S` (`_start` that passes
`argc`/`argv` to `main` and calls `exit`), `ulib.c` (system call wrappers and
print helpers) and `user.ld`. The build links every program at its own address,
and `tools/mktar.c` packs them into the ramdisk under `/bin`.

- General tools and demos: `hello`, `counter`, `date`, `false`, `argecho`,
  `envprint`, `globcat`, `fileread`.
- Shell pipeline and redirection helpers: `pipesrc`, `pipecat`, `pipesink`,
  `hcat`, `hsink`, `redirwriter`, `redirreader`, `stderrwriter`, `stderrreader`,
  `herestest`.
- Test programs, each with the helper processes it spawns: memory (`sbrktest`,
  `mmaptest`), processes (`proctest`, `argtest`, `waitanytest`, `killtest`,
  `pidfdtest`, `nicetest`, `cwdtest`), pipes and FIFOs (`pipetest`, `fifomaster`,
  `namedreader`, `namedwriter`, `polltest`), console (`consolereader1`,
  `consolereader2`), signals (`sigtest`, `nesttest`, `masktest`, `eintrtest`,
  `restarttest`, `nodefertest`, `alarmtest`, `siginfotest`, `sigchldtest`),
  job control (`jobtest`, `stopchldtest`, `selfstoptest`, `pgroupfg`, `pgroupbg`,
  `jobctlfg`, `jobctlbg`), and filesystems and permissions (`disktest`, `fstest`,
  `linktest`, `rmtest`, `timetest`, `permtest`, `identtest`, `dirpermtest`,
  `grouptest`).

## Build and run

Requirements: an x86_64 Linux host with GCC (tested with GCC 13), GNU binutils,
`grub-mkrescue` with `xorriso` and `mtools`, and `qemu-system-x86_64` (tested with
QEMU 8.2). No cross compiler is needed, because both the kernel and the user
programs are built with freestanding flags.

```sh
make            # builds build/frostbyte.elf, the ramdisk and os.iso
make run        # boots in a QEMU window (VGA + PS/2 keyboard), serial on stdout
make runserial  # boots headless, serial on stdout
make test       # headless boot gate, see below
make clean
```

`make run` and `make runserial` attach `build/disk.img`, an 8 MiB raw image
created on first use, as the ATA disk behind `/mnt` and `/dev/disk0`. Shell input
comes from the PS/2 keyboard, so type commands in the QEMU window. Serial is
output only.

## Tests

- In-kernel self-tests: 444 tests in `src/*Test.c`, registered through a
  `.ktests` linker section (`include/ktest.h`). They run on every boot, before the
  scheduler starts, and cover the allocators, paging, ELF loading, the scheduler
  and thread table, signals, pipes, FIFOs, poll, mmap, SFS, ATA, the RTC, the
  keyboard, the shell parser and the library code. The run ends with a
  machine-readable line:

  ```
  KTEST SUMMARY: 444 tests, 8961 checks, 0 failed
  ```

- Boot gate: `tools/ci-test.sh` (run by `make test`) boots the ISO in QEMU with
  a blank disk, captures COM1, and waits for 299 markers printed by the kernel
  demos, the ring-3 test programs and the shell commands that boot threads type
  in, plus a `KTEST SUMMARY` line with zero failures. It stops as soon as every
  marker has appeared, as soon as the kernel prints `FAIL` or `[PANIC]`, or after
  120 seconds (`CI_TEST_MAX_SECONDS` changes the limit), and exits non-zero if
  any marker is missing.

- CI: `.github/workflows/ci.yml` installs the toolchain and QEMU on a
  GitHub-hosted Ubuntu runner, builds the ISO and runs the boot gate on every
  push to `main` and every pull request.

## Project layout

```
src/            kernel sources; *Test.c files are the in-kernel self-tests
  boot.S        Multiboot2 header, 32-bit entry, long mode bring-up
  isrStubs.S    interrupt entry stubs and the common save/restore path
  sigTrampoline.S  user-mode signal return trampoline
  kmain.c       bring-up sequence, boot demo threads and user program launch
  sched.c       scheduler, thread table, sleep/block/wake, reaping
  syscall.c     int 0x80 dispatch, spawn/exec, argument copying
  signal.c      signal delivery, masks, handlers, siginfo
  jobctl.c      foreground/background job launch for the shell
  vfs.c         VFS, tmpfs, descriptors, FIFOs, permission checks
  sfs.c         persistent inode filesystem on the ATA disk
  initrd.c      USTAR reader
  pipe.c        pipes and their wait channels
  mmap.c        lazy anonymous and file-backed mappings
  pidfd.c       pidfd handles
  elf.c         static ELF64 loader and initial user stack
  pmm.c paging.c heap.c slab.c   memory management
  ata.c rtc.c keyboard.c serial.c vga.c pit.c pic.c   drivers
  shell.c       in-kernel shell
include/        kernel headers; syscall.h, fs.h, poll.h and signalnum.h are shared with user/
user/           user programs, crt.S, ulib.c, user.ld
fsroot/         text files packed into the ramdisk under /etc and /home
tools/mktar.c   host tool that writes the USTAR ramdisk
tools/ci-test.sh  headless QEMU boot gate
.github/workflows/ci.yml  build and boot gate on GitHub Actions
iso/boot/grub/grub.cfg
linker.ld       kernel link script (low boot segment, higher-half kernel)
Makefile
```

## Limitations

- Single CPU. There is no SMP, and system calls run with interrupts disabled.
  Kernel threads (the shell and the boot demos) run with interrupts enabled; the
  scheduler, wait channels, signals, pipes, console readers, the heap, the frame
  allocator and page mapping disable interrupts around their critical sections,
  but the VFS, tmpfs and SFS code that the shell calls directly does not yet.
- No per-process address spaces and no `fork`. User binaries are static,
  non-relocatable ELF files, each linked at a fixed slot, so only one instance of
  a binary can be alive at a time. `spawn` and `exec` return `spawnSlotBusy` (-2)
  while another process (running, stopped, or exited but not yet reaped) still
  holds the binary's slot, and the kernel shell refuses to start it with an
  "already running" message. A process can still `exec` its own binary.
- Scheduling time is charged in whole timer ticks. A thread that blocks before
  the tick ends is not charged for that part of the tick.
- Fixed-size tables: 288 threads, 16 descriptors per process, 32 pipes of 256
  bytes, 8 mappings per process, 8 environment variables, 512 SFS inodes.
- FIFOs are one-way. Opening one with `oReadWrite` fails, and opening one for
  reading does not wait for a writer.
- Symbolic and hard links exist only on `/mnt`. tmpfs contents are lost on
  reboot.
- A file-backed mapping needs its file descriptor to stay open until every page
  has been touched.
- The ATA driver supports only the primary master drive, with PIO and LBA28.
- The RTC century is assumed to be 20 (years 2000-2099).
- The shell runs inside the kernel, and its only input is the PS/2 keyboard.

## License

MIT, see [LICENSE](LICENSE).
