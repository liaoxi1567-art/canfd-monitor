/* SPDX-License-Identifier: MIT */
/*
 * 万能车窗按键模拟器（APM 售后测试扩展模块）核心驱动。
 *
 * 依据 apm.md，按 Hardware-C 原理图第 12 页第 2 版调整：
 *   - 电阻阵 13 位（apm.md 为 8 位），控制字为 uint16_t，查找表 8192 项（32 KB）；
 *   - 4 路接口 K1–K4 共用一套电阻阵，同一时刻只接通一路（硬件译码保证）；
 *   - 每路串联 PTC 和光耦继电器，选通道时用 0 Ω 直通档自动标定这段串联电阻；
 *   - 回读取在各路接口侧，1:4 分压，负载 400 kΩ；电阻阵高阻时可以被动测量原车开关。
 *   硬件映射见 win_sim_tca9539.h。
 *
 * C99，无动态内存，适用于裸机和 RTOS。同一个 ctx 不能被并发调用；
 * ctx 约 33 KB，请放在静态存储区，不要放在栈上。
 */
#ifndef WIN_SIM_H
#define WIN_SIM_H

#include <stdbool.h>
#include <stdint.h>

#define WINSIM_NUM_BITS    13u
#define WINSIM_LUT_SIZE    (1u << WINSIM_NUM_BITS)
#define WINSIM_MASK_ALL    ((uint16_t)(WINSIM_LUT_SIZE - 1u))
#define WINSIM_NUM_CH      4u   /* K1–K4 */
#define WINSIM_ADC_SAMPLES 5u   /* 每次回读取 5 次中值 */

typedef enum {
    WINSIM_OK = 0,
    WINSIM_ERR_PARAM,
    WINSIM_ERR_OVERVOLTAGE,   /* 接口电压 > ovp_threshold_mv（如误接 12 V），已锁定高阻 */
    WINSIM_ERR_NO_SUPPLY,     /* 空载电压 < min_open_mv（APM 未唤醒或未接） */
    WINSIM_ERR_VERIFY_FAIL,   /* 回读与目标（或理论值）偏差超过 verify_tol_mv */
    WINSIM_ERR_CALIBRATION    /* 串联电阻标定结果超出 [0, r_series_max_ohm]，如 PTC 断开 */
} winsim_status_t;

/* 硬件抽象。前五项名称沿用 apm.md。 */
typedef struct {
    void     (*shift_write)(uint16_t mask);   /* 写 13 位电阻阵控制字，整体同步生效 */
    void     (*set_oe)(bool enable);          /* false：全部开关和 4 路继电器强制关断；true：按已写入的值输出 */
    void     (*set_short_0r)(bool enable);    /* 0 Ω 对地直通 */
    uint16_t (*adc_read_mv)(void);            /* 所选通道的接口电压，已按分压比折算，单位 mV */
    void     (*delay_ms)(uint32_t ms);
    void     (*select_channel)(uint8_t ch);   /* 0–3 对应 K1–K4；只改通道地址，不碰电阻阵 */
} winsim_hal_t;

typedef struct {
    float    r_bit_ohm[WINSIM_NUM_BITS]; /* 电阻阵各位阻值；默认为标称值，可填实测值提高精度 */
    float    r_pullup_ohm;        /* APM 内部上拉 R295，默认 2200 */
    float    r_adc_load_ohm;      /* 回读分压总阻值 R815–R818 之一 + R819，默认 400000；0 表示忽略 */
    float    mos_rds_on_ohm;      /* 电阻阵开关管导通电阻，默认 5.0（BSS138 @ Vgs 3.3 V） */
    float    short_rds_on_ohm;    /* 直通管导通电阻，默认 0.05（AO3400A） */
    float    r_series_ohm;        /* 未标定通道使用的串联电阻（PTC + 继电器），默认 10 */
    float    r_series_max_ohm;    /* 标定结果的上限，超过视为故障，默认 100 */
    uint16_t ovp_threshold_mv;    /* 默认 5500 */
    uint16_t min_open_mv;         /* 默认 4000 */
    uint16_t settle_delay_ms;     /* 默认 10 */
    uint16_t channel_switch_ms;   /* 换通道后的等待，默认 5（TLP176AM 接通 ≤ 3 ms） */
    uint16_t verify_tol_mv;       /* 默认 80 */
    bool     verify_enable;       /* 默认 true；VCC2_5V_SW 为脉冲供电的车型设为 false */
    uint8_t  num_channels;        /* 板上实际通道数，1–4，默认 4 */
    uint16_t learn_drop_mv;       /* 学习：低于空闲电压这么多才算按下，默认 150 */
    uint16_t learn_stable_mv;     /* 学习：相邻采样差在此以内算稳定，默认 25 */
    uint16_t learn_stable_ms;     /* 学习：稳定这么久才记录，默认 500 */
} winsim_cfg_t;

typedef struct {
    uint8_t  channel;         /* 0–3 */
    uint16_t mask;            /* 13 位控制字（apm.md 中的 mask_595） */
    bool     short_0r;
    float    actual_r_ohm;    /* 接口对地等效电阻（含串联电阻，不含回读负载）；高阻时为 INFINITY */
    uint16_t target_mv;       /* 本次目标电压；按阻值设置时为 0 */
    uint16_t expected_mv;     /* 理论接口电压；未标定空载电压时为 0 */
    uint16_t measured_mv;     /* 回读电压 */
} winsim_state_t;

typedef struct {
    winsim_hal_t   hal;
    winsim_cfg_t   cfg;
    float          r_lut[WINSIM_LUT_SIZE];  /* r_lut[0] = INFINITY */
    float          r_series_ch[WINSIM_NUM_CH];
    bool           series_cal[WINSIM_NUM_CH];
    uint8_t        channel;
    uint16_t       last_open_mv;            /* 当前通道最近一次有效的空载电压，0 表示未标定 */
    winsim_state_t current_state;
    bool           initialized;
    bool           ovp_latched;
} winsim_ctx_t;

/* 扫描回调。回调里不要再调用本模块的函数。 */
typedef void (*winsim_sweep_cb_t)(const winsim_state_t *state, void *user_ctx);

winsim_cfg_t winsim_default_cfg(void);

/* 安全初始化：set_oe(false) → set_short_0r(false) → shift_write(0) → select_channel(0) → set_oe(true)，
 * 然后建查找表。cfg 为 NULL 时使用默认配置。 */
winsim_status_t winsim_init(winsim_ctx_t *ctx, const winsim_hal_t *hal, const winsim_cfg_t *cfg);

/* 关闭直通、清零控制字，接口恢复高阻。 */
winsim_status_t winsim_set_hiz(winsim_ctx_t *ctx);

/* 换到通道 ch（0–3）：先高阻，再改通道地址，等 channel_switch_ms。
 * calibrate 为 true 时接着标定这一路的串联电阻（会短暂接通 0 Ω 直通，约 2 × settle_delay_ms），
 * 结果写入 r_series_out（可为 NULL）。标定失败时这一路沿用上次的标定值或 cfg.r_series_ohm。
 * 过压锁定不会因换通道解除；换到正常的通道后调用 winsim_clear_ovp()。 */
winsim_status_t winsim_select_channel(winsim_ctx_t *ctx, uint8_t ch, bool calibrate, float *r_series_out);

/* 在当前通道上重新标定串联电阻：高阻测空载电压，再接通直通测一次，算出 PTC + 继电器的阻值。 */
winsim_status_t winsim_calibrate_series(winsim_ctx_t *ctx, float *r_series_out);

/* 切高阻、等待稳定、取中值读空载电压。超过 OVP 门限时锁定高阻。 */
winsim_status_t winsim_probe_open_voltage(winsim_ctx_t *ctx, uint16_t *v_open_mv);

/* 被动测量：保持高阻（若正在下拉则先断开并等待稳定），读当前接口电压。
 * 原车开关并在这一路时，读到的就是开关当前档位的电压。超过 OVP 门限时锁定。 */
winsim_status_t winsim_measure(winsim_ctx_t *ctx, uint16_t *mv);

/* 由被动测量的电压反推外部开关对地电阻（扣除 400 kΩ 回读负载）。
 * 需要先有空载电压（winsim_probe_open_voltage）；否则返回 NAN。
 * mv ≥ 空载电压时返回 INFINITY，mv 为 0 时返回 0。 */
float winsim_equiv_ohm(const winsim_ctx_t *ctx, uint16_t mv);

/* 按阻值设置。target_ohm 为模拟的按键对地电阻（含串联电阻，不含 400 kΩ 回读负载）；
 * 0 = 直通，INFINITY = 高阻。不做回读校验。out_state 可为 NULL。 */
winsim_status_t winsim_set_resistance(winsim_ctx_t *ctx, float target_ohm, winsim_state_t *out_state);

/* 按目标电压设置。未标定时先自动探测空载电压；
 * verify_enable 时检查 |回读 − 目标| ≤ verify_tol_mv。 */
winsim_status_t winsim_set_voltage(winsim_ctx_t *ctx, uint16_t target_mv, winsim_state_t *out_state);

/* 按分压比设置，ratio = 接口电压 / APM 内部电源，范围 [0, 1]；
 * 0 = 直通，1 = 高阻。不依赖电源绝对值。 */
winsim_status_t winsim_set_ratio(winsim_ctx_t *ctx, float ratio_0_to_1, winsim_state_t *out_state);

/* 自动扫描当前通道：先重新探测空载电压，再从 start_mv 到 end_mv（升序或降序）按 step_mv 逐档输出。
 * 每档稳定并回读后等待 hold_ms，再调用 on_step。步数 = |end − start| / step + 1。
 * 每档回读与理论值比较（不是与目标比较，避免无档位区间误报）。
 * 正常结束或出错时都恢复高阻。 */
winsim_status_t winsim_auto_sweep(winsim_ctx_t *ctx, uint16_t start_mv, uint16_t end_mv,
                                  uint16_t step_mv, uint16_t hold_ms,
                                  winsim_sweep_cb_t on_step, void *user_ctx);

/* 解除过压锁定：在高阻下重新测量当前通道，低于门限才解除。 */
winsim_status_t winsim_clear_ovp(winsim_ctx_t *ctx);

const char *winsim_status_str(winsim_status_t status);

/* ------------------------------------------------------------------ 按键学习
 * 纯算法，不碰硬件：调用方周期性地用 winsim_measure() 采样，把结果喂进来。
 * 规则：电压低于 idle − learn_drop_mv 算按下；相邻采样差 ≤ learn_stable_mv 持续
 * learn_stable_ms 后记录一档（取稳定段平均值）；之后必须回到空闲附近才能记录下一档。 */
typedef struct {
    uint16_t idle_mv;
    uint16_t drop_mv, stable_mv, stable_ms;
    bool     armed;
    bool     have_last;
    uint16_t last_mv;
    uint32_t stable_for_ms;
    uint32_t sum_mv;
    uint32_t n;
} winsim_learn_t;

void winsim_learn_begin(winsim_learn_t *l, const winsim_cfg_t *cfg, uint16_t idle_mv);

/* 喂一个采样，dt_ms 为距上一个采样的时间。记录到一档时返回 true，并写入 captured_mv。 */
bool winsim_learn_feed(winsim_learn_t *l, uint16_t mv, uint32_t dt_ms, uint16_t *captured_mv);

#endif /* WIN_SIM_H */
