# 主机构建：状态机 + 模拟板子 → 模拟器动态库、单元测试。
# ARM固件：make firmware（ST HAL + FatFs + TinyUSB + LVGL）。
#
#   make test     编译并运行单元测试
#   make sim      编译模拟器动态库并启动网页（http://localhost:8765）
#   make lib      只编译动态库

CC      ?= cc
PYTHON  ?= $(if $(wildcard .venv-sim/bin/python),.venv-sim/bin/python,python3)
CFLAGS  ?= -std=c11 -O1 -g -Wall -Wextra -Werror -Wshadow -Wno-unused-parameter -fvisibility=hidden
CFLAGS  += -I.
BUILD   := build/host

CORE_SRC := $(wildcard services/*/*.c) app/app.c app/ui/core/ui_sm.c
SIM_SRC  := $(wildcard sim/*.c)
HDRS     := $(wildcard services/*/*.h) app/app.h app/ui/core/ui_sm.h sim/*.h
TEST_SRC := $(wildcard tests/test_*.c)
TEST_BIN := $(patsubst tests/%.c,$(BUILD)/%,$(TEST_SRC))

UNAME := $(shell uname)
ifeq ($(UNAME),Darwin)
  LIB := $(BUILD)/libcanfdsim.dylib
  LIBFLAGS := -dynamiclib
else
  LIB := $(BUILD)/libcanfdsim.so
  LIBFLAGS := -shared -fPIC
endif

.PHONY: all lib test test-web sim clean

all: lib test

.PHONY: test-reve
.PHONY: test-reve-display
test-reve-display:
	DISPLAY_TEST_BOARD=reve $(PYTHON) tests/display_dma/run_test.py
test-reve: $(BUILD)/test_reve_three_key $(BUILD)/test_reve_input_policy $(BUILD)/test_reve_power_runtime $(BUILD)/test_reve_touch_recovery
	./$(BUILD)/test_reve_three_key
	./$(BUILD)/test_reve_input_policy
	./$(BUILD)/test_reve_power_runtime
	./$(BUILD)/test_reve_touch_recovery
$(BUILD)/test_reve_touch_recovery: platform/stm32h743_revd/tests/test_touch_reve_recovery.c platform/stm32h743_revd/touch_ft6336.c platform/stm32h743_revd/touch_ft6336.h platform/stm32h743_revd/tests/hal_mock/stm32h7xx_hal.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVE_TARGET -Iplatform/stm32h743_revd/tests/hal_mock -o $@ $(filter %.c,$^)
$(BUILD)/test_reve_input_policy: tests/reve/test_input_policy.c services/power/input_policy.c services/power/input_policy.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVE_TARGET -o $@ $(filter %.c,$^)
$(BUILD)/test_reve_three_key: tests/reve/test_three_key.c $(CORE_SRC) $(SIM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVE_TARGET -o $@ $(filter %.c,$^)

$(BUILD):
	mkdir -p $(BUILD)

lib: $(LIB)

$(LIB): $(CORE_SRC) $(SIM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) $(LIBFLAGS) -o $@ $(CORE_SRC) $(SIM_SRC)

$(BUILD)/test_%: tests/test_%.c $(CORE_SRC) $(SIM_SRC) $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $< $(CORE_SRC) $(SIM_SRC)

test: $(TEST_BIN)
	@for test_bin in $(TEST_BIN); do ./$$test_bin || exit $$?; done

$(BUILD)/test_plan_names: tests/test_plan_names.c $(CORE_SRC) $(SIM_SRC) platform/stm32h743_revd/send_plan_json.c platform/stm32h743_revd/send_plan_json.h $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(filter %.c,$^)

test-web: $(LIB)
	$(PYTHON) tests/test_web.py
	$(PYTHON) tests/test_web_files.py
	$(PYTHON) tests/test_web_sequence.py
	$(PYTHON) tests/test_local_device_flow.py
	$(PYTHON) tests/test_database_import.py
	$(PYTHON) tests/test_send_plan_format.py
	$(PYTHON) tests/test_send_plan_web.py
	$(PYTHON) tests/test_menu_workflow.py
	$(PYTHON) tests/test_recording_save_workflow.py
	$(PYTHON) tests/test_hold_live_channels.py
	$(PYTHON) tests/test_direct_send_keyboard_scroll.py
	$(PYTHON) tests/test_can_switch_rec_key.py

sim: $(LIB)
	$(PYTHON) sim/web/server.py

# Compile the actual target UI/core branch against deterministic simulated ports.
TEST_BIN += $(BUILD)/test_device_core
test: $(BUILD)/test_device_core
$(BUILD)/test_device_core: tests/device/test_device_core.c $(CORE_SRC) $(filter-out sim/file_port_host.c,$(SIM_SRC)) platform/stm32h743_revd/tests/file_port_target_double.c $(HDRS) | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVD_TARGET -o $@ $(filter %.c,$^)

clean:
	rm -rf build

.PHONY: firmware
firmware:
	$(MAKE) -f Makefile.arm all

$(BUILD)/test_usb_protocol: tests/test_usb_protocol.c platform/stm32h743_revd/usb_protocol.c services/logfmt/candump.c services/core/app_evt.c platform/stm32h743_revd/usb_protocol.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -o $@ $(filter %.c,$^)

# Production filesystem/storage driver on a RAM-backed mock SD card.
# Force the board configuration before ff.h's vendor-local default ffconf.h.
$(BUILD)/test_storage_sd: tests/test_storage_sd.c platform/stm32h743_revd/storage_sd.c platform/stm32h743_revd/storage_files.inc platform/stm32h743_revd/send_plan_json.c platform/stm32h743_revd/diskio_sd.c vendor/fatfs/source/ff.c vendor/fatfs/source/ffunicode.c services/device/send_plan_validate.c services/tx_sched/tx_validate.c services/core/jsonw.c services/logfmt/candump.c services/config/cfg.c tests/test_storage_hal/stm32h7xx_hal.h platform/stm32h743_revd/storage_sd.h platform/stm32h743_revd/fatfs_conf/ffconf.h platform/stm32h743_revd/send_plan_json.h services/device/file_port.h services/device/send_plan_validate.h sim/send_plan.h sim/tx_sequence.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -include platform/stm32h743_revd/fatfs_conf/ffconf.h -Itests/test_storage_hal -Iplatform/stm32h743_revd -Ivendor/fatfs/source -o $@ $(filter %.c,$^)

# USB control/MSC integration on a fake transport and a four-sector RAM card.
# The firmware itself is separately compiled with the real HAL/TinyUSB headers.
TEST_BIN += $(BUILD)/test_usb_device
test: $(BUILD)/test_usb_device
$(BUILD)/test_usb_device: tests/usb/test_usb_device.c platform/stm32h743_revd/usb_device.c platform/stm32h743_revd/usb_protocol.c services/logfmt/candump.c services/core/app_evt.c services/frame_ring/frame_ring.c tests/usb/include/stm32h7xx_hal.h tests/usb/include/tusb.h platform/stm32h743_revd/usb_device.h platform/stm32h743_revd/usb_protocol.h services/bench/bench.h platform/stm32h743_revd/storage_sd.h platform/stm32h743_revd/can_ingress.h platform/stm32h743_revd/display.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVD_TARGET -Itests/usb/include -Iplatform/stm32h743_revd -c platform/stm32h743_revd/usb_device.c -o $(BUILD)/usb_device_diag.o
	$(CC) $(CFLAGS) -fsanitize=address,undefined -Itests/usb/include -Iplatform/stm32h743_revd -o $@ $(filter-out platform/stm32h743_revd/usb_device.c,$(filter %.c,$^)) $(BUILD)/usb_device_diag.o

.PHONY: test-display firmware-check
firmware-check:
	$(MAKE) -f Makefile.arm check
test-display:
	python3 platform/stm32h743_revd/tests/run_ui_host.py

.PHONY: test-display-pipe release-check
test-display-pipe:
	python3 tests/disp/run_target.py

# Product release gate: the maintained hardware target is bare metal. Legacy
# FreeRTOS comparison sources remain in the checkout but are not this release.
release-check:
	$(MAKE) -f Makefile.arm check
	$(MAKE) test test-web
	$(MAKE) test-display test-display-pipe test-display-dma test-bench test-stress

# Rev E is an explicit bare-metal target; preserve the legacy Rev D default.
.PHONY: release-check-reve
release-check-reve:
	$(MAKE) -f Makefile.arm BOARD=reve check
	$(MAKE) test test-web test-reve test-reve-storage
	$(MAKE) test-display test-display-pipe test-reve-display test-bench test-stress

.PHONY: test-can test-display-dma
test-can: $(BUILD)/test_can_ingress
	./$(BUILD)/test_can_ingress
TEST_BIN += $(BUILD)/test_can_ingress
test: $(BUILD)/test_can_ingress
$(BUILD)/test_can_ingress: tests/can/test_can_ingress.c platform/stm32h743_revd/can_fdcan.c platform/stm32h743_revd/can_ingress.c platform/stm32h743_revd/can_ingress.h tests/can/include/stm32h7xx_hal.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -Itests/can/include -Iplatform/stm32h743_revd -pthread -o $@ $(filter %.c,$^)
test-display-dma:
	python3 tests/display_dma/run_test.py

$(BUILD)/test_realtime_io: tests/test_realtime_io.c platform/stm32h743_revd/realtime_io.c platform/stm32h743_revd/realtime_io.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVD_TARGET -Itests/can/include -Iplatform/stm32h743_revd -o $@ $(filter %.c,$^)

TEST_BIN += $(BUILD)/test_sdmmc_wait
test: $(BUILD)/test_sdmmc_wait
$(BUILD)/test_sdmmc_wait: tests/sdmmc/test_sdmmc_wait.c vendor/stm32_hal/Src/stm32h7xx_ll_sdmmc.c | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DSTM32H743xx -DUSE_HAL_DRIVER -DCANFD_REVD_TARGET -Iplatform/stm32h743_revd -Ivendor/stm32_hal/Inc -Ivendor/cmsis_device/Include -Ivendor/cmsis_core/CMSIS/Core/Include -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -Wno-unknown-attributes -o $@ $<

.PHONY: test-bench test-stress
test-bench:
	python3 -m unittest discover -s tests/bench_tools -v
test-stress:
	python3 tests/stress/run_stress.py

.PHONY: firmware-freertos firmware-check-all test-rtos test-rtos-stress
firmware-freertos:
	$(MAKE) -f Makefile.freertos check
firmware-check-all:
	$(MAKE) -f Makefile.arm check
	$(MAKE) -f Makefile.freertos check
test-rtos:
	python3 tests/rtos_host/run_host.py --kind smoke --report build/rtos_host/smoke.json
	python3 tests/rtos_host/run_host.py --kind storage --report build/rtos_host/storage-report.json
	python3 tests/rtos_host/run_host.py --kind bridges --report build/rtos_host/bridges-report.json
test-rtos-stress:
	python3 tests/rtos_stress/run_kernel.py --report build/rtos_stress/report.json

$(BUILD)/test_reve_power_runtime: tests/reve/test_power_runtime.c tests/reve/power_hal/stm32h7xx_hal.h platform/stm32h743_revd/power_reve.c platform/stm32h743_revd/power_reve.h services/power/input_policy.c services/power/input_policy.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVE_TARGET -Itests/reve/power_hal -Iplatform/stm32h743_revd -o $@ $(filter %.c,$^)

.PHONY: test-reve-storage
test-reve-storage: $(BUILD)/test_reve_storage_sd
	./$(BUILD)/test_reve_storage_sd
$(BUILD)/test_reve_storage_sd: tests/test_storage_sd.c platform/stm32h743_revd/storage_sd.c platform/stm32h743_revd/storage_files.inc platform/stm32h743_revd/send_plan_json.c platform/stm32h743_revd/diskio_sd.c vendor/fatfs/source/ff.c vendor/fatfs/source/ffunicode.c services/device/send_plan_validate.c services/tx_sched/tx_validate.c services/core/jsonw.c services/logfmt/candump.c services/config/cfg.c tests/test_storage_hal/stm32h7xx_hal.h platform/stm32h743_revd/storage_sd.h platform/stm32h743_revd/fatfs_conf/ffconf.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_REVE_TARGET -include platform/stm32h743_revd/fatfs_conf/ffconf.h -Itests/test_storage_hal -Iplatform/stm32h743_revd -Ivendor/fatfs/source -o $@ $(filter %.c,$^)

# Device SD storage of APM key tables: same RAM-card harness, built with the
# page-12 hardware flag that current Rev D/E targets leave off.
TEST_BIN += $(BUILD)/test_storage_sd_winsim
test: $(BUILD)/test_storage_sd_winsim
$(BUILD)/test_storage_sd_winsim: tests/test_storage_sd.c platform/stm32h743_revd/storage_sd.c platform/stm32h743_revd/storage_files.inc platform/stm32h743_revd/send_plan_json.c platform/stm32h743_revd/diskio_sd.c vendor/fatfs/source/ff.c vendor/fatfs/source/ffunicode.c services/device/send_plan_validate.c services/tx_sched/tx_validate.c services/core/jsonw.c services/logfmt/candump.c services/config/cfg.c services/keysim/keysim_file.c services/keysim/keysim_file.h services/keysim/keysim.h tests/test_storage_hal/stm32h7xx_hal.h platform/stm32h743_revd/storage_sd.h platform/stm32h743_revd/fatfs_conf/ffconf.h platform/stm32h743_revd/send_plan_json.h services/device/file_port.h services/device/send_plan_validate.h sim/send_plan.h sim/tx_sequence.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DCANFD_WINSIM_HW -include platform/stm32h743_revd/fatfs_conf/ffconf.h -Itests/test_storage_hal -Iplatform/stm32h743_revd -Ivendor/fatfs/source -o $@ $(filter %.c,$^)

# Window-switch simulator driver (Hardware-C page-12 proposal). `make test`
# already runs tests/test_win_sim.c; these targets run it on its own and build
# the desktop demo of the sweep example.
.PHONY: test-winsim winsim-example
test-winsim: $(BUILD)/test_win_sim
	./$(BUILD)/test_win_sim
winsim-example: $(BUILD)/winsim_example
	./$(BUILD)/winsim_example
$(BUILD)/winsim_example: examples/winsim/win_sim_can_example.c services/winsim/win_sim.c services/winsim/win_sim.h | $(BUILD)
	$(CC) $(CFLAGS) -fsanitize=address,undefined -DWINSIM_EXAMPLE_SIM -o $@ $(filter %.c,$^)
