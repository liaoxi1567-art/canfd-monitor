/* 平台接口：状态机通过这些函数操作硬件。
 * 现在由 sim/env_sim.c 实现（模拟板子）；上板后由 platform/ 下的驱动实现。
 * 耗时操作（挂载、预分配、关闭文件、格式化、波特率探测）都是异步的：
 * 平台做完后投递对应的完成事件（见 app_types.h 的 EV_SD_* / EV_CAN_*）。 */
#ifndef PORT_H
#define PORT_H

#include "services/core/app_types.h"
#include "services/winsim/win_sim.h"
/* Same monotonic microsecond epoch used by received/transmitted frame stamps.
 * It may differ from the logical boot/gesture clock during startup. */
uint64_t port_capture_time_us(void);

/* ---- 电源与外设 ---- */
void port_pwr_hold(bool on);          /* PE2 */
void port_5v(bool on);                /* PE3：屏幕 + 两路收发器 */
void port_backlight(uint8_t pct);     /* PE9 PWM，0 = 关（同时让屏幕进睡眠） */
void port_chg_hi(bool on);            /* PE4 */
bool port_key_down(key_id_t k);       /* 读按键电平（开机阶段用） */
bool port_screen_present(void);       /* 开机时读屏幕 ID / 触摸地址判断 */
void port_screen_redraw(void);        /* Foreground request; renderer consumes once in display_task. */
char port_chip_rev(void);             /* 'V' 或 'Y' */
uint32_t port_serial(void);           /* 由 96 位 UID 生成 */
bool port_rtc_valid(void);
void port_rtc_mark_synced(void);

/* ---- CAN ---- */
bool port_can_start(int ch, uint32_t nbr, uint32_t dbr, bool fd, can_mode_t mode); /* false leaves the transceiver in standby */
uint32_t port_can_generation(int ch); /* Changes whenever queued CAN status/probe results become obsolete. */
void port_can_standby(int ch);        /* STB 拉高，控制器停止 */
bool port_can_tx(int ch, const can_frame_t *f); /* true only when accepted by the controller; not TX completion */
void port_can_tx_abort(int ch);       /* 清空 TX FIFO */
void port_can_flush_rx(void);         /* Foreground drain after both controllers stop. */
bool port_can_recover(int ch);        /* Bus-Off 恢复，成功前不得宣称正常 */
bool port_can_probe(int ch, uint32_t nbr, uint32_t dbr, bool data_phase, can_mode_t mode); /* 自动识别：试一档 */

/* ---- SD 卡 ---- */
typedef enum { FK_CFR, FK_ASC, FK_CANDUMP, FK_TXLIST, FK_UNKNOWN } file_kind_t;

typedef struct {
    char        name[48];
    file_kind_t kind;
    uint32_t    size_kb;
    uint32_t    nbr, dbr;
    bool        fd;
    uint8_t     channels;
    uint32_t    frames;
    uint32_t    duration_ms;
    bool        protected_;
    bool        is_session;   /* 录制会话目录 */
    bool        imported;     /* host candump 导入；nbr=0 表示日志没有声明速率 */
    bool        sample_available;
    bool        writing;       /* 尚未关闭的录制文件不可导出/回放 */
    bool        sample_truncated; /* host 录制样例满；实际SD驱动应流式写入 */
    uint32_t    sample_frames, sample_dropped;
} file_info_t;

bool     port_sd_present(void);
void     port_sd_mount(void);
void     port_sd_unmount(void);
void     port_sd_prealloc(uint32_t session, uint32_t seg, uint32_t seg_kb, uint32_t nbr0, uint32_t dbr0, bool fd0);
void     port_sd_next_segment(uint32_t seg);
/* 存储一条真实采集记录。host 端只保留有界样例，满后显式累计 sample_dropped。
 * 板级端须把帧交给 SD 流式写入队列，不应复制 host 的样例数组。 */
bool     port_sd_record_frame(const can_frame_t *frame); /* true: accepted by platform queue */
/* Fixed source mapping: source 0/CH1 -> target CAN1, source 1/CH2 -> target CAN2.
 * Read ordinal is within that source only. Returned timestamps are relative to
 * the first record in the entire file. Errors are records too; player skips them. */
bool     port_sd_replay_info(int idx, uint8_t source_ch, uint32_t *frames, uint64_t *duration_us);
bool     port_sd_read_frame(int idx, uint8_t source_ch, uint32_t ordinal, can_frame_t *out);
void     port_sd_write(uint32_t bytes);   /* 记账：录制写了多少字节 */
void     port_sd_reinit(void);
void     port_sd_close(void);
void     port_sd_delete_oldest(void);
void     port_sd_format(void);
void     port_sd_protect_toggle(int idx);
int      port_sd_file_count(void);
bool     port_sd_file_info(int idx, file_info_t *out);
uint32_t port_sd_free_kb(void);
uint32_t port_sd_total_kb(void);
uint32_t port_sd_next_session(void);
bool     port_sd_busy(void);              /* SD 正在长时间写忙（回放可能断流） */

/* ---- USB ---- */
void port_usb_msc(bool share);

/* ---- 按键模拟（Hardware-C 第 12 页，见 services/winsim、services/keysim） ----
 * 只有启用按键模拟的目标需要实现；Rev D/E 目标上 keysim 编译成空壳，不引用这两个函数。 */
bool port_keysim_hal(winsim_hal_t *out);   /* 填好驱动 HAL 并返回 true；板上没有这部分硬件时返回 false */
int  port_keysim_apm_state(uint8_t ch);    /* APM 上报的开关状态（编号见 keysim_state_name）；读不到返回 -1 */

#endif
