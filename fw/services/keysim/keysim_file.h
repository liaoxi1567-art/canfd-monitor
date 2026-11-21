/* SPDX-License-Identifier: MIT */
/* 键值表文件：SD 卡 /APMKEY/K1.TXT … K4.TXT，每路一个，文件名决定通道。
 * UTF-8 文本，电脑上可以直接查看和修改，也可以拷到别的设备：
 *
 *   CANFD-APMKEY 1
 *   channel=2
 *   name=学习 · K2
 *   key=上升,3480,5100
 *   key=对地短路,0,-
 *
 * 第一行固定；channel 只作记录，读取时以文件名为准；name 必填，最多 23 字节；
 * key=名称,接口电压 mV,开关电阻 Ω（不知道写 -），名称最多 15 字节、不含逗号，电压 0–5000，
 * 1–8 个键。允许 \r\n 换行、开头的 UTF-8 BOM、空行和 # 开头的注释行；其它内容一律拒绝。
 * 纯函数，不碰硬件，模拟器和板级存储共用。 */
#ifndef KEYSIM_FILE_H
#define KEYSIM_FILE_H

#include <stddef.h>
#include <stdint.h>

#include "services/keysim/keysim.h"

#define KS_FILE_DIR       "APMKEY"
#define KS_FILE_MAX_BYTES 1024u
#define KS_FILE_MAX_MV    5000u

typedef enum {
    KS_FILE_OK = 0,
    KS_FILE_ERR_ARGUMENT = -1,
    KS_FILE_ERR_SPACE = -2,     /* 输出缓冲区不够 */
    KS_FILE_ERR_HEADER = -3,    /* 第一行不是 CANFD-APMKEY 1 */
    KS_FILE_ERR_SYNTAX = -4,    /* 不认识的行或字段格式不对 */
    KS_FILE_ERR_NAME = -5,      /* 名称为空、太长、含逗号或控制字符、不是合法 UTF-8 */
    KS_FILE_ERR_RANGE = -6,     /* 电压、电阻或通道超出范围 */
    KS_FILE_ERR_COUNT = -7,     /* 没有键，或超过 8 个，或 name 出现多次 */
    KS_FILE_ERR_SIZE = -8       /* 文件超过 1024 字节 */
} ks_file_result_t;

/* "K1.TXT" … "K4.TXT"；ch 为 0–3。 */
int ks_file_name(uint8_t ch, char *out, size_t capacity);

/* 生成文件内容（不含结尾 NUL 的长度写到 *length）。表无效或字段越界时不输出。 */
int ks_table_encode(const ks_table_t *table, uint8_t ch, char *buffer, size_t capacity, size_t *length);

/* 解析文件内容。失败时 *out 被清零（valid = false）。 */
int ks_table_decode(const char *text, size_t length, ks_table_t *out);

const char *ks_file_result_text(int result);

#endif
