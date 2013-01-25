/**************************************************************/
/* PROTOTHREAD_SEM.H */
/* Copyright (c) 2008, Larry Ruane, LeftHand Networks Inc. */
/* See license.txt */
/**************************************************************/
#ifndef PROTOTHREAD_SEM_H
#define PROTOTHREAD_SEM_H

#include "protothread.h"

typedef struct _pt_sem_env_t {
    pt_func_t pt_func ;
} pt_sem_env_t ;

pt_t pt_sem_acquire_f(pt_sem_env_t *c, unsigned int *value) ;
#define pt_sem_acquire(c, sem_env, value) pt_call(c, pt_sem_acquire_f, sem_env, value)

/* guaranteed not to break context */
void pt_sem_release(pt_sem_env_t *c, unsigned int *value) ;

#endif /* PROTOTHREAD_SEM_H */
