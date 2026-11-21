/* SPDX-License-Identifier: MIT */
/*
 * 集成示例：CAN 唤醒 APM → 切到指定通道并标定串联电阻 → 确认 VCC2_5V_SW 已上电 →
 * 4800→0 mV 每 50 mV 扫描，每一档读 CAN 车窗开关状态报文，最后打印电压与开关状态的对应表和档位门限。
 *
 * 接入固件时，实现下面三个 apm_* 函数（用现有的 CAN 收发逻辑），
 * 用 winsim_tca9539_bind() 得到 HAL，再调用 winsim_example_run()。
 * 打印用的是 printf，固件里换成自己的日志输出。
 *
 * 用 -DWINSIM_EXAMPLE_SIM 编译时附带一个桌面仿真（假 APM 和假门限），可以直接运行：
 *   make -C fw winsim-example
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "services/winsim/win_sim.h"

/* ------------------------------------------------------------------ 由集成方实现 */

/* 发送唤醒报文，使 APM 打开 VCC2_5V_SW。 */
bool apm_can_send_wakeup(void);
/* 读取 APM 上报的车窗开关状态（车型相关的信号解码后的值）。超时返回 false。 */
bool apm_can_read_switch_state(uint8_t *state, uint32_t timeout_ms);
void apm_delay_ms(uint32_t ms);

/* ------------------------------------------------------------------ 示例流程 */

#define SWEEP_START_MV 4800u
#define SWEEP_END_MV   0u
#define SWEEP_STEP_MV  50u
#define SWEEP_HOLD_MS  30u     /* 留给 APM 去抖和发报文 */
#define CAN_TIMEOUT_MS 100u
#define MAX_EDGES      16u

static const char *switch_name(uint8_t s)
{
    switch (s) {
    case 0: return "空闲";
    case 1: return "上升";
    case 2: return "一键上升";
    case 3: return "下降";
    case 4: return "一键下降";
    case 5: return "故障";
    default: return "未知";
    }
}

typedef struct {
    bool     have_prev;
    uint8_t  prev_state;
    uint16_t prev_mv;
    unsigned n_edges;
    struct {
        uint16_t above_mv, below_mv;   /* 状态在这两个回读电压之间切换 */
        uint8_t  from, to;
    } edges[MAX_EDGES];
    unsigned can_timeouts;
} sweep_log_t;

static void on_step(const winsim_state_t *st, void *user)
{
    sweep_log_t *rec = (sweep_log_t *)user;
    uint8_t state = 0xFFu;
    if (!apm_can_read_switch_state(&state, CAN_TIMEOUT_MS)) {
        rec->can_timeouts++;
        state = 0xFFu;
    }

    printf("%5u  %5u  %5u  %04X  %s  %s\n", (unsigned)st->target_mv, (unsigned)st->expected_mv,
           (unsigned)st->measured_mv, (unsigned)st->mask, st->short_0r ? "Y" : "-", switch_name(state));

    if (rec->have_prev && state != rec->prev_state && rec->n_edges < MAX_EDGES) {
        rec->edges[rec->n_edges].above_mv = rec->prev_mv;
        rec->edges[rec->n_edges].below_mv = st->measured_mv;
        rec->edges[rec->n_edges].from = rec->prev_state;
        rec->edges[rec->n_edges].to = state;
        rec->n_edges++;
    }
    rec->have_prev = true;
    rec->prev_state = state;
    rec->prev_mv = st->measured_mv;
}

int winsim_example_run(winsim_ctx_t *ctx, uint8_t ch)
{
    static sweep_log_t slog;
    uint16_t v_open = 0u;
    float r_series = 0.0f;

    if (!apm_can_send_wakeup()) {
        printf("CAN 唤醒报文发送失败\n");
        return -1;
    }
    apm_delay_ms(200u);   /* 等 VCC2_5V_SW 建立 */

    winsim_status_t rc = winsim_select_channel(ctx, ch, true, &r_series);
    if (rc != WINSIM_OK) {
        printf("K%u 选通或标定失败：%s\n", (unsigned)ch + 1u, winsim_status_str(rc));
        return -1;
    }
    printf("K%u 已接通，串联电阻 %.1f Ω\n", (unsigned)ch + 1u, (double)r_series);

    rc = winsim_probe_open_voltage(ctx, &v_open);
    if (rc != WINSIM_OK) {
        printf("空载电压 %u mV：%s\n", (unsigned)v_open, winsim_status_str(rc));
        return -1;
    }
    printf("VCC2_5V_SW 已上电，空载电压 %u mV\n\n", (unsigned)v_open);
    printf(" 目标   理论   回读  控制字 直通 CAN 开关状态\n");

    memset(&slog, 0, sizeof slog);
    rc = winsim_auto_sweep(ctx, SWEEP_START_MV, SWEEP_END_MV, SWEEP_STEP_MV, SWEEP_HOLD_MS, on_step, &slog);
    if (rc != WINSIM_OK) {
        printf("扫描中止：%s（接口已恢复高阻）\n", winsim_status_str(rc));
        return -1;
    }

    printf("\n档位门限（按回读电压）：\n");
    for (unsigned i = 0; i < slog.n_edges; i++)
        printf("  %4u–%4u mV  %s → %s\n", (unsigned)slog.edges[i].below_mv, (unsigned)slog.edges[i].above_mv,
               switch_name(slog.edges[i].from), switch_name(slog.edges[i].to));
    if (slog.can_timeouts)
        printf("  有 %u 档未收到 CAN 状态报文\n", slog.can_timeouts);
    return 0;
}

/* ------------------------------------------------------------------ 桌面仿真 */
#ifdef WINSIM_EXAMPLE_SIM

/* 仿真 APM：2.2 kΩ 上拉到 5 V，唤醒后才供电；每路串联 PTC + 继电器约 12 Ω。门限是编的，只为演示表格。 */
static bool     sim_awake;
static uint16_t sim_mask;
static bool     sim_short, sim_oe;
static uint8_t  sim_ch;
static const float sim_series[WINSIM_NUM_CH] = { 12.4f, 9.8f, 15.1f, 11.2f };

static float sim_port_mv(void)
{
    static const float bits[WINSIM_NUM_BITS] = {
        200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
        13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
    };
    float g_in = 0.0f;
    if (sim_oe && sim_short)
        g_in += 1.0f / 0.05f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (sim_oe && (sim_mask & (1u << i)))
            g_in += 1.0f / (bits[i] + 5.0f);
    float g = 1.0f / 400000.0f;
    if (g_in > 0.0f)
        g += 1.0f / (sim_series[sim_ch] + 1.0f / g_in);
    float rp = 1.0f / g;
    return (sim_awake ? 5000.0f : 0.0f) * rp / (rp + 2200.0f);
}

static void     sim_shift_write(uint16_t m) { sim_mask = m; }
static void     sim_set_oe(bool en)         { sim_oe = en; }
static void     sim_set_short(bool en)      { sim_short = en; }
static uint16_t sim_adc(void)               { return (uint16_t)(sim_port_mv() + 0.5f); }
static void     sim_delay(uint32_t ms)      { (void)ms; }
static void     sim_select(uint8_t ch)      { sim_ch = ch; }

bool apm_can_send_wakeup(void)
{
    sim_awake = true;
    return true;
}

bool apm_can_read_switch_state(uint8_t *state, uint32_t timeout_ms)
{
    (void)timeout_ms;
    float v = sim_port_mv();
    if (v > 4000.0f)      *state = 0;
    else if (v > 3000.0f) *state = 1;
    else if (v > 2000.0f) *state = 2;
    else if (v > 1100.0f) *state = 3;
    else if (v > 300.0f)  *state = 4;
    else                  *state = 5;
    return true;
}

void apm_delay_ms(uint32_t ms)
{
    (void)ms;
}

int main(void)
{
    static winsim_ctx_t ctx;
    const winsim_hal_t hal = { sim_shift_write, sim_set_oe, sim_set_short, sim_adc, sim_delay, sim_select };

    if (winsim_init(&ctx, &hal, NULL) != WINSIM_OK)
        return 1;

    /* 未唤醒时探测：应报告未供电。 */
    printf("唤醒前探测：%s\n", winsim_status_str(winsim_probe_open_voltage(&ctx, NULL)));
    return winsim_example_run(&ctx, 2u) == 0 ? 0 : 1;   /* K3 */
}

#endif /* WINSIM_EXAMPLE_SIM */
