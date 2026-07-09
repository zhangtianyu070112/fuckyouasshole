/**
 * @file    config.h
 * @brief   Simple INI-style configuration file parser.
 *
 * Supports:
 *   [sections]
 *   key = value
 *   # comments
 *
 * Values are always stored as strings; convenience getters provide
 * int/float/bool/string typed access with defaults.
 */

#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>

/* Opaque config handle */
typedef struct Config Config;

/* --- Lifecycle --------------------------------------------------------- */

/**
 * @brief Load and parse a configuration file.
 * @param path  Path to .cfg file.
 * @return Config handle, or NULL on failure (logged).
 */
Config* config_load(const char* path);

/**
 * @brief Free all memory associated with the config.
 */
void    config_free(Config* cfg);

/* --- Typed getters ----------------------------------------------------- */

/**
 * @brief Get a string value.
 * @return Pointer to internal string (do not free), or `default_val` if not found.
 */
const char* config_get_str(const Config* cfg, const char* section,
                           const char* key, const char* default_val);

/**
 * @brief Get an integer value.
 */
int64_t config_get_int(const Config* cfg, const char* section,
                       const char* key, int64_t default_val);

/**
 * @brief Get a double value.
 */
double config_get_double(const Config* cfg, const char* section,
                         const char* key, double default_val);

/**
 * @brief Get a boolean value (1/0, true/false, yes/no, on/off).
 */
int config_get_bool(const Config* cfg, const char* section,
                    const char* key, int default_val);

#endif /* CONFIG_H */
