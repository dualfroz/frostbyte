#include "shell.h"
#include "console.h"
#include "keyboard.h"
#include "vga.h"
#include "pmm.h"
#include "heap.h"
#include "pit.h"
#include "sched.h"
#include "string.h"
#include "vfs.h"
#include "fs.h"

#define lineCapacity 128
#define pathCapacity 256

static char cwd[pathCapacity] = "/";

static void printHelp(void)
{
    kprintf("available commands:\n");
    kprintf("  help          show this list\n");
    kprintf("  echo TEXT     print the given text\n");
    kprintf("  echo TEXT >F  write the text to file F\n");
    kprintf("  ls [PATH]     list a directory\n");
    kprintf("  cat FILE      print a file\n");
    kprintf("  mkdir PATH    create a directory (writable mounts only)\n");
    kprintf("  cd PATH       change the working directory\n");
    kprintf("  pwd           print the working directory\n");
    kprintf("  mem           physical memory and heap usage\n");
    kprintf("  uptime        ticks and seconds since boot\n");
    kprintf("  ps            list kernel threads\n");
    kprintf("  clear         clear the screen\n");
}

static void printMem(void)
{
    uint64 totalKib = pmmTotalBytes() / 1024;
    uint64 usedKib = pmmUsedFrames() * frameSize / 1024;
    uint64 freeKib = pmmFreeFrames() * frameSize / 1024;

    kprintf("physical memory: %lu KiB total\n", totalKib);
    kprintf("  frames: %lu total, %lu used, %lu free\n",
            pmmTotalFrames(), pmmUsedFrames(), pmmFreeFrames());
    kprintf("  used %lu KiB, free %lu KiB\n", usedKib, freeKib);
    kprintf("heap: %lu KiB used of %lu KiB\n",
            heapUsed() / 1024, heapTotal() / 1024);
}

static void printUptime(void)
{
    uint64 ticks = pitTicks();
    uint32 frequency = pitFrequency();
    kprintf("uptime: %lu ticks, %lu seconds (at %u Hz)\n",
            ticks, ticks / frequency, frequency);
}

static void printThreads(void)
{
    kprintf("id  state     name\n");
    for (uint32 i = 0; i < threadCount(); i++) {
        Thread* thread = threadAt(i);
        const char* state = thread->state == threadRunning ? "running" : "runnable";
        char marker = thread->id == currentThreadId() ? '*' : ' ';
        kprintf("%c%u   %s  %s\n", marker, thread->id, state, thread->name);
    }
}

static void buildPath(const char* argument, char* out)
{
    if (argument[0] == '/') {
        uint64 length = strlen(argument);
        if (length >= pathCapacity)
            length = pathCapacity - 1;
        memcpy(out, argument, length);
        out[length] = 0;
        return;
    }

    uint64 pos = strlen(cwd);
    if (pos >= pathCapacity)
        pos = pathCapacity - 1;
    memcpy(out, cwd, pos);
    if (pos == 0 || out[pos - 1] != '/') {
        if (pos < pathCapacity - 1)
            out[pos++] = '/';
    }
    for (uint64 i = 0; argument[i] && pos < pathCapacity - 1; i++)
        out[pos++] = argument[i];
    out[pos] = 0;
}

static void listDir(const char* argument)
{
    char path[pathCapacity];
    if (argument[0] == 0)
        memcpy(path, cwd, strlen(cwd) + 1);
    else
        buildPath(argument, path);

    VfsNode* node = vfsLookup(path);
    if (!node) {
        kprintf("ls: no such path: %s\n", path);
        return;
    }
    if (!vfsIsDir(node)) {
        kprintf("%s  %lu\n", vfsName(node), vfsSize(node));
        return;
    }
    for (VfsNode* child = vfsChild(node); child; child = vfsNext(child)) {
        if (vfsIsDir(child))
            kprintf("%s/\n", vfsName(child));
        else
            kprintf("%s  %lu\n", vfsName(child), vfsSize(child));
    }
}

static void catFile(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("cat: missing file\n");
        return;
    }
    char path[pathCapacity];
    buildPath(argument, path);

    VfsNode* node = vfsLookup(path);
    if (!node) {
        kprintf("cat: no such file: %s\n", path);
        return;
    }
    if (vfsIsDir(node)) {
        kprintf("cat: %s is a directory\n", path);
        return;
    }
    const char* data = (const char*)vfsFileData(node);
    uint64 size = vfsSize(node);
    for (uint64 i = 0; i < size; i++)
        kputchar(data[i]);
}

static void echoWrite(char* text, const char* target)
{
    char path[pathCapacity];
    buildPath(target, path);

    int fd = vfsOpen(path, oCreate | oWriteOnly | oTrunc);
    if (fd < 0) {
        kprintf("echo: cannot write %s\n", path);
        return;
    }
    uint64 length = strlen(text);
    vfsWrite(fd, text, length);
    vfsWrite(fd, "\n", 1);
    vfsClose(fd);
}

static void makeDir(const char* argument)
{
    if (argument[0] == 0) {
        kprintf("mkdir: missing path\n");
        return;
    }
    char path[pathCapacity];
    buildPath(argument, path);
    if (vfsMkdir(path) != 0)
        kprintf("mkdir: cannot create %s\n", path);
}

static void changeDir(const char* argument)
{
    char path[pathCapacity];
    if (argument[0] == 0)
        memcpy(path, "/", 2);
    else
        buildPath(argument, path);

    VfsNode* node = vfsLookup(path);
    if (!node || !vfsIsDir(node)) {
        kprintf("cd: no such directory: %s\n", path);
        return;
    }
    vfsPath(node, cwd, sizeof(cwd));
}

static void doEcho(char* argument)
{
    char* redirect = argument;
    while (*redirect && *redirect != '>')
        redirect++;

    if (*redirect == '>') {
        *redirect = 0;
        char* target = redirect + 1;
        while (*target == ' ')
            target++;

        char* textEnd = redirect;
        while (textEnd > argument && textEnd[-1] == ' ')
            textEnd--;
        *textEnd = 0;

        echoWrite(argument, target);
    } else {
        kprintf("%s\n", argument);
    }
}

static void execute(char* line)
{
    if (line[0] == 0)
        return;

    char* argument = line;
    while (*argument && *argument != ' ')
        argument++;
    if (*argument == ' ') {
        *argument = 0;
        argument++;
    }
    while (*argument == ' ')
        argument++;

    if (strcmp(line, "help") == 0)
        printHelp();
    else if (strcmp(line, "echo") == 0)
        doEcho(argument);
    else if (strcmp(line, "ls") == 0)
        listDir(argument);
    else if (strcmp(line, "cat") == 0)
        catFile(argument);
    else if (strcmp(line, "mkdir") == 0)
        makeDir(argument);
    else if (strcmp(line, "cd") == 0)
        changeDir(argument);
    else if (strcmp(line, "pwd") == 0)
        kprintf("%s\n", cwd);
    else if (strcmp(line, "mem") == 0)
        printMem();
    else if (strcmp(line, "uptime") == 0)
        printUptime();
    else if (strcmp(line, "ps") == 0)
        printThreads();
    else if (strcmp(line, "clear") == 0)
        vgaClear();
    else
        kprintf("unknown command: %s (try 'help')\n", line);
}

void shellRun(void)
{
    char line[lineCapacity];
    uint32 length = 0;

    kprintf("\nfrostbyte shell ready. type 'help'.\n");
    kprintf("frostbyte> ");

    for (;;) {
        char c = keyboardReadChar();

        if (c == '\n') {
            kputchar('\n');
            line[length] = 0;
            execute(line);
            length = 0;
            kprintf("frostbyte> ");
        } else if (c == '\b') {
            if (length > 0) {
                length--;
                kprintf("\b \b");
            }
        } else if (length < lineCapacity - 1) {
            line[length++] = c;
            kputchar(c);
        }
    }
}
