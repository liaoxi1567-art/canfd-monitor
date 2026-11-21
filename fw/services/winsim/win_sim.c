/* SPDX-License-Identifier: MIT */
#include "services/winsim/win_sim.h"

#include <math.h>
#include <stddef.h>

/* Hardware-C 第 12 页第 2 版：R801–R812，b12 = R831。 */
static const float k_bits_default[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
};

typedef enum {
    VERIFY_NONE,
    VERIFY_TARGET,    /* 回读对比目标电压（set_voltage） */
    VERIFY_EXPECTED   /* 回读对比理论电压（扫描，用来发现硬件故障） */
} verify_mode_t;

winsim_cfg_t winsim_default_cfg(void)
{
    winsim_cfg_t c;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        c.r_bit_ohm[i] = k_bits_default[i];
    c.r_pullup_ohm      = 2200.0f;
    c.r_adc_load_ohm    = 400000.0f;
    c.mos_rds_on_ohm    = 5.0f;
    c.short_rds_on_ohm  = 0.05f;
    c.r_series_ohm      = 10.0f;
    c.r_series_max_ohm  = 100.0f;
    c.ovp_threshold_mv  = 5500u;
    c.min_open_mv       = 4000u;
    c.settle_delay_ms   = 10u;
    c.channel_switch_ms = 5u;
    c.verify_tol_mv     = 80u;
    c.verify_enable     = true;
    c.num_channels      = WINSIM_NUM_CH;
    c.learn_drop_mv     = 150u;
    c.learn_stable_mv   = 25u;
    c.learn_stable_ms   = 500u;
    return c;
}

const char *winsim_status_str(winsim_status_t status)
{
    switch (status) {
    case WINSIM_OK:              return "OK";
    case WINSIM_ERR_PARAM:       return "ERR_PARAM";
    case WINSIM_ERR_OVERVOLTAGE: return "ERR_OVERVOLTAGE";
    case WINSIM_ERR_NO_SUPPLY:   return "ERR_NO_SUPPLY";
    case WINSIM_ERR_VERIFY_FAIL: return "ERR_VERIFY_FAIL";
    case WINSIM_ERR_CALIBRATION: return "ERR_CALIBRATION";
    }
    return "?";
}

/* ------------------------------------------------------------------ 校验 */

static bool hal_ok(const winsim_hal_t *h)
{
    return h && h->shift_write && h->set_oe && h->set_short_0r && h->adc_read_mv && h->delay_ms &&
           h->select_channel;
}

static bool cfg_ok(const winsim_cfg_t *c)
{
    if (!(c->r_pullup_ohm > 0.0f) || !(c->r_adc_load_ohm >= 0.0f) || !(c->mos_rds_on_ohm >= 0.0f) ||
        !(c->short_rds_on_ohm >= 0.0f) || !(c->r_series_ohm >= 0.0f) ||
        !(c->r_series_max_ohm >= c->r_series_ohm) || !isfinite(c->r_series_max_ohm) ||
        c->ovp_threshold_mv == 0u || c->min_open_mv >= c->ovp_threshold_mv ||
        c->num_channels == 0u || c->num_channels > WINSIM_NUM_CH ||
        c->learn_drop_mv == 0u || c->learn_stable_ms == 0u)
        return false;
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        if (!(c->r_bit_ohm[i] > 0.0f) || !isfinite(c->r_bit_ohm[i]))
            return false;
    return true;
}

static bool ready(const winsim_ctx_t *ctx)
{
    return ctx && ctx->initialized;
}

/* ------------------------------------------------------------------ 电路模型 */

/* 当前通道的串联电阻：已标定用标定值，否则用配置的默认值。 */
static float series(const winsim_ctx_t *ctx)
{
    return ctx->series_cal[ctx->channel] ? ctx->r_series_ch[ctx->channel] : ctx->cfg.r_series_ohm;
}

/* 接口对地下拉 r_pull（不含回读负载，INFINITY = 高阻）时，接口电压 / APM 内部电源。 */
static float port_ratio(const winsim_cfg_t *c, float r_pull)
{
    float g = 0.0f;
    if (!(r_pull > 0.0f))
        return 0.0f;
    if (isfinite(r_pull))
        g += 1.0f / r_pull;
    if (c->r_adc_load_ohm > 0.0f)
        g += 1.0f / c->r_adc_load_ohm;
    if (!(g > 0.0f))
        return 1.0f;
    float rp = 1.0f / g;
    return rp / (rp + c->r_pullup_ohm);
}

static float state_r(const winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    if (sh)
        return series(ctx) + ctx->cfg.short_rds_on_ohm;
    if (mask == 0u)
        return INFINITY;
    return series(ctx) + ctx->r_lut[mask];
}

/* 由空载回读电压反推 APM 内部电源（扣除回读负载的分压）。 */
static float supply_mv(const winsim_ctx_t *ctx)
{
    return (float)ctx->last_open_mv / port_ratio(&ctx->cfg, INFINITY);
}

/* 接口电压为 mv 时，接口对地的外部电阻（不含回读负载）。 */
static float pull_from_mv(const winsim_ctx_t *ctx, float mv)
{
    float vs = supply_mv(ctx);
    if (!(mv > 0.0f))
        return 0.0f;
    if (!(mv < vs))
        return INFINITY;
    float rx = ctx->cfg.r_pullup_ohm * mv / (vs - mv);   /* 外部电阻 ∥ 回读负载 */
    if (!(ctx->cfg.r_adc_load_ohm > 0.0f))
        return rx;
    float g = 1.0f / rx - 1.0f / ctx->cfg.r_adc_load_ohm;
    return g > 0.0f ? 1.0f / g : INFINITY;
}

static uint16_t to_mv(float v)
{
    if (!(v > 0.0f))
        return 0u;
    if (v >= 65535.0f)
        return 65535u;
    return (uint16_t)(v + 0.5f);
}

static uint16_t absdiff(uint16_t a, uint16_t b)
{
    return (uint16_t)(a > b ? a - b : b - a);
}

/* 每次加一位：G(m) = G(m 去掉最低位) + g(最低位)，8192 项共 8191 次除法。 */
static void build_lut(winsim_ctx_t *ctx)
{
    float g_bit[WINSIM_NUM_BITS];
    for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
        g_bit[i] = 1.0f / (ctx->cfg.r_bit_ohm[i] + ctx->cfg.mos_rds_on_ohm);

    ctx->r_lut[0] = INFINITY;
    for (uint32_t m = 1u; m < WINSIM_LUT_SIZE; m++) {
        uint32_t rest = m & (m - 1u);
        uint32_t low = m ^ rest;
        unsigned b = 0u;
        while ((low >> b) != 1u)
            b++;
        float g = g_bit[b] + (rest ? 1.0f / ctx->r_lut[rest] : 0.0f);
        ctx->r_lut[m] = 1.0f / g;
    }
}

/* 在 直通 / 高阻 / 8191 种组合 中选分压比最接近 k 的状态。 */
static void select_by_ratio(const winsim_ctx_t *ctx, float k, uint16_t *mask, bool *sh)
{
    const float rs = series(ctx);
    uint16_t best_mask = 0u;
    bool best_sh = true;
    float best = fabsf(port_ratio(&ctx->cfg, state_r(ctx, 0u, true)) - k);

    float e = fabsf(port_ratio(&ctx->cfg, INFINITY) - k);
    if (e < best) {
        best = e;
        best_sh = false;
    }
    for (uint32_t m = 1u; m < WINSIM_LUT_SIZE; m++) {
        e = fabsf(port_ratio(&ctx->cfg, rs + ctx->r_lut[m]) - k);
        if (e < best) {
            best = e;
            best_mask = (uint16_t)m;
            best_sh = false;
        }
    }
    *mask = best_mask;
    *sh = best_sh;
}

/* 选阻值最接近 r_target 的状态（有限正值）。 */
static void select_by_resistance(const winsim_ctx_t *ctx, float r_target, uint16_t *mask, bool *sh)
{
    const float rs = series(ctx);
    uint16_t best_mask = 0u;
    bool best_sh = true;
    float best = fabsf(state_r(ctx, 0u, true) - r_target);

    for (uint32_t m = 1u; m < WINSIM_LUT_SIZE; m++) {
        float e = fabsf(rs + ctx->r_lut[m] - r_target);
        if (e < best) {
            best = e;
            best_mask = (uint16_t)m;
            best_sh = false;
        }
    }
    *mask = best_mask;
    *sh = best_sh;
}

/* ------------------------------------------------------------------ 输出与回读 */

static void record_state(winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    ctx->current_state.channel = ctx->channel;
    ctx->current_state.mask = mask;
    ctx->current_state.short_0r = sh;
    ctx->current_state.actual_r_ohm = state_r(ctx, mask, sh);
}

static bool is_hiz(const winsim_ctx_t *ctx)
{
    return ctx->current_state.mask == 0u && !ctx->current_state.short_0r;
}

static void go_hiz(winsim_ctx_t *ctx)
{
    ctx->hal.set_short_0r(false);
    ctx->hal.shift_write(0u);
    record_state(ctx, 0u, false);
    ctx->current_state.target_mv = 0u;
    ctx->current_state.expected_mv = ctx->last_open_mv;
    ctx->current_state.measured_mv = 0u;
}

/* 切换顺序：进入直通前先撤掉电阻阵，退出直通时先断开直通再写电阻阵。
 * 两种情况的中间态都是高阻，不会出现比两端都低的阻值。 */
static void apply_raw(winsim_ctx_t *ctx, uint16_t mask, bool sh)
{
    if (sh) {
        ctx->hal.shift_write(0u);
        ctx->hal.set_short_0r(true);
        mask = 0u;
    } else {
        ctx->hal.set_short_0r(false);
        ctx->hal.shift_write(mask);
    }
    record_state(ctx, mask, sh);
}

static uint16_t read_median(winsim_ctx_t *ctx)
{
    uint16_t s[WINSIM_ADC_SAMPLES];
    for (unsigned i = 0; i < WINSIM_ADC_SAMPLES; i++)
        s[i] = ctx->hal.adc_read_mv();
    for (unsigned i = 1; i < WINSIM_ADC_SAMPLES; i++) {
        uint16_t v = s[i];
        unsigned j = i;
        while (j > 0u && s[j - 1u] > v) {
            s[j] = s[j - 1u];
            j--;
        }
        s[j] = v;
    }
    return s[WINSIM_ADC_SAMPLES / 2u];
}

static bool ovp_trip(winsim_ctx_t *ctx, uint16_t mv)
{
    if (mv <= ctx->cfg.ovp_threshold_mv)
        return false;
    go_hiz(ctx);
    ctx->ovp_latched = true;
    ctx->current_state.measured_mv = mv;
    return true;
}

static winsim_status_t apply_and_measure(winsim_ctx_t *ctx, uint16_t mask, bool sh,
                                         uint16_t target_mv, verify_mode_t vm,
                                         winsim_state_t *out)
{
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    /* 下拉前先看一次接口电压。 */
    if (ovp_trip(ctx, read_median(ctx))) {
        if (out)
            *out = ctx->current_state;
        return WINSIM_ERR_OVERVOLTAGE;
    }

    apply_raw(ctx, mask, sh);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_median(ctx);
    if (ovp_trip(ctx, mv)) {
        if (out)
            *out = ctx->current_state;
        return WINSIM_ERR_OVERVOLTAGE;
    }

    winsim_state_t st = ctx->current_state;
    st.target_mv = target_mv;
    st.expected_mv = ctx->last_open_mv
        ? to_mv(supply_mv(ctx) * port_ratio(&ctx->cfg, st.actual_r_ohm))
        : 0u;
    st.measured_mv = mv;
    ctx->current_state = st;
    if (out)
        *out = st;

    if (ctx->cfg.verify_enable) {
        if (vm == VERIFY_TARGET && absdiff(mv, target_mv) > ctx->cfg.verify_tol_mv)
            return WINSIM_ERR_VERIFY_FAIL;
        if (vm == VERIFY_EXPECTED && ctx->last_open_mv &&
            absdiff(mv, st.expected_mv) > ctx->cfg.verify_tol_mv)
            return WINSIM_ERR_VERIFY_FAIL;
    }
    return WINSIM_OK;
}

/* ------------------------------------------------------------------ 公共接口 */

winsim_status_t winsim_init(winsim_ctx_t *ctx, const winsim_hal_t *hal, const winsim_cfg_t *cfg)
{
    winsim_cfg_t c = cfg ? *cfg : winsim_default_cfg();
    if (!ctx || !hal_ok(hal) || !cfg_ok(&c))
        return WINSIM_ERR_PARAM;

    ctx->initialized = false;
    ctx->hal = *hal;
    ctx->cfg = c;

    /* 先禁止输出，锁存全 0、通道 K1，再允许输出。 */
    ctx->hal.set_oe(false);
    ctx->hal.set_short_0r(false);
    ctx->hal.shift_write(0u);
    ctx->hal.select_channel(0u);
    ctx->hal.set_oe(true);

    build_lut(ctx);
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        ctx->r_series_ch[k] = c.r_series_ohm;
        ctx->series_cal[k] = false;
    }
    ctx->channel = 0u;
    ctx->last_open_mv = 0u;
    ctx->ovp_latched = false;
    record_state(ctx, 0u, false);
    ctx->current_state.target_mv = 0u;
    ctx->current_state.expected_mv = 0u;
    ctx->current_state.measured_mv = 0u;
    ctx->initialized = true;
    return WINSIM_OK;
}

winsim_status_t winsim_set_hiz(winsim_ctx_t *ctx)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    return WINSIM_OK;
}

winsim_status_t winsim_probe_open_voltage(winsim_ctx_t *ctx, uint16_t *v_open_mv)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_median(ctx);
    ctx->current_state.measured_mv = mv;
    if (v_open_mv)
        *v_open_mv = mv;

    if (mv > ctx->cfg.ovp_threshold_mv) {
        ctx->ovp_latched = true;
        ctx->last_open_mv = 0u;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    if (mv < ctx->cfg.min_open_mv) {
        ctx->last_open_mv = 0u;
        return WINSIM_ERR_NO_SUPPLY;
    }
    ctx->last_open_mv = mv;
    ctx->current_state.expected_mv = mv;
    return WINSIM_OK;
}

winsim_status_t winsim_calibrate_series(winsim_ctx_t *ctx, float *r_series_out)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    winsim_status_t rc = winsim_probe_open_voltage(ctx, NULL);   /* 同时完成下拉前的过压检查 */
    if (rc != WINSIM_OK)
        return rc;

    apply_raw(ctx, 0u, true);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t v_s = read_median(ctx);
    if (ovp_trip(ctx, v_s))
        return WINSIM_ERR_OVERVOLTAGE;
    go_hiz(ctx);

    /* 直通时接口对地 = 串联电阻 + 直通管；扣掉直通管就是这一路的 PTC + 继电器。 */
    float r = pull_from_mv(ctx, (float)v_s) - ctx->cfg.short_rds_on_ohm;
    if (r < 0.0f && r > -2.0f)
        r = 0.0f;   /* ADC 取整造成的小负值 */
    if (!(r >= 0.0f) || !(r <= ctx->cfg.r_series_max_ohm))
        return WINSIM_ERR_CALIBRATION;

    ctx->r_series_ch[ctx->channel] = r;
    ctx->series_cal[ctx->channel] = true;
    if (r_series_out)
        *r_series_out = r;
    return WINSIM_OK;
}

winsim_status_t winsim_select_channel(winsim_ctx_t *ctx, uint8_t ch, bool calibrate, float *r_series_out)
{
    if (!ready(ctx) || ch >= ctx->cfg.num_channels)
        return WINSIM_ERR_PARAM;

    go_hiz(ctx);                       /* 先断开电阻阵，切换过程中两路都只看到高阻 */
    ctx->hal.select_channel(ch);
    ctx->hal.delay_ms(ctx->cfg.channel_switch_ms);
    ctx->channel = ch;
    ctx->last_open_mv = 0u;            /* 空载电压属于上一路 */
    record_state(ctx, 0u, false);
    ctx->current_state.expected_mv = 0u;

    if (!calibrate) {
        if (r_series_out)
            *r_series_out = series(ctx);
        return WINSIM_OK;
    }
    return winsim_calibrate_series(ctx, r_series_out);
}

winsim_status_t winsim_measure(winsim_ctx_t *ctx, uint16_t *mv_out)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (!is_hiz(ctx)) {
        go_hiz(ctx);
        ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    }
    uint16_t mv = read_median(ctx);
    ctx->current_state.measured_mv = mv;
    if (mv_out)
        *mv_out = mv;
    if (mv > ctx->cfg.ovp_threshold_mv) {
        ctx->ovp_latched = true;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    return WINSIM_OK;
}

float winsim_equiv_ohm(const winsim_ctx_t *ctx, uint16_t mv)
{
    if (!ready(ctx) || ctx->last_open_mv == 0u)
        return NAN;
    if (mv >= ctx->last_open_mv)
        return INFINITY;
    return pull_from_mv(ctx, (float)mv);
}

winsim_status_t winsim_set_resistance(winsim_ctx_t *ctx, float target_ohm, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    if (!ready(ctx) || !(target_ohm >= 0.0f))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    if (target_ohm == 0.0f)
        sh = true;
    else if (isfinite(target_ohm))
        select_by_resistance(ctx, target_ohm, &mask, &sh);
    /* INFINITY：保持 mask = 0、sh = false，即高阻 */

    return apply_and_measure(ctx, mask, sh, 0u, VERIFY_NONE, out_state);
}

static winsim_status_t ensure_open(winsim_ctx_t *ctx)
{
    if (ctx->last_open_mv != 0u)
        return WINSIM_OK;
    return winsim_probe_open_voltage(ctx, NULL);
}

static void select_by_voltage(const winsim_ctx_t *ctx, uint16_t target_mv, uint16_t *mask, bool *sh)
{
    if (target_mv == 0u) {
        *mask = 0u;
        *sh = true;
        return;
    }
    select_by_ratio(ctx, (float)target_mv / supply_mv(ctx), mask, sh);
}

winsim_status_t winsim_set_voltage(winsim_ctx_t *ctx, uint16_t target_mv, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    winsim_status_t rc = ensure_open(ctx);
    if (rc != WINSIM_OK)
        return rc;

    select_by_voltage(ctx, target_mv, &mask, &sh);
    return apply_and_measure(ctx, mask, sh, target_mv, VERIFY_TARGET, out_state);
}

winsim_status_t winsim_set_ratio(winsim_ctx_t *ctx, float ratio_0_to_1, winsim_state_t *out_state)
{
    uint16_t mask = 0u;
    bool sh = false;
    if (!ready(ctx) || !(ratio_0_to_1 >= 0.0f && ratio_0_to_1 <= 1.0f))
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    if (ratio_0_to_1 == 0.0f)
        sh = true;
    else if (ratio_0_to_1 < 1.0f)
        select_by_ratio(ctx, ratio_0_to_1, &mask, &sh);

    uint16_t target_mv = ctx->last_open_mv ? to_mv(ratio_0_to_1 * supply_mv(ctx)) : 0u;
    return apply_and_measure(ctx, mask, sh, target_mv, VERIFY_NONE, out_state);
}

winsim_status_t winsim_auto_sweep(winsim_ctx_t *ctx, uint16_t start_mv, uint16_t end_mv,
                                  uint16_t step_mv, uint16_t hold_ms,
                                  winsim_sweep_cb_t on_step, void *user_ctx)
{
    if (!ready(ctx) || step_mv == 0u)
        return WINSIM_ERR_PARAM;
    if (ctx->ovp_latched)
        return WINSIM_ERR_OVERVOLTAGE;

    /* 唤醒后 VCC2_5V_SW 可能刚建立，扫描前重新标定。 */
    winsim_status_t rc = winsim_probe_open_voltage(ctx, NULL);
    if (rc != WINSIM_OK) {
        go_hiz(ctx);
        return rc;
    }

    bool up = end_mv >= start_mv;
    uint32_t span = up ? (uint32_t)end_mv - start_mv : (uint32_t)start_mv - end_mv;
    uint32_t steps = span / step_mv + 1u;

    for (uint32_t i = 0; i < steps; i++) {
        uint32_t d = i * step_mv;
        uint16_t t = (uint16_t)(up ? start_mv + d : start_mv - d);
        uint16_t mask = 0u;
        bool sh = false;
        winsim_state_t st;

        select_by_voltage(ctx, t, &mask, &sh);
        rc = apply_and_measure(ctx, mask, sh, t, VERIFY_EXPECTED, &st);
        if (rc != WINSIM_OK) {
            go_hiz(ctx);
            return rc;
        }
        ctx->hal.delay_ms(hold_ms);
        if (on_step)
            on_step(&st, user_ctx);
    }
    go_hiz(ctx);
    return WINSIM_OK;
}

winsim_status_t winsim_clear_ovp(winsim_ctx_t *ctx)
{
    if (!ready(ctx))
        return WINSIM_ERR_PARAM;
    go_hiz(ctx);
    ctx->hal.delay_ms(ctx->cfg.settle_delay_ms);
    uint16_t mv = read_median(ctx);
    ctx->current_state.measured_mv = mv;
    if (mv > ctx->cfg.ovp_threshold_mv) {
        ctx->ovp_latched = true;
        return WINSIM_ERR_OVERVOLTAGE;
    }
    ctx->ovp_latched = false;
    ctx->last_open_mv = mv >= ctx->cfg.min_open_mv ? mv : 0u;
    return WINSIM_OK;
}

/* ------------------------------------------------------------------ 按键学习 */

void winsim_learn_begin(winsim_learn_t *l, const winsim_cfg_t *cfg, uint16_t idle_mv)
{
    winsim_cfg_t d = winsim_default_cfg();
    if (!cfg)
        cfg = &d;
    l->idle_mv = idle_mv;
    l->drop_mv = cfg->learn_drop_mv;
    l->stable_mv = cfg->learn_stable_mv;
    l->stable_ms = cfg->learn_stable_ms;
    l->armed = true;
    l->have_last = false;
    l->last_mv = 0u;
    l->stable_for_ms = 0u;
    l->sum_mv = 0u;
    l->n = 0u;
}

bool winsim_learn_feed(winsim_learn_t *l, uint16_t mv, uint32_t dt_ms, uint16_t *captured_mv)
{
    bool pressed = (uint32_t)mv + l->drop_mv < l->idle_mv;
    if (!pressed) {
        l->armed = true;               /* 回到空闲，可以学下一档 */
        l->have_last = false;
        l->stable_for_ms = 0u;
        return false;
    }
    if (!l->armed)
        return false;

    if (l->have_last && absdiff(mv, l->last_mv) <= l->stable_mv) {
        l->stable_for_ms += dt_ms;
        l->sum_mv += mv;
        l->n++;
    } else {
        l->stable_for_ms = 0u;         /* 新的一段，从这个采样重新计 */
        l->sum_mv = mv;
        l->n = 1u;
    }
    l->have_last = true;
    l->last_mv = mv;

    if (l->stable_for_ms < l->stable_ms)
        return false;
    if (captured_mv)
        *captured_mv = (uint16_t)((l->sum_mv + l->n / 2u) / l->n);
    l->armed = false;
    l->stable_for_ms = 0u;
    return true;
}
