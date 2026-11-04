# OnlineGDB examples

These are the sources behind the OnlineGDB projects in the main README's
[Try it online](../README.md#try-it-online) section:

| File | Project |
|---|---|
| [`hello.c`](hello.c) | [Hello, world](https://onlinegdb.com/UBllxDHRj) |
| [`pc.c`](pc.c) | [Producer/consumer](https://onlinegdb.com/GtGhLrlr8) |
| [`pc_expanded.c`](pc_expanded.c) | [Producer/consumer with the macros expanded](https://onlinegdb.com/m5UvTW_YK) |

They're kept here in case a project link stops working. If that happens, a
maintainer (or anyone else) can re-create a project. OnlineGDB's editor won't
follow the debugger into a second file, so each project is a single `main.c`
with `protothread.h` pasted in. `make-main.sh` does the pasting:

```
./make-main.sh pc.c > main.c
```

At <https://www.onlinegdb.com/>, click Sign Up or Login (you need an
account, which is free). Click Create New Project, set
the language to C (at the upper right), and replace the small `main()` that's
there with the local `main.c`. Then click Share and select "Keep Forever" to
get the link. Copy it into this README and the main README.

Each file also builds against the repo's header as it is:

```
cc -I.. pc.c && ./a.out
```
