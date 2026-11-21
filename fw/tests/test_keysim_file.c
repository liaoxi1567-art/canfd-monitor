/* SPDX-License-Identifier: MIT */
/* 键值表文件格式：编码 / 解码往返、电脑上手改的容错，以及各种坏文件一律拒绝。 */
#include "services/keysim/keysim_file.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static ks_table_t sample(void)
{
    ks_table_t t;
    memset(&t, 0, sizeof t);
    t.valid = true;
    snprintf(t.name, sizeof t.name, "学习 · K2");
    static const char *const names[] = { "上升", "一键上升", "下降", "一键下降", "对地短路" };
    static const uint16_t mv[] = { 3480, 2510, 1550, 680, 0 };
    static const float ohm[] = { 5100.4f, 2200.0f, 997.6f, 349.0f, NAN };
    for (unsigned i = 0; i < 5; i++) {
        snprintf(t.keys[i].name, sizeof t.keys[i].name, "%s", names[i]);
        t.keys[i].mv = mv[i];
        t.keys[i].ohm = ohm[i];
    }
    t.n = 5;
    return t;
}

static int decode_str(const char *s, ks_table_t *out) { return ks_table_decode(s, strlen(s), out); }

int main(void)
{
    char buf[KS_FILE_MAX_BYTES + 1], name[8];
    size_t len = 0;
    ks_table_t t = sample(), back;

    CHECK(ks_file_name(1, name, sizeof name) == KS_FILE_OK && !strcmp(name, "K2.TXT"));
    CHECK(ks_file_name(4, name, sizeof name) == KS_FILE_ERR_ARGUMENT);

    /* 往返 */
    CHECK(ks_table_encode(&t, 1, buf, sizeof buf, &len) == KS_FILE_OK);
    buf[len] = '\0';
    CHECK(!strncmp(buf, "CANFD-APMKEY 1\nchannel=2\nname=学习 · K2\nkey=上升,3480,5100\n", 63));
    CHECK(strstr(buf, "key=对地短路,0,-\n") && strstr(buf, "key=下降,1550,998\n"));
    CHECK(ks_table_decode(buf, len, &back) == KS_FILE_OK && back.valid && back.n == 5);
    CHECK(!strcmp(back.name, t.name));
    for (unsigned i = 0; i < 5; i++) {
        CHECK(!strcmp(back.keys[i].name, t.keys[i].name) && back.keys[i].mv == t.keys[i].mv);
        CHECK(i == 4 ? isnan(back.keys[i].ohm) : fabsf(back.keys[i].ohm - roundf(t.keys[i].ohm)) < 0.01f);
    }

    /* 电脑上编辑过：BOM、\r\n、注释、空行、channel 与文件名不同都能读 */
    CHECK(decode_str("\xEF\xBB\xBF" "CANFD-APMKEY 1\r\n# 主驾车窗\r\n\r\nchannel=4\r\nname=车型 C\r\nkey=上升,3500,-\r\n", &back) == KS_FILE_OK);
    CHECK(back.valid && back.n == 1 && !strcmp(back.name, "车型 C") && back.keys[0].mv == 3500 && isnan(back.keys[0].ohm));
    CHECK(decode_str("CANFD-APMKEY 1\nname=x\nkey=a,5000,0", &back) == KS_FILE_OK && back.keys[0].mv == 5000);   /* 最后一行没换行 */

    /* 坏文件 */
    struct { const char *text; int rc; } bad[] = {
        { "", KS_FILE_ERR_HEADER },
        { "CANFD-APMKEY 2\nname=x\nkey=a,1,-\n", KS_FILE_ERR_HEADER },
        { "name=x\nkey=a,1,-\n", KS_FILE_ERR_HEADER },
        { "CANFD-APMKEY 1\nkey=a,1,-\n", KS_FILE_ERR_NAME },                      /* 没有 name */
        { "CANFD-APMKEY 1\nname=x\n", KS_FILE_ERR_COUNT },                         /* 没有键 */
        { "CANFD-APMKEY 1\nname=x\nname=y\nkey=a,1,-\n", KS_FILE_ERR_COUNT },
        { "CANFD-APMKEY 1\nname=\nkey=a,1,-\n", KS_FILE_ERR_NAME },
        { "CANFD-APMKEY 1\nname=x\nkey=a,5001,-\n", KS_FILE_ERR_RANGE },
        { "CANFD-APMKEY 1\nname=x\nkey=a,-1,-\n", KS_FILE_ERR_SYNTAX },
        { "CANFD-APMKEY 1\nname=x\nkey=a,1\n", KS_FILE_ERR_SYNTAX },
        { "CANFD-APMKEY 1\nname=x\nkey=a,1,2,3\n", KS_FILE_ERR_SYNTAX },
        { "CANFD-APMKEY 1\nname=x\nkey=a,1,1.5\n", KS_FILE_ERR_SYNTAX },
        { "CANFD-APMKEY 1\nname=x\nkey=,1,-\n", KS_FILE_ERR_NAME },
        { "CANFD-APMKEY 1\nname=x\nkey=0123456789abcdef,1,-\n", KS_FILE_ERR_NAME },   /* 16 字节 */
        { "CANFD-APMKEY 1\nname=x\nkey=a\tb,1,-\n", KS_FILE_ERR_NAME },
        { "CANFD-APMKEY 1\nname=\xE4\xB8\nkey=a,1,-\n", KS_FILE_ERR_NAME },       /* 截断的 UTF-8 */
        { "CANFD-APMKEY 1\nname=\xC0\xAF\nkey=a,1,-\n", KS_FILE_ERR_NAME },       /* 过长编码 */
        { "CANFD-APMKEY 1\nchannel=5\nname=x\nkey=a,1,-\n", KS_FILE_ERR_RANGE },
        { "CANFD-APMKEY 1\nname=x\ncolor=red\nkey=a,1,-\n", KS_FILE_ERR_SYNTAX },
        { "CANFD-APMKEY 1\nname=x\n key=a,1,-\n", KS_FILE_ERR_SYNTAX },
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = decode_str(bad[i].text, &back);
        if (rc != bad[i].rc)
            fprintf(stderr, "case %u: got %d want %d\n", i, rc, bad[i].rc);
        CHECK(rc == bad[i].rc && !back.valid && back.n == 0);
    }
    /* 9 个键 */
    char nine[512] = "CANFD-APMKEY 1\nname=x\n";
    for (unsigned i = 0; i < 9; i++)
        snprintf(nine + strlen(nine), sizeof nine - strlen(nine), "key=k%u,%u,-\n", i, 100u * i);
    CHECK(decode_str(nine, &back) == KS_FILE_ERR_COUNT);
    /* 内嵌 NUL、超长文件 */
    static const char nul[] = "CANFD-APMKEY 1\nname=a\0b\nkey=a,1,-\n";
    CHECK(ks_table_decode(nul, sizeof nul - 1, &back) == KS_FILE_ERR_SYNTAX);
    char big[KS_FILE_MAX_BYTES + 2];
    memset(big, '#', sizeof big);
    CHECK(ks_table_decode(big, sizeof big, &back) == KS_FILE_ERR_SIZE);

    /* 编码拒绝无效表，缓冲区不够时不输出 */
    ks_table_t bad_t = t;
    bad_t.valid = false;
    CHECK(ks_table_encode(&bad_t, 0, buf, sizeof buf, &len) != KS_FILE_OK);
    bad_t = t;
    snprintf(bad_t.keys[0].name, sizeof bad_t.keys[0].name, "a,b");
    CHECK(ks_table_encode(&bad_t, 0, buf, sizeof buf, &len) == KS_FILE_ERR_NAME);
    bad_t = t;
    bad_t.keys[2].mv = 6000;
    CHECK(ks_table_encode(&bad_t, 0, buf, sizeof buf, &len) == KS_FILE_ERR_NAME);
    CHECK(ks_table_encode(&t, 0, buf, 40, &len) == KS_FILE_ERR_SPACE);
    CHECK(ks_table_encode(&t, 4, buf, sizeof buf, &len) == KS_FILE_ERR_ARGUMENT);
    /* 满 8 个键、最长名字也放得下 1024 字节 */
    ks_table_t full = t;
    snprintf(full.name, sizeof full.name, "一二三四五六七");                    /* 21 字节 */
    for (unsigned i = 0; i < KS_MAX_KEYS; i++) {
        snprintf(full.keys[i].name, sizeof full.keys[i].name, "一键上升%u", i);   /* 13 字节 */
        full.keys[i].mv = 5000;
        full.keys[i].ohm = 99999999.0f;
    }
    full.n = KS_MAX_KEYS;
    CHECK(ks_table_encode(&full, 3, buf, sizeof buf, &len) == KS_FILE_OK && len < KS_FILE_MAX_BYTES);
    CHECK(ks_table_decode(buf, len, &back) == KS_FILE_OK && back.n == KS_MAX_KEYS);

    printf("Key table file format: %u checks passed\n", checks);
    return 0;
}
