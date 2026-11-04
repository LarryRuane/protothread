# OnlineGDB examples

These are the sources behind the OnlineGDB projects in the main README's
[Try it online](../README.md#try-it-online) section:

| File | Project |
|---|---|
| [`hello.c`](hello.c) | [Hello, world](https://onlinegdb.com/UBllxDHRj) |
| [`pc.c`](pc.c) | [Producer/consumer](https://onlinegdb.com/GtGhLrlr8) |
| [`pc_expanded.c`](pc_expanded.c) | [Producer/consumer with the macros expanded](https://onlinegdb.com/m5UvTW_YK) |

They're kept here in case a project link stops working. The comment at the
top of each file is what a visitor reads first, and the example is at the
bottom.

OnlineGDB's editor won't follow the debugger into a second file, so each
project is a single `main.c` with `protothread.h` pasted in where the
`#include` is. To make one:

```
awk '/^#include "protothread.h"$/ {
    while ((getline l < "../protothread.h") > 0) print l
    print ""
    print "/******************** END OF protothread.h ************************************/"
    next
} 1' pc.c > main.c
```

Then, at <https://www.onlinegdb.com/>, set the language to C, paste `main.c`
in, and click Share (or Save, when logged in) for a link. Update the links
here and in the main README.

Each file also builds against the repo's header as it is:

```
cc -I.. pc.c && ./a.out
```
