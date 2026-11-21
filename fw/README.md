# CAN FD 便携监控器固件

本目录提供 Rev E 裸机开发版的板级驱动、应用服务、桌面模拟器和测试。源码基线为 06fc68643cf1620be7e0c722e15ac1effa039caf，USB 版本名为 `reve-bare-reliability-20261104-dev`。固件针对 100 引脚 STM32H743VIT6 的旧 Rev E。外供电 Hardware-C 采用 144 引脚主控，仍需要独立板级移植。当前代码另有 Rev D 兼容目标，已打样的 Rev D/r2 使用历史镜像。

## 在仓库根目录构建

需要 Python 3、GitHub 上游文件的网络访问、GNU Make、Arm GNU Toolchain（包含 gcc、binutils 和 newlib-nano）。本次核对使用 Arm GNU Toolchain 15.2.Rel1。首次运行下列命令从 LVGL 固定提交取回依赖。每个文件按随附清单核对 SHA256。已有上游压缩包时，可给脚本传入 `--archive /path/to/lvgl.tar.gz`。

```sh
python3 fw/third_party/fetch_lvgl.py
make -C fw -j4 -f Makefile.arm BOARD=reve check
```

输出文件为 `fw/build/reve/canfd-reve.bin`、`.elf` 和 `.hex`。参考 BIN 大小为 1,236,104 字节，SHA256 为 `e1178da6ac698a2331e5a12a444d567553e3db03eb421ce94b9ea9c11f0d8b49`。构建 Rev E 时始终显式指定板型，Makefile 的历史默认值是 Rev D。

## 主机回归与模拟器

本机需要支持 ASan/UBSan 的 C 编译器、POSIX 环境和 Python venv。从仓库根目录执行以下命令。回归使用软件模型、RAM 模拟存储和本地 HTTP 服务。

```sh
python3 -m venv fw/.venv-sim
fw/.venv-sim/bin/python -m pip install -r fw/sim/web/requirements.txt
make -C fw -j4 release-check-reve
make -C fw sim
```

`app/` 和 `services/` 存放状态机、采集、录制、回放与发送逻辑。`platform/stm32h743_revd/` 是 Rev D/E 共用板级代码，其中的宏选择 Rev E。主循环轮询 CAN、SD、USB 和显示。`sim/` 复用业务代码和虚拟端口，`tests/` 提供主机模型，`tools/` 包含镜像校验和台架工具。模拟器附带的是通用测试数据库。`services/winsim/` 是 Hardware-C 车窗开关模拟器的驱动，`services/keysim/` 和主页的“按键模拟”页面在它上面实现学习、扫描和模拟发送；对应的硬件还是提案，Rev D/E 目标上不显示这个入口，详见[该目录说明](services/winsim/README.md)。

## 已知限制

现有回归通过后，仍有队列拥塞时周期停发可能误报成功、多分段文件保护对象错误和特定拔卡时序下存储状态残留等已知问题。现有主机模型和可重复镜像不能证明实板 CAN、SD、USB、显示、电源切换或温升通过。Hardware-C 所需的引脚映射、外部 SDRAM 和 RTC 日历尚待实现。

## 第三方来源

- STM32H7 HAL 使用 BSD-3-Clause，CMSIS Core/Device 使用 Apache-2.0。所选源文件和各自许可证位于 `vendor/`。平台启动汇编从 ST 的 GCC 启动文件修改，加入 PE2 早期置位及片内 RAM 初始化；SDMMC 本地补丁和上游文件校验值在 `third_party/patches/`。
- TinyUSB 和 FreeRTOS-Kernel 的许可证随各自源码提供。FatFs 保留 ChaN 的声明和两份官方补丁。can-utils 日志格式适配使用 BSD-3-Clause 选项，随附 `services/logfmt/LICENSE.can-utils`。
- `ui_font_revd_16` 是 Adobe 字体的派生字库，沿用 SIL OFL 1.1，包含在固件中。版权与保留名称见 `platform/stm32h743_revd/fonts/LICENSE.SourceHanSansSC.txt`。
- LVGL 由安装脚本恢复。LVGL 主许可证、各组件与字体的单独声明也都列在下载文件清单内。模拟器 Python 包版本锁定在 `sim/web/requirements.txt`，相关声明在 `sim/web/licenses/`。依赖来源、精确提交与本地补丁见[依赖锁定清单](third_party/dependencies.lock.json)。
