/* SPDX-License-Identifier: MIT */
/*
 * win_sim 主机单元测试（第 2 版硬件：4 路共用 13 位电阻阵）。
 * Mock HAL 按物理模型仿真每一路 APM 输入：R295 = 2.2 kΩ 上拉到 VCC2_5V_SW，
 * 接口侧 400 kΩ 回读负载，可选的原车开关电阻，以及驱动事先不知道的串联电阻（PTC + 继电器）。
 * 另有一个 TCA9539 寄存器级模型，测试硬件适配的上电顺序、位映射、通道地址和 I2C 失败处理。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "services/winsim/win_sim.h"
#include "services/winsim/win_sim_tca9539.h"

static int g_checks, g_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        g_checks++;                                                              \
        if (!(c)) {                                                              \
            g_fail++;                                                            \
            printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);               \
        }                                                                        \
    } while (0)

static winsim_ctx_t g_ctx;   /* 约 33 KB，放在静态区 */

/* ------------------------------------------------------------------ 物理模型 */

typedef struct {
    float vs_mv;       /* 这一路 APM 的 VCC2_5V_SW */
    float rpu;         /* R295 */
    float rload;       /* 回读分压总阻值 */
    float rseries;     /* PTC + 继电器，驱动不知道 */
    float ext_r;       /* 原车开关对地电阻，INFINITY = 没接或松开 */
} chan_t;

typedef struct {
    float  rds;        /* 电阻阵开关管 */
    float  short_rds;  /* 直通管 */
    float  rbit[WINSIM_NUM_BITS];
    chan_t ch[WINSIM_NUM_CH];
} phys_t;

static const float k_bits[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
};

static phys_t phys_default(void)
{
    phys_t p;
    p.rds = 5.0f;
    p.short_rds = 0.05f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        p.rbit[i] = k_bits[i];
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        p.ch[k].vs_mv = 5000.0f;
        p.ch[k].rpu = 2200.0f;
        p.ch[k].rload = 400000.0f;
        p.ch[k].rseries = 12.0f;
        p.ch[k].ext_r = INFINITY;
    }
    return p;
}

static float phys_port_mv(const phys_t *p, unsigned ch, uint16_t mask, bool sh)
{
    const chan_t *c = &p->ch[ch];
    float g_in = sh ? 1.0f / p->short_rds : 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (mask & (1u << i))
            g_in += 1.0f / (p->rbit[i] + p->rds);
    float g = c->rload > 0.0f ? 1.0f / c->rload : 0.0f;
    if (isfinite(c->ext_r))
        g += c->ext_r > 0.0f ? 1.0f / c->ext_r : 1e9f;
    if (g_in > 0.0f)
        g += 1.0f / (c->rseries + 1.0f / g_in);
    if (!(g > 0.0f))
        return c->vs_mv;
    float rp = 1.0f / g;
    return c->vs_mv * rp / (rp + c->rpu);
}

static uint16_t clamp_adc_mv(float v)
{
    if (v < 0.0f)
        v = 0.0f;
    if (v > 13200.0f)          /* 1:4 分压 × 3.3 V 满量程 */
        v = 13200.0f;
    return (uint16_t)(v + 0.5f);
}

/* ------------------------------------------------------------------ 核心 Mock HAL */

#define LOG_MAX 1024

static struct {
    phys_t   ph;
    bool     oe, sh;
    uint16_t mask;
    uint8_t  ch;
    char     log[LOG_MAX][8];
    unsigned nlog;
    unsigned bad_states;    /* 直通与电阻阵同时导通的次数 */
    unsigned hot_switches;  /* 电阻阵或直通还接着时换通道的次数 */
    unsigned adc_reads;
    uint32_t delay_total;
} M;

static void mlog(const char *s)
{
    if (M.nlog < LOG_MAX) {
        strncpy(M.log[M.nlog], s, sizeof M.log[0] - 1u);
        M.nlog++;
    }
}

static void m_check(void)
{
    if (M.oe && M.sh && M.mask)
        M.bad_states++;
}

static void m_shift_write(uint16_t mask)
{
    char b[8];
    snprintf(b, sizeof b, "w%04X", (unsigned)mask);
    mlog(b);
    M.mask = mask;
    m_check();
}

static void m_set_oe(bool en)
{
    mlog(en ? "oe1" : "oe0");
    M.oe = en;
    m_check();
}

static void m_set_short(bool en)
{
    mlog(en ? "sh1" : "sh0");
    M.sh = en;
    m_check();
}

static void m_select(uint8_t ch)
{
    char b[8];
    snprintf(b, sizeof b, "c%u", (unsigned)ch);
    mlog(b);
    if (M.oe && (M.mask || M.sh))
        M.hot_switches++;
    M.ch = ch;
}

static uint16_t m_adc(void)
{
    M.adc_reads++;
    if (!M.oe)
        return 0u;   /* 使能关闭时回读开关断开，R819 拉到 0 */
    return clamp_adc_mv(phys_port_mv(&M.ph, M.ch, M.mask, M.sh));
}

static void m_delay(uint32_t ms)
{
    M.delay_total += ms;
}

static winsim_hal_t mock_reset(void)
{
    winsim_hal_t h;
    memset(&M, 0, sizeof M);
    M.ph = phys_default();
    h.shift_write = m_shift_write;
    h.set_oe = m_set_oe;
    h.set_short_0r = m_set_short;
    h.adc_read_mv = m_adc;
    h.delay_ms = m_delay;
    h.select_channel = m_select;
    return h;
}

static bool mock_is_hiz(void)
{
    return !M.sh && M.mask == 0u;
}

static unsigned udiff(unsigned a, unsigned b)
{
    return a > b ? a - b : b - a;
}

static uint32_t g_seed = 12345u;
static float frand(void)   /* [0, 1) */
{
    g_seed = g_seed * 1664525u + 1013904223u;
    return (float)(g_seed >> 8) / 16777216.0f;
}

/* ------------------------------------------------------------------ 测试 */

static void test_init_order(void)
{
    winsim_hal_t h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(M.nlog == 5u);
    CHECK(strcmp(M.log[0], "oe0") == 0);
    CHECK(strcmp(M.log[1], "sh0") == 0);
    CHECK(strcmp(M.log[2], "w0000") == 0);
    CHECK(strcmp(M.log[3], "c0") == 0);
    CHECK(strcmp(M.log[4], "oe1") == 0);

    CHECK(isinf(g_ctx.r_lut[0]));
    CHECK(fabsf(g_ctx.r_lut[1] - 205.0f) < 0.01f);
    CHECK(fabsf(g_ctx.r_lut[1u << 12] - 105.0f) < 0.01f);    /* b12 = 100 Ω */
    float g = 0.0f;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        g += 1.0f / (k_bits[i] + 5.0f);
    CHECK(fabsf(g_ctx.r_lut[WINSIM_MASK_ALL] * g - 1.0f) < 1e-4f);
    CHECK(g_ctx.r_lut[WINSIM_MASK_ALL] < 55.0f);             /* 全开约 52 Ω */
    CHECK(g_ctx.current_state.mask == 0u && !g_ctx.current_state.short_0r);
    CHECK(g_ctx.channel == 0u && g_ctx.current_state.channel == 0u);
    CHECK(!g_ctx.ovp_latched && g_ctx.last_open_mv == 0u);
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        CHECK(!g_ctx.series_cal[k]);
}

static void test_init_params(void)
{
    winsim_hal_t h = mock_reset();
    winsim_hal_t bad = h;
    bad.adc_read_mv = NULL;
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    bad = h;
    bad.select_channel = NULL;
    CHECK(winsim_init(&g_ctx, &bad, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_init(NULL, &h, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_init(&g_ctx, NULL, NULL) == WINSIM_ERR_PARAM);

    winsim_cfg_t c = winsim_default_cfg();
    c.min_open_mv = 6000u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.r_bit_ohm[12] = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.num_channels = 0u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c.num_channels = 5u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    c = winsim_default_cfg();
    c.r_series_max_ohm = 5.0f;   /* 小于默认串联电阻 */
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_ERR_PARAM);
    CHECK(M.nlog == 0u);         /* 参数错误时不碰硬件 */

    memset(&g_ctx, 0, sizeof g_ctx);
    CHECK(winsim_set_voltage(&g_ctx, 1000u, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_ERR_PARAM);
    CHECK(winsim_select_channel(&g_ctx, 0u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(winsim_measure(&g_ctx, NULL) == WINSIM_ERR_PARAM);
    CHECK(isnan(winsim_equiv_ohm(&g_ctx, 1000u)));
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 100u, 10u, 0u, NULL, NULL) == WINSIM_ERR_PARAM);
}

static void test_channel_select(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    float rs = 0.0f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 2500u, &st) == WINSIM_OK);   /* K1 正在下拉 */
    CHECK(!mock_is_hiz());

    unsigned a = M.nlog;
    uint32_t d0 = M.delay_total;
    CHECK(winsim_select_channel(&g_ctx, 2u, false, &rs) == WINSIM_OK);
    CHECK(M.nlog == a + 3u);
    CHECK(strcmp(M.log[a], "sh0") == 0);
    CHECK(strcmp(M.log[a + 1u], "w0000") == 0);   /* 先断开电阻阵 */
    CHECK(strcmp(M.log[a + 2u], "c2") == 0);      /* 再换通道 */
    CHECK(M.delay_total - d0 >= 5u);              /* 等继电器接通 */
    CHECK(M.hot_switches == 0u);
    CHECK(g_ctx.channel == 2u && g_ctx.current_state.channel == 2u);
    CHECK(g_ctx.last_open_mv == 0u);              /* 空载电压属于上一路 */
    CHECK(fabsf(rs - 10.0f) < 1e-6f);             /* 未标定：用默认值 */

    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);   /* 自动在 K3 上探测 */
    CHECK(st.channel == 2u && M.ch == 2u);

    a = M.nlog;
    CHECK(winsim_select_channel(&g_ctx, 4u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(M.nlog == a);
    winsim_cfg_t c = winsim_default_cfg();
    c.num_channels = 2u;
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 1u, false, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 2u, false, NULL) == WINSIM_ERR_PARAM);
    CHECK(M.hot_switches == 0u && M.bad_states == 0u);
}

static void test_calibration(void)
{
    static const float rs_true[WINSIM_NUM_CH] = { 5.0f, 12.4f, 17.0f, 9.8f };
    winsim_hal_t h = mock_reset();
    float rs = 0.0f;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        M.ph.ch[k].rseries = rs_true[k];
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    float worst = 0.0f;
    for (uint8_t k = 0; k < WINSIM_NUM_CH; k++) {
        CHECK(winsim_select_channel(&g_ctx, k, true, &rs) == WINSIM_OK);
        CHECK(g_ctx.series_cal[k]);
        float e = fabsf(rs - rs_true[k]);
        if (e > worst)
            worst = e;
        CHECK(e < 0.6f);
        CHECK(mock_is_hiz());                    /* 标定后回到高阻 */
        CHECK(g_ctx.last_open_mv > 4900u);
    }
    printf("  series 5 / 12.4 / 17 / 9.8 ohm: worst calibration error %.2f ohm\n", (double)worst);

    /* 回到 K2：沿用 K2 的标定值。 */
    CHECK(winsim_select_channel(&g_ctx, 1u, false, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - g_ctx.r_series_ch[1]) < 1e-6f);

    /* K3 的 PTC 断开：直通时电压几乎不变，标定失败，保留原值。 */
    M.ph.ch[2].rseries = 1e7f;
    float before = g_ctx.r_series_ch[2];
    CHECK(winsim_select_channel(&g_ctx, 2u, true, &rs) == WINSIM_ERR_CALIBRATION);
    CHECK(fabsf(g_ctx.r_series_ch[2] - before) < 1e-6f);
    CHECK(mock_is_hiz());

    /* K4 的 APM 没上电。 */
    M.ph.ch[3].vs_mv = 0.0f;
    CHECK(winsim_select_channel(&g_ctx, 3u, true, &rs) == WINSIM_ERR_NO_SUPPLY);
    CHECK(mock_is_hiz());
    CHECK(M.bad_states == 0u && M.hot_switches == 0u);
}

static unsigned run_grid(unsigned *zero_mv)
{
    unsigned worst = 0u;
    for (unsigned t = 200u; t <= 4600u; t += 200u) {
        winsim_state_t st;
        winsim_status_t rc = winsim_set_voltage(&g_ctx, (uint16_t)t, &st);
        unsigned e = udiff(st.measured_mv, t);
        CHECK(rc == WINSIM_OK);
        if (e >= 25u)
            printf("  target %4u mV -> %4u mV (mask %04X short %d, series %.1f)\n", t, st.measured_mv,
                   (unsigned)st.mask, (int)st.short_0r, (double)g_ctx.r_series_ch[g_ctx.channel]);
        CHECK(e < 25u);
        if (e > worst)
            worst = e;
    }
    winsim_state_t st;
    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    if (zero_mv)
        *zero_mv = st.measured_mv;
    return worst;
}

static void test_full_range_nominal(void)
{
    winsim_hal_t h = mock_reset();
    unsigned zero = 0u;
    M.ph.ch[0].rseries = 12.4f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    CHECK(udiff(g_ctx.last_open_mv, 4973u) <= 1u);   /* 5 V × 400k / 402.2k */
    unsigned worst = run_grid(&zero);
    CHECK(zero < 40u);
    printf("  200–4600 mV / 200 mV, nominal parts, series 12.4 ohm: worst %u mV; 0 mV -> %u mV\n", worst, zero);
    CHECK(M.bad_states == 0u);
}

/* 1% 电阻、导通电阻 3 Ω（模型按 5 Ω）、APM 上拉 ±1%、每路串联 5–17 Ω：固件用标称查找表 + 自动标定。 */
static void test_full_range_tolerance(void)
{
    unsigned worst = 0u, worst_zero = 0u;
    g_seed = 12345u;
    for (int trial = 0; trial < 20; trial++) {
        winsim_hal_t h = mock_reset();
        for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
            M.ph.rbit[i] = k_bits[i] * (0.99f + 0.02f * frand());
        M.ph.rds = 3.0f;
        uint8_t ch = (uint8_t)(trial % WINSIM_NUM_CH);
        M.ph.ch[ch].rseries = 5.0f + 12.0f * frand();
        M.ph.ch[ch].rpu = 2200.0f * (0.99f + 0.02f * frand());
        CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
        CHECK(winsim_select_channel(&g_ctx, ch, true, NULL) == WINSIM_OK);
        unsigned zero = 0u;
        unsigned w = run_grid(&zero);
        if (w > worst)
            worst = w;
        if (zero > worst_zero)
            worst_zero = zero;
    }
    CHECK(worst_zero < 45u);
    printf("  20 sets: 1%% parts, pull-up ±1%%, series 5–17 ohm: worst %u mV; 0 mV -> ≤ %u mV\n", worst, worst_zero);
}

static void test_zero(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);

    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.sh && M.mask == 0u);
    CHECK(st.measured_mv < 40u);                 /* 串联电阻 12 Ω：约 27 mV */

    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);
    CHECK(winsim_set_resistance(&g_ctx, 0.0f, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.sh && M.mask == 0u);

    CHECK(winsim_set_hiz(&g_ctx) == WINSIM_OK);
    CHECK(winsim_set_ratio(&g_ctx, 0.0f, &st) == WINSIM_OK);
    CHECK(st.short_0r && st.mask == 0u);
    CHECK(M.bad_states == 0u);
}

static void test_switch_order(void)
{
    winsim_hal_t h = mock_reset();
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, NULL) == WINSIM_OK);

    unsigned a = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 0u, NULL) == WINSIM_OK);
    CHECK(M.nlog == a + 2u);
    CHECK(strcmp(M.log[a], "w0000") == 0);      /* 先撤电阻阵 */
    CHECK(strcmp(M.log[a + 1u], "sh1") == 0);   /* 再接通直通 */

    unsigned b = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 2500u, NULL) == WINSIM_OK);
    CHECK(M.nlog == b + 2u);
    CHECK(strcmp(M.log[b], "sh0") == 0);        /* 先断直通 */
    CHECK(M.log[b + 1u][0] == 'w' && strcmp(M.log[b + 1u], "w0000") != 0);
    CHECK(M.bad_states == 0u);
}

static void test_ovp(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    uint16_t vo = 0u;
    M.ph.ch[0].vs_mv = 12000.0f;   /* K1 误接 12 V */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(vo > 5500u);
    CHECK(g_ctx.ovp_latched);
    CHECK(mock_is_hiz());

    unsigned n = M.nlog;
    CHECK(winsim_set_voltage(&g_ctx, 2000u, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_set_resistance(&g_ctx, 1000.0f, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_set_ratio(&g_ctx, 0.5f, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_auto_sweep(&g_ctx, 4000u, 0u, 500u, 0u, NULL, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_calibrate_series(&g_ctx, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(M.nlog == n);      /* 锁定后没有任何下拉动作 */
    CHECK(mock_is_hiz());
    CHECK(winsim_measure(&g_ctx, &vo) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_ERR_OVERVOLTAGE);   /* 仍是 12 V，不解锁 */
    CHECK(g_ctx.ovp_latched);

    /* 换到正常的 K2：换通道本身不解锁，也不能标定；重新检测后解锁。 */
    CHECK(winsim_select_channel(&g_ctx, 1u, true, NULL) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(g_ctx.channel == 1u && g_ctx.ovp_latched && mock_is_hiz());
    CHECK(winsim_clear_ovp(&g_ctx) == WINSIM_OK);
    CHECK(!g_ctx.ovp_latched);
    CHECK(winsim_calibrate_series(&g_ctx, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 2000u, &st) == WINSIM_OK);

    /* 已经下拉时电源跳到 12 V：下一次回读触发保护。 */
    M.ph.ch[1].vs_mv = 12000.0f;
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(g_ctx.ovp_latched);
    CHECK(mock_is_hiz());
}

static void test_no_supply(void)
{
    winsim_hal_t h = mock_reset();
    uint16_t vo = 1u;
    M.ph.ch[0].vs_mv = 0.0f;   /* APM 未唤醒 */
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_ERR_NO_SUPPLY);
    CHECK(vo == 0u);
    CHECK(winsim_set_voltage(&g_ctx, 1000u, NULL) == WINSIM_ERR_NO_SUPPLY);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 50u, 0u, NULL, NULL) == WINSIM_ERR_NO_SUPPLY);
    CHECK(mock_is_hiz());
    CHECK(!g_ctx.ovp_latched);
}

static void test_verify(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);

    /* 把这一档用到的最低阻值那一位弄断，再设同一个电压：回读应超差。 */
    unsigned broken = WINSIM_NUM_BITS;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if ((st.mask & (1u << i)) && (broken == WINSIM_NUM_BITS || k_bits[i] < k_bits[broken]))
            broken = i;
    CHECK(broken < WINSIM_NUM_BITS);
    M.ph.rbit[broken] = 1e9f;
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_ERR_VERIFY_FAIL);

    winsim_cfg_t c = winsim_default_cfg();
    c.verify_enable = false;     /* 脉冲供电车型 */
    CHECK(winsim_init(&g_ctx, &h, &c) == WINSIM_OK);
    CHECK(winsim_set_voltage(&g_ctx, 3000u, &st) == WINSIM_OK);
}

typedef struct {
    unsigned n;
    uint16_t targets[128];
    unsigned worst_target;     /* |回读 − 目标|，目标 ≥ 200 mV */
    unsigned zero_mv;          /* 目标 0 mV 时的回读 */
    unsigned worst_expected;   /* |回读 − 理论| */
    unsigned mismatched;       /* 回调状态与硬件不一致 */
    unsigned inject_at;        /* 第 N 次回调后把电源改成 12 V，0 = 不注入 */
} sweep_rec_t;

static void sweep_cb(const winsim_state_t *st, void *user)
{
    sweep_rec_t *r = (sweep_rec_t *)user;
    if (r->n < 128u)
        r->targets[r->n] = st->target_mv;
    r->n++;
    unsigned et = udiff(st->measured_mv, st->target_mv);
    unsigned ee = udiff(st->measured_mv, st->expected_mv);
    if (st->target_mv >= 200u && et > r->worst_target)
        r->worst_target = et;
    if (st->target_mv == 0u)
        r->zero_mv = st->measured_mv;
    if (ee > r->worst_expected)
        r->worst_expected = ee;
    if (st->mask != M.mask || st->short_0r != M.sh || st->channel != M.ch)
        r->mismatched++;
    if (r->inject_at && r->n == r->inject_at)
        M.ph.ch[M.ch].vs_mv = 12000.0f;
}

static void test_sweep(void)
{
    winsim_hal_t h = mock_reset();
    sweep_rec_t r;
    M.ph.ch[3].rseries = 15.1f;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 3u, true, NULL) == WINSIM_OK);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 200u, 50u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 25u);
    for (unsigned i = 0; i < 25u; i++)
        CHECK(r.targets[i] == 4800u - 200u * i);
    CHECK(r.worst_target < 25u);
    CHECK(r.zero_mv < 45u);
    CHECK(r.worst_expected <= 2u);
    CHECK(r.mismatched == 0u);
    CHECK(mock_is_hiz());
    CHECK(M.delay_total >= 25u * 50u);
    printf("  K4 sweep 4800→0 mV / 200 mV: %u steps, worst %u mV (≥ 200 mV), 0 mV -> %u mV\n",
           r.n, r.worst_target, r.zero_mv);

    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 0u, 1000u, 300u, 0u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 4u);
    CHECK(r.targets[0] == 0u && r.targets[3] == 900u);
    CHECK(mock_is_hiz());

    /* 4800→0 / 50 mV，含无档位区间：按理论值校验，不应误报。 */
    memset(&r, 0, sizeof r);
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 50u, 0u, sweep_cb, &r) == WINSIM_OK);
    CHECK(r.n == 97u);

    CHECK(winsim_auto_sweep(&g_ctx, 0u, 1000u, 0u, 0u, sweep_cb, &r) == WINSIM_ERR_PARAM);

    /* 扫描中途电源变成 12 V：返回过压并恢复高阻。 */
    memset(&r, 0, sizeof r);
    r.inject_at = 3u;
    CHECK(winsim_auto_sweep(&g_ctx, 4800u, 0u, 200u, 0u, sweep_cb, &r) == WINSIM_ERR_OVERVOLTAGE);
    CHECK(r.n == 3u);
    CHECK(mock_is_hiz());
    CHECK(g_ctx.ovp_latched);
}

static void test_ratio(void)
{
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_set_ratio(&g_ctx, 0.5f, &st) == WINSIM_OK);   /* 不需要先探测 */
    CHECK(udiff(st.measured_mv, 2500u) < 25u);
    CHECK(winsim_set_ratio(&g_ctx, 1.0f, &st) == WINSIM_OK);
    CHECK(st.mask == 0u && !st.short_0r && isinf(st.actual_r_ohm));
    CHECK(winsim_set_ratio(&g_ctx, -0.1f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, 1.5f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_ratio(&g_ctx, NAN, &st) == WINSIM_ERR_PARAM);
}

static void test_resistance(void)
{
    static const float targets[] = { 150.0f, 1000.0f, 3300.0f, 10000.0f, 22000.0f, 47000.0f };
    const float g_lsb = 1.0f / (360000.0f + 5.0f);
    winsim_hal_t h = mock_reset();
    winsim_state_t st;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, NULL) == WINSIM_OK);
    for (unsigned i = 0; i < sizeof targets / sizeof targets[0]; i++) {
        CHECK(winsim_set_resistance(&g_ctx, targets[i], &st) == WINSIM_OK);
        float rel = fabsf(st.actual_r_ohm - targets[i]) / targets[i];
        /* 误差不超过一个最低位电导相对目标的比例 */
        CHECK(rel <= g_lsb * targets[i] + 0.001f);
    }
    CHECK(winsim_set_resistance(&g_ctx, INFINITY, &st) == WINSIM_OK);
    CHECK(mock_is_hiz() && isinf(st.actual_r_ohm));
    CHECK(winsim_set_resistance(&g_ctx, -1.0f, &st) == WINSIM_ERR_PARAM);
    CHECK(winsim_set_resistance(&g_ctx, NAN, &st) == WINSIM_ERR_PARAM);
}

/* 被动测量：原车开关并在 K2 上，电阻阵保持高阻，由电压反推开关电阻。 */
static void test_passive_measure(void)
{
    static const float sw[] = { 5100.0f, 2200.0f, 1000.0f, 330.0f, 47.0f };
    winsim_hal_t h = mock_reset();
    uint16_t mv = 0u;
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(winsim_select_channel(&g_ctx, 1u, false, NULL) == WINSIM_OK);
    CHECK(isnan(winsim_equiv_ohm(&g_ctx, 3000u)));            /* 还没有空载电压 */
    CHECK(winsim_probe_open_voltage(&g_ctx, NULL) == WINSIM_OK);   /* 开关松开 = 开路 */

    unsigned writes_before = 0u;
    for (unsigned i = 0; i < M.nlog; i++)
        if (M.log[i][0] == 'w' && strcmp(M.log[i], "w0000") != 0)
            writes_before++;
    float worst = 0.0f;
    for (unsigned i = 0; i < sizeof sw / sizeof sw[0]; i++) {
        M.ph.ch[1].ext_r = sw[i];
        CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
        float r = winsim_equiv_ohm(&g_ctx, mv);
        float rel = fabsf(r - sw[i]) / sw[i];
        if (rel > worst)
            worst = rel;
        CHECK(rel < 0.03f);
    }
    printf("  original switch 47 Ω–5.1 kΩ: worst %.2f%%\n", (double)(worst * 100.0f));
    unsigned writes_after = 0u;
    for (unsigned i = 0; i < M.nlog; i++)
        if (M.log[i][0] == 'w' && strcmp(M.log[i], "w0000") != 0)
            writes_after++;
    CHECK(writes_after == writes_before);   /* 被动测量从不下拉 */

    M.ph.ch[1].ext_r = INFINITY;
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(isinf(winsim_equiv_ohm(&g_ctx, mv)));
    CHECK(winsim_equiv_ohm(&g_ctx, 0u) == 0.0f);

    /* 正在下拉时调用：先断开再测。 */
    CHECK(winsim_set_voltage(&g_ctx, 2000u, NULL) == WINSIM_OK);
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(mock_is_hiz() && udiff(mv, g_ctx.last_open_mv) <= 2u);
}

static unsigned feed_for(winsim_learn_t *l, uint16_t mv, int jitter, uint32_t ms, uint16_t *cap)
{
    unsigned hits = 0u;
    for (uint32_t t = 0; t < ms; t += 50u) {
        int v = (int)mv + (int)((t / 50u) % 3u) * jitter - jitter;
        if (winsim_learn_feed(l, (uint16_t)(v < 0 ? 0 : v), 50u, cap))
            hits++;
    }
    return hits;
}

static void test_learn(void)
{
    winsim_learn_t l;
    uint16_t cap = 0u;
    winsim_learn_begin(&l, NULL, 4970u);

    CHECK(feed_for(&l, 4970u, 3, 300u, &cap) == 0u);         /* 空闲 */
    CHECK(feed_for(&l, 3480u, 8, 900u, &cap) == 1u);         /* 按住 0.9 s：记一次 */
    CHECK(udiff(cap, 3480u) <= 8u);
    CHECK(feed_for(&l, 3480u, 8, 900u, &cap) == 0u);         /* 一直按着：不重复记 */
    CHECK(feed_for(&l, 4970u, 3, 200u, &cap) == 0u);         /* 松开 */
    CHECK(feed_for(&l, 2510u, 5, 300u, &cap) == 0u);         /* 只按 0.3 s：不记 */
    CHECK(feed_for(&l, 2510u, 5, 400u, &cap) == 1u);         /* 继续按满 0.5 s 以上 */
    CHECK(udiff(cap, 2510u) <= 5u);
    CHECK(feed_for(&l, 4970u, 3, 200u, &cap) == 0u);
    CHECK(feed_for(&l, 1550u, 120, 2000u, &cap) == 0u);      /* 抖动 120 mV：一直不稳定 */
    CHECK(feed_for(&l, 4900u, 0, 200u, &cap) == 0u);         /* 只低 70 mV：不算按下 */
    CHECK(feed_for(&l, 680u, 0, 600u, &cap) == 1u);
    CHECK(cap == 680u);

    winsim_cfg_t c = winsim_default_cfg();
    c.learn_stable_ms = 200u;
    winsim_learn_begin(&l, &c, 4970u);
    CHECK(feed_for(&l, 3000u, 0, 300u, &cap) == 1u);
}

/* ------------------------------------------------------------------ TCA9539 寄存器模型 */

static struct {
    bool     reset_low;
    uint8_t  out[2], cfg[2];
    int      nack_in;      /* 再成功写 N 次后 NACK 一次；-1 = 不注入 */
    unsigned writes;
    unsigned unsafe;       /* 出现过未被允许的“输出且为高”引脚 */
    uint16_t allowed;
    uint16_t raw_fixed;    /* 非 0 时 ADC 直接返回该码值 */
    phys_t   ph;
} D;

#define ADDR_PINS 0xC000u   /* P16/P17 是通道地址线，为高不算危险 */

static uint16_t dev_driven_high(void)
{
    if (D.reset_low)
        return 0u;
    uint16_t out = (uint16_t)(D.out[0] | (D.out[1] << 8));
    uint16_t is_out = (uint16_t)~(D.cfg[0] | (D.cfg[1] << 8));
    return (uint16_t)(out & is_out);
}

static void dev_check(void)
{
    if (dev_driven_high() & (uint16_t)~(D.allowed | ADDR_PINS))
        D.unsafe++;
}

static void dev_reset_pin(bool high)
{
    D.reset_low = !high;
    if (!high) {
        D.out[0] = D.out[1] = 0xFFu;   /* 数据手册：上电/复位默认值 */
        D.cfg[0] = D.cfg[1] = 0xFFu;
    }
    dev_check();
}

static bool dev_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *data, uint8_t len)
{
    if (addr != WINSIM_TCA9539_ADDR || D.reset_low)
        return false;
    if (D.nack_in == 0) {
        D.nack_in = -1;
        return false;
    }
    if (D.nack_in > 0)
        D.nack_in--;
    for (uint8_t k = 0; k < len; k++) {
        uint8_t r = (uint8_t)((reg & 0xFEu) | ((reg + k) & 1u));   /* 寄存器对内自动切换 */
        if (r == 0x02u || r == 0x03u)
            D.out[r - 0x02u] = data[k];
        else if (r == 0x06u || r == 0x07u)
            D.cfg[r - 0x06u] = data[k];
        dev_check();   /* 每个字节后检查一次 */
    }
    D.writes++;
    return true;
}

static uint16_t dev_adc_raw(void)
{
    if (D.raw_fixed)
        return D.raw_fixed;
    if (D.reset_low)
        return 0u;     /* PG2 低：CH_EN_N 高，回读开关全断 */
    /* 地址线在复位后是输入，由 R825/R826 下拉为 0 */
    uint16_t out = (uint16_t)(D.out[0] | (D.out[1] << 8));
    uint16_t is_out = (uint16_t)~(D.cfg[0] | (D.cfg[1] << 8));
    unsigned ch = (unsigned)(((out & is_out) >> 14) & 3u);
    uint16_t hi = dev_driven_high();
    uint16_t mask = (uint16_t)((hi & 0xFFu) | (((hi >> 8) & 0x0Fu) << 8) | (((hi >> 13) & 1u) << 12));
    bool sh = (hi >> 8) & WINSIM_TCA9539_P1_SHORT;
    float v = phys_port_mv(&D.ph, ch, mask, sh);
    float raw = v / 4.0f / 3300.0f * 65535.0f;
    if (raw > 65535.0f)
        raw = 65535.0f;
    return (uint16_t)(raw + 0.5f);
}

static void dev_delay(uint32_t ms)
{
    (void)ms;
}

static winsim_hal_t dev_bind(int nack_in)
{
    winsim_hal_t h;
    winsim_tca9539_port_t p = winsim_tca9539_default_port();
    memset(&D, 0, sizeof D);
    D.nack_in = nack_in;
    D.ph = phys_default();
    D.out[0] = D.out[1] = D.cfg[0] = D.cfg[1] = 0xFFu;
    p.i2c_write = dev_i2c_write;
    p.reset_pin = dev_reset_pin;
    p.adc_read_raw = dev_adc_raw;
    p.delay_ms = dev_delay;
    CHECK(winsim_tca9539_bind(&p, &h));
    return h;
}

static void test_tca9539(void)
{
    winsim_state_t st;
    uint16_t vo = 0u;

    winsim_hal_t h = dev_bind(-1);
    CHECK(D.reset_low);                       /* 绑定即保持复位 */
    h.select_channel(3u);                     /* 复位期间只改影子寄存器 */
    CHECK(D.writes == 0u);
    h.select_channel(0u);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(!D.reset_low);
    CHECK(D.out[0] == 0u && D.out[1] == 0u);
    CHECK(D.cfg[0] == 0u && D.cfg[1] == 0u);
    CHECK(D.unsafe == 0u);                    /* 释放复位过程中没有任何开关引脚输出高 */

    D.allowed = 0xFFFFu;
    h.shift_write(0x1ABCu);
    CHECK(D.out[0] == 0xBCu && D.out[1] == 0x2Au);           /* b8–b11 = 0xA，b12 → P15 */
    h.set_short_0r(true);
    CHECK(D.out[1] == 0x3Au);
    h.select_channel(2u);
    CHECK(D.out[1] == 0xBAu);                                 /* P17:P16 = 10 */
    h.set_short_0r(false);
    h.shift_write(0u);
    CHECK(D.out[0] == 0u && D.out[1] == 0x80u);               /* 通道地址保持 */
    h.select_channel(0u);
    CHECK(D.out[1] == 0u);

    /* 端到端：核心 → 适配 → 寄存器 → 物理模型 → ADC 码值 → mV，每路串联电阻不同 */
    D.allowed = 0x3FFFu;                      /* P00–P15 */
    D.ph.ch[0].rseries = 6.0f;
    D.ph.ch[2].rseries = 16.0f;
    float rs = 0.0f;
    CHECK(winsim_select_channel(&g_ctx, 2u, true, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - 16.0f) < 0.8f);
    CHECK((D.out[1] & WINSIM_TCA9539_P1_CH_MASK) == 0x80u);
    CHECK(winsim_probe_open_voltage(&g_ctx, &vo) == WINSIM_OK);
    CHECK(udiff(vo, 4973u) <= 2u);
    for (unsigned t = 300u; t <= 4500u; t += 700u) {
        CHECK(winsim_set_voltage(&g_ctx, (uint16_t)t, &st) == WINSIM_OK);
        CHECK(udiff(st.measured_mv, t) < 25u);
    }
    CHECK(winsim_set_voltage(&g_ctx, 0u, &st) == WINSIM_OK);
    CHECK((D.out[1] & WINSIM_TCA9539_P1_SHORT) && D.out[0] == 0u && (D.out[1] & 0x2Fu) == 0u);
    CHECK(winsim_select_channel(&g_ctx, 0u, true, &rs) == WINSIM_OK);
    CHECK(fabsf(rs - 6.0f) < 0.8f);
    CHECK((D.out[1] & (uint8_t)~WINSIM_TCA9539_P1_CH_MASK) == 0u && D.out[0] == 0u);   /* 换完是高阻 */
    D.ph.ch[0].ext_r = 3300.0f;               /* K1 上并着原车开关 */
    uint16_t mv = 0u;
    CHECK(winsim_measure(&g_ctx, &mv) == WINSIM_OK);
    CHECK(fabsf(winsim_equiv_ohm(&g_ctx, mv) - 3300.0f) < 100.0f);
    CHECK(D.unsafe == 0u);

    h.set_oe(false);
    CHECK(D.reset_low && dev_driven_high() == 0u);
    CHECK(winsim_tca9539_error_count() == 0u);

    /* 释放复位后第一次写输出寄存器失败：不能切输出方向，回到复位。 */
    h = dev_bind(0);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && dev_driven_high() == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* 写方向寄存器失败：同样回到复位。 */
    h = dev_bind(1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    CHECK(D.reset_low && dev_driven_high() == 0u && D.unsafe == 0u);
    CHECK(winsim_tca9539_error_count() == 1u);

    /* 运行中写失败：立刻复位关断（换通道也一样）。 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    D.allowed = 0xFFFFu;
    D.nack_in = 0;
    h.shift_write(0x00FFu);
    CHECK(D.reset_low && dev_driven_high() == 0u);
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    D.nack_in = 0;
    h.select_channel(1u);
    CHECK(D.reset_low);

    /* 应急关断 */
    h = dev_bind(-1);
    CHECK(winsim_init(&g_ctx, &h, NULL) == WINSIM_OK);
    winsim_tca9539_emergency_off();
    CHECK(D.reset_low);

    /* ADC 折算：满量程 = 3.3 V × 4 */
    D.raw_fixed = 65535u;
    CHECK(h.adc_read_mv() == 13200u);
    D.raw_fixed = 16384u;
    CHECK(udiff(h.adc_read_mv(), 3300u) <= 1u);

    winsim_tca9539_port_t bad = winsim_tca9539_default_port();
    CHECK(!winsim_tca9539_bind(&bad, &h));    /* 回调未填 */
}

int main(void)
{
    static const struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        { "init order", test_init_order },
        { "init params", test_init_params },
        { "channel select", test_channel_select },
        { "series calibration", test_calibration },
        { "full range, nominal", test_full_range_nominal },
        { "full range, tolerances", test_full_range_tolerance },
        { "0 V / 0 ohm", test_zero },
        { "short switching order", test_switch_order },
        { "over-voltage", test_ovp },
        { "APM asleep", test_no_supply },
        { "readback verify", test_verify },
        { "auto sweep", test_sweep },
        { "ratio", test_ratio },
        { "resistance", test_resistance },
        { "passive measure", test_passive_measure },
        { "key learning", test_learn },
        { "TCA9539 adapter", test_tca9539 },
    };
    for (unsigned i = 0; i < sizeof tests / sizeof tests[0]; i++) {
        int before = g_fail;
        printf("[%s]\n", tests[i].name);
        tests[i].fn();
        printf("  %s\n", g_fail == before ? "ok" : "FAILED");
    }
    printf("\n%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
