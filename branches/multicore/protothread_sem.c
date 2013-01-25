/**************************************************************/
/* PROTOTHREAD_SEM.C */
/* Larry Ruane */
/**************************************************************/
#include <string.h>

#include "protothread_sem.h"

/* This implementation is arguably not fair, because a thread can release
 * the semaphore and then acquire it again without blocking, even if there
 * are waiters.  But this has better performance (fewer context switches).
 * If a thread is worried about monopolizing the semaphore, it can call
 * pt_yield() just before the sem_acquire() (that's always safe since the
 * sem_acquire() can cause a context break anyway).
 *
 * Semaphore-acquire could be implemented as a macro, which would allow it
 * to use the caller's context and not require one of its own.
 */

pt_t
pt_sem_acquire_f(pt_sem_env_t *c, unsigned int *value)
{
    pt_resume(c) ;
    while (!(*value)) {
        pt_wait(c, value) ;
    }
    (*value) -- ;
    return PT_DONE ;
}

void
pt_sem_release(pt_sem_env_t *c, unsigned int *value)
{
    (*value) ++ ;
    pt_broadcast(pt_get_pt(c), value) ;
}
