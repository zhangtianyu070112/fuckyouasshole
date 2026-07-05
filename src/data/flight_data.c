/**
 * @file    flight_data.c
 * @brief   Thread-safe flight data container implementation.
 */

#include "flight_data.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>

/* --- Lifecycle --------------------------------------------------------- */

FlightData* flight_data_create(void)
{
    FlightData* fd = calloc(1, sizeof(FlightData));
    if (!fd) {
        LOG_ERROR("Out of memory allocating FlightData");
        return NULL;
    }

    fd->mutex = SDL_CreateMutex();
    if (!fd->mutex) {
        LOG_ERROR("SDL_CreateMutex failed: %s", SDL_GetError());
        free(fd);
        return NULL;
    }

    LOG_DEBUG("FlightData container created");
    return fd;
}

void flight_data_destroy(FlightData* fd)
{
    if (!fd) return;
    if (fd->mutex) {
        SDL_DestroyMutex(fd->mutex);
    }
    free(fd);
    LOG_DEBUG("FlightData container destroyed");
}

/* --- Thread-safe access ------------------------------------------------ */

void flight_data_snapshot(FlightData* fd, FlightDataValues* snapshot)
{
    if (!fd || !snapshot) return;
    SDL_LockMutex(fd->mutex);
    memcpy(snapshot, &fd->current, sizeof(FlightDataValues));
    SDL_UnlockMutex(fd->mutex);
}

void flight_data_update(FlightData* fd, const FlightDataValues* src)
{
    if (!fd || !src) return;
    SDL_LockMutex(fd->mutex);
    memcpy(&fd->current, src, sizeof(FlightDataValues));
    fd->update_count++;
    SDL_UnlockMutex(fd->mutex);
}

void flight_data_read_field(FlightData* fd, size_t offset,
                            size_t size, void* out)
{
    if (!fd || !out) return;
    if (offset + size > sizeof(FlightDataValues)) {
        LOG_WARN("flight_data_read_field: offset=%zu size=%zu out of bounds",
                 offset, size);
        return;
    }
    SDL_LockMutex(fd->mutex);
    memcpy(out, ((const uint8_t*)&fd->current) + offset, size);
    SDL_UnlockMutex(fd->mutex);
}
