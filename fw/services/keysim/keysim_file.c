/* SPDX-License-Identifier: MIT */
#include "services/keysim/keysim_file.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define HEADER "CANFD-APMKEY 1"

int ks_file_name(uint8_t ch, char *out, size_t capacity)
{
    if (!out || ch >= WINSIM_NUM_CH || capacity < 7u)
        return KS_FILE_ERR_ARGUMENT;
    snprintf(out, capacity, "K%u.TXT", (unsigned)ch + 1u);
    return KS_FILE_OK;
}

const char *ks_file_result_text(int result)
{
    switch (result) {
    case KS_FILE_OK:           return "OK";
    case KS_FILE_ERR_ARGUMENT: return "参数无效";
    case KS_FILE_ERR_SPACE:    return "缓冲区不够";
    case KS_FILE_ERR_HEADER:   return "第一行不是 CANFD-APMKEY 1";
    case KS_FILE_ERR_SYNTAX:   return "有不认识的行或字段";
    case KS_FILE_ERR_NAME:     return "名称为空、太长或含非法字符";
    case KS_FILE_ERR_RANGE:    return "电压或电阻超出范围";
    case KS_FILE_ERR_COUNT:    return "键的数量不对";
    case KS_FILE_ERR_SIZE:     return "文件超过 1024 字节";
    default:                   return "未知错误";
    }
}

/* 合法 UTF-8、没有控制字符；forbid_comma 时也不许逗号。长度 1–max_bytes。 */
static bool name_ok(const char *s, size_t n, size_t max_bytes, bool forbid_comma)
{
    if (n == 0u || n > max_bytes)
        return false;
    for (size_t i = 0; i < n;) {
        unsigned char c = (unsigned char)s[i];
        size_t extra;
        if (c < 0x20u || c == 0x7Fu || (forbid_comma && c == ','))
            return false;
        if (c < 0x80u)
            extra = 0u;
        else if ((c & 0xE0u) == 0xC0u && c >= 0xC2u)
            extra = 1u;
        else if ((c & 0xF0u) == 0xE0u)
            extra = 2u;
        else if ((c & 0xF8u) == 0xF0u && c <= 0xF4u)
            extra = 3u;
        else
            return false;
        for (size_t k = 1; k <= extra; k++)   /* 续字节必须存在且为 10xxxxxx */
            if (i + k >= n || ((unsigned char)s[i + k] & 0xC0u) != 0x80u)
                return false;
        i += extra + 1u;
    }
    return true;
}

static bool table_ok(const ks_table_t *t)
{
    if (!t || !t->valid || t->n == 0u || t->n > KS_MAX_KEYS)
        return false;
    if (!name_ok(t->name, strnlen(t->name, sizeof t->name), sizeof t->name - 1u, false))
        return false;
    for (unsigned i = 0; i < t->n; i++) {
        const ks_key_t *k = &t->keys[i];
        if (!name_ok(k->name, strnlen(k->name, sizeof k->name), sizeof k->name - 1u, true) ||
            k->mv > KS_FILE_MAX_MV)
            return false;
    }
    return true;
}

int ks_table_encode(const ks_table_t *table, uint8_t ch, char *buffer, size_t capacity, size_t *length)
{
    size_t used = 0u;
    if (!buffer || !length || ch >= WINSIM_NUM_CH)
        return KS_FILE_ERR_ARGUMENT;
    if (!table_ok(table))
        return KS_FILE_ERR_NAME;
    if (capacity > KS_FILE_MAX_BYTES + 1u)
        capacity = KS_FILE_MAX_BYTES + 1u;
#define PUT(...)                                                              \
    do {                                                                      \
        int w_ = snprintf(buffer + used, capacity - used, __VA_ARGS__);       \
        if (w_ < 0 || (size_t)w_ >= capacity - used)                          \
            return KS_FILE_ERR_SPACE;                                         \
        used += (size_t)w_;                                                   \
    } while (0)
    if (capacity == 0u)
        return KS_FILE_ERR_SPACE;
    PUT("%s\nchannel=%u\nname=%s\n", HEADER, (unsigned)ch + 1u, table->name);
    for (unsigned i = 0; i < table->n; i++) {
        const ks_key_t *k = &table->keys[i];
        if (isfinite(k->ohm) && k->ohm >= 0.0f && k->ohm < 1e8f)
            PUT("key=%s,%u,%lu\n", k->name, (unsigned)k->mv, (unsigned long)(k->ohm + 0.5f));
        else
            PUT("key=%s,%u,-\n", k->name, (unsigned)k->mv);
    }
#undef PUT
    *length = used;
    return KS_FILE_OK;
}

/* 无符号十进制，0–max；不接受空串、符号和前导空格。 */
static bool parse_uint(const char *s, size_t n, unsigned long max, unsigned long *out)
{
    unsigned long v = 0u;
    if (n == 0u || n > 9u)
        return false;
    for (size_t i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9')
            return false;
        v = v * 10u + (unsigned long)(s[i] - '0');
    }
    if (v > max)
        return false;
    *out = v;
    return true;
}

static int parse_key(const char *s, size_t n, ks_key_t *k)
{
    const char *c1 = memchr(s, ',', n);
    if (!c1)
        return KS_FILE_ERR_SYNTAX;
    size_t name_n = (size_t)(c1 - s);
    const char *rest = c1 + 1;
    size_t rest_n = n - name_n - 1u;
    const char *c2 = memchr(rest, ',', rest_n);
    if (!c2)
        return KS_FILE_ERR_SYNTAX;
    size_t mv_n = (size_t)(c2 - rest);
    const char *ohm = c2 + 1;
    size_t ohm_n = rest_n - mv_n - 1u;
    unsigned long mv = 0u, r = 0u;

    if (!name_ok(s, name_n, sizeof k->name - 1u, true))
        return KS_FILE_ERR_NAME;
    if (!parse_uint(rest, mv_n, 99999999u, &mv))
        return KS_FILE_ERR_SYNTAX;
    if (mv > KS_FILE_MAX_MV)
        return KS_FILE_ERR_RANGE;
    memcpy(k->name, s, name_n);
    k->name[name_n] = '\0';
    k->mv = (uint16_t)mv;
    if (ohm_n == 1u && ohm[0] == '-') {
        k->ohm = NAN;
    } else {
        if (!parse_uint(ohm, ohm_n, 99999999u, &r))
            return KS_FILE_ERR_SYNTAX;
        k->ohm = (float)r;
    }
    return KS_FILE_OK;
}

int ks_table_decode(const char *text, size_t length, ks_table_t *out)
{
    size_t pos = 0u;
    bool header = false, have_name = false;
    if (!out)
        return KS_FILE_ERR_ARGUMENT;
    memset(out, 0, sizeof *out);
    if (!text)
        return KS_FILE_ERR_ARGUMENT;
    if (length > KS_FILE_MAX_BYTES)
        return KS_FILE_ERR_SIZE;
    if (length >= 3u && (unsigned char)text[0] == 0xEFu && (unsigned char)text[1] == 0xBBu &&
        (unsigned char)text[2] == 0xBFu)
        pos = 3u;   /* UTF-8 BOM */

    while (pos < length) {
        const char *line = text + pos;
        const char *nl = memchr(line, '\n', length - pos);
        size_t n = nl ? (size_t)(nl - line) : length - pos;
        pos += n + (nl ? 1u : 0u);
        if (n && line[n - 1u] == '\r')
            n--;
        if (memchr(line, '\0', n))
            return KS_FILE_ERR_SYNTAX;
        if (!header) {
            if (n != strlen(HEADER) || memcmp(line, HEADER, n) != 0) {
                memset(out, 0, sizeof *out);
                return KS_FILE_ERR_HEADER;
            }
            header = true;
            continue;
        }
        if (n == 0u || line[0] == '#')
            continue;
        int rc = KS_FILE_OK;
        if (n > 8u && memcmp(line, "channel=", 8u) == 0) {
            unsigned long ch = 0u;
            if (!parse_uint(line + 8, n - 8u, 99u, &ch))
                rc = KS_FILE_ERR_SYNTAX;
            else if (ch < 1u || ch > WINSIM_NUM_CH)
                rc = KS_FILE_ERR_RANGE;
        } else if (n >= 5u && memcmp(line, "name=", 5u) == 0) {
            if (have_name)
                rc = KS_FILE_ERR_COUNT;
            else if (!name_ok(line + 5, n - 5u, sizeof out->name - 1u, false))
                rc = KS_FILE_ERR_NAME;
            else {
                memcpy(out->name, line + 5, n - 5u);
                out->name[n - 5u] = '\0';
                have_name = true;
            }
        } else if (n >= 4u && memcmp(line, "key=", 4u) == 0) {
            if (out->n >= KS_MAX_KEYS)
                rc = KS_FILE_ERR_COUNT;
            else if ((rc = parse_key(line + 4, n - 4u, &out->keys[out->n])) == KS_FILE_OK)
                out->n++;
        } else {
            rc = KS_FILE_ERR_SYNTAX;
        }
        if (rc != KS_FILE_OK) {
            memset(out, 0, sizeof *out);
            return rc;
        }
    }
    if (!header) {
        memset(out, 0, sizeof *out);
        return KS_FILE_ERR_HEADER;
    }
    if (!have_name) {
        memset(out, 0, sizeof *out);
        return KS_FILE_ERR_NAME;
    }
    if (out->n == 0u) {
        memset(out, 0, sizeof *out);
        return KS_FILE_ERR_COUNT;
    }
    out->valid = true;
    return KS_FILE_OK;
}
