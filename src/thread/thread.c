/**
 * @file    thread.c
 * @brief   Thread and synchronization helpers implementation.
 */

#include "thread.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

/* --- Internal glue ----------------------------------------------------- */

typedef struct {
    ThreadFunc  func;
    void*       userdata;
    Thread*     thread;  /* back-reference for should_stop check */
} ThreadWrapper;

static int thread_wrapper(void* data)
{
    ThreadWrapper* wrap = (ThreadWrapper*)data;
    LOG_INFO("Thread '%s' started", wrap->thread->name);
    int ret = wrap->func(wrap->userdata);
    LOG_INFO("Thread '%s' exited with code %d", wrap->thread->name, ret);
    return ret;
}

/* --- Thread API -------------------------------------------------------- */

Thread* thread_create(const char* name, ThreadFunc func, void* userdata)
{
    Thread* t = calloc(1, sizeof(Thread));
    if (!t) {
        LOG_ERROR("Out of memory allocating Thread '%s'", name);
        return NULL;
    }

    t->mutex = SDL_CreateMutex();
    if (!t->mutex) {
        LOG_ERROR("Failed to create mutex for thread '%s': %s", name, SDL_GetError());
        free(t);
        return NULL;
    }

    t->name    = name;
    t->running = 1;

    ThreadWrapper* wrap = malloc(sizeof(ThreadWrapper));
    if (!wrap) {
        LOG_ERROR("Out of memory allocating ThreadWrapper");
        SDL_DestroyMutex(t->mutex);
        free(t);
        return NULL;
    }
    wrap->func     = func;
    wrap->userdata = userdata;
    wrap->thread   = t;

    t->sdl_thread = SDL_CreateThread(thread_wrapper, name, wrap);
    if (!t->sdl_thread) {
        LOG_ERROR("Failed to create thread '%s': %s", name, SDL_GetError());
        SDL_DestroyMutex(t->mutex);
        free(wrap);
        free(t);
        return NULL;
    }

    return t;
}

int thread_stop(Thread* t, int timeout_ms)
{
    (void)timeout_ms;  /* TODO: implement timeout via SDL_WaitThreadTimeout */
    if (!t) return 0;

    /* Signal stop */
    SDL_LockMutex(t->mutex);
    t->running = 0;
    SDL_UnlockMutex(t->mutex);

    /* Wait for thread to finish */
    int status = 0;
    SDL_WaitThread(t->sdl_thread, &status);
    t->sdl_thread = NULL;

    return status;
}

void thread_free(Thread* t)
{
    if (!t) return;
    if (t->mutex)  SDL_DestroyMutex(t->mutex);
    free(t);
}

int thread_should_stop(Thread* t)
{
    if (!t) return 1;
    int running;
    SDL_LockMutex(t->mutex);
    running = t->running;
    SDL_UnlockMutex(t->mutex);
    return !running;
}
