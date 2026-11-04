#!/bin/sh
# Paste protothread.h into an example, making the single main.c OnlineGDB needs.
# Usage: ./make-main.sh pc.c > main.c
set -e
[ $# -eq 1 ] || { echo "usage: $0 example.c > main.c" >&2; exit 1; }
header="$(dirname "$0")/../protothread.h"
[ -r "$header" ] || { echo "$0: can't read $header" >&2; exit 1; }
awk -v header="$header" '
/^#include "protothread.h"$/ {
    while ((getline line < header) > 0) print line
    print ""
    print "/******************** END OF protothread.h ************************************/"
    next
}
{ print }
' "$1"
