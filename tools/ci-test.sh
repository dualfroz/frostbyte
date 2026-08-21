#!/bin/sh
# Headless boot gate: run the ISO in QEMU, capture COM1, and require that a
# ring-3 program read a known ramdisk (USTAR) file, round-tripped a tmpfs
# file, and round-tripped real sector I/O against a QEMU-emulated ATA disk
# via /dev/disk0. Exits non-zero when any expected string is missing.

here=$(dirname "$0")
root=$(cd "$here/.." && pwd)
iso="$root/os.iso"
disk="$root/build/disk.img"

if [ ! -f "$iso" ]; then
    echo "ci-test: $iso not found, build it first (make)"
    exit 2
fi

if [ ! -f "$disk" ]; then
    mkdir -p "$root/build"
    dd if=/dev/zero of="$disk" bs=1M count=8 status=none
fi

log=$(mktemp)
timeout 25 qemu-system-x86_64 -cdrom "$iso" \
    -drive file="$disk",format=raw,if=ide -serial "file:$log" \
    -display none -m 128M -no-reboot >/dev/null 2>&1 || true

echo "----- serial log (tail) -----"
tail -n 30 "$log" 2>/dev/null
echo "-----------------------------"

status=0
for needle in \
    "frostbyte-ustar-read-ok" \
    "frostbyte-tmpfs-rw-ok" \
    "fileread: done" \
    "pipetest: done" \
    "pipe demo: eof, count=10" \
    "disktest: done" \
    "fstest: done" \
    "linktest: done" \
    "sched-priority-ok" \
    "join-reap-ok" \
    "proctest: spawn+wait ok" \
    "proctest: spawn bad path rejected" \
    "proctest: wait bad pid rejected" \
    "exectarget: running" \
    "exec-pid-preserved-ok" \
    "user-mem-reclaimed-ok" \
    "exec-old-slot-freed-ok" \
    "proctest: spawn with argv ok" \
    "argtest: argv-ok" \
    "fiforeader: attached, blocking on read" \
    "fifowriter: wrote payload" \
    "fiforeader: payload-ok" \
    "fifomaster: pipeshare ok" \
    "killtest: self-kill rejected" \
    "killtest: kill bad pid rejected" \
    "killvictim: blocking on read forever" \
    "killtest: killed victim" \
    "killtest: kill-ok" \
    "mkfifo /tmp/rendezvous ready" \
    "namedreader: opened, blocking on read" \
    "namedwriter: wrote payload" \
    "namedreader: payload-ok" \
    "consolereader1: start" \
    "consolereader2: start" \
    "injecting synthetic keypress" \
    "console-fanout-1-ok" \
    "console-fanout-2-ok" \
    "sigtest: signal to bad pid rejected" \
    "sigtest: sigkill-handler-rejected-ok" \
    "sigtarget: waiting for signal" \
    "sigtest: sent sigUsr1 to sigtarget" \
    "sigtarget: handler invoked signum=10" \
    "sigtarget: handler-fired-ok" \
    "sigtarget: resumed-after-handler-ok" \
    "sigtest: handler-roundtrip-ok" \
    "sigdefault: waiting to be terminated by default disposition" \
    "sigtest: sent sigTerm to sigdefault" \
    "sigtest: default-terminate-ok" \
    "jobtarget: ready, blocking on go pipe" \
    "jobtest: jobtarget ready" \
    "jobtest: child-inherits-parent-pgid-ok" \
    "jobtest: sent sigStop to jobtarget" \
    "jobtest: sent sigCont to jobtarget" \
    "jobtarget: received go" \
    "jobtest: job-stop-continue-ok" \
    "nesttarget: ready, waiting for sigUsr1" \
    "nesttest: nesttarget ready" \
    "nesttest: sent sigUsr1 to nesttarget" \
    "nesttarget: usr1-handler-enter" \
    "nesttest: nesttarget usr1-handler-entered" \
    "nesttest: sent sigUsr2 to nesttarget" \
    "nesttarget: usr2-handler-fired-nested" \
    "nesttarget: usr1-handler-resumed-after-nested-usr2" \
    "nesttarget: nested-delivery-ok" \
    "nesttarget: resumed-after-both-handlers-ok" \
    "nesttest: nested-signal-delivery-ok" \
    "eintrtarget: blocking on read forever" \
    "eintrtest: eintrtarget ready" \
    "eintrtest: sent sigTerm to eintrtarget" \
    "eintrtest: early-wake-signal-ok" \
    "restarttarget: ready, handlers installed" \
    "restarttest: restarttarget ready" \
    "restarttest: sent sigUsr1 (no restart) to restarttarget" \
    "restarttarget: usr1-handler-fired" \
    "restarttarget: phase1-eintr-no-restart-ok" \
    "restarttest: sent sigUsr2 (restart) to restarttarget" \
    "restarttarget: usr2-handler-fired" \
    "restarttarget: phase2-restart-transparent-ok" \
    "restarttarget: all-phases-ok" \
    "restarttest: sa-restart-eintr-distinction-ok" \
    "masktarget: ready, waiting for sigUsr1" \
    "masktest: masktarget ready" \
    "masktest: sent sigUsr1 to masktarget" \
    "masktarget: usr1-handler-enter" \
    "masktest: masktarget usr1-handler-entered" \
    "masktest: sent sigUsr2 to masktarget (should stay masked)" \
    "masktarget: still-masked-after-read-ok" \
    "masktarget: usr2-handler-fired-after-unmask" \
    "masktarget: mask-defers-then-delivers-ok" \
    "masktarget: resumed-after-deferred-usr2-ok" \
    "masktest: sa-mask-defer-then-deliver-ok" \
    "sigchldchild: exiting now" \
    "sigchldtest: sigchld-handler-fired" \
    "sigchldtest: sigchld-delivered-to-parent-ok" \
    "sigchldtest: chld-reap-after-signal-ok" \
    "waitanyfast: exiting now" \
    "waitanyslow: exiting now" \
    "waitanytest: any-child-reap-set-ok" \
    "waitanytest: echild-when-no-children-ok" \
    "stopchldtarget: ready, blocking on go pipe" \
    "stopchldtest: stopchldtarget ready" \
    "stopchldtest: sent sigStop to stopchldtarget" \
    "stopchldtest: sigchld-on-stop-ok" \
    "stopchldtest: sent sigCont to stopchldtarget" \
    "stopchldtest: sigchld-on-continue-ok" \
    "stopchldtarget: received go" \
    "stopchldtest: job-control-sigchld-ok" \
    "nodefertest: onUsr1 first entry, self-raising sigUsr1 (NODEFER)" \
    "nodefertest: onUsr1 nested entry (depth 2)" \
    "nodefertest: onUsr1 observed its own nested re-entry (nodefer-self-nest-ok)" \
    "nodefertest: nodefer-self-nest-verified-ok" \
    "nodefertest: onUsr2 first entry, self-raising sigUsr2 (default defer)" \
    "nodefertest: onUsr2 confirmed no self-nest while deferred (still-deferred-ok)" \
    "nodefertest: onUsr2 second, TOP-LEVEL entry after first returned" \
    "nodefertest: default-defer-no-self-nest-verified-ok" \
    "nodefertest: sa-nodefer-contrast-ok" \
    "alarmtest: alarm-replace-and-cancel-bookkeeping-ok" \
    "alarmtest: blocking on read, waiting for real alarm to fire" \
    "alarmtest: onAlrm fired" \
    "alarmtest: alarm-fired-interrupted-blocking-read-ok" \
    "alarmdefault: blocking on read forever" \
    "alarmtest: alarm-default-disposition-terminated-ok" \
    "alarmtest: sigalrm-ok" \
    "siginfotarget: siginfo-sender-pid-ok" \
    "siginfotarget: resumed-after-siginfo-handler-ok" \
    "siginfotest: sent sigUsr1 to siginfotarget" \
    "siginfotest: sigchld-siginfo-accurate-ok" \
    "siginfotest: siginfo-roundtrip-ok" \
    "selfstoptarget: self-stopping now" \
    "selfstoptest: self-stop-confirmed-via-sigchld-ok" \
    "selfstoptest: still-stopped-before-cont-ok" \
    "selfstoptest: sent sigCont to selfstoptarget" \
    "selfstoptarget: resumed-after-self-stop-ok" \
    "selfstoptest: post-cont-proof-received-ok" \
    "selfstoptest: self-stop-continue-ok" \
    "pgroupfg: ready, blocking on go pipe" \
    "pgroupbg: own-process-group-ok" \
    "pgroupbg: ready, blocking on go pipe" \
    "injecting synthetic ctrl-c for the process-group demo" \
    "pgrouptest-fg-terminated-by-sigint-ok" \
    "pgroupbg: still-alive-after-ctrlc-ok" \
    "pgroupbg: resumed-after-signal-ok" \
    "jobctlbg started in background" \
    "jobctlbg: own-process-group-ok" \
    "jobctlbg: ready, blocking on go pipe" \
    "jobctlfg: ready, blocking on go pipe" \
    "injecting synthetic ctrl-c for the on-demand job-control demo" \
    "jobctlfg exited with code 130" \
    "shell-foreground-reclaimed-ok" \
    "jobctlbg: still-alive-after-ctrlc-ok" \
    "jobctlbg: resumed-after-signal-ok" \
    "polltest: non-blocking-poll-empty-ok" \
    "polltest: poll-timeout-ok" \
    "pollwriter: attached, sleeping before write" \
    "polltest: blocking on multi-fd poll, waiting for pollwriter" \
    "pollwriter: wrote payload" \
    "polltest: poll-woken-by-write-ok" \
    "polltest: payload-ok" \
    "polltest: poll-multiplex-ok" \
    "typing shell command for the pipeline demo" \
    "pipesrc: wrote payload" \
    "pipesink: blocking on read, waiting for pipesrc" \
    "pipesink: payload-ok" \
    "pipesrc | pipesink exited with code 0" \
    "typing shell commands for the file redirection demo" \
    "redirwriter: wrote payload" \
    "redirwriter exited with code 0" \
    "redirreader: reading redirected stdin" \
    "redirreader: payload-ok" \
    "redirreader exited with code 0" \
    "typing shell commands for the stderr redirection demo" \
    "stderrwriter: wrote payload" \
    "stderrwriter exited with code 0" \
    "stderrreader: reading redirected stdin" \
    "stderrreader: payload-ok" \
    "stderrreader exited with code 0" \
    "typing shell command for the here-string demo" \
    "herestest: reading redirected stdin" \
    "herestest: payload-ok" \
    "herestest exited with code 0" \
    "typing shell command for the here-string pipeline demo" \
    "hcat: reading here-string stdin" \
    "hcat: forwarded payload" \
    "hsink: blocking on read, waiting for hcat" \
    "hsink: payload-ok" \
    "hcat | hsink exited with code 0" \
    "typing shell command for the three-stage pipeline demo" \
    "pipecat: reading piped stdin" \
    "pipecat: forwarded payload" \
    "pipesrc | pipecat | pipesink exited with code 0" \
    "cwdtest: chdir-cwda-ok" \
    "cwdtest: relative-write-cwda-ok" \
    "cwdtarget: inherited-cwd-ok" \
    "cwdtarget: relative-write-cwdb-ok" \
    "cwdtest: parent-cwd-unaffected-ok" \
    "cwdtest: isolation-verified-ok" \
    "typing shell commands for the environment variable demo" \
    "envprint: inherited-ok GREETING=frostbyte" \
    "envprint: self-set-ok GREETING=child-mutated" \
    "envprint: unset-ok" \
    "envprint: unset-missing-ok" \
    "envprint exited with code 0" \
    "env: GREETING=frostbyte" \
    "nicetest: bogus-pid-rejected-ok" \
    "nicetest: zero-weight-rejected-ok" \
    "nicetest: self-renice-ok" \
    "nicetarget: reniced-while-sleeping-ok" \
    "nicetest: other-renice-ok" \
    "nicetest: reaped-pid-rejected-ok" \
    "nicetest: priority-syscalls-ok" \
    "date: printed-ok" \
    "date: roundtrip-ok" \
    "typing shell commands for the wildcard glob demo" \
    "globcat: got 2 files" \
    "globcat: /tmp/gd/a.txt=a" \
    "globcat: /tmp/gd/b.txt=b" \
    "timetest: create-stamped-ok" \
    "timetest: write-stamped-ok" \
    "timetest: hardlink-shares-mtime-ok" \
    "timetest: PASS" \
    "rmtest: tmpfs-reject-ok" \
    "rmtest: tmpfs-remove-ok" \
    "rmtest: sfs-reject-ok" \
    "rmtest: sfs-remove-ok" \
    "rmtest: PASS" \
    "permtest: default-mode-ok" \
    "permtest: write-denied-ok" \
    "permtest: write-restored-ok" \
    "permtest: copy-not-executable-ok" \
    "permtest: exec-denied-ok" \
    "permtest: exec-after-chmod-ok" \
    "permtest: PASS" \
    "identtest: root-by-default-ok" \
    "identtest: owner-root-ok" \
    "identtest: owner-write-ok-before-drop" \
    "identtest: owner-exec-ok-before-drop" \
    "identtest: chown-root-ok" \
    "identtest: drop-to-1000-ok" \
    "identtest: cannot-regain-root-ok" \
    "identtest: setuid-self-noop-ok" \
    "identtest: write-denied-non-owner-ok" \
    "identtest: exec-denied-non-owner-ok" \
    "identtest: chmod-nonowner-denied-ok" \
    "identtest: chown-nonroot-denied-ok" \
    "identtest: chmod-owner-after-drop-ok" \
    "identtest: chown-owner-nonroot-denied-ok" \
    "identtest: PASS" \
    "dirpermtest: setup-ok" \
    "dirpermtest: root-self-write-denied-ok" \
    "dirpermtest: drop-to-1000-ok" \
    "dirpermtest: parent-write-denied-ok" \
    "dirpermtest: other-write-allowed-ok" \
    "dirpermtest: other-unlink-allowed-ok" \
    "dirpermtest: traverse-denied-ok" \
    "dirpermtest: traverse-allowed-ok" \
    "dirpermtest: list-denied-ok" \
    "dirpermtest: search-without-list-ok" \
    "dirpermtest: owner-write-allowed-ok" \
    "dirpermtest: PASS" \
    "grouptest: root-by-default-ok" \
    "grouptest: fresh-file-gid-stamped-ok" \
    "grouptest: chgrp-root-arbitrary-ok" \
    "grouptest: setgid-root-arbitrary-ok" \
    "grouptest: drop-to-6000-7000-ok" \
    "grouptest: setgid-nonroot-denied-ok" \
    "grouptest: setgid-self-noop-ok" \
    "grouptest: group-read-allowed-ok" \
    "grouptest: group-write-denied-ok" \
    "grouptest: other-fallback-denied-ok" \
    "grouptest: group-exec-allowed-ok" \
    "grouptest: group-exec-denied-ok" \
    "grouptest: chgrp-owner-self-gid-ok" \
    "grouptest: chgrp-owner-arbitrary-denied-ok" \
    "grouptest: chgrp-nonowner-nonroot-denied-ok" \
    "grouptest: PASS" \
    "pidfdtest: bad-target rejected" \
    "pidfdtest: pidfd-open-ok" \
    "pidfdtest: pidfd-send-ok" \
    "pidfdtest: pidfd-wait-ok" \
    "pidfdtest: reaped-target-detected-ok" \
    "pidfdtest: PASS" \
    "mmaptest: anon-zero-fill-ok" \
    "mmaptest: anon-write-read-ok" \
    "mmaptest: anon-munmap-ok" \
    "mmaptest: file-map-content-ok" \
    "mmapprotvictim: read-ok" \
    "mmaptest: protection-sigsegv-ok" \
    "mmaptest: exec-allowed-ok" \
    "mmapexecvictim: write-ok" \
    "mmaptest: exec-denied-sigsegv-ok" \
    "mmaptest: PASS" \
    "typing shell commands for the ls -l demo" \
    "f 17 data.txt " \
    "f 17 alias.txt " \
    "f 17 data.txt 20" \
    "typing shell commands for the &&/|| chaining demo" \
    "ao2"
do
    if grep -qF "$needle" "$log"; then
        echo "PASS: found '$needle'"
    else
        echo "FAIL: missing '$needle'"
        status=1
    fi
done

# In-kernel self-test battery (ktest). The summary line is machine-readable:
#   "KTEST SUMMARY: <tests> tests, <checks> checks, <failures> failed"
# The gate requires that line to exist, report 0 failures, and run at least
# one test. A hung or crashing test suppresses the line and fails the gate.
summary=$(grep -F "KTEST SUMMARY:" "$log" | tail -n 1)
if [ -z "$summary" ]; then
    echo "FAIL: no 'KTEST SUMMARY:' line (in-kernel tests did not complete)"
    status=1
else
    echo "PASS: $summary"
    tests=$(echo "$summary" | sed -n 's/.*: \([0-9]*\) tests.*/\1/p')
    fails=$(echo "$summary" | sed -n 's/.* \([0-9]*\) failed.*/\1/p')
    if [ "${tests:-0}" -lt 1 ]; then
        echo "FAIL: ktest ran 0 tests"
        status=1
    fi
    if [ "${fails:-1}" -ne 0 ]; then
        echo "FAIL: ktest reported $fails failed check(s)"
        status=1
    fi
fi

rm -f "$log"
if [ "$status" -eq 0 ]; then
    echo "ci-test: all boot and self-test checks passed"
else
    echo "ci-test: checks FAILED"
fi
exit $status
