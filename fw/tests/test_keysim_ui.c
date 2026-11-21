/* SPDX-License-Identifier: MIT */
/* 按键模拟界面：主页入口 → 4 路总览 → 通道页按键 → 学习 → 扫描 → 过压 / 未上电。
 * 走真实的 ui_sm + keysim + winsim，外部条件由模拟器里的仿真 APM（env_cmd ks_*）提供。 */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/keysim/keysim.h"
#include "services/keysim/keysim_file.h"
#include "services/power/power_sm.h"
#include "services/storage/storage_sm.h"
#include "sim/env_sim.h"
#include "sim/sim_api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static void run(unsigned ms) { for (unsigned i = 0; i < ms; i++) sim_tick(1); }
static void post(ev_type_t type, int ch, int a, int b) { CHECK(evt_post4(type, (uint8_t)ch, SRC_UI, a, b)); app_tick(0); }
static void key(key_id_t k) { post(EV_KEY, k, GEST_SHORT, 0); }
static void touch(int row) { post(EV_TOUCH_ITEM, 0, row, 0); }
static void back(void) { post(EV_TOUCH_BACK, 0, 0, 0); }
static const char *json(void)
{
    static char data[65536];
    jw_t j;
    jw_init(&j, data, sizeof data);
    jw_obj(&j);
    ui_json(&j);
    jw_end_obj(&j);
    CHECK(jw_finish(&j) > 0 && !j.overflow);
    return data;
}
static bool has(const char *s) { return strstr(json(), s) != NULL; }
static bool toast_has(const char *s) { return strstr(ui_last_toast(), s) != NULL; }
static int row(const char *label)
{
    const char *at = strstr(json(), "\"items\":[");
    char expected[160];
    int index = 0;
    CHECK(at != NULL);
    snprintf(expected, sizeof expected, "\"l\":\"%s\"", label);
    while ((at = strstr(at, "\"l\":"))) {
        if (!strncmp(at, expected, strlen(expected)))
            return index;
        index++;
        at++;
    }
    fprintf(stderr, "no row %s\n", label);
    CHECK(false);
    return -1;
}
static void tap(const char *label) { touch(row(label)); }
static void boot(void)
{
    sim_reset();
    sim_key(KEY_POWER, 1);
    run(1200);
    sim_key(KEY_POWER, 0);
    run(600);
    env_cmd("bus_fps", 0, 0);
    env_cmd("bus_fps", 1, 0);
    CHECK(power_running() && storage_get()->state == ST_READY);
}
static unsigned udiff(unsigned a, unsigned b) { return a > b ? a - b : b - a; }

static void home_entry_and_overview(void)
{
    boot();
    CHECK(keysim_available());
    CHECK(has("\"home_columns\":4"));
    CHECK(row("按键模拟") == 6);
    tap("按键模拟");
    CHECK(ui_page() == PG_KEYSIM);
    CHECK(row("K1") == 0 && row("K4") == 3 && row("APM 供电") == 4 && row("全部松开") == 5);
    run(400);                                   /* 空闲测量 */
    CHECK(keysim_get()->supply_ok && has("已上电"));
    CHECK(has("K1\",\"v\":\"已接通 · 未学习"));
    CHECK(ui_review_navigate(PG_HOME) == 0 && ui_review_navigate(PG_KEYSIM) == 0 && ui_page() == PG_KEYSIM);
}

static void learn_then_press(void)
{
    home_entry_and_overview();
    tap("K2");
    CHECK(ui_page() == PG_KEYCH && keysim_get()->ch == 1);
    CHECK(has("\"title\":\"K2 按键模拟\""));
    CHECK(keysim_get()->calibrated[1] && udiff((unsigned)(keysim_get()->r_series[1] * 10.0f), 98u) <= 5u);
    CHECK(toast_has("已接通 K2"));
    CHECK(has("还没有键值表"));
    tap("还没有键值表");
    CHECK(toast_has("先学习或扫描"));

    tap("学习键值");
    CHECK(ui_page() == PG_KEYLEARN);
    tap("学习");
    CHECK(keysim_get()->activity == KS_LEARNING && keysim_get()->learn_idle_mv > 4900u);
    for (int pos = 1; pos <= 4; pos++) {
        env_cmd("ks_switch", 1, pos);           /* 按住原车开关第 pos 档 */
        run(900);
        env_cmd("ks_switch", 1, 0);
        run(300);
        CHECK(keysim_get()->learn_n == (uint8_t)pos);
    }
    env_cmd("ks_switch", 1, 1);                 /* 同一档再按一次：不重复记录 */
    run(900);
    env_cmd("ks_switch", 1, 0);
    run(300);
    CHECK(keysim_get()->learn_n == 4u);
    const ks_key_t *k0 = &keysim_get()->learn_keys[0];
    CHECK(!strcmp(k0->name, "上升") && udiff(k0->mv, 3485u) < 40u);
    CHECK(k0->ohm > 4900.0f && k0->ohm < 5300.0f);         /* 原车开关 5.1 千欧 */
    CHECK(has("第 1 档 上升") && has("千欧"));
    int fourth = row("第 4 档 一键下降");        /* 改名在 6 个预设名称间循环 */
    touch(fourth);
    CHECK(has("第 4 档 对地短路"));
    for (int i = 0; i < 5; i++)
        touch(fourth);
    CHECK(has("第 4 档 一键下降"));
    tap("保存");
    CHECK(ui_page() == PG_KEYCH && keysim_get()->tables[1].valid && keysim_get()->tables[1].n == 4u);
    CHECK(toast_has("已保存 4 个键值到 K2"));
    CHECK(keysim_get()->activity == KS_IDLE);

    /* 点按：300 ms 后自动松开 */
    tap("上升");
    CHECK(keysim_get()->pressed == 0 && keysim_get()->activity == KS_PRESSING);
    CHECK(udiff(keysim_get()->port_mv, 3485u) < 25u);
    CHECK(keysim_apm_state() == 1 && has("APM 判定：上升"));
    run(350);
    CHECK(keysim_get()->pressed == -1 && keysim_get()->activity == KS_IDLE);

    /* 保持：再按一次松开 */
    touch(1);                                   /* 按法 → 保持 */
    CHECK(keysim_get()->press_mode == KS_LATCH && has("保持：再按一次松开"));
    tap("下降");
    run(1000);
    CHECK(keysim_get()->pressed == 2 && keysim_apm_state() == 3);
    tap("下降");
    CHECK(keysim_get()->pressed == -1);

    /* 手动电压：2.50 V → 3.00 V，OK 按下，离开页面自动松开 */
    int manual = row("手动电压");
    for (int i = 0; i < 20; i++)
        key(KEY_UP);
    for (int i = 0; i < manual; i++)
        key(KEY_DOWN);
    CHECK(ui_cursor() == manual);
    for (int i = 0; i < 5; i++)
        key(KEY_RIGHT);
    CHECK(keysim_get()->manual_mv == 3000u);
    key(KEY_OK);
    CHECK(keysim_get()->pressed == KS_MANUAL && udiff(keysim_get()->port_mv, 3000u) < 25u);
    back();
    CHECK(ui_page() == PG_KEYSIM && keysim_get()->pressed == -1);
    CHECK(has("K2\",\"v\":\"已接通 · 学习 · K2 · 4 个键值"));
}

static void scan_on_k3(void)
{
    home_entry_and_overview();
    env_cmd("ks_attach", 2, 0);                 /* 扫描时原车开关断开 */
    tap("K3");
    CHECK(ui_page() == PG_KEYCH && keysim_get()->ch == 2);
    tap("门限扫描");
    CHECK(ui_page() == PG_KEYSCAN && has("APM 上报 · 当前 空闲"));
    tap("扫描");
    CHECK(keysim_get()->activity == KS_SCANNING);
    run(500);
    CHECK(keysim_get()->scan_mv < 4800u && has("进行中"));
    run(6000);
    CHECK(keysim_get()->activity == KS_IDLE && keysim_get()->scan_done);
    CHECK(keysim_get()->scan_n == 5u);
    CHECK(has("空闲 → 上升") && has("一键下降 → 故障"));
    const ks_edge_t *e = &keysim_get()->scan_edges[0];
    CHECK(e->below_mv <= 4000u && e->above_mv > 4000u);
    tap("保存");
    CHECK(ui_page() == PG_KEYCH);
    const ks_table_t *t = &keysim_get()->tables[2];
    CHECK(t->valid && t->n == 5u);
    CHECK(!strcmp(t->keys[0].name, "上升") && t->keys[0].mv > 3000u && t->keys[0].mv < 4000u);
    CHECK(!strcmp(t->keys[4].name, "对地短路") && t->keys[4].mv == 0u);
    tap("对地短路");
    CHECK(keysim_get()->pressed == 4 && keysim_get()->port_mv < 45u && keysim_apm_state() == 5);
    run(400);

    /* 扫描中途退出：回到高阻 */
    tap("门限扫描");
    tap("扫描");
    run(300);
    back();
    CHECK(ui_page() == PG_KEYCH && keysim_get()->activity == KS_IDLE);
}

static void scan_without_state_source(void)
{
    home_entry_and_overview();
    tap("K4");
    tap("门限扫描");
    env_cmd("ks_apm", 0, 0);                    /* APM 睡着：既没电也没有状态报文 */
    run(400);
    tap("扫描");
    CHECK(keysim_get()->activity == KS_IDLE && toast_has("APM 未上电"));
    CHECK(has("读不到：需要配置 CAN 开关状态信号"));
    env_cmd("ks_apm", 1, 0);
    run(400);
    tap("扫描");
    CHECK(keysim_get()->activity == KS_SCANNING);
    back();
}

static void overvoltage(void)
{
    home_entry_and_overview();
    tap("K1");
    tap("学习键值");
    tap("学习");
    env_cmd("ks_switch", 0, 1);
    run(900);
    env_cmd("ks_switch", 0, 0);
    run(300);
    tap("保存");
    CHECK(keysim_get()->tables[0].valid);
    touch(1);                                   /* 保持模式 */
    tap("上升");
    CHECK(keysim_get()->pressed == 0);
    env_cmd("ks_fault", 0, 1);                  /* K1 误接 12 V */
    run(400);
    tap("上升");                                 /* 保持模式下再按 = 松开 */
    tap("上升");                                 /* 再按下：被过压拦下 */
    CHECK(keysim_get()->ovp && keysim_get()->pressed == -1);
    CHECK(toast_has("接口过压") || has("过压"));
    back();
    CHECK(ui_page() == PG_KEYSIM && has("过压已断开"));
    tap("APM 供电");                             /* 仍是 12 V：不解锁 */
    CHECK(keysim_get()->ovp);
    env_cmd("ks_fault", 0, 0);
    tap("APM 供电");
    CHECK(!keysim_get()->ovp && toast_has("已解除过压锁定"));
}

static void leaves_release(void)
{
    home_entry_and_overview();
    tap("K1");
    tap("学习键值");
    tap("学习");
    env_cmd("ks_switch", 0, 2);
    run(900);
    env_cmd("ks_switch", 0, 0);
    run(300);
    tap("保存");
    touch(1);                                   /* 保持模式 */
    tap("上升");                                 /* 第一次学到的档位默认叫“上升”，可改名 */
    CHECK(keysim_get()->pressed == 0 && keysim_apm_state() == 2);   /* 实际是一键上升那一档 */
    post(EV_KEY, KEY_BACK, GEST_LONG, 0);       /* 长按 BACK 回主页 */
    CHECK(ui_page() == PG_HOME && keysim_get()->pressed == -1 && keysim_get()->activity == KS_IDLE);
}

static void power_cycle(void)
{
    sim_key(KEY_POWER, 1);                      /* 长按 2 s 关机 */
    run(2300);
    sim_key(KEY_POWER, 0);
    run(3000);
    CHECK(!power_running());
    sim_key(KEY_POWER, 1);                      /* 再开机 */
    run(1200);
    sim_key(KEY_POWER, 0);
    run(1500);
    CHECK(power_running() && storage_get()->state == ST_READY);
}

static void learn_positions(int ch, int count)
{
    tap("学习键值");
    tap("学习");
    for (int pos = 1; pos <= count; pos++) {
        env_cmd("ks_switch", ch, pos);
        run(900);
        env_cmd("ks_switch", ch, 0);
        run(300);
    }
    tap("保存");
    CHECK(ui_page() == PG_KEYCH && toast_has("正在写入 SD 卡"));
}

static void sd_persistence(void)
{
    home_entry_and_overview();
    tap("K2");
    learn_positions(1, 2);
    run(50);
    CHECK(keysim_get()->sd_saved[1] && keysim_get()->sd_error[1] == 0);
    CHECK(has("已存 SD 卡 APMKEY/K2.TXT"));
    back();
    CHECK(has("K2\",\"v\":\"已接通 · 学习 · K2 · 2 个键值 · 已存 SD"));

    /* 断电重启：开机挂载 SD 后自动读回 */
    power_cycle();
    run(100);
    CHECK(keysim_get()->sd_loaded);
    CHECK(keysim_get()->tables[1].valid && keysim_get()->tables[1].n == 2u && keysim_get()->sd_saved[1]);
    CHECK(!strcmp(keysim_get()->tables[1].keys[0].name, "上升") && udiff(keysim_get()->tables[1].keys[0].mv, 3485u) < 40u);
    CHECK(!keysim_get()->tables[0].valid && !keysim_get()->tables[2].valid);

    /* 拔掉 SD 卡时学习：只在内存，插卡后自动补存 */
    CHECK(ui_review_navigate(PG_KEYSIM) == 0);
    env_cmd("sd_remove", 0, 0);
    run(300);
    tap("K3");
    learn_positions(2, 3);
    run(100);
    CHECK(!keysim_get()->sd_saved[2] && keysim_get()->sd_error[2] == KS_IO_ERR_UNAVAILABLE);
    CHECK(has("只在内存：SD 卡不可用，插卡后自动保存"));
    env_cmd("sd_insert", 0, 0);
    run(1500);
    CHECK(storage_get()->state == ST_READY && keysim_get()->sd_saved[2]);
    CHECK(keysim_get()->tables[1].valid && keysim_get()->sd_saved[1]);   /* 读回时 K2 仍在 */

    /* 写保护：保存失败，界面可以重试 */
    env_cmd("sd_wp", 1, 0);
    learn_positions(2, 1);
    run(100);
    CHECK(!keysim_get()->sd_saved[2] && keysim_get()->sd_error[2] == KS_IO_ERR_IO);
    CHECK(has("写入 SD 卡失败 · OK 重试"));
    env_cmd("sd_wp", 0, 0);
    tap("键值表");
    CHECK(toast_has("正在重新写入 SD 卡"));
    run(50);
    CHECK(keysim_get()->sd_saved[2] && keysim_get()->tables[2].n == 1u);
    tap("键值表");
    CHECK(toast_has("已经在 SD 卡上"));

    /* 换一个空白模拟器（相当于换了张空卡）：什么都读不到 */
    boot();
    run(100);
    CHECK(keysim_get()->sd_loaded && !keysim_get()->tables[1].valid && !keysim_get()->tables[2].valid);
}

int main(void)
{
    home_entry_and_overview();
    learn_then_press();
    scan_on_k3();
    scan_without_state_source();
    overvoltage();
    leaves_release();
    sd_persistence();
    printf("Key simulation UI: %u checks passed (simulated APM, not hardware)\n", checks);
    return 0;
}
