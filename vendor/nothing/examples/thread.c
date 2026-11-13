#include "nb/nbase.h"

#include <stdio.h>

typedef struct thread_state {
    nb_atomic_int counter;
} thread_state;

static void *worker(void *arg)
{
    thread_state *state = arg;
    for (int i = 0; i < 100000; i++) {
        nb_atomic_int_fetch_add(&state->counter, 1);
    }
    return NULL;
}

int main(void)
{
    thread_state state;
    nb_atomic_int_init(&state.counter, 0);
    nb_thread *threads[4];

    for (size_t i = 0; i < NB_ARRAY_SIZE(threads); i++) {
        threads[i] = NULL;
        if (nb_thread_create(&threads[i], worker, &state, NULL) != NB_OK) {
            return 1;
        }
    }
    for (size_t i = 0; i < NB_ARRAY_SIZE(threads); i++) {
        nb_thread_join(threads[i], NULL);
        nb_thread_free(threads[i]);
    }
    printf("counter=%d\n", nb_atomic_int_load(&state.counter));

    nb_mutex *mutex = nb_mutex_new(0);
    nb_cond *cond = nb_cond_new();
    if (mutex && cond) {
        nb_mutex_lock(mutex);
        nb_status signalled = nb_cond_timedwait(cond, mutex, INT64_C(1000000));
        nb_mutex_unlock(mutex);
        printf("cond timedwait=%s\n", signalled == NB_OK ? "signalled" : "timeout");
    }
    nb_cond_free(cond);
    nb_mutex_free(mutex);
    return 0;
}
