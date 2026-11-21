/* SPDX-License-Identifier: MIT */
#include "services/winsim/win_sim_tca9539.h"

#include <stddef.h>

#define P1_MASK_BITS 0x0Fu   /* P10–P13 = b8–b11 */
#define P1_KEEP_ON_WRITE (WINSIM_TCA9539_P1_SHORT | WINSIM_TCA9539_P1_CH_MASK)

static struct {
    winsim_tca9539_port_t port;
    uint8_t  out[2];     /* 输出寄存器影子：先改影子，允许输出时再写入芯片 */
    bool     enabled;
    bool     bound;
    uint32_t errors;
} s_tca;

winsim_tca9539_port_t winsim_tca9539_default_port(void)
{
    winsim_tca9539_port_t p;
    p.i2c_write = NULL;
    p.reset_pin = NULL;
    p.adc_read_raw = NULL;
    p.delay_ms = NULL;
    p.addr7 = WINSIM_TCA9539_ADDR;
    p.adc_full_scale = 65535u;
    p.adc_vref_mv = 3300u;
    p.divider_ratio = 4.0f;
    return p;
}

static void force_off(void)
{
    s_tca.port.reset_pin(false);
    s_tca.enabled = false;
}

static bool wr(uint8_t reg, const uint8_t *data, uint8_t len)
{
    if (s_tca.port.i2c_write(s_tca.port.addr7, reg, data, len))
        return true;
    s_tca.errors++;
    return false;
}

static void tca_shift_write(uint16_t mask)
{
    s_tca.out[0] = (uint8_t)(mask & 0xFFu);
    s_tca.out[1] = (uint8_t)((s_tca.out[1] & P1_KEEP_ON_WRITE) | ((mask >> 8) & P1_MASK_BITS) |
                             ((mask & 0x1000u) ? WINSIM_TCA9539_P1_B12 : 0u));
    if (s_tca.enabled && !wr(WINSIM_TCA9539_REG_OUT, s_tca.out, 2u))
        force_off();
}

static void tca_set_short_0r(bool enable)
{
    if (enable)
        s_tca.out[1] = (uint8_t)(s_tca.out[1] | WINSIM_TCA9539_P1_SHORT);
    else
        s_tca.out[1] = (uint8_t)(s_tca.out[1] & ~WINSIM_TCA9539_P1_SHORT);
    if (s_tca.enabled && !wr((uint8_t)(WINSIM_TCA9539_REG_OUT + 1u), &s_tca.out[1], 1u))
        force_off();
}

static void tca_select_channel(uint8_t ch)
{
    s_tca.out[1] = (uint8_t)((s_tca.out[1] & (uint8_t)~WINSIM_TCA9539_P1_CH_MASK) |
                             ((uint8_t)(ch & 3u) << WINSIM_TCA9539_P1_CH_SHIFT));
    if (s_tca.enabled && !wr((uint8_t)(WINSIM_TCA9539_REG_OUT + 1u), &s_tca.out[1], 1u))
        force_off();
}

static void tca_set_oe(bool enable)
{
    static const uint8_t all_outputs[2] = { 0x00u, 0x00u };
    if (!enable) {
        force_off();
        return;
    }
    if (s_tca.enabled)
        return;
    s_tca.port.reset_pin(true);
    s_tca.port.delay_ms(1u);
    /* 复位后输出寄存器为 0xFF、方向为输入：先写输出值，再切输出方向。 */
    if (!wr(WINSIM_TCA9539_REG_OUT, s_tca.out, 2u) || !wr(WINSIM_TCA9539_REG_CFG, all_outputs, 2u)) {
        force_off();
        return;
    }
    s_tca.enabled = true;
}

static uint16_t tca_adc_read_mv(void)
{
    float mv = (float)s_tca.port.adc_read_raw() * (float)s_tca.port.adc_vref_mv *
               s_tca.port.divider_ratio / (float)s_tca.port.adc_full_scale;
    if (mv >= 65535.0f)
        return 65535u;
    return (uint16_t)(mv + 0.5f);
}

static void tca_delay_ms(uint32_t ms)
{
    s_tca.port.delay_ms(ms);
}

bool winsim_tca9539_bind(const winsim_tca9539_port_t *port, winsim_hal_t *hal_out)
{
    if (!port || !hal_out || !port->i2c_write || !port->reset_pin || !port->adc_read_raw ||
        !port->delay_ms || port->adc_full_scale == 0u || !(port->divider_ratio > 0.0f))
        return false;

    s_tca.port = *port;
    s_tca.out[0] = 0u;
    s_tca.out[1] = 0u;
    s_tca.errors = 0u;
    s_tca.bound = true;
    force_off();

    hal_out->shift_write = tca_shift_write;
    hal_out->set_oe = tca_set_oe;
    hal_out->set_short_0r = tca_set_short_0r;
    hal_out->adc_read_mv = tca_adc_read_mv;
    hal_out->delay_ms = tca_delay_ms;
    hal_out->select_channel = tca_select_channel;
    return true;
}

void winsim_tca9539_emergency_off(void)
{
    if (s_tca.bound)
        force_off();
}

uint32_t winsim_tca9539_error_count(void)
{
    return s_tca.errors;
}
