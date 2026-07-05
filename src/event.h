/**
 * @file    event.h
 * @brief   Event dispatch system — routes SDL events to registered handlers.
 *
 * Instruments and subsystems register callbacks for specific event types.
 * The main loop pumps all SDL events through this dispatcher.
 */

#ifndef EVENT_H
#define EVENT_H

#include <SDL2/SDL.h>

/* Maximum number of registered event handlers */
#define MAX_EVENT_HANDLERS 32

/**
 * @brief Event handler callback signature.
 * @param event  The SDL event to handle.
 * @param userdata  Opaque user pointer registered with the handler.
 * @return 1 if the event was consumed (stop propagation), 0 otherwise.
 */
typedef int (*EventHandler)(const SDL_Event* event, void* userdata);

/**
 * @brief Event system handle.
 */
typedef struct EventSys EventSys;

/* --- Lifecycle --------------------------------------------------------- */

/**
 * @brief Create the event dispatch system.
 * @return Event system handle, or NULL on failure.
 */
EventSys* eventsys_create(void);

/**
 * @brief Destroy the event system, freeing all handlers.
 */
void      eventsys_destroy(EventSys* es);

/* --- Registration ------------------------------------------------------ */

/**
 * @brief Register an event handler.
 * @param es        The event system.
 * @param handler   Callback function.
 * @param userdata  Opaque pointer passed to callback.
 * @param type      Specific SDL event type to watch, or SDL_FIRSTEVENT for all.
 * @return 0 on success, -1 if handler table is full.
 */
int eventsys_register(EventSys* es, EventHandler handler, void* userdata,
                      Uint32 type);

/**
 * @brief Pump and dispatch all pending SDL events.
 * @param es  The event system.
 * @return Number of events processed.
 */
int eventsys_pump(EventSys* es);

#endif /* EVENT_H */
