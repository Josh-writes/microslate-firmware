#pragma once
#include <SDCardManager.h>
#include "sd_safe.h"
#include <cstdio>
#include <cstring>

// Read entire file into buf. Returns false if missing or too large. A file lost between sdWriteFile's remove and
// rename is read from its copy (path + ".tmp"), which was complete before the old file was removed.
static inline bool sdReadFile(const char* path, char* buf, size_t bufSize) {
    char tmp[96];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    const char* from = SdMan.exists(path) ? path : SdMan.exists(tmp) ? tmp : nullptr;
    if (!from) return false;
    auto f = SdMan.open(from, O_RDONLY);
    if (!f.isOpen()) return false;
    int n = f.read(buf, (int)(bufSize - 1));
    f.close();
    if (n <= 0) return false;
    buf[n] = '\0';
    return true;
}

// Write a string to a file (replaces it; sdWriteSafely). Returns false on error (the old file is then kept).
static inline bool sdWriteFile(const char* path, const char* content) {
    return sdWriteSafely(path, content, strlen(content));
}

// Parse an integer value from flat JSON: {"key":42,...}
static inline int jsonGetInt(const char* json, const char* key) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":", key);
    const char* p = strstr(json, needle);
    if (!p) return -1;
    p += strlen(needle);
    while (*p == ' ') p++;
    return atoi(p);
}

// Parse a string value from flat JSON: {"key":"value",...}
// Returns true if key found (even if value is empty string).
static inline bool jsonGetStr(const char* json, const char* key, char* out, size_t outSize) {
    char needle[32];
    snprintf(needle, sizeof(needle), "\"%s\":\"", key);
    const char* p = strstr(json, needle);
    if (!p) return false;
    p += strlen(needle);
    size_t i = 0;
    while (*p && i < outSize - 1) {
        if (*p == '"') break;
        if (*p == '\\' && *(p + 1)) {
            p++;
            switch (*p) {
                case '"':  out[i++] = '"';  break;
                case '\\': out[i++] = '\\'; break;
                case 'n':  out[i++] = '\n'; break;
                default:   out[i++] = *p;   break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return true;
}

// Append an escaped JSON string value (no surrounding quotes).
static inline void jsonAppendStr(char* buf, size_t bufSize, const char* src) {
    size_t pos = strlen(buf);
    while (*src && pos < bufSize - 2) {
        if (*src == '"' || *src == '\\') buf[pos++] = '\\';
        buf[pos++] = *src++;
    }
    buf[pos] = '\0';
}
