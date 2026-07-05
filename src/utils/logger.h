/**
 * @file    logger.h
 * @brief   Centralized logging system with severity levels and file/line info.
 *
 * Provides LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG macros that include
 * timestamp, source file, line number, and function name automatically.
 * Thread-safe: guards stdout with a mutex.
 */

#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <stdint.h>

/* --- Log severity levels ----------------------------------------------- */
typedef enum {
    LOG_LVL_DEBUG = 0,
    LOG_LVL_INFO  = 1,
    LOG_LVL_WARN  = 2,
    LOG_LVL_ERROR = 3,
    LOG_LVL_NONE  = 4
} LogLevel;

/* --- Initialization & cleanup ------------------------------------------ */

/**
 * @brief Initialize the logging subsystem.
 * @param min_level  Minimum severity to output (lower = more verbose).
 * @param log_file   Path to optional log file, or NULL for stdout only.
 * @return 0 on success, negative on failure.
 */
int  logger_init(LogLevel min_level, const char* log_file);

/**
 * @brief Shut down the logging subsystem (flushes & closes file if any).
 */
void logger_shutdown(void);

/* --- Internal core (used by macros) ------------------------------------ */
void _logger_write(LogLevel level, const char* file, int line,
                   const char* func, const char* fmt, ...);

/* --- Convenience macros ------------------------------------------------ */
#define LOG_ERROR(fmt, ...) \
    _logger_write(LOG_LVL_ERROR, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_WARN(fmt, ...) \
    _logger_write(LOG_LVL_WARN,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_INFO(fmt, ...) \
    _logger_write(LOG_LVL_INFO,  __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#define LOG_DEBUG(fmt, ...) \
    _logger_write(LOG_LVL_DEBUG, __FILE__, __LINE__, __func__, fmt, ##__VA_ARGS__)

#endif /* LOGGER_H */
