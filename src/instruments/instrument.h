/**
 * @file    instrument.h
 * @brief   Common instrument interface ("vtable" pattern).
 *
 * Every cockpit instrument (PFD, ND, EICAS, FMC, map) implements this
 * interface. The application owns an array of Instrument pointers and
 * calls on_init/on_update/on_render/on_event in order each frame.
 *
 * To add a new instrument:
 *   1. Implement these 5 callbacks in a new .c file.
 *   2. Write a public constructor (e.g., pfd_create(...)) that returns
 *      a heap-allocated Instrument with all fields filled in.
 *   3. Register the instrument in app.c during initialization.
 */

#ifndef INSTRUMENT_H
#define INSTRUMENT_H

#include <SDL2/SDL.h>

/* Forward declarations */
typedef struct App         App;
typedef struct FlightData  FlightData;

/**
 * @brief Instrument base structure — treat as an abstract base class.
 *
 * Fields with `set by app` are written by the application framework after
 * the constructor returns; the instrument should read them but not modify.
 */
typedef struct Instrument {
    /* --- Identification --- */
    const char* name;          /* e.g. "PFD", "ND", "EICAS" */

    /* --- Callbacks (must be set by constructor) --- */

    /** Called once after all instruments are created & SDL is ready. */
    void (*on_init)(struct Instrument* self, App* app);

    /**
     * @brief Called every frame before on_render.
     * @param data  Shared flight data (already locked by caller, or NULL).
     * @param dt    Delta time in seconds since last frame.
     */
    void (*on_update)(struct Instrument* self, const FlightData* data, float dt);

    /** Called every frame to draw the instrument. */
    void (*on_render)(struct Instrument* self, SDL_Renderer* r);

    /**
     * @brief Handle an SDL event that was routed to this instrument.
     * @return 1 if consumed, 0 otherwise.
     */
    int  (*on_event)(struct Instrument* self, const SDL_Event* ev);

    /** Called once on shutdown to free instrument-private resources. */
    void (*on_destroy)(struct Instrument* self);

    /* --- Layout (set by app after creation) --- */
    SDL_Rect rect;             /* Screen region for this instrument */
    int      needs_redraw;     /* Hint: set to 1 to force full redraw */

    /* --- Private data (instrument owns this, frees in on_destroy) --- */
    void*    private_data;

} Instrument;

#endif /* INSTRUMENT_H */
