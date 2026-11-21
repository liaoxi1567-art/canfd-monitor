# win_sim：车窗开关模拟器驱动

APM 售后测试扩展模块。监控器在 APM 的车窗开关输入上模拟按键电阻，同时用 CAN 读回 APM 识别出的开关状态，用来测出各档位的电压门限。没有键值表时，也可以并在原车开关上被动学习每个档位。驱动按 APM 车窗开关模拟规范（apm.md）实现，并针对 Hardware-C 新增的第 12 页第 2 版电路做了调整。C99，不用动态内存，裸机和 RTOS 都能用。

**硬件状态：第 12 页还只是提案。** 它尚未加入仓库里 11 页的 `Hardware-C-schematic.pdf`，现有固件也还没有移植到 Hardware-C。因此本目录只提供与平台无关的驱动和主机测试，没有接入 `platform/` 和主循环。

## 第 12 页第 2 版电路

4 路接口 K1–K4 共用一套 13 位电阻阵，同一时刻只接通一路。

| 功能 | 器件和引脚 |
| --- | --- |
| I/O 扩展 | U801 TCA9539PWR，地址 0x74（A0 = A1 = GND），挂在 I2C4（PD12 SCL / PD13 SDA），与 U111 TUSB320、J302 共用总线 |
| 复位和使能 | PG2 = WS_RST_N，经 R814 10 kΩ 下拉。PG2 低时 U801 复位、电阻阵全关；同时经 Q808B 反相成 CH_EN_N，4 路继电器全断 |
| 电阻阵 | b0–b11 = R801–R812（200、390、820、1.6k、3.3k、6.2k、13k、24k、47k、91k、180k、360k Ω），b12 = R831（100 Ω），1%；Q801–Q806、Q808A BSS138DW |
| 0 Ω 直通 | Q807 AO3400A，由 P14 控制 |
| 通道选择 | P16/P17 = CH_A0/CH_A1 → U806 SN74LVC139A 译码 → U802–U805 TLP176AM 光耦继电器 |
| 每路保护 | F801–F804 50 mA PTC，D801–D804 SMF15CA |
| 回读 | 每路接口侧 300 kΩ（R815–R818）→ U807 CD74HC4052 选通 → R819 100 kΩ + C802 10 nF → PC2_C = WS_ADC |
| 接口 | J801 KF2EDGR-3.81-8P，奇数脚 K1–K4，偶数脚 GND |

位映射和上电顺序见 `win_sim_tca9539.h` 的注释。

## 文件

| 文件 | 内容 |
| --- | --- |
| `win_sim.h` / `win_sim.c` | 核心驱动：查找表、控制字求解、通道选择、串联电阻标定、被动测量、按键学习、中值回读、过压锁定、自动扫描 |
| `win_sim_tca9539.h` / `.c` | 第 12 页硬件适配：HAL 到 TCA9539 寄存器、PG2 复位、PC2_C 折算 |
| `../../examples/winsim/win_sim_can_example.c` | 集成示例：CAN 唤醒 → 选通道并标定 → 探测空载电压 → 4800→0 mV 扫描 → 打印电压与开关状态对应表 |
| `../../tests/test_win_sim.c` | 主机单元测试，Mock HAL 按物理模型仿真每一路 APM 输入和 TCA9539 寄存器 |

## 运行

从仓库根目录执行：

```sh
make -C fw test-winsim
make -C fw winsim-example
```

`make -C fw test` 也会运行 `test_win_sim`。`services/winsim/*.c` 由 `services/*/*.c` 通配编进主机测试、模拟器动态库和 ARM 固件。ARM 固件里目前没有调用方，`--gc-sections` 会把它们丢掉。示例依赖集成方实现的 CAN 回调，所以放在 `fw/examples/winsim/`，不放在 `services/` 下。`run_ui_host.py` 和 `generate_ui_font.py` 会递归扫描 `services/` 下的全部 `.c`。

## 用法

```c
static winsim_ctx_t ws;                       // 约 33 KB，放静态区
winsim_init(&ws, &hal, NULL);

// 切到 K2 并标定这一路的 PTC + 继电器串联电阻（会短暂接通 0 Ω 直通）
float r_series;
winsim_select_channel(&ws, 1, true, &r_series);

// 模拟按键：按目标电压或阻值
winsim_state_t st;
winsim_set_voltage(&ws, 3480, &st);
winsim_set_hiz(&ws);                          // 松开

// 被动学习：原车开关并在这一路，电阻阵保持高阻
winsim_learn_t l;
uint16_t idle, mv, key_mv;
winsim_probe_open_voltage(&ws, &idle);        // 开关松开时
winsim_learn_begin(&l, &ws.cfg, idle);
for (;;) {                                    // 每 50 ms 一次
    winsim_measure(&ws, &mv);
    if (winsim_learn_feed(&l, mv, 50, &key_mv))
        save_key(key_mv, winsim_equiv_ohm(&ws, key_mv));
}
```

- **换通道**：`winsim_select_channel()` 先把电阻阵切到高阻，再改通道地址，等 `channel_switch_ms`（默认 5 ms，TLP176AM 接通最长 3 ms）。换通道会清掉空载电压，下次设置时重新探测。
- **串联电阻**：每路串联 PTC 和继电器，约 5–17 Ω，驱动事先不知道。标定方法是先在高阻下测空载电压，再接通直通测一次，扣掉直通管和 400 kΩ 回读负载后得到这一路的值。PTC 断开时返回 `WINSIM_ERR_CALIBRATION`，保留原值。
- **0 mV**：受串联电阻限制，直通时接口约 11–39 mV，仍远低于 APM 的对地短路门限。
- **过压锁定**不随换通道解除。换到正常的通道后调用 `winsim_clear_ovp()` 重新检测。
- **学习规则**：低于空闲电压 150 mV 算按下，相邻采样差 ≤ 25 mV 持续 500 ms 后记录一档（取稳定段平均值），之后要回到空闲附近才能学下一档。三个参数都在 `winsim_cfg_t` 里。

## 界面

`services/keysim/` 把驱动包装成界面用的非阻塞状态机，`app_tick()` 每个节拍推进它；界面页面在 `app/ui/core/ui_sm.c`。

- **入口：** 主页第 7 格“按键模拟”，主页随之变成 4×2。板上没有这部分硬件时（当前的 Rev D/E 目标）不显示入口，keysim 编译成空壳，不分配约 33 KB 的驱动上下文；Rev E 固件 RAM 只多 64 字节。将来 Hardware-C 目标定义 `CANFD_WINSIM_HW` 并实现 `port_keysim_hal()`、`port_keysim_apm_state()` 即可启用。
- **按键模拟（总览）：** K1–K4 各一行，显示是否接通和键值表；APM 供电一行（未上电、过压时标黄，过压时 OK 重新检测）；全部松开。OK 一路就切换并标定串联电阻。
- **K*n* 按键模拟（通道页）：** 接口电压和 APM 判定、按法（点按 300 ms / 保持）、键值表里的各键、手动电压（左右键调 0.1 V）、全部松开、学习键值、门限扫描、串联电阻（OK 重新标定）。
- **学习键值：** OK 开始后每 50 ms 测一次接口电压，按住原车开关一个档位 0.5 秒自动记录一档，显示电压和反推的开关电阻；OK 一档可以在预设名称间改名；保存后成为这一路的键值表。
- **门限扫描：** 从 4.8 V 每 50 mV 往下设，每档停 30 ms 读 APM 开关状态，记录门限；保存时每档取中点。读不到 APM 开关状态时提示需要配置 CAN 信号。
- **安全：** 进出这几页、长按回主页、关机时都会停止学习 / 扫描并松开；过压时锁定，直到在总览里重新检测。
- **键值表存 SD 卡：** 每路一个文件 `/APMKEY/K1.TXT … K4.TXT`，格式见 `services/keysim/keysim_file.h`，是纯文本，电脑上可以直接查看、修改或拷到别的设备。学习或扫描后保存时写盘，每次插卡 / 开机挂载后读回。拔卡时保存的表留在内存里，标“未存 SD”，插卡后自动补存；写保护或写失败时标黄，可在通道页“键值表”一行重试。
  - 板级：`storage_files.inc` 在卡空闲时处理请求，先写临时文件，再把旧文件改名为 `.BAK`、临时文件改名为 `K*n*.TXT`，最后删掉 `.BAK`；只剩 `.BAK` 时下次读取自动恢复。这段代码只在定义 `CANFD_WINSIM_HW` 时编译，`test_storage_sd_winsim` 用真实 FatFs 和 RAM 卡测试它。
  - 模拟器：`env_sim.c` 用内存模拟这几个文件，设备断电重启不丢，格式化 SD 或重置模拟器才清空。
- **还没做：** 扫描用的 CAN 开关状态信号还没有配置界面（模拟器里由仿真 APM 直接给出）；“按住才生效”的按法需要触摸的按下 / 抬起事件，现有手势只有点击，所以暂时只有点按和保持。设备字库没有 Ω，界面里阻值写成“欧 / 千欧”。

桌面模拟器（`make -C fw sim`）右侧“环境”里有 APM 按键模拟台架：唤醒或休眠 APM、给每一路接上或断开原车开关、按住某个档位、模拟误接 12 V。

## 与 apm.md 的差异

| apm.md | 本实现 | 原因 |
| --- | --- | --- |
| 8 位电阻阵，`uint8_t mask`，`r_lut[256]` | 13 位，`uint16_t mask`，`r_lut[8192]`（32 KB） | 8 位达不到 25 mV 精度；每路串联电阻约 5–17 Ω，12 位到不了 200 mV，所以在低阻端加了 b12 = 100 Ω |
| 1 路 | 4 路共用电阻阵，`select_channel()` 加进 HAL | 同一时刻只按一路，复用一套电阻阵；译码器保证不会同时接通两路 |
| 74HCT595，`/OE` 上拉 | TCA9539，RESET 下拉到 PG2 | 主控只剩 PG2、PC2_C 两个空闲脚；HAL 名称不变，映射见 `win_sim_tca9539.h` |
| 回读 1:1，负载 200 kΩ | 1:4，负载 400 kΩ，取在各路接口侧 | 误接 12 V 时 ADC 脚不超过 3.3 V；接口侧回读才能被动学习 |
| `winsim_state_t.mask_595` | `mask`，另加 `channel`、`target_mv` | 控制字不再来自 595；回调里需要知道通道和目标值 |
| 扫描中按目标值校验 | 扫描中按理论值校验 | 0 mV 附近无档位，按目标值校验会误报 |
| — | `winsim_calibrate_series()`、`winsim_measure()`、`winsim_equiv_ohm()`、`winsim_learn_*()` | 自动标定串联电阻、被动学习原车开关 |
| — | `winsim_clear_ovp()` | 规范要求的显式解锁接口 |

`set_resistance()` 的目标阻值是接口对地的电阻，含串联电阻，不含 400 kΩ 回读负载；`set_voltage()` 和 `set_ratio()` 的模型包含负载。

## 接入 Hardware-C 固件

以下内容要等 Hardware-C 板级移植完成后再做。

1. 在板级代码里实现 `winsim_tca9539_port_t` 的四个回调，再用 `winsim_tca9539_bind()` 得到 `winsim_hal_t`：
   ```c
   static bool ws_i2c_write(uint8_t addr7, uint8_t reg, const uint8_t *d, uint8_t n)
   {
       return HAL_I2C_Mem_Write(&hi2c4, (uint16_t)(addr7 << 1), reg, I2C_MEMADD_SIZE_8BIT,
                                (uint8_t *)d, n, 5) == HAL_OK;
   }
   static void ws_reset_pin(bool high)
   {
       HAL_GPIO_WritePin(GPIOG, GPIO_PIN_2, high ? GPIO_PIN_SET : GPIO_PIN_RESET);
   }
   ```
   `adc_read_raw` 读 ADC3_INP0（PC2_C）。
2. PG2 配置为推挽输出，初始低电平。PC2_C 保持模拟输入，SYSCFG 模拟开关保持默认的断开状态即可。
3. 在 `HardFault_Handler` 和致命错误处理里先调用 `winsim_tca9539_emergency_off()`。
4. I2C4 上还有 U111 TUSB320。两边都只能在前台循环里访问，不要在中断里调用本模块。
5. **阻塞时间：** 每次设置约等待 `settle_delay_ms`（10 ms），换通道并标定约 25 ms，4800 到 0 mV、步长 50 mV、每档保持 30 ms 的扫描约 4 s。在裸机主循环里，`delay_ms` 回调必须在等待期间继续处理 CAN 采集、USB 和显示，做法和现有 SD 协作等待相同，否则会丢帧。FreeRTOS 版本可以放进独立任务。
6. `winsim_ctx_t` 约 33 KB，要放在静态区（AXI SRAM），不要放在栈上。

## 已验证和未验证

- **已验证：** 驱动测试 17 组共 1372 项检查、界面测试 `test_keysim_ui` 466 项（含 SD 卡读写、断电重启、拔插卡、写保护）、文件格式测试 50 项、板级 SD 存储测试 `test_storage_sd_winsim` 全部通过，在 ASan/UBSan 下运行；LVGL 主机渲染测试走了一遍总览、通道页和学习页。
- **精度（仿真）：** 200–4600 mV 每 200 mV 一个测试点。标称器件、串联 12.4 Ω 时最差 11 mV；20 组随机器件（1% 电阻、APM 上拉 ±1%、导通电阻偏离模型 2 Ω、串联 5–17 Ω）最差 19 mV。0 mV 时直通读数不超过 35 mV。
- **标定和学习（仿真）：** 串联电阻 5–17 Ω 的标定误差不超过 0.2 Ω；被动测量 47 Ω–5.1 kΩ 的原车开关，反推电阻误差不超过 0.41%。
- **示例：** 示例扫描在 K3 上还原出了仿真 APM 设定的 5 个门限。
- **ARM 编译：** 用 Arm GNU Toolchain 15.2.Rel1 跑 `make -C fw -f Makefile.arm BOARD=reve check` 通过，没有告警。Rev E 上 keysim 是空壳，链接后不留任何 winsim 符号，也不含 SD 键值表代码；接入界面和 SD 存储后 Flash 共多 8 KB、RAM 多 64 字节，主页仍是 6 格。打开 `CANFD_WINSIM_HW` 的那部分代码也用 Arm GCC 单独编译检查过，没有告警。驱动函数栈帧最大 208 B（`winsim_init`）。
- **未验证：** STM32 平台回调、实板 I2C 和 ADC、与 APM 的联调；PTC 的具体料号和阻值。
