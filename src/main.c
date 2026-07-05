/**
 * @file    main.c
 * @brief   Application entry point.
 *
 * IMPORTANT: Must use standard main() — NOT WinMain — because SDL2 on
 * Windows redefines main → SDL_main and provides its own WinMain wrapper
 * that handles subsystem initialization. Using WinMain directly bypasses
 * SDL2's init, causing a segfault on startup.
 */

#include <SDL2/SDL.h>  /* Must be included before main: SDL redefines main */
#include "app.h"
#include "utils/logger.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>

/* Defined in app.c */
extern int app_run_with_config(const char* config_path);

int main(int argc, char** argv)
{
    const char* config_path = (argc > 1) ? argv[1] : "config/default.cfg";
    return app_run_with_config(config_path);
}
