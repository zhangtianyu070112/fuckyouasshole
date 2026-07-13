/**
 * @file    tile_cache.h
 * @brief   LRU cache for downloaded 高德 static map tile textures.
 *
 * Stores up to TILE_CACHE_MAX SDL textures indexed by "z/x/y" string key.
 * Evicts least-recently-used tile when the cache is full.
 *
 * Thread safety: all public functions must be called with external locking.
 * The caller (cabin_old.c) holds a mutex around cache access.
 */

#ifndef TILE_CACHE_H
#define TILE_CACHE_H

#include <SDL2/SDL.h>
#include <stdint.h>

/* =========================================================================
 *  Constants
 * ========================================================================= */

/** Maximum number of cached tiles. */
#define TILE_CACHE_MAX  64

/* =========================================================================
 *  Types
 * ========================================================================= */

/** A single cached tile entry. */
typedef struct {
    char         key[32];       /* "z/x/y" string (e.g. "8/217/105") */
    SDL_Texture* texture;       /* Cached SDL texture, or NULL if slot empty */
    int          tex_w;         /* Texture width in pixels */
    int          tex_h;         /* Texture height in pixels */
    uint64_t     last_access;   /* SDL_GetTicks64() timestamp for LRU */
} CachedTile;

/** LRU tile cache. */
typedef struct {
    CachedTile    tiles[TILE_CACHE_MAX];
    int           count;           /* Number of occupied slots */
    SDL_Renderer* renderer;        /* For SDL_CreateTextureFromSurface */
} TileCache;

/* =========================================================================
 *  API
 * ========================================================================= */

/**
 * @brief Create a new tile cache.
 * @param renderer  SDL renderer (needed to create textures from surfaces).
 * @return Heap-allocated TileCache, or NULL on failure.
 */
TileCache* tile_cache_create(SDL_Renderer* renderer);

/**
 * @brief Destroy the cache and free all textures.
 */
void tile_cache_destroy(TileCache* tc);

/**
 * @brief Look up a tile in the cache.
 * @param key  Tile key in "z/x/y" format (integer values, e.g. "8/217/105").
 * @return The SDL_Texture if found, or NULL on cache miss.
 *         Updates last_access on hit.
 */
SDL_Texture* tile_cache_get(TileCache* tc, const char* key);

/**
 * @brief Insert a tile into the cache.
 *
 * Takes ownership of the SDL_Surface: converts it to a texture and frees
 * the surface. If the cache is full, evicts the LRU entry.
 *
 * @param key   Tile key in "z/x/y" format.
 * @param surf  SDL_Surface containing the decoded tile image (PNG/JPEG).
 *              This surface is freed by the cache.
 */
void tile_cache_put(TileCache* tc, const char* key, SDL_Surface* surf);

/**
 * @brief Clear all tiles from the cache (e.g. on zoom change).
 */
void tile_cache_clear(TileCache* tc);

#endif /* TILE_CACHE_H */
