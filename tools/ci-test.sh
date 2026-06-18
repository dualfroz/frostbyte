#!/bin/sh
# Headless boot gate: run the ISO in QEMU, capture COM1, and require that a
# ring-3 program read a known ramdisk (USTAR) file and round-tripped a tmpfs
# file. Exits non-zero when any expected string is missing.

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
iso="$root/os.iso"

if [ ! -f "$iso" ]; then
    echo "ci-test: $iso not found, build it first (make)"
    exit 2
fi

log=$(mktemp)
timeout 25 qemu-system-x86_64 -cdrom "$iso" -serial "file:$log" \
    -display none -m 128M -no-reboot >/dev/null 2>&1 || true

echo "----- serial log (tail) -----"
tail -n 30 "$log" 2>/dev/null
echo "-----------------------------"

status=0
for needle in \
    "frostbyte-ustar-read-ok" \
    "frostbyte-tmpfs-rw-ok" \
    "fileread: done"
do
    if grep -qF "$needle" "$log"; then
        echo "PASS: found '$needle'"
    else
        echo "FAIL: missing '$needle'"
        status=1
    fi
done

rm -f "$log"
if [ "$status" -eq 0 ]; then
    echo "ci-test: all filesystem checks passed"
else
    echo "ci-test: filesystem checks FAILED"
fi
exit $status
