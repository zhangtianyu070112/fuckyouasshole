/**
 * @file    event.c
 * @brief   Event dispatch system implementation.
 */

#include "event.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

/* --- Internal structures ----------------------------------------------- */

typedef struct {
    EventHandler callback;
    void*        userdata;
    Uint32       type;       /* SDL event type, or SDL_FIRSTEVENT for any */
    int          active;
} EvtHandler;

struct EventSys {
    EvtHandler handlers[MAX_EVENT_HANDLERS];
    int        handler_count;
};

/* --- Lifecycle --------------------------------------------------------- */

EventSys* eventsys_create(void)
{
    EventSys* es = calloc(1, sizeof(EventSys));
    if (!es) {
        LOG_ERROR("Out of memory allocating EventSys");
        return NULL;
    }
    return es;
}

void eventsys_destroy(EventSys* es)
{
    if (es) free(es);
}

/* --- Registration ------------------------------------------------------ */

int eventsys_register(EventSys* es, EventHandler handler, void* userdata,
                      Uint32 type)
{
    if (!es || !handler) return -1;
    if (es->handler_count >= MAX_EVENT_HANDLERS) {
        LOG_ERROR("Event handler table full (max %d)", MAX_EVENT_HANDLERS);
        return -1;
    }
    EvtHandler* h = &es->handlers[es->handler_count++];
    h->callback = handler;
    h->userdata = userdata;
    h->type     = type;
    h->active   = 1;
    return 0;
}

/* --- Pump -------------------------------------------------------------- */

int eventsys_pump(EventSys* es)
{
    if (!es) return 0;

    SDL_Event event;
    int processed = 0;

    while (SDL_PollEvent(&event)) {
        processed++;

        /* Dispatch to matching handlers */
        for (int i = 0; i < es->handler_count; i++) {
            EvtHandler* h = &es->handlers[i];
            if (!h->active) continue;
            if (h->type != SDL_FIRSTEVENT && h->type != event.type) continue;

            if (h->callback(&event, h->userdata)) {
                break;  /* Event consumed — don't propagate further */
            }
        }
    }

    return processed;
}
