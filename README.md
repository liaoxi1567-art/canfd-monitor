# CAN FD 便携监控器

| 文件 | 版本和用途 |
| --- | --- |
| [原理图 PDF](Hardware-C-schematic.pdf) | Hardware-C 外部供电版，11 页。主控为 144 脚 STM32H743ZIT6。 |
| [软件源码和构建说明](fw/README.md) | Rev E 裸机基线 `06fc686`，含板级驱动、应用、桌面模拟器和测试。 |
| [下载 Rev E 固件 BIN](firmware/canfd-reve.bin) | 适用于 100 脚 STM32H743VIT6 的旧 Rev E。大小为 1,236,104 字节。 |
| [APM 按键模拟进展](docs/apm-keysim/README.md) | Hardware-C 原理图第 12 页第 2 版（提案）、4 路按键模拟的驱动、界面和 SD 键值表；Rev D/E 固件上不显示这个功能。 |

**Hardware-C 的固件适配仍在待办中。** 该板的供电控制、CAN、USB、显示和存储引脚与旧 Rev E 有差异，不能将上面的 BIN 刷入 Hardware-C。

用源码复建固件时显式指定 `BOARD=reve`。发布 BIN 的 SHA-256 为 `e1178da6ac698a2331e5a12a444d567553e3db03eb421ce94b9ea9c11f0d8b49`。
