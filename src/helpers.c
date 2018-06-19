#include <stdlib.h>
#include <stdio.h>
#include <pthread.h>

#include "helpers.h"

vmod_state_t vmod_state = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .version = 0
};
