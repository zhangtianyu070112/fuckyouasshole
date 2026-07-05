/**
 * @file    file_io.c
 * @brief   Portable file I/O implementation.
 */

#include "file_io.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>

/* --- Read entire file -------------------------------------------------- */

char* file_read_all(const char* path, size_t* out_size)
{
    if (!path) return NULL;

    FILE* fp = fopen(path, "rb");
    if (!fp) {
        LOG_WARN("Cannot open file for reading: %s", path);
        return NULL;
    }

    /* Get file size */
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize < 0) {
        fclose(fp);
        return NULL;
    }
    rewind(fp);

    size_t size = (size_t)fsize;
    char* buf = malloc(size + 1);
    if (!buf) {
        LOG_ERROR("Out of memory reading file: %s", path);
        fclose(fp);
        return NULL;
    }

    size_t read = fread(buf, 1, size, fp);
    fclose(fp);

    if (read != size) {
        LOG_WARN("Short read: %s (expected %zu, got %zu)", path, size, read);
    }

    buf[read] = '\0';  /* Null-terminate for text files */

    if (out_size) *out_size = read;
    return buf;
}

/* --- Write all --------------------------------------------------------- */

int file_write_all(const char* path, const void* data, size_t len)
{
    if (!path || !data) return -1;

    FILE* fp = fopen(path, "wb");
    if (!fp) {
        LOG_ERROR("Cannot open file for writing: %s", path);
        return -1;
    }

    size_t written = fwrite(data, 1, len, fp);
    fclose(fp);

    if (written != len) {
        LOG_ERROR("Short write: %s (expected %zu, wrote %zu)", path, len, written);
        return -1;
    }

    return 0;
}

/* --- Exists ------------------------------------------------------------ */

int file_exists(const char* path)
{
    if (!path) return 0;
    struct stat st;
    return (stat(path, &st) == 0);
}

/* --- Path join --------------------------------------------------------- */

char* path_join(char* buf, size_t buf_size, const char* dir, const char* filename)
{
    if (!buf || buf_size == 0) return NULL;
    if (!dir && !filename) { buf[0] = '\0'; return buf; }
    if (!dir)  { snprintf(buf, buf_size, "%s", filename); return buf; }
    if (!filename) { snprintf(buf, buf_size, "%s", dir); return buf; }

#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif

    size_t dlen = strlen(dir);
    int has_sep = (dlen > 0 && (dir[dlen - 1] == '/' || dir[dlen - 1] == '\\'));

    if (has_sep) {
        snprintf(buf, buf_size, "%s%s", dir, filename);
    } else {
        snprintf(buf, buf_size, "%s%c%s", dir, sep, filename);
    }

    return buf;
}
