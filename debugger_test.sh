#!/usr/bin/env bash
# https://github.com/LarryRuane/protothread
# Copyright (c) 2008-present Larry Ruane
# Distributed under the MIT software license, see the accompanying
# file LICENSE or https://opensource.org/licenses/MIT.
# SPDX-License-Identifier: MIT
#
# Check the ptbt/ptbtall debugger commands in gdbinit and lldbinit.py.
# Stops protothread_test in test_pc() and test_barrier(), runs ptbtall in
# gdb and lldb, and checks that both print the expected threads.
# Usage: ./debugger_test.sh  (set CC, GDB or LLDB to override)

set -euo pipefail
cd "$(dirname "$0")"
CC=${CC:-cc} GDB=${GDB:-gdb}
if [ -z "${LLDB:-}" ] ; then
    # distributions often install only a versioned lldb-NN
    LLDB=$(compgen -c lldb | grep -E '^lldb(-[0-9]+)?$' | sort -t- -k2n |
        tail -1)
fi
for d in "$CC" "$GDB" "$LLDB" ; do
    if ! command -v "$d" > /dev/null ; then
        echo "${d:-lldb} not found (set CC, GDB or LLDB)" ; exit 1
    fi
done
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

$CC -g -O0 protothread_test.c -o "$tmp/pttest"

# stop 1: consumer resumed (running), producer waiting on the mailbox
pc_bp=consumer_thr
pc_expr='((pc_thread_context_t*)env)->pt_thread.s'
# stop 2: last thread arriving at a barrier, so all three states appear
bar_line=$(grep -n 'pt_broadcast(pt_get_pt(c), &b->gen)' protothread_test.c |
    cut -d: -f1)
bar_expr='c->pt_func.thread->s'

"$GDB" -nx -q -batch -x gdbinit \
    -ex "break $pc_bp" -ex 'ignore 1 1' -ex run -ex "ptbtall $pc_expr" \
    -ex delete -ex "break protothread_test.c:$bar_line" -ex continue \
    -ex "ptbtall $bar_expr" -ex kill "$tmp/pttest" > "$tmp/gdb.out" 2>&1 || :

bar_bp="breakpoint set -f protothread_test.c -l $bar_line"
"$LLDB" -x -b -o 'command script import lldbinit.py' \
    -o "breakpoint set -n $pc_bp -i 1" -o run -o "ptbtall $pc_expr" \
    -o 'breakpoint delete -f' -o "$bar_bp" -o continue \
    -o "ptbtall $bar_expr" -o kill "$tmp/pttest" > "$tmp/lldb.out" 2>&1 || :

# keep the state and frame lines, without addresses
summary() {
    { grep -E '^(state|#)' "$1" || : ; } | sed -E \
        -e 's/ p \*\(struct pt_thread_s \*\)0x[0-9a-f]+$//' \
        -e 's/ \(0x[0-9a-f]+\)//'
}

# sort each run of waiting threads, since bucket order follows their addresses
sort_waits() {
    awk '/^state/ { if (r) print r ; r = $0 ; next } { r = r "|" $0 }
        END { if (r) print r }' |
    awk '{ w = /^state: wait/ ; if (!w || !pw) g++
        pw = w ; print g "\t" $0 }' |
    sort -t "$(printf '\t')" -k1,1n -k2 | cut -f2- | tr '|' '\n'
}
summary "$tmp/gdb.out" | sort_waits > "$tmp/gdb.sum"
summary "$tmp/lldb.out" | sort_waits > "$tmp/lldb.sum"

fail=0
for d in gdb lldb ; do
    if grep -i -E 'error|not a valid command|undefined command' \
            "$tmp/$d.out" ; then
        echo "$d reported an error" ; fail=1
    fi
done
if ! diff -u "$tmp/gdb.sum" "$tmp/lldb.sum" ; then
    echo "gdb and lldb disagree" ; fail=1
fi

# each frame's line should be where that function blocks
while read -r _ func _ loc ; do
    line=${loc##*:}
    src=$(sed -n "${line}p" protothread_test.c)
    if ! grep -q -E 'pt_(wait|yield|call)\(' <<< "$src" ; then
        echo "$func at line $line doesn't block there: $src" ; fail=1
    fi
done < <(grep '^#' "$tmp/gdb.sum")

# the threads and states, without line numbers
expected='state: running
#0 consumer_thr
state: wait
#0 producer_thr
state: running
state: ready_to_run
#0 barrier_thr
state: ready_to_run
#0 barrier_thr
state: wait
#0 barrier_thr
#1 barrier_arrive
state: wait
#0 exchange_thr
#1 barrier_arrive'
if ! diff -u <(echo "$expected") <(sed 's/ at .*//' "$tmp/gdb.sum") ; then
    echo "unexpected threads or states" ; fail=1
fi

if [ $fail = 0 ] ; then
    echo PASS
else
    echo "--- gdb output" ; cat "$tmp/gdb.out"
    echo "--- lldb output" ; cat "$tmp/lldb.out"
fi
exit $fail
