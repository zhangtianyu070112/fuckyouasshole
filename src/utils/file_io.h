/**
 * @file    file_io.h
 * @brief   Portable file I/O utilities.
 *
 * Provides:
 *   - Read entire file into a malloc'd buffer
 *   - Write buffer to file
 *   - Path joining with platform separator
 *   - File existence check
 */

#ifndef FILE_IO_H
#define FILE_IO_H

#include <stdint.h>
#include <stddef.h>

/**
 * @brief Read entire file into a heap-allocated buffer.
 * @param path      File path.
 * @param out_size  If non-NULL, receives the file size.
 * @return malloc'd buffer (caller must free), or NULL on failure.
 */
char* file_read_all(const char* path, size_t* out_size);

/**
 * @brief Write a buffer to a file (overwrites if exists).
 * @param path  File path.
 * @param data  Data to write.
 * @param len   Number of bytes.
 * @return 0 on success, -1 on failure.
 */
int file_write_all(const char* path, const void* data, size_t len);

/**
 * @brief Check if a file exists and is readable.
 * @return 1 if exists, 0 otherwise.
 */
int file_exists(const char* path);

/**
 * @brief Join directory and filename with platform path separator.
 * @param buf       Output buffer.
 * @param buf_size  Buffer capacity.
 * @param dir       Directory path.
 * @param filename  File name.
 * @return buf (for chaining).
 */
char* path_join(char* buf, size_t buf_size, const char* dir, const char* filename);

#endif /* FILE_IO_H */
