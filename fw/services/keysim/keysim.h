/* SPDX-License-Identifier: MIT */
/* 按键模拟服务：把 winsim 驱动包装成界面可以直接调用的非阻塞状态机。
 *
 *   - 选通道（自动标定串联电阻）、按键（点按 / 保持）、手动电压、全部松开；
 *   - 学习：电阻阵保持高阻，每 50 ms 测一次接口电压，喂给 winsim_learn_feed()；
 *   - 扫描：从 4800 mV 每 50 mV 往下设，每档停 30 ms 后读 APM 上报的开关状态，记下门限；
 *   - 空闲时每 300 ms 测一次接口电压，用来显示 APM 是否上电、检测过压；
 *   - 键值表存 SD 卡 /APMKEY/K1.TXT … K4.TXT（格式见 keysim_file.h）：保存时排一个写请求，
 *     SD 卡每次就绪时排一个读请求；由存储所有者（板级 storage_files.inc、模拟器 env_sim.c）在
 *     卡空闲时执行，和发送方案的存储方式相同。
 *
 * 板上没有第 12 页硬件时（当前的 Rev D/E 目标），服务编译成空壳：不分配约 33 KB 的驱动上下文，
 * keysim_available() 返回 false，界面不显示入口，也不会产生 SD 请求。定义 CANFD_WINSIM_HW 可在目标板上启用。 */
#ifndef KEYSIM_H
#define KEYSIM_H

#include <stdbool.h>
#include <stdint.h>

#include "services/winsim/win_sim.h"

#define KS_MAX_KEYS   8
#define KS_MAX_EDGES  8
#define KS_NAME_LEN   16
#define KS_MANUAL     (-2)       /* pressed 的特殊值：手动电压 */
#define KS_SCAN_START_MV 4800u
#define KS_SCAN_STEP_MV  50u
#define KS_SCAN_HOLD_MS  30u

typedef enum { KS_IDLE, KS_PRESSING, KS_LEARNING, KS_SCANNING } ks_activity_t;
typedef enum { KS_TAP, KS_LATCH } ks_press_mode_t;   /* 点按 / 保持（再按一次松开） */

typedef struct {
    char     name[KS_NAME_LEN];
    uint16_t mv;                 /* 0 = 对地短路 */
    float    ohm;                /* 学习时反推的开关电阻；扫描得到的键为 NAN */
} ks_key_t;

typedef struct {
    bool     valid;
    char     name[24];
    uint8_t  n;
    ks_key_t keys[KS_MAX_KEYS];
} ks_table_t;

typedef struct {
    int16_t  from, to;           /* APM 开关状态 */
    uint16_t above_mv, below_mv; /* 状态在这两个电压之间切换 */
} ks_edge_t;

typedef struct {
    bool            available;
    uint8_t         channels;
    uint8_t         ch;
    bool            calibrated[WINSIM_NUM_CH];
    float           r_series[WINSIM_NUM_CH];
    uint16_t        port_mv;         /* 最近一次接口电压 */
    bool            supply_ok;       /* 最近一次空闲测量 ≥ min_open_mv */
    bool            ovp;
    ks_activity_t   activity;
    ks_press_mode_t press_mode;
    uint16_t        tap_ms;
    int8_t          pressed;         /* 键序号，-1 = 无，KS_MANUAL = 手动 */
    uint16_t        pressed_mv;
    uint16_t        manual_mv;
    winsim_status_t last_status;
    /* 学习 */
    uint16_t        learn_idle_mv;
    uint8_t         learn_n;
    uint8_t         learn_progress;  /* 0–100：当前档位稳定了多久 */
    ks_key_t        learn_keys[KS_MAX_KEYS];
    /* 扫描 */
    uint16_t        scan_mv;
    bool            scan_done;
    uint8_t         scan_n;
    ks_edge_t       scan_edges[KS_MAX_EDGES];
    /* 每路的键值表 */
    ks_table_t      tables[WINSIM_NUM_CH];
    /* SD 卡 */
    bool            sd_saved[WINSIM_NUM_CH];   /* 当前键值表已经写到 SD 卡 */
    int16_t         sd_error[WINSIM_NUM_CH];   /* 最近一次保存失败的原因，0 = 没有 */
    bool            sd_loaded;                 /* 这次插卡后已经读过 /APMKEY */
} keysim_t;

/* ---- SD 卡存储：由存储所有者在卡空闲时调用 ---- */
typedef enum { KS_IO_NONE, KS_IO_SAVE, KS_IO_LOAD } ks_io_kind_t;
#define KS_IO_ERR_UNAVAILABLE (-100)   /* SD 卡不在、没挂载或处于 U 盘模式 */
#define KS_IO_ERR_IO          (-101)   /* 读写失败或写保护 */
typedef struct {
    uint32_t   id;
    uint8_t    kind;                   /* ks_io_kind_t */
    uint8_t    ch;                     /* SAVE：0–3 */
    ks_table_t table;                  /* SAVE：要写的表（请求时的快照） */
} ks_io_request_t;

/* 有待处理的请求时复制一份并返回 true。读请求优先。 */
bool keysim_io_peek(ks_io_request_t *out);
/* 处理完成。result 为 0 或 KS_FILE_* / KS_IO_ERR_*。id 过期（期间又改了表）时忽略。 */
void keysim_io_complete(uint32_t id, int result);
/* 读请求里每读到一个有效文件调用一次。这一路有还没写盘的新表时不覆盖。 */
void keysim_table_loaded(uint8_t ch, const ks_table_t *table);
/* 重新写一次还没存上的表（界面里“键值表”一行的 OK）。 */
bool keysim_resave(void);

void            keysim_init(void);
void            keysim_tick(uint32_t ms);
const keysim_t *keysim_get(void);
bool            keysim_available(void);
bool            keysim_state_known(void);         /* 当前通道能读到 APM 开关状态 */
int             keysim_apm_state(void);           /* 当前通道 APM 开关状态，未知为 -1 */
const char     *keysim_state_name(int state);

winsim_status_t keysim_select(uint8_t ch);        /* 换通道并标定串联电阻 */
winsim_status_t keysim_recalibrate(void);
winsim_status_t keysim_press(int index);          /* 按 press_mode 按下第 index 个键；保持模式下再按一次松开 */
winsim_status_t keysim_manual_toggle(void);
void            keysim_manual_adjust(int delta_mv);
void            keysim_release(void);             /* 全部松开，回到高阻 */
void            keysim_set_press_mode(ks_press_mode_t mode);
winsim_status_t keysim_clear_ovp(void);
void            keysim_leave(void);               /* 离开按键模拟页：停止学习 / 扫描并松开 */

winsim_status_t keysim_learn_start(void);
void            keysim_learn_stop(void);
void            keysim_learn_rename(int index);   /* 在预设名称间循环 */
void            keysim_learn_clear(void);
bool            keysim_learn_save(void);

winsim_status_t keysim_scan_start(void);
void            keysim_scan_stop(void);
bool            keysim_scan_save(void);           /* 每档取中点存为键值表 */

#endif
