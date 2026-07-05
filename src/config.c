/**
 * @file    config.c
 * @brief   INI-style configuration parser implementation.
 *
 * Internal storage: array of sections, each containing an array of key-value
 * pairs. All strings are copied into heap memory on load.
 */

#include "config.h"
#include "utils/logger.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* --- Internal data structures ------------------------------------------ */

#define MAX_SECTIONS  32
#define MAX_KEYS      64
#define MAX_LINE_LEN  512
#define MAX_KEY_LEN   128
#define MAX_VAL_LEN   384

typedef struct {
    char key[MAX_KEY_LEN];
    char val[MAX_VAL_LEN];
} CfgEntry;

typedef struct {
    char     name[MAX_KEY_LEN];
    CfgEntry entries[MAX_KEYS];
    int      entry_count;
} CfgSection;

struct Config {
    CfgSection sections[MAX_SECTIONS];
    int        section_count;
};

/* --- Helpers ----------------------------------------------------------- */

static char* trim(char* str)
{
    while (isspace((unsigned char)*str)) str++;
    if (*str == '\0') return str;
    char* end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}

static CfgSection* add_section(Config* cfg, const char* name)
{
    if (cfg->section_count >= MAX_SECTIONS) {
        LOG_ERROR("Too many config sections (max %d)", MAX_SECTIONS);
        return NULL;
    }
    CfgSection* sec = &cfg->sections[cfg->section_count++];
    strncpy(sec->name, name, MAX_KEY_LEN - 1);
    sec->name[MAX_KEY_LEN - 1] = '\0';
    sec->entry_count = 0;
    return sec;
}

static int add_entry(CfgSection* sec, const char* key, const char* val)
{
    if (sec->entry_count >= MAX_KEYS) {
        LOG_ERROR("Too many keys in section [%s] (max %d)", sec->name, MAX_KEYS);
        return -1;
    }
    CfgEntry* e = &sec->entries[sec->entry_count++];
    strncpy(e->key, key, MAX_KEY_LEN - 1);
    e->key[MAX_KEY_LEN - 1] = '\0';
    strncpy(e->val, val, MAX_VAL_LEN - 1);
    e->val[MAX_VAL_LEN - 1] = '\0';
    return 0;
}

static const CfgEntry* find_entry(const CfgSection* sec, const char* key)
{
    if (!sec) return NULL;
    for (int i = 0; i < sec->entry_count; i++) {
        if (strcmp(sec->entries[i].key, key) == 0)
            return &sec->entries[i];
    }
    return NULL;
}

/* --- Load -------------------------------------------------------------- */
Config* config_load(const char* path)
{
    FILE* fp = fopen(path, "r");
    if (!fp) {
        LOG_WARN("Config file not found: %s (using defaults)", path);
        /* Return empty config — all gets will use defaults */
        Config* cfg = calloc(1, sizeof(Config));
        if (!cfg) {
            LOG_ERROR("Out of memory allocating Config");
        }
        return cfg;
    }

    Config* cfg = calloc(1, sizeof(Config));
    if (!cfg) {
        LOG_ERROR("Out of memory allocating Config");
        fclose(fp);
        return NULL;
    }

    char line[MAX_LINE_LEN];
    CfgSection* current_section = NULL;
    int line_no = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        char* p = trim(line);

        /* Skip empty lines and comments */
        if (*p == '\0' || *p == '#' || *p == ';') continue;

        /* Section header [name] */
        if (*p == '[') {
            char* end = strchr(p, ']');
            if (!end) {
                LOG_WARN("%s:%d: malformed section header", path, line_no);
                continue;
            }
            *end = '\0';
            current_section = add_section(cfg, trim(p + 1));
            continue;
        }

        /* Key = Value */
        char* eq = strchr(p, '=');
        if (!eq) {
            LOG_WARN("%s:%d: line without '=' ignored", path, line_no);
            continue;
        }

        *eq = '\0';
        char* key = trim(p);
        char* val = trim(eq + 1);

        if (*key == '\0') {
            LOG_WARN("%s:%d: empty key ignored", path, line_no);
            continue;
        }

        /* If we haven't seen a section header, use a default section */
        if (!current_section) {
            current_section = add_section(cfg, "global");
        }

        if (add_entry(current_section, key, val) != 0) {
            LOG_ERROR("%s:%d: failed to add entry '%s'", path, line_no, key);
        }
    }

    fclose(fp);
    LOG_INFO("Config loaded: %s (%d sections)", path, cfg->section_count);
    return cfg;
}

/* --- Free -------------------------------------------------------------- */
void config_free(Config* cfg)
{
    if (cfg) {
        free(cfg);
    }
}

/* --- Getters ----------------------------------------------------------- */
const char* config_get_str(const Config* cfg, const char* section,
                           const char* key, const char* default_val)
{
    const CfgSection* sec = NULL;
    for (int i = 0; i < cfg->section_count; i++) {
        if (strcmp(cfg->sections[i].name, section) == 0) {
            sec = &cfg->sections[i];
            break;
        }
    }
    const CfgEntry* entry = find_entry(sec, key);
    return entry ? entry->val : default_val;
}

int64_t config_get_int(const Config* cfg, const char* section,
                       const char* key, int64_t default_val)
{
    const char* str = config_get_str(cfg, section, key, NULL);
    if (!str) return default_val;
    char* end = NULL;
    int64_t val = strtoll(str, &end, 10);
    return (end != str) ? val : default_val;
}

double config_get_double(const Config* cfg, const char* section,
                         const char* key, double default_val)
{
    const char* str = config_get_str(cfg, section, key, NULL);
    if (!str) return default_val;
    char* end = NULL;
    double val = strtod(str, &end);
    return (end != str) ? val : default_val;
}

int config_get_bool(const Config* cfg, const char* section,
                    const char* key, int default_val)
{
    const char* str = config_get_str(cfg, section, key, NULL);
    if (!str) return default_val;
    if (strcmp(str, "1") == 0 || strcmp(str, "true")  == 0 ||
        strcmp(str, "yes") == 0 || strcmp(str, "on")   == 0) return 1;
    if (strcmp(str, "0") == 0 || strcmp(str, "false") == 0 ||
        strcmp(str, "no") == 0  || strcmp(str, "off")   == 0) return 0;
    return default_val;
}
