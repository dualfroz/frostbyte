frostbyte filesystem layout

/bin    ring-3 ELF programs loaded by the kernel at boot
/etc    text files packed into the ramdisk
/home   more sample text
/tmp    writable in-RAM tmpfs, empty at boot

Try from the shell: ls /etc, cat /etc/motd.txt, mkdir /tmp/x, echo hi >/tmp/x/a.
