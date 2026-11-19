/**************************************************************/
/* DEMO/FMT.C */
/* https://github.com/LarryRuane/protothread */
/* Copyright (c) 2008-present Larry Ruane */
/* Distributed under the MIT software license, see the accompanying */
/* file LICENSE or https://opensource.org/licenses/MIT. */
/* SPDX-License-Identifier: MIT */
/**************************************************************/

/* A simple version of the Unix fmt filter: it fills lines with words, up to
 * a width, and keeps blank lines as paragraph breaks.
 *
 * The input is structured as lines of words, and the output as filled lines,
 * and the two don't line up. Jackson called this a structure clash. Conway
 * coined the word coroutine in 1963 for programs with this structure.
 *
 * Here the reader is one protothread, a loop over lines with a loop over
 * words inside it, and it sends each word to a queue. The writer is another
 * protothread, which receives words and fills lines. Each one is written the
 * way you'd describe it.
 *
 * Without threads, one side has to be turned inside out. With -e, the reader
 * becomes get_word(), which returns one word per call and keeps its place in
 * the current line in static variables between calls. The output is the
 * same.
 *
 *   ./ptfmt < file            fill to 75 columns
 *   ./ptfmt -w 40 < file      fill to 40 columns
 *   ./ptfmt -e < file         the same, without protothreads
 *
 * Unlike GNU fmt, it doesn't keep indentation, join sentences with two
 * spaces, or balance line lengths.
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "protothread.h"
#include "protothread_queue.h"

PT_QUEUE_DEFINE(wordq, char *)

static char paragraph[1] ;              /* a blank line, sent as a word */
#define PARAGRAPH paragraph

/* the writer's line, which both versions fill the same way */
static size_t width = 75 ;
static char *out ;
static size_t outlen ;

/*****************************************************************************
 * helper functions that both versions use
 */

static void
flush_line(void)
{
    if (outlen) {
        fwrite(out, 1, outlen, stdout) ;
        putchar('\n') ;
        outlen = 0 ;
    }
}

static void
put_word(char const * const w)
{
    size_t const n = strlen(w) ;

    if (outlen && outlen + 1 + n > width) {
        flush_line() ;
    }
    if (n > width) {
        puts(w) ;                       /* too long for any line */
        return ;
    }
    if (outlen) {
        out[outlen++] = ' ' ;
    }
    memcpy(out + outlen, w, n) ;
    outlen += n ;
}

static void
end_paragraph(void)
{
    flush_line() ;
    putchar('\n') ;
}

static bool_t
is_blank(char const *p)
{
    while (isspace((unsigned char)*p)) {
        p++ ;
    }
    return *p == '\0' ;
}

/* Copy the next word in *pp and advance past it, or return NULL at the end */
static char *
next_word(char ** const pp)
{
    char *p = *pp ;
    char *start ;

    while (isspace((unsigned char)*p)) {
        p++ ;
    }
    if (*p == '\0') {
        return NULL ;
    }
    start = p ;
    while (*p && !isspace((unsigned char)*p)) {
        p++ ;
    }
    *pp = p ;
    return strndup(start, p - start) ;
}

/*****************************************************************************
 * The protothread version
 */

typedef struct {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    wordq_t *words ;
    char *line ;                        /* from getline() */
    size_t cap ;
    char *p ;                           /* next unread character in line */
    char *word ;
} reader_context_t ;

static pt_t
reader_thr(void * const env)
{
    reader_context_t * const c = env ;
    pt_resume(c) ;

    while (getline(&c->line, &c->cap, stdin) >= 0) {
        if (is_blank(c->line)) {
            c->word = PARAGRAPH ;
            pt_queue_send(c, c->words, &c->word) ;
            continue ;
        }
        c->p = c->line ;
        while ((c->word = next_word(&c->p))) {
            pt_queue_send(c, c->words, &c->word) ;
        }
    }
    free(c->line) ;
    c->word = NULL ;
    pt_queue_send(c, c->words, &c->word) ;
    return PT_DONE ;
}

typedef struct {
    pt_thread_t pt_thread ;
    pt_func_t pt_func ;
    wordq_t *words ;
    char *word ;
} writer_context_t ;

static pt_t
writer_thr(void * const env)
{
    writer_context_t * const c = env ;
    pt_resume(c) ;

    for (;;) {
        pt_queue_receive(c, c->words, &c->word) ;
        if (c->word == NULL) {
            break ;
        }
        if (c->word == PARAGRAPH) {
            end_paragraph() ;
        } else {
            put_word(c->word) ;
            free(c->word) ;
        }
    }
    flush_line() ;
    return PT_DONE ;
}

static void
fmt_protothreads(void)
{
    struct protothread_s pt ;
    char *slot ;                        /* one word at a time is enough */
    wordq_t words ;
    reader_context_t reader = { .words = &words } ;
    writer_context_t writer = { .words = &words } ;

    protothread_init(&pt) ;
    wordq_init(&words, &slot, 1) ;
    pt_create(&pt, &reader.pt_thread, reader_thr, &reader) ;
    pt_create(&pt, &writer.pt_thread, writer_thr, &writer) ;
    while (protothread_run(&pt)) {
    }
    protothread_deinit(&pt) ;
}

/*****************************************************************************
 * The same reader turned inside out: each call returns the next word
 */

static char *
get_word(void)
{
    static char *line ;
    static size_t cap ;
    static char *p ;                    /* NULL when a new line is needed */
    char *word ;

    for (;;) {
        if (p == NULL) {
            if (getline(&line, &cap, stdin) < 0) {
                free(line) ;
                line = NULL ;
                return NULL ;
            }
            if (is_blank(line)) {
                return PARAGRAPH ;
            }
            p = line ;
        }
        word = next_word(&p) ;
        if (word) {
            return word ;
        }
        p = NULL ;
    }
}

static void
fmt_event_driven(void)
{
    char *word ;

    while ((word = get_word())) {
        if (word == PARAGRAPH) {
            end_paragraph() ;
        } else {
            put_word(word) ;
            free(word) ;
        }
    }
    flush_line() ;
}

int
main(int argc, char **argv)
{
    bool_t event_driven = false ;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-e")) {
            event_driven = true ;
        } else if (i + 1 < argc && !strcmp(argv[i], "-w")
                && atoi(argv[i + 1]) > 0) {
            width = atoi(argv[++i]) ;
        } else {
            fprintf(stderr, "usage: %s [-e] [-w width] < input\n", argv[0]) ;
            return 2 ;
        }
    }
    out = malloc(width + 1) ;
    if (out == NULL) {
        perror("malloc") ;
        return 1 ;
    }
    if (event_driven) {
        fmt_event_driven() ;
    } else {
        fmt_protothreads() ;
    }
    free(out) ;
    return 0 ;
}
