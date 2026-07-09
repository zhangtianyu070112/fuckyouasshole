/**
 * @file    thread.h
 * @brief   Platform-agnostic thread and synchronization helpers.
 *
 * Wraps SDL thread/mutex for portability. Provides:
 *   - Thread creation/join
 *   - Mutex create/lock/unlock/destroy
 *   - Atomic flag for clean shutdown signaling
 */

#ifndef THREAD_H
#define THREAD_H

#include <SDL2/SDL.h>
#include <stdint.h>

/* --- Opaque types ------------------------------------------------------ */

/**
 * @brief A managed thread with shutdown signaling.
 */
typedef struct Thread {
    SDL_Thread*  sdl_thread;
    SDL_mutex*   mutex;
    int           running;      /* Protected by mutex */
    const char*   name;
} Thread;

/**
 * @brief Thread worker function signature.
 * @param userdata  Opaque pointer passed at creation.
 * @return Exit code (0 = success).
 */
typedef int (*ThreadFunc)(void* userdata);

/* --- Thread API -------------------------------------------------------- */

/**
 * @brief Create and start a new thread.
 * @param name      Human-readable name for debugging.
 * @param func      Worker function.
 * @param userdata  Passed to worker.
 * @return Thread handle, or NULL on failure.
 */
Thread* thread_create(const char* name, ThreadFunc func, void* userdata);

/**
 * @brief Signal the thread to stop (sets running = 0) and wait for join.
 * @param timeout_ms  Max wait time in ms, or -1 for infinite.
 * @return 0 if thread exited cleanly, -1 on timeout.
 */
int     thread_stop(Thread* thread, int timeout_ms);

/**
 * @brief Free thread resources. Call only after thread has stopped.
 */
void    thread_free(Thread* thread);

/**
 * @brief Check if this thread has been asked to stop.
 *        Worker functions should call this in their main loop.
 * @return 1 if should stop, 0 otherwise.
 */
int     thread_should_stop(Thread* thread);

/* --- Convenience: SDL mutex wrapper ------------------------------------ */

/**
 * @brief Lock the flight data mutex. Returns immediately if data is NULL.
 */
static inline void data_lock(SDL_mutex* m) { if (m) SDL_LockMutex(m); }
static inline void data_unlock(SDL_mutex* m) { if (m) SDL_UnlockMutex(m); }

#endif /* THREAD_H */
