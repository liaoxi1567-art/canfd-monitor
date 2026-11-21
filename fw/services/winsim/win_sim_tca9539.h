/* SPDX-License-Identifier: MIT */
/*
 * Hardware-C 第 12 页第 2 版的硬件适配：把 winsim_hal_t 映射到 TCA9539 + PG2 + PC2_C。
 *
 *   U801 TCA9539 @ 0x74（A0 = A1 = GND），I2C4：PD12 = SCL，PD13 = SDA
 *   P00–P07 → 电阻阵 b0–b7          （输出寄存器 0x02）
 *   P10–P13 → 电阻阵 b8–b11         （输出寄存器 0x03 bit0–3）
 *   P14     → 0 Ω 直通 Q807         （0x03 bit4）
 *   P15     → 电阻阵 b12（100 Ω）   （0x03 bit5）
 *   P16/P17 → 通道地址 CH_A0/CH_A1  （0x03 bit6–7，K1–K4 = 0–3）
 *   RESET   ← PG2（WS_RST_N），R814 下拉；低 = 复位，全部引脚变输入，MOS 由 100 kΩ 下拉关断；
 *             PG2 同时经 Q808B 反相成 CH_EN_N，低时 4 路继电器全断、回读开关关闭
 *   ADC     ← PC2_C（WS_ADC），所选通道 R815–R818 之一 / R819 = 300k/100k，折算系数 4
 *
 * set_oe(false) 拉低 PG2；set_oe(true) 释放复位后，先写输出寄存器再设为输出方向。
 * 芯片复位后输出寄存器默认 0xFF，顺序反了会让全部 MOS 瞬间导通。
 * 任何一次 I2C 写失败都会立刻拉低 PG2，回到全部关断。
 *
 * winsim_hal_t 没有用户指针，所以本适配是单实例的。
 */
#ifndef WIN_SIM_TCA9539_H
#define WIN_SIM_TCA9539_H

#include <stdbool.h>
#include <stdint.h>

#include "services/winsim/win_sim.h"

#define WINSIM_TCA9539_ADDR    0x74u
#define WINSIM_TCA9539_REG_OUT 0x02u   /* Output Port 0；0x03 = Port 1 */
#define WINSIM_TCA9539_REG_CFG 0x06u   /* Configuration 0；0x07 = Port 1，0 = 输出 */
#define WINSIM_TCA9539_P1_SHORT 0x10u  /* P14 */
#define WINSIM_TCA9539_P1_B12   0x20u  /* P15 */
#define WINSIM_TCA9539_P1_CH_SHIFT 6u  /* P16/P17 */
#define WINSIM_TCA9539_P1_CH_MASK  0xC0u

typedef struct {
    /* 从 reg 开始连续写 len 字节（芯片自动递增），收到 ACK 返回 true。 */
    bool     (*i2c_write)(uint8_t addr7, uint8_t reg, const uint8_t *data, uint8_t len);
    void     (*reset_pin)(bool high);   /* PG2：false = 拉低（复位），true = 释放 */
    uint16_t (*adc_read_raw)(void);     /* PC2_C 原始码值 */
    void     (*delay_ms)(uint32_t ms);
    uint8_t  addr7;
    uint16_t adc_full_scale;            /* 16 位 ADC 为 65535 */
    uint16_t adc_vref_mv;               /* VREF+ = +3V3A */
    float    divider_ratio;             /* (R815 + R816) / R816 */
} winsim_tca9539_port_t;

/* 地址、ADC 满量程、参考电压和分压比的默认值；四个回调需要调用方填写。 */
winsim_tca9539_port_t winsim_tca9539_default_port(void);

/* 绑定平台接口并输出 HAL。绑定时立即拉低 PG2。 */
bool winsim_tca9539_bind(const winsim_tca9539_port_t *port, winsim_hal_t *hal_out);

/* 在 HardFault、看门狗预警等场合直接调用：拉低 PG2，全部开关关断。 */
void winsim_tca9539_emergency_off(void);

/* 累计 I2C 写失败次数。 */
uint32_t winsim_tca9539_error_count(void);

#endif /* WIN_SIM_TCA9539_H */
