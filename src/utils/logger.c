/**
 * @file    logger.c
 * @brief   Logging subsystem implementation.
 */

#include "logger.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/time.h>
#endif

/* --- Module state ------------------------------------------------------ */
static LogLevel  g_min_level = LOG_LVL_INFO;
static FILE*     g_log_file  = NULL;
static int       g_initialized = 0;
#ifdef _WIN32
static CRITICAL_SECTION g_mutex;
#else
#include <pthread.h>
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

/* --- Level-to-string mapping ------------------------------------------- */
static const char* level_str(LogLevel level)
{
    switch (level) {
        case LOG_LVL_DEBUG: return "DEBUG";
        case LOG_LVL_INFO:  return "INFO";
        case LOG_LVL_WARN:  return "WARN";
        case LOG_LVL_ERROR: return "ERROR";
        default:            return "????";
    }
}

/* --- Initialization ---------------------------------------------------- */
int logger_init(LogLevel min_level, const char* log_file)
{
    if (g_initialized) return 0;  /* already initialized */

    g_min_level = min_level;

#ifdef _WIN32
    InitializeCriticalSection(&g_mutex);
#endif

    if (log_file && log_file[0] != '\0') {
        g_log_file = fopen(log_file, "a");
        if (!g_log_file) {
#ifdef _WIN32
            DeleteCriticalSection(&g_mutex);
#endif
            return -1;
        }
    }

    g_initialized = 1;
    return 0;
}

/* --- Shutdown ---------------------------------------------------------- */
void logger_shutdown(void)
{
    if (!g_initialized) return;

#ifdef _WIN32
    EnterCriticalSection(&g_mutex);
#else
    pthread_mutex_lock(&g_mutex);
#endif

    if (g_log_file) {
        fflush(g_log_file);
        fclose(g_log_file);
        g_log_file = NULL;
    }

#ifdef _WIN32
    LeaveCriticalSection(&g_mutex);
    DeleteCriticalSection(&g_mutex);
#else
    pthread_mutex_unlock(&g_mutex);
#endif

    g_initialized = 0;
}

/* --- Core write function ----------------------------------------------- */
void _logger_write(LogLevel level, const char* file, int line,
                   const char* func, const char* fmt, ...)
{
    if (!g_initialized) {
        /* Fallback: write directly to stderr before init */
        va_list args;
        va_start(args, fmt);
        fprintf(stderr, "[%s] %s:%d (%s): ", level_str(level), file, line, func);
        vfprintf(stderr, fmt, args);
        fprintf(stderr, "\n");
        va_end(args);
        return;
    }

    if (level < g_min_level) return;

    /* Timestamp */
    char time_buf[32];
#ifdef _WIN32
    SYSTEMTIME st;
    GetLocalTime(&st);
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d.%03d",
             st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    struct tm tm_info;
    localtime_r(&ts.tv_sec, &tm_info);
    snprintf(time_buf, sizeof(time_buf), "%02d:%02d:%02d.%03ld",
             tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec,
             ts.tv_nsec / 1000000L);
#endif

#ifdef _WIN32
    EnterCriticalSection(&g_mutex);
#else
    pthread_mutex_lock(&g_mutex);
#endif

    FILE* out = g_log_file ? g_log_file : stderr;
    fprintf(out, "[%s] %s %s:%d (%s): ",
            time_buf, level_str(level), file, line, func);

    va_list args;
    va_start(args, fmt);
    vfprintf(out, fmt, args);
    va_end(args);

    fprintf(out, "\n");
    fflush(out);

#ifdef _WIN32
    LeaveCriticalSection(&g_mutex);
#else
    pthread_mutex_unlock(&g_mutex);
#endif
}
