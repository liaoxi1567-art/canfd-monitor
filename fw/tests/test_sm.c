/* 状态机场景测试：用模拟板子按真实时序操作整机，检查需求里的规则。
 * 运行：make test（带 AddressSanitizer / UBSan） */
#include "app/ui/core/ui_sm.h"
#include "services/can_core/bittime.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/sys/led.h"
#include "services/tx_sched/dynfield.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/sim_api.h"

#include <stdio.h>
#include <string.h>
#include "services/keysim/keysim.h"

static int checks, fails;
#define CHECK(c)                                                                 \
    do {                                                                         \
        checks++;                                                                \
        if (!(c)) {                                                              \
            fails++;                                                             \
            printf("    FAIL %s:%d  %s\n", __FILE__, __LINE__, #c);              \
        }                                                                        \
    } while (0)

enum { UP, DOWN, LEFT, RIGHT, OK, BACK, REC, POWER };

static void run(int ms)
{
    for (int t = 0; t < ms; t += 10)
        sim_tick(10);
}

static void hold(int k, int ms) { sim_key(k, 1); run(ms); sim_key(k, 0); run(20); }
static void tap(int k) { hold(k, 60); }
static bool toast_has(const char *s) { return strstr(ui_last_toast(), s) != NULL; }
static void post(ev_type_t t, int ch, int a, int b) { evt_post4(t, (uint8_t)ch, SRC_UI, a, b); run(20); }

static void boot(void)
{
    sim_reset();
    hold(POWER, 1200);
    run(500);                         /* 挂载 SD */
}

/* Original logical destinations now use the home tiles and send submenu.
 * Home is 3 columns with six tiles, 4 columns once key simulation adds a seventh. */
static void open_page(int n)
{
    static const int tile_for_destination[]={0,1,0,1,2,3,4,5};
    const int columns = keysim_available() ? 4 : 3;
    n=tile_for_destination[n];
    hold(BACK, 1100);                 /* 长按回主页 */
    for (int i = 0; i < 8; i++)
        tap(LEFT);
    for (int r = 0; r < n / columns; r++)
        tap(DOWN);
    for (int i = 0; i < n % columns; i++)
        tap(RIGHT);
    tap(OK);
}

static void cursor_to(int idx)
{
    for (int i = 0; i < 30; i++)
        tap(UP);
    for (int i = 0; i < idx; i++)
        tap(DOWN);
}

/* ---------------- 开关机 ---------------- */
static void t_boot_misfire(void)
{
    sim_reset();
    hold(POWER, 500);
    CHECK(power_get()->state == PWR_OFF);
    CHECK(power_get()->reason == SHUT_MISFIRE);
}

static void t_boot_normal(void)
{
    boot();
    CHECK(power_get()->state == PWR_RUNNING);
    CHECK(power_get()->screen_on);
    CHECK(chan_get(0)->state == CH_LISTEN && chan_get(1)->state == CH_LISTEN);
    CHECK(ui_page() == PG_HOME);
    CHECK(storage_get()->state == ST_READY);
}

static void t_boot_always_listen(void)
{
    boot();
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 1);
    CHECK(chan_get(0)->state == CH_NORMAL);
    hold(POWER, 2200);
    run(500);
    CHECK(power_get()->state == PWR_OFF);
    hold(POWER, 1200);
    run(200);
    CHECK(chan_get(0)->state == CH_LISTEN);    /* 每次开机强制只听（F8.6） */
}

static void t_shutdown_waits_release(void)
{
    boot();
    sim_key(POWER, 1);
    run(2100);
    CHECK(power_get()->state == PWR_SHUTDOWN);
    run(1000);
    CHECK(power_get()->state == PWR_SHUTDOWN); /* 按着 POWER 不能断电 */
    sim_key(POWER, 0);
    run(200);
    CHECK(power_get()->state == PWR_OFF);
}

static void t_boot_combos(void)
{
    sim_reset();
    sim_key(OK, 1);
    sim_key(BACK, 1);
    hold(POWER, 1200);
    sim_key(OK, 0);
    sim_key(BACK, 0);
    run(100);
    CHECK(ui_page() == PG_ENG);

    sim_reset();
    sim_key(BACK, 1);
    sim_key(REC, 1);
    sim_key(POWER, 1);
    run(3200);
    CHECK(power_get()->state == PWR_BOOTLOADER);
    sim_key(BACK, 0);
    sim_key(REC, 0);
    sim_key(POWER, 0);
    run(100);
    hold(POWER, 2200);
    CHECK(power_get()->state == PWR_OFF);
}

static void t_chip_rev_y(void)
{
    sim_reset();
    sim_env("chip", 'Y', 0);
    hold(POWER, 1200);
    run(100);
    CHECK(toast_has("rev Y") || strstr(ui_last_toast(), "rev Y") != NULL);
}

/* ---------------- 屏幕和按键 ---------------- */
static void t_wake_consumes_key(void)
{
    boot();
    tap(POWER);
    CHECK(!power_get()->screen_on);
    tap(OK);
    CHECK(power_get()->screen_on);
    CHECK(ui_page() == PG_HOME);               /* 熄屏时第一下只唤醒 */
    tap(OK);
    CHECK(ui_page() == PG_LIVE);
}

static void t_key_lock(void)
{
    boot();
    sim_key(LEFT, 1);
    sim_key(RIGHT, 1);
    run(2100);
    sim_key(LEFT, 0);
    sim_key(RIGHT, 0);
    run(20);
    CHECK(power_get()->locked);
    tap(OK);
    CHECK(ui_page() == PG_HOME);
    CHECK(toast_has("锁定"));
    tap(POWER);
    CHECK(!power_get()->screen_on);
    tap(OK);
    CHECK(!power_get()->screen_on);            /* 锁定时只有 POWER 能唤醒 */
    tap(POWER);
    CHECK(power_get()->screen_on);
    sim_key(LEFT, 1);
    sim_key(RIGHT, 1);
    run(2100);
    sim_key(LEFT, 0);
    sim_key(RIGHT, 0);
    run(20);
    CHECK(!power_get()->locked);
}

static void t_auto_off(void)
{
    boot();
    cfg_mut()->auto_off_min = 1;
    cfg_mut()->screen_off_s = 15;
    power_activity();
    run(16000);
    CHECK(!power_get()->screen_on);            /* 自动熄屏 */
    run(45000);
    CHECK(power_get()->countdown_s > 0);       /* 倒计时开始，屏幕点亮 */
    CHECK(power_get()->screen_on);
    tap(OK);
    CHECK(power_get()->countdown_s == 0);      /* 任意键取消 */
    sim_env("ext", 1, 0);
    run(61000 + 31000);
    CHECK(power_get()->state == PWR_RUNNING);  /* 接外部电源不自动关机 */
    sim_env("ext", 0, 0);
    power_activity();
    run(61000 + 31000);
    CHECK(power_get()->state == PWR_OFF && power_get()->reason == SHUT_AUTO_OFF);
}

static void t_low_batt(void)
{
    boot();
    tap(REC);
    run(300);
    CHECK(rec_get()->state == REC_RUNNING);
    sim_env("batt", 3350, 0);
    run(50);
    CHECK(toast_has("电量低"));
    sim_env("batt", 3250, 0);
    run(50);
    CHECK(power_get()->state == PWR_SHUTDOWN);
    run(1000);
    CHECK(power_get()->state == PWR_OFF && power_get()->reason == SHUT_LOW_BATT);
    CHECK(rec_get()->last_result == MSG_REC_STOPPED);   /* 先关好文件再断电 */
}

/* ---------------- 录制 ---------------- */
static void t_rec_no_sd(void)
{
    boot();
    sim_env("sd_remove", 0, 0);
    run(50);
    tap(REC);
    CHECK(rec_get()->state == REC_IDLE);
    CHECK(toast_has("未插"));
    CHECK(strcmp(led_rec_pattern(), "快闪") == 0);
}

static void t_rec_flow(void)
{
    boot();
    tap(REC);
    run(200);
    CHECK(rec_get()->state == REC_RUNNING);
    run(1000);
    CHECK(rec_get()->frames > 100);
    post(EV_REQ_REC_MARK,0,0,0);                /* Marking remains an explicit service action. */
    CHECK(rec_get()->markers == 1);
    tap(REC);                                 /* A second REC press now stops. */
    run(300);
    CHECK(rec_get()->state == REC_IDLE);
    CHECK(toast_has("录制已停止"));
}

static void t_rec_sd_removed(void)
{
    boot();
    tap(REC);
    run(300);
    sim_env("sd_remove", 0, 0);
    run(50);
    CHECK(rec_get()->state == REC_IDLE && rec_get()->last_result == MSG_REC_SD_REMOVED);
    sim_env("sd_insert", 0, 0);
    run(500);
    CHECK(storage_get()->state == ST_READY);
    CHECK(toast_has("已修复"));
}

static void t_rec_cfg_change_segment(void)
{
    boot();
    tap(REC);
    run(300);
    uint32_t seg = rec_get()->seg;
    post(EV_REQ_CH_RATE, 0, 250000, 0);
    CHECK(rec_get()->seg == seg + 1);          /* 录制中改速率：切新分段（F3.12） */
    CHECK(toast_has("新分段"));
    post(EV_REQ_AUTOBAUD, 0, CAN_MODE_LISTEN, 0);
    CHECK(chan_get(0)->state != CH_AUTOBAUD);  /* 录制中禁止自动识别 */
}

static void t_rec_sd_full(void)
{
    boot();
    tap(REC);
    run(300);
    sim_env("sd_fill", 0, 0);
    run(500);
    CHECK(rec_get()->state == REC_IDLE && rec_get()->last_result == MSG_REC_SD_FULL);
}

static void t_rec_write_errors(void)
{
    boot();
    tap(REC);
    run(300);
    sim_env("sd_werr", 1, 0);
    run(200);
    CHECK(rec_get()->state == REC_RUNNING);   /* 偶发错误：重试后继续 */
    sim_env("sd_werr", 2, 0);
    run(1500);
    CHECK(rec_get()->state == REC_IDLE && rec_get()->last_result == MSG_REC_WRITE_FAIL);
}

static void t_rec_frag(void)
{
    boot();
    sim_env("sd_frag", 1, 0);
    tap(REC);
    run(300);
    CHECK(rec_get()->state == REC_IDLE);
    CHECK(toast_has("碎片"));
}

/* ---------------- 发送 ---------------- */
static void t_tx_needs_normal(void)
{
    boot();
    open_page(1);
    CHECK(ui_page() == PG_TX);
    tap(OK);                                   /* 发送 CAN1 第 0 条 */
    CHECK(ui_dialog_ask() == ASK_SCOPE_START);
    tap(OK);
    run(50);
    CHECK(chan_get(0)->state == CH_NORMAL);
    CHECK(tx_get()->e[0][0].sent == 1);
    CHECK(tx_get()->pending[0] == 0);          /* 有节点回 ACK，已发出 */
}

static void t_tx_first_send_confirm(void)
{
    boot();
    open_page(6);                              /* 通道设置 */
    tap(RIGHT);                                /* 只听 → 受限 */
    CHECK(chan_get(0)->state == CH_RESTRICTED);
    tap(RIGHT);                                /* 受限 → 正常：要确认 */
    CHECK(ui_dialog_ask() == ASK_CH_NORMAL);
    tap(OK);
    CHECK(chan_get(0)->state == CH_NORMAL && chan_get(0)->first_tx_pending);
    open_page(1);
    tap(OK);
    CHECK(ui_dialog_ask() == ASK_SCOPE_START); /* Scope still confirms first-TX permission. */
    tap(OK);
    CHECK(tx_get()->e[0][0].sent == 1);
    tap(OK);
    CHECK(tx_get()->e[0][0].sent == 2);        /* 之后不再问 */
}

static void t_tx_noack(void)
{
    boot();
    sim_env("bus_nodes", 0, 0);
    open_page(1);
    tap(RIGHT);                                /* 启动 CAN1 第 0 条周期发送 */
    CHECK(ui_dialog_ask() == ASK_CYC_NORMAL);
    tap(OK);
    run(20);
    CHECK(tx_get()->e[0][0].active);
    run(400);
    CHECK(!tx_get()->e[0][0].active);          /* 无应答：暂停 */
    CHECK(tx_get()->noack_count[0] >= 1);
    CHECK(toast_has("无应答"));
}

static void t_tx_low_rate(void)
{
    boot();
    post(EV_REQ_CH_RATE, 0, 20000, 0);
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 1);
    CHECK(chan_get(0)->state == CH_LISTEN);
    CHECK(toast_has("50k"));
}

static void t_tx_cyclic_dynfield(void)
{
    boot();
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    post(EV_REQ_TX_CYC, 0, 0, 1);
    run(1000);
    const tx_entry_t *e = &tx_get()->e[0][0];
    CHECK(e->active);
    CHECK(e->sent >= 9 && e->sent <= 11);      /* 100 ms 周期，1 s 约 10 帧 */
    CHECK(e->dyn.cnt_val == e->sent % 16);     /* 计数器 0..15 循环 */
    post(EV_REQ_TX_STOP_ALL, 0, 0, 0);
    CHECK(!tx_any_active());
}

/* ---------------- 通道 ---------------- */
static void t_autobaud(void)
{
    boot();
    sim_env("bus_nbr", 0, 250000);
    sim_env("bus_fd", 0, 0);
    post(EV_REQ_AUTOBAUD, 0, CAN_MODE_LISTEN, 0);
    CHECK(chan_get(0)->state == CH_AUTOBAUD);
    run(5000);
    CHECK(chan_get(0)->state == CH_LISTEN);
    CHECK(cfg_get()->nbr[0] == 250000 && !cfg_get()->fd[0]);
    CHECK(toast_has("识别成功"));

    sim_env("bus_nbr", 1, 1000000);
    sim_env("bus_dbr", 1, 5000000);
    sim_env("bus_fd", 1, 1);
    post(EV_REQ_AUTOBAUD, 1, CAN_MODE_LISTEN, 0);
    run(5000);
    CHECK(cfg_get()->nbr[1] == 1000000 && cfg_get()->dbr[1] == 5000000 && cfg_get()->fd[1]);
}

static void t_autobaud_single_node(void)
{
    boot();
    sim_env("bus_nodes", 0, 1);
    sim_env("bus_nbr", 0, 125000);
    sim_env("bus_fd", 0, 0);
    post(EV_REQ_AUTOBAUD, 0, CAN_MODE_LISTEN, 0);
    run(4000);
    CHECK(ui_dialog_ask() == ASK_AB_RESTRICTED);   /* 只有一个节点：建议用受限模式 */
    tap(OK);
    run(4000);
    CHECK(cfg_get()->nbr[0] == 125000);
    CHECK(chan_get(0)->state == CH_LISTEN);
}

static void t_autobaud_no_traffic(void)
{
    boot();
    sim_env("bus_fps", 0, 0);
    post(EV_REQ_AUTOBAUD, 0, CAN_MODE_LISTEN, 0);
    run(4000);
    CHECK(toast_has("没有流量"));
}

static void t_busoff(void)
{
    boot();
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    sim_env("bus_err", 0, 1000);
    run(600);
    CHECK(chan_get(0)->state == CH_BUSOFF);
    CHECK(strstr(led_stat_pattern(), "双闪") != NULL);
    sim_env("bus_err", 0, 0);
    post(EV_REQ_CH_RECOVER, 0, 0, 0);
    CHECK(chan_get(0)->state == CH_NORMAL);
    cfg_mut()->busoff_auto = true;
    sim_env("bus_err", 0, 600);
    run(500);
    sim_env("bus_err", 0, 0);
    run(500);
    CHECK(chan_get(0)->state == CH_NORMAL);    /* 自动恢复 */
    CHECK(chan_get(0)->busoff_count == 2);
}

static void t_bus_abnormal_hint(void)
{
    boot();
    sim_env("bus_nbr", 0, 250000);             /* 总线实际 250k，设备设的是 500k */
    run(2500);
    CHECK(toast_has("错误很多"));
}

/* ---------------- SD / U 盘 / 上位机 ---------------- */
static void t_msc(void)
{
    boot();
    sim_env("usb", 1, 0);
    run(20);
    tap(REC);
    run(300);
    post(EV_REQ_MSC, 0, 1, 0);
    CHECK(storage_get()->state == ST_READY);   /* 录制中拒绝 */
    CHECK(toast_has("不能进入 U 盘"));
    hold(REC, 2100);
    run(300);
    tap(BACK);                               /* Keep the completed file's default name. */
    open_page(5);
    cursor_to(port_sd_file_count());           /* File list first; USB action follows it. */
    tap(OK);
    CHECK(ui_dialog_ask() == ASK_MSC);
    tap(OK);
    CHECK(storage_get()->state == ST_MSC);
    tap(REC);
    CHECK(rec_get()->state == REC_IDLE);       /* U 盘模式下不能录制 */
    sim_env("eject", 0, 0);
    run(500);
    CHECK(storage_get()->state == ST_READY);
}

static void t_host(void)
{
    boot();
    sim_env("usb", 1, 0);
    sim_env("host", 1, 0);
    run(20);
    CHECK(power_get()->host_connected);
    tap(REC);
    run(300);
    sim_host("rec_stop", 0, 0);
    run(20);
    CHECK(ui_dialog_ask() == ASK_REC_STOP);    /* 上位机停止录制要设备确认 */
    tap(BACK);
    CHECK(rec_get()->state == REC_RUNNING);
    CHECK(toast_has("拒绝"));
    sim_host("rec_stop", 0, 0);
    run(20);
    tap(OK);
    run(300);
    CHECK(rec_get()->state == REC_IDLE);

    sim_host("ch_normal", 1, 0);
    run(20);
    CHECK(ui_dialog_ask() == ASK_CH_NORMAL);
    tap(OK);
    CHECK(chan_get(1)->state == CH_NORMAL);

    open_page(6);                              /* 通道设置，光标在 CAN1 */
    sim_host("ch_rate", 0, 250000);
    run(20);
    CHECK(toast_has("上位机修改"));
}

/* ---------------- 回放 ---------------- */
static void t_play(void)
{
    boot();
    CHECK(!ui_play_select(0,0)&&ui_plan_open(3,0)==UI_REVIEW_RANGE);
    /* Player is retained only as an internal service; its UI was removed. */
    post(EV_REQ_PLAY_OPEN,0,0,0);
    CHECK(ui_dialog_ask() == ASK_PLAY_NORMAL);
    tap(OK);
    run(1000);
    CHECK(play_get(0)->state == PL_PLAYING);
    CHECK(play_get(0)->frames_sent > 10);
    post(EV_REQ_PLAY_CTRL,0,PLAYCTL_PAUSE_TOGGLE,0);
    CHECK(play_get(0)->state == PL_PAUSED);
    uint32_t n = play_get(0)->frames_sent;
    post(EV_REQ_PLAY_CTRL,0,PLAYCTL_STEP,0);
    CHECK(play_get(0)->frames_sent == n + 1);  /* 单步 */
    post(EV_REQ_PLAY_CTRL,0,PLAYCTL_STOP,0);
    CHECK(play_get(0)->state == PL_IDLE);

    post(EV_REQ_PLAY_OPEN,0,1,0);              /* Internal ASC sample, 3 channels. */
    CHECK(ui_dialog_ask() == ASK_PLAY_CHMAP);
    tap(OK);
    CHECK(ui_dialog_ask() == ASK_PLAY_RATE);
    tap(OK);                                   /* 套用文件参数 */
    run(50);
    CHECK(cfg_get()->nbr[0] == 250000);
    run(200);
    CHECK(play_get(0)->state == PL_PLAYING);   /* 通道已经是正常模式，不再问 */
    sim_env("sd_remove", 0, 0);
    run(50);
    CHECK(play_get(0)->state == PL_IDLE);

    sim_env("sd_insert", 0, 0);
    run(500);
    post(EV_REQ_PLAY_OPEN,0,4,0);              /* Internal unknown-format sample. */
    CHECK(play_get(0)->state == PL_IDLE);
    CHECK(toast_has("无法识别"));
}

/* ---------------- 显示 ---------------- */
static void t_live_pause(void)
{
    boot();
    open_page(0);
    run(500);
    CHECK(disp_rows_available() > 20);
    tap(UP);
    CHECK(disp_paused());
    uint32_t n = disp_rows_available();
    run(500);
    CHECK(disp_rows_available() == n);         /* 暂停：显示环冻结 */
    CHECK(disp_hidden_while_paused() > 0);
    tap(BACK);
    CHECK(!disp_paused());
    CHECK(ui_page() == PG_LIVE);               /* 暂停中 BACK 先恢复跟随 */
    hold(OK,1100);tap(OK);                     /* Live actions -> scroll / ID view. */
    run(300);
    CHECK(disp_id_count() >= 5);               /* 按 ID 视图 */
}

static void t_no_screen(void)
{
    sim_reset();
    sim_env("screen", 0, 0);
    hold(POWER, 1200);
    run(500);
    CHECK(power_get()->no_screen);
    tap(REC);
    run(300);
    CHECK(rec_get()->state == REC_RUNNING);    /* 无屏模式下 REC 照常工作 */
    CHECK(led_rec_on());
}

/* ---------------- 纯函数 ---------------- */
static void t_crc_and_bittime(void)
{
    const uint8_t s[] = "123456789";
    CHECK(crc8_j1850(s, 9) == 0x4B);
    CHECK(crc8_h2f(s, 9) == 0xDF);
    uint32_t ns = can_frame_time_ns(8, 0, 500000, 0);
    CHECK(ns > 220000 && ns < 240000);         /* 111 位 @500k ≈ 222 µs，+5% */
    uint32_t fd = can_frame_time_ns(64, CANF_FD | CANF_BRS, 1000000, 5000000);
    CHECK(fd > 120000 && fd < 160000);         /* 调研核算约 140 µs */
}

static void t_snapshot(void)
{
    static char buf[64 * 1024];
    boot();
    open_page(0);
    run(300);
    int n = sim_snapshot(buf, sizeof buf);
    CHECK(n > 100 && buf[0] == '{' && buf[n - 1] == '}');
}

/* 接管复核：覆盖原 34 个场景未触及的状态交界。 */
static void t_reset_clean(void)
{
    boot();
    sim_env("usb", 1, 0);
    run(20);
    CHECK(storage_get()->usb_attached);
    sim_reset();
    CHECK(app_now_ms() == 0);
    CHECK(!storage_get()->usb_attached);
}

static void t_tx_reconfigure_pending(void)
{
    boot();
    sim_env("bus_nodes", 0, 0);
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    post(EV_REQ_TX_SEND, 0, 0, 1);
    CHECK(tx_get()->pending[0] == 1);
    post(EV_REQ_CH_RATE, 0, 250000, 0);
    CHECK(tx_get()->pending[0] == 0);   /* 控制器重启已清 FIFO */
    run(150);
    CHECK(tx_get()->noack_count[0] == 0);
    evt_post4(EV_REQ_CH_RATE, 0, SRC_UI, 500000, 0);
    evt_post4(EV_REQ_TX_SEND, 0, SRC_UI, 0, 1);
    sim_tick(1);
    CHECK(tx_get()->pending[0] == 1); /* 延后的配置通知不能抹去新发送 */
}

static void t_tx_full_counter(void)
{
    boot();
    sim_env("bus_nodes", 0, 0);
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    for (int i = 0; i < TX_FIFO_LEN + 1; i++)
        evt_post4(EV_REQ_TX_SEND, 0, SRC_UI, 0, 1);
    sim_tick(1);
    CHECK(tx_get()->pending[0] == TX_FIFO_LEN);
    CHECK(tx_get()->e[0][0].sent == TX_FIFO_LEN);
    CHECK(tx_get()->e[0][0].dyn.cnt_val == TX_FIFO_LEN); /* 拒绝入队不消耗计数器 */
}

static void t_play_first_confirm(void)
{
    boot();
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 1);
    CHECK(chan_get(0)->first_tx_pending);
    post(EV_REQ_PLAY_OPEN, 0, 0, 0);
    CHECK(play_get(0)->state == PL_CHECKING);
    CHECK(ui_dialog_ask() == ASK_PLAY_NORMAL);
    CHECK(play_get(0)->frames_sent == 0);
    tap(BACK);
    CHECK(play_get(0)->state == PL_IDLE);
    CHECK(chan_get(0)->first_tx_pending);
    post(EV_REQ_PLAY_OPEN, 0, 0, 0);
    tap(OK);
    run(200);
    CHECK(play_get(0)->state == PL_PLAYING);
    CHECK(!chan_get(0)->first_tx_pending);
    CHECK(play_get(0)->frames_sent > 0);
}

static void t_play_fractional_tick(void)
{
    boot();
    post(EV_REQ_CH_MODE, 0, CAN_MODE_NORMAL, 2);
    cfg_mut()->play_speed_idx = 0;  /* 0.5×：两次 1 ms 等价于一次 2 ms */
    post(EV_REQ_PLAY_OPEN, 0, 0, 0);
    uint32_t before = play_get(0)->vt_ms;
    for (int i = 0; i < 100; i++)
        sim_tick(1);
    CHECK(play_get(0)->vt_ms - before == 50);
}

typedef struct { const char *name; void (*fn)(void); } test_t;
static const test_t tests[] = {
    { "开机误触", t_boot_misfire },
    { "正常开机", t_boot_normal },
    { "开机强制只听", t_boot_always_listen },
    { "关机等松开 POWER", t_shutdown_waits_release },
    { "开机组合键", t_boot_combos },
    { "rev Y 提示", t_chip_rev_y },
    { "熄屏唤醒不执行动作", t_wake_consumes_key },
    { "按键锁", t_key_lock },
    { "自动熄屏和自动关机", t_auto_off },
    { "低电保护", t_low_batt },
    { "没卡按 REC", t_rec_no_sd },
    { "录制流程", t_rec_flow },
    { "录制中拔卡", t_rec_sd_removed },
    { "录制中改参数切段", t_rec_cfg_change_segment },
    { "卡满停止", t_rec_sd_full },
    { "写错误重试和失败", t_rec_write_errors },
    { "卡碎片拒绝录制", t_rec_frag },
    { "发送要切正常模式", t_tx_needs_normal },
    { "首次发送确认", t_tx_first_send_confirm },
    { "无应答暂停", t_tx_noack },
    { "低速率禁止发送", t_tx_low_rate },
    { "周期发送和动态字段", t_tx_cyclic_dynfield },
    { "自动识别", t_autobaud },
    { "单节点自动识别", t_autobaud_single_node },
    { "无流量自动识别", t_autobaud_no_traffic },
    { "Bus-Off 恢复", t_busoff },
    { "错误多无有效帧提示", t_bus_abnormal_hint },
    { "U 盘模式", t_msc },
    { "上位机请求", t_host },
    { "文件回放", t_play },
    { "实时显示暂停", t_live_pause },
    { "无屏模式", t_no_screen },
    { "CRC 和帧时长", t_crc_and_bittime },
    { "快照", t_snapshot },
    { "重置时钟与 USB 状态", t_reset_clean },
    { "通道重配置清发送计数", t_tx_reconfigure_pending },
    { "FIFO 满时保留动态计数", t_tx_full_counter },
    { "回放首次发送确认", t_play_first_confirm },
    { "半速回放小节拍", t_play_fractional_tick },
};

int main(void)
{
    int n = (int)(sizeof tests / sizeof tests[0]);
    for (int i = 0; i < n; i++) {
        int before = fails;
        tests[i].fn();
        printf("%s %s\n", fails == before ? "  ok  " : "  FAIL", tests[i].name);
    }
    printf("\n%d 个场景，%d 项检查，%d 项失败\n", n, checks, fails);
    return fails ? 1 : 0;
}
