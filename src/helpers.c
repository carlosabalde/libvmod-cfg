#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "cache/cache.h"

#include "helpers.h"

/******************************************************************************
 * VMOD STATE.
 *****************************************************************************/

vmod_state_t vmod_state = {
    .locks.refs = 0,
    .locks.script = NULL
};

/******************************************************************************
 * TASK STATE.
 *****************************************************************************/

static pthread_once_t shared_task_state_thread_once = PTHREAD_ONCE_INIT;
static pthread_key_t shared_task_state_thread_key;

static shared_task_state_t *
new_shared_task_state()
{
    shared_task_state_t *result;
    ALLOC_OBJ(result, SHARED_TASK_STATE_MAGIC);
    AN(result);

    result->script.state = NULL;
    result->script.free = NULL;

    return result;
}

static void
free_shared_task_state(shared_task_state_t *state)
{
    if (state->script.state != NULL && state->script.free != NULL) {
        (*state->script.free)(state->script.state);
    }

    FREE_OBJ(state);
}

static void
make_shared_task_state_key()
{
    AZ(pthread_key_create(
        &shared_task_state_thread_key,
        (void *) free_shared_task_state));
}

void init_shared_task_state()
{
    AZ(pthread_once(
        &shared_task_state_thread_once,
        make_shared_task_state_key));
}

shared_task_state_t *get_shared_task_state()
{
    shared_task_state_t *result = pthread_getspecific(shared_task_state_thread_key);

    if (result == NULL) {
        result = new_shared_task_state();
        pthread_setspecific(shared_task_state_thread_key, result);
    } else {
        CHECK_OBJ(result, SHARED_TASK_STATE_MAGIC);
    }

    return result;
}
