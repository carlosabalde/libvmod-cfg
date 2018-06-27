#include <stdlib.h>
#include <stdio.h>

#include "helpers.h"

vmod_state_t vmod_state = {
    .locks.refs = 0,
    .locks.script = NULL
};
