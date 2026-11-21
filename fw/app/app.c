#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/bench/bench.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/input/keys.h"
#include "services/keysim/keysim.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/stats/stats.h"
#include "services/storage/storage_sm.h"
#include "services/sys/led.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/tx_scope.h"
#include "sim/send_plan.h"
#include "sim/tx_sequence.h"
#include "sim/local_files_ui.h"

#include <stdio.h>
#ifdef REVD_FREERTOS
#include "platform/stm32h743_revd/rtos/bridges.h"
#endif

#define DRAIN_GUARD 2000   /* 一次最多处理这么多事件，防止事件互相触发死循环 */
static bool realtime_advanced;

static void dispatch(const app_evt_t *e)
{
#ifdef REVD_FREERTOS
    if (e->type == EV_NTF_FILE_REMOVED) play_file_removed(e->a);
#endif
    power_handle(e);
    storage_handle(e);
    for (int ch = 0; ch < CH_NUM; ch++)
        chan_handle(ch, e);
    if (e->type == EV_SYS_STOP_ALL) {
        /* Both hardware producers are stopped. Preserve their received tail
         * before recorder places its SD close fence. */
        port_can_flush_rx();
        app_capture_poll();
    }
    scope_handle(e);
    sp_handle(e);
    tx_handle(e);
    rec_handle(e);
    play_handle(e);
    stats_handle(e);
    led_handle(e);
    ui_handle(e);
#ifdef REVD_FREERTOS
    revd_ui_core_observe();
#endif
}

static void drain(void)
{
    app_evt_t e;
    for (int i = 0; i < DRAIN_GUARD && evt_pop(&e); i++)
        dispatch(&e);
}

/* 开机（OFF → BOOT_HOLD）时把各服务恢复到初始状态；配置（cfg）保留，相当于 config.ini */
static void boot_reset(void)
{
    realtime_advanced=false;
    for (int t = 0; t < TMR_NUM; t++)
        if (t != TMR_BOOT)
            tmr_stop((timer_id_t)t);
    ring_init();             /* 先清读者登记，下面各模块重新挂读者 */
    keys_reset();
    storage_init();
    chan_init();
    tx_init();
    txseq_init();
    scope_init();
    sp_reset_runtime();
    rec_init();
    play_init();
    stats_init();
    disp_init();
    led_init();
    ui_init();
    bench_init();
    keysim_init();
}

void app_init(void)
{
    evt_init();
    tmr_init();
    cfg_defaults();
    power_init();
    sp_init();
    boot_reset();
}

void app_post(const app_evt_t *e)
{
    evt_post(e);
}

void app_key_raw(key_id_t k, bool down)
{
    pwr_state_t before = power_get()->state;
    if (power_raw_key(k, down)) {
        if (before == PWR_OFF && power_get()->state == PWR_BOOT_HOLD)
            boot_reset();
        drain();
        return;
    }
    const power_t *p = power_get();
    bool consume = false;
    if (down) {
        if (!p->screen_on) {
            if (p->locked && k != KEY_POWER)
                return;              /* 锁定时只有 POWER 能唤醒 */
            power_wake();
            consume = true;          /* 熄屏时第一下只唤醒，不执行动作 */
        }
        power_activity();
    }
    keys_raw(k, down, consume);
    drain();
}

void app_touch(ev_type_t type, int32_t a)
{
    const power_t *p = power_get();
    if (p->state != PWR_RUNNING)
        return;
    if (!p->screen_on) {
        if (!p->locked)
            power_wake();            /* 第一下触摸只唤醒 */
        return;
    }
    power_activity();
    evt_post4(type, 0, SRC_UI, a, 0);
    drain();
}

void app_capture_poll(void)
{
    /* Safe during a FatFs diskio wait: these consumers only read the frame
     * ring and enqueue SD records. Do not dispatch commands, timers or LVGL. */
    pwr_state_t ps = power_get()->state;
    if (ps == PWR_RUNNING || ps == PWR_SHUTDOWN) {
        stats_tick();
        disp_tick();
        rec_tick();
    }
}

void app_tx_complete(int ch)
{
    if(ch<0||ch>=CH_NUM)return;
    const app_evt_t event={.type=EV_CAN_TX_DONE,.ch=(uint8_t)ch,.src=SRC_SYS};
    tx_handle(&event);
}

void app_realtime_poll(uint32_t ms)
{
    app_clock_advance(ms);
    if(ms)realtime_advanced=true;
    app_capture_poll();
    if(power_running()){
        tx_realtime_tick();
        /* Service only already-running groups. Activating ARMING here could
         * bypass normal command/confirmation ordering while FatFs owns its job. */
        txseq_realtime_tick();
        tx_tick();
    }
}

void app_tick(uint32_t ms)
{
    app_clock_advance(ms);
    tmr_tick();
    drain();
    pwr_state_t ps = power_get()->state;
    if (ps == PWR_RUNNING)
        keys_tick();
    bench_tick();
    if (ps == PWR_RUNNING || ps == PWR_SHUTDOWN) {
        stats_tick();
        disp_tick();
        rec_tick();
        tx_tick();
        play_tick(ms);
    }
    if (ps == PWR_RUNNING)
        keysim_tick(ms);
    else
        keysim_leave();   /* 关机或未运行：接口保持高阻 */
    led_tick();
    drain();
    /* The device and simulator advance the same owners exactly once here.
     * These CPU-only steps must never be called from the SD wait pump. */
    if(ms||realtime_advanced){
        /* SD waits can consume the complete HAL interval before this normal
         * foreground turn. Service owners once even when its remaining dt=0. */
        realtime_advanced=false;
        local_files_tick();
        sp_tick();
        scope_tick();
        txseq_tick();
        sp_tick();
    }
}

/* ---------- 快照 ---------- */
static void sm_entry(jw_t *j, const char *name, const char *(*fn)(int), int n, int cur)
{
    jw_obj(j);
    jw_kstr(j, "n", name);
    jw_key(j, "s");
    jw_arr(j);
    for (int i = 0; i < n; i++)
        jw_str(j, fn(i));
    jw_end_arr(j);
    jw_kint(j, "c", cur);
    jw_end_obj(j);
}

static const char *pw_name(int i) { return power_state_name((pwr_state_t)i); }
static const char *st_name(int i) { return storage_state_name((storage_state_t)i); }
static const char *ch_name(int i) { return chan_state_name((ch_state_t)i); }
static const char *rc_name(int i) { return rec_state_name((rec_state_t)i); }
static const char *pl_name(int i) { return play_state_name((play_state_t)i); }

static void rate_str(char *o, size_t n, int ch)
{
    const cfg_t *c = cfg_get();
    unsigned nb = (unsigned)c->nbr[ch], db = (unsigned)c->dbr[ch];
    if (c->fd[ch])
        snprintf(o, n, "%u%s/%u%s", nb >= 1000000 ? nb / 1000000 : nb / 1000, nb >= 1000000 ? "M" : "k",
                 db >= 1000000 ? db / 1000000 : db / 1000, db >= 1000000 ? "M" : "k");
    else
        snprintf(o, n, "%u%s", nb >= 1000000 ? nb / 1000000 : nb / 1000, nb >= 1000000 ? "M" : "k");
}

void app_json(jw_t *j)
{
    const power_t *p = power_get();
    jw_obj(j);
    jw_kint(j, "now", app_now_ms());

    jw_key(j, "power");
    jw_obj(j);
    jw_kstr(j, "state", power_state_name(p->state));
    jw_kint(j, "sid", p->state);
    jw_kbool(j, "screen", p->screen_on);
    jw_kbool(j, "locked", p->locked);
    jw_kint(j, "countdown", p->countdown_s);
    jw_kint(j, "batt", p->batt_mv);
    jw_kbool(j, "ext", p->ext_power);
    jw_kbool(j, "host", p->host_connected);
    jw_kbool(j, "no_screen", p->no_screen);
    jw_kbool(j, "armed", keys_power_armed());
    jw_end_obj(j);

    jw_key(j, "storage");
    jw_obj(j);
    jw_kstr(j, "state", storage_state_name(storage_get()->state));
    jw_kbool(j, "usb", storage_get()->usb_attached);
    jw_end_obj(j);

    jw_key(j, "ch");
    jw_arr(j);
    for (int ch = 0; ch < CH_NUM; ch++) {
        const chan_t *c = chan_get(ch);
        const ch_stats_t *s = stats_get(ch);
        char r[32];
        rate_str(r, sizeof r, ch);
        jw_obj(j);
        jw_kstr(j, "state", chan_state_name(c->state));
        jw_kint(j, "sid", c->state);
        jw_kint(j, "mode", c->mode);
        jw_kstr(j, "rate", r);
        jw_kint(j, "tec", c->tec);
        jw_kint(j, "rec", c->rec);
        jw_kint(j, "errst", c->errst);
        jw_kint(j, "load", s->load_permille);
        jw_kint(j, "fps", s->fps);
        jw_kint(j, "ab", chan_ab_progress(ch));
        jw_kbool(j, "first_tx", c->first_tx_pending);
        jw_kint(j, "tx_pending", tx_get()->pending[ch]);
        jw_end_obj(j);
    }
    jw_end_arr(j);

    const rec_t *r = rec_get();
    jw_key(j, "rec");
    jw_obj(j);
    jw_kstr(j, "state", rec_state_name(r->state));
    jw_kint(j, "sid", r->state);
    jw_kint(j, "session", r->session);
    jw_kint(j, "elapsed", r->elapsed_ms);
    jw_kint(j, "frames", r->frames);
    jw_kint(j, "bytes", r->bytes);
    jw_kint(j, "drops", r->drops);
    jw_kint(j, "markers", r->markers);
    jw_kint(j, "seg", r->seg);
    jw_end_obj(j);

    jw_kbool(j, "tx_active", tx_any_active());
    jw_kbool(j, "play_active", play_any_active());

    jw_key(j, "led");
    jw_obj(j);
    jw_kbool(j, "stat", led_stat_on());
    jw_kbool(j, "rec", led_rec_on());
    jw_kstr(j, "stat_pat", led_stat_pattern());
    jw_kstr(j, "rec_pat", led_rec_pattern());
    jw_end_obj(j);

    jw_key(j, "sm");
    jw_arr(j);
    sm_entry(j, "电源", pw_name, PWR_STATE_NUM, p->state);
    sm_entry(j, "SD 卡", st_name, ST_STATE_NUM, storage_get()->state);
    sm_entry(j, "CAN1", ch_name, CH_STATE_NUM, chan_get(0)->state);
    sm_entry(j, "CAN2", ch_name, CH_STATE_NUM, chan_get(1)->state);
    sm_entry(j, "录制", rc_name, REC_STATE_NUM, r->state);
    sm_entry(j, "回放 CAN1", pl_name, PL_STATE_NUM, play_get(0)->state);
    sm_entry(j, "回放 CAN2", pl_name, PL_STATE_NUM, play_get(1)->state);
    jw_end_arr(j);

    ui_json(j);
    jw_end_obj(j);
}
