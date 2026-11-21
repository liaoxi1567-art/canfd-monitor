#include "sim/env_sim.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/frame_ring/frame_ring.h"
#include "services/logfmt/candump.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/storage/storage_sm.h"
#include "services/keysim/keysim.h"
#include "services/keysim/keysim_file.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#define MAX_FILES     24
#define PROBE_MS      300u
#define MOUNT_MS      300u
#define PREALLOC_MS   100u
#define CLOSE_MS      150u
#define FORMAT_MS     500u
#define SD_TOTAL_KB   (32u * 1024u * 1024u)   /* 32 GB */

typedef struct {
    /* 总线本身（被测网络） */
    bool       connected;
    uint32_t   nbr, dbr;
    bool       fd;
    uint8_t    nodes;        /* 总线上别的节点数：0 = 空总线，1 = 只有一个发送节点，2 = 正常网络 */
    uint16_t   fps;
    uint32_t   err_burst_ms;
    /* 本机控制器 */
    bool       started, start_fail;
    uint32_t   generation;
    can_mode_t mode;
    uint32_t   c_nbr, c_dbr;
    bool       c_fd;
    uint16_t   tec, rec;
    bool       busoff;
    uint16_t   s_tec, s_rec;
    bool       s_busoff;
    can_frame_t txq[8];
    int        txn;
    bool       probing;
    bool       probe_ready;
    app_evt_t  probe_event;
    uint32_t   p_nbr, p_dbr;
    bool       p_data;
    can_mode_t p_mode;
    uint32_t   p_due;
    uint32_t   acc_milli;    /* 帧生成累加器（千分之一帧） */
    uint32_t   seq;
} bus_t;

static struct {
    bool     powered, v5, chg_hi, msc;
    uint8_t  backlight;
    bool     key[KEY_NUM];
    uint16_t batt_mv;
    bool     ext;
    bool     sd_present, sd_wp, sd_unsupported, sd_frag, sd_busy, sd_dirty, full_posted;
    uint8_t  sd_werr;        /* 0 无，1 出一次错，2 持续出错（坏卡） */
    uint32_t werr_last;
    uint64_t sd_used;
    uint32_t mount_due, prealloc_due, close_due, format_due;
    sd_result_t prealloc_res;
    file_info_t files[MAX_FILES];
    uint32_t file_token[MAX_FILES],file_session[MAX_FILES],token_next;
    int8_t   file_slot[MAX_FILES];
    bool     file_fixture[MAX_FILES];
    uint64_t first_record_us[MAX_FILES], last_record_us[MAX_FILES];
    int      nfiles;
    int      cur_session;    /* 正在录的会话在列表里的位置，-1 = 没有 */
    uint32_t session_max;
    bool     usb, host;
    char     chip;
    bool     screen, rtc_valid;
    time_t   rtc_epoch;
    uint64_t rtc_elapsed_ms;
    char     rec_prefix[ENV_REC_PREFIX_MAX_BYTES + 1u];
    bus_t    bus[CH_NUM];
} E;

/* Host-only retained samples: ~200 KiB including atomic import staging, never
 * linked as the board SD implementation. Actual storage should stream blocks. */
typedef struct {
    uint32_t count;
    can_frame_t frames[ENV_LOG_MAX_FRAMES];
} log_sample_t;
static log_sample_t samples[ENV_LOG_MAX_FILES];
static log_sample_t import_stage;

static int free_sample_slot(void)
{
    for (unsigned slot = 0; slot < ENV_LOG_MAX_FILES; slot++) {
        bool used = false;
        for (int i = 0; i < E.nfiles; i++)
            if (E.file_slot[i] == (int)slot) used = true;
        if (!used) return (int)slot;
    }
    return -1;
}

/* ---------- 工具 ---------- */
static uint32_t now(void) { return app_now_ms(); }

static unsigned ascii_lower(unsigned c)
{
    return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c;
}

static bool same_name(const char *left, const char *right)
{
    while (*left && *right) {
        if (ascii_lower((unsigned char)*left++) != ascii_lower((unsigned char)*right++)) return false;
    }
    return *left == *right;
}

static bool name_exists(const char *name, int except)
{
    for (int i = 0; i < E.nfiles; ++i)
        if (i != except && same_name(name, E.files[i].name)) return true;
    return false;
}

/* Reject incomplete/overlong encodings rather than truncating a Chinese name
 * into invalid JSON or a different on-disk name. The ASCII subset follows FAT
 * naming rules, although this code never touches the physical filesystem. */
static int safe_name(const char *name, size_t maximum, bool allow_empty)
{
    if (!name) return ENV_LOG_ERR_NAME;
    size_t length = 0;
    while (length <= maximum && name[length]) ++length;
    if (length > maximum) return ENV_LOG_ERR_LIMIT;
    if (!length) return allow_empty ? ENV_LOG_OK : ENV_LOG_ERR_NAME;
    if (name[0] == '.' || name[0] == ' ' || name[length - 1u] == '.' || name[length - 1u] == ' ')
        return ENV_LOG_ERR_NAME;
    size_t at = 0;
    while (at < length) {
        unsigned first = (unsigned char)name[at++], point = first, minimum = 0;
        unsigned trailing = 0;
        if (first >= 0xC2 && first <= 0xDF) { trailing = 1; point = first & 0x1Fu; minimum = 0x80; }
        else if (first >= 0xE0 && first <= 0xEF) { trailing = 2; point = first & 0x0Fu; minimum = 0x800; }
        else if (first >= 0xF0 && first <= 0xF4) { trailing = 3; point = first & 7u; minimum = 0x10000; }
        else if (first >= 0x80) return ENV_LOG_ERR_NAME;
        if (at + trailing > length) return ENV_LOG_ERR_NAME;
        while (trailing--) {
            unsigned next = (unsigned char)name[at++];
            if (next < 0x80 || next > 0xBF) return ENV_LOG_ERR_NAME;
            point = (point << 6) | (next & 0x3Fu);
        }
        if (point < minimum || point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF) ||
            point < 0x20 || (point >= 0x7F && point <= 0x9F) ||
            (point < 0x80 && strchr("/\\:*?\"<>|", (int)point)) ||
            (point >= 0x200B && point <= 0x200F) || (point >= 0x2028 && point <= 0x202E) ||
            (point >= 0x2066 && point <= 0x2069) || point == 0xFEFF)
            return ENV_LOG_ERR_NAME;
    }
    char base[5] = {0};
    size_t base_len = 0;
    while (base_len < length && name[base_len] != '.') ++base_len;
    if (base_len <= 4) {
        for (size_t i = 0; i < base_len; ++i) base[i] = (char)ascii_lower((unsigned char)name[i]);
        if (!strcmp(base, "con") || !strcmp(base, "prn") || !strcmp(base, "aux") || !strcmp(base, "nul") ||
            (base_len == 4 && (!strncmp(base, "com", 3) || !strncmp(base, "lpt", 3)) && base[3] >= '1' && base[3] <= '9'))
            return ENV_LOG_ERR_NAME;
    }
    return ENV_LOG_OK;
}

static int sample_name(const char *name)
{
    int result = safe_name(name, ENV_FILE_NAME_MAX_BYTES, false);
    if (result != ENV_LOG_OK) return result;
    const char *dot = strrchr(name, '.');
    return dot && (same_name(dot, ".log") || same_name(dot, ".txt")) ? ENV_LOG_OK : ENV_LOG_ERR_EXTENSION;
}

static bool naming_writable(void)
{
    return E.powered && power_get()->state == PWR_RUNNING && storage_get()->state == ST_READY &&
        E.sd_present && !E.sd_wp && !E.sd_unsupported && !E.msc && E.cur_session < 0 &&
        !E.prealloc_due && !E.close_due && !E.format_due;
}

int env_rec_name_set(const char *prefix)
{
    int result = safe_name(prefix, ENV_REC_PREFIX_MAX_BYTES, true);
    if (result != ENV_LOG_OK) return result;
    if (!naming_writable()) return ENV_LOG_ERR_BUSY;
    memcpy(E.rec_prefix, prefix, strlen(prefix) + 1u);
    return ENV_LOG_OK;
}

const char *env_rec_name_get(void) { return E.rec_prefix; }

uint32_t env_file_token(int idx)
{
    return E.sd_present && idx >= 0 && idx < E.nfiles ? E.file_token[idx] : 0;
}

uint32_t env_record_file_token(uint32_t session)
{
    if(!session||!E.sd_present)return 0;
    uint32_t token=0;
    for(int i=0;i<E.nfiles;i++)if(E.file_session[i]==session&&E.files[i].is_session&&!E.file_fixture[i]){
        if(token)return 0;
        token=E.file_token[i];
    }
    return token;
}

int env_log_rename(int idx, uint32_t expected_token, const char *name)
{
    if (!expected_token || expected_token != env_file_token(idx)) return ENV_LOG_ERR_CHANGED;
    int result = sample_name(name);
    if (result != ENV_LOG_OK) return result;
    if (!naming_writable() || play_file_in_use(idx)) return ENV_LOG_ERR_BUSY;
    if (E.file_fixture[idx] || E.files[idx].protected_) return ENV_LOG_ERR_READONLY;
    if (name_exists(name, idx)) return ENV_LOG_ERR_EXISTS;
    memcpy(E.files[idx].name, name, strlen(name) + 1u);
    return ENV_LOG_OK;
}

static void post(ev_type_t t, uint8_t ch, int32_t a, int32_t b)
{
    if (E.powered)
        evt_post4(t, ch, SRC_SYS, a, b);
}

static void add_file(const char *name, file_kind_t k, uint32_t kb, uint32_t nbr, uint32_t dbr, bool fd,
                     uint8_t chans, uint32_t frames, uint32_t dur, bool session)
{
    if (E.nfiles >= MAX_FILES)
        return;
    E.file_slot[E.nfiles] = -1;
    E.file_token[E.nfiles] = ++E.token_next;
    E.file_session[E.nfiles] = 0;
    E.file_fixture[E.nfiles] = false;
    E.first_record_us[E.nfiles] = E.last_record_us[E.nfiles] = 0;
    file_info_t *f = &E.files[E.nfiles++];
    memset(f, 0, sizeof *f);
    snprintf(f->name, sizeof f->name, "%s", name);
    f->kind = k;
    f->size_kb = kb;
    f->nbr = nbr;
    f->dbr = dbr;
    f->fd = fd;
    f->channels = chans;
    f->frames = frames;
    f->duration_ms = dur;
    f->is_session = session;
}

static void preset_files(void)
{
    E.nfiles = 0;
    add_file("LOG/00000007_20261023-153000", FK_CFR, 2400, 500000, 2000000, true, 2, 2400, 60000, true);
    add_file("trace_250k.asc", FK_ASC, 180, 250000, 0, false, 3, 1500, 30000, false);
    add_file("candump_1M.log", FK_CANDUMP, 320, 1000000, 0, false, 1, 5000, 20000, false);
    add_file("tx_test.csv", FK_TXLIST, 2, 500000, 2000000, true, 1, 10, 5000, false);
    add_file("export.csv", FK_UNKNOWN, 12, 0, 0, false, 0, 0, 0, false);
    E.session_max = 7;
    E.sd_used = 0;
    for (int i = 0; i < E.nfiles; i++) {
        E.file_fixture[i] = true;
        E.sd_used += (uint64_t)E.files[i].size_kb * 1024u;
    }
}

static void bus_defaults(int ch)
{
    bus_t *b = &E.bus[ch];
    memset(b, 0, sizeof *b);
    b->connected = true;
    b->nodes = 2;
    if (ch == 0) {
        b->nbr = 500000;
        b->dbr = 2000000;
        b->fd = true;
        b->fps = 200;
    } else {
        b->nbr = 500000;
        b->dbr = 2000000;
        b->fd = true;
        b->fps = 60;
    }
}


/* ---------- 按键模拟：第 12 页电路 + 4 路仿真 APM ----------
 * 每一路：APM 内部 2.2 kΩ 上拉到 5 V（唤醒后才有电），接口侧 400 kΩ 回读负载，
 * 可选并着的原车开关（4 个档位），以及驱动事先不知道的串联电阻（PTC + 继电器）。
 * APM 的开关状态按演示门限判定，代替将来从 CAN 报文解码出来的信号。 */
static const float ks_bits[WINSIM_NUM_BITS] = {
    200.0f, 390.0f, 820.0f, 1600.0f, 3300.0f, 6200.0f,
    13000.0f, 24000.0f, 47000.0f, 91000.0f, 180000.0f, 360000.0f, 100.0f,
};
static const float ks_switch_ohm[5] = { 0.0f, 5100.0f, 2200.0f, 1000.0f, 330.0f };   /* 0 = 松开 */
static const float ks_series_ohm[WINSIM_NUM_CH] = { 12.4f, 9.8f, 15.1f, 11.2f };
static struct {
    bool     oe, sh;
    uint16_t mask;
    uint8_t  ch;
    bool     apm_awake;
    bool     attached[WINSIM_NUM_CH];
    uint8_t  pos[WINSIM_NUM_CH];
    bool     fault[WINSIM_NUM_CH];
} KS;

/* 模拟 SD 卡上的 /APMKEY/K1.TXT … K4.TXT。设备断电重启不丢，格式化或重置模拟器才清空。 */
static struct {
    char     text[WINSIM_NUM_CH][KS_FILE_MAX_BYTES];
    uint16_t len[WINSIM_NUM_CH];
} KSF;

static void ks_files_clear(void)
{
    memset(&KSF, 0, sizeof KSF);
}

/* 和板级存储所有者同样的约定：卡就绪且空闲时处理一个请求。 */
static void ks_storage_tick(void)
{
    ks_io_request_t r;
    if (!keysim_io_peek(&r))
        return;
    storage_state_t st = storage_get()->state;
    if (st == ST_MOUNTING || st == ST_FORMATTING)
        return;   /* 等一等 */
    if (!E.sd_present || E.msc || st != ST_READY) {
        keysim_io_complete(r.id, KS_IO_ERR_UNAVAILABLE);
        return;
    }
    if (r.kind == KS_IO_SAVE) {
        size_t len = 0u;
        if (E.sd_wp) {
            keysim_io_complete(r.id, KS_IO_ERR_IO);
            return;
        }
        int rc = ks_table_encode(&r.table, r.ch, KSF.text[r.ch], sizeof KSF.text[r.ch], &len);
        if (rc == KS_FILE_OK)
            KSF.len[r.ch] = (uint16_t)len;
        keysim_io_complete(r.id, rc);
    } else {
        for (uint8_t ch = 0; ch < WINSIM_NUM_CH; ch++) {
            ks_table_t t;
            if (KSF.len[ch] && ks_table_decode(KSF.text[ch], KSF.len[ch], &t) == KS_FILE_OK)
                keysim_table_loaded(ch, &t);
        }
        keysim_io_complete(r.id, 0);
    }
}

static void ks_reset(void)
{
    ks_files_clear();
    memset(&KS, 0, sizeof KS);
    KS.apm_awake = true;
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++)
        KS.attached[k] = true;
}

static float ks_port_mv(unsigned ch)
{
    if (KS.fault[ch])
        return 12000.0f;
    float vs = KS.apm_awake ? 5000.0f : 0.0f;
    float g = 1.0f / 400000.0f;
    if (KS.attached[ch] && KS.pos[ch])
        g += 1.0f / ks_switch_ohm[KS.pos[ch]];
    if (KS.oe && ch == KS.ch) {
        float g_in = KS.sh ? 1.0f / 0.05f : 0.0f;
        for (unsigned i = 0; i < WINSIM_NUM_BITS; i++)
            if (KS.mask & (1u << i))
                g_in += 1.0f / (ks_bits[i] + 5.0f);
        if (g_in > 0.0f)
            g += 1.0f / (ks_series_ohm[ch] + 1.0f / g_in);
    }
    float rp = 1.0f / g;
    return vs * rp / (rp + 2200.0f);
}

static void     ks_shift(uint16_t mask) { KS.mask = mask; }
static void     ks_oe(bool en) { KS.oe = en; }
static void     ks_short(bool en) { KS.sh = en; }
static void     ks_delay(uint32_t ms) { (void)ms; }   /* 物理模型没有时间常数 */
static void     ks_select(uint8_t ch) { KS.ch = (uint8_t)(ch % WINSIM_NUM_CH); }
static uint16_t ks_adc(void)
{
    if (!KS.oe)
        return 0u;
    float v = ks_port_mv(KS.ch);
    if (v > 13200.0f)
        v = 13200.0f;
    return (uint16_t)(v + 0.5f);
}

bool port_keysim_hal(winsim_hal_t *out)
{
    out->shift_write = ks_shift;
    out->set_oe = ks_oe;
    out->set_short_0r = ks_short;
    out->adc_read_mv = ks_adc;
    out->delay_ms = ks_delay;
    out->select_channel = ks_select;
    return true;
}

int port_keysim_apm_state(uint8_t ch)
{
    if (ch >= WINSIM_NUM_CH || !KS.apm_awake || KS.fault[ch])
        return -1;   /* APM 没醒或接口异常时没有 CAN 状态报文 */
    float mv = ks_port_mv(ch);
    return mv > 4000.0f ? 0 : mv > 3000.0f ? 1 : mv > 2000.0f ? 2 : mv > 1100.0f ? 3 : mv > 300.0f ? 4 : 5;
}

static int ks_cmd(const char *c, int a, int b)
{
    if (strcmp(c, "ks_apm") == 0) {
        KS.apm_awake = a != 0;
        return 0;
    }
    if (a < 0 || a >= (int)WINSIM_NUM_CH)
        return -1;
    if (strcmp(c, "ks_attach") == 0) {
        KS.attached[a] = b != 0;
        if (!KS.attached[a])
            KS.pos[a] = 0u;
    } else if (strcmp(c, "ks_switch") == 0) {
        if (b < 0 || b > 4)
            return -1;
        KS.pos[a] = KS.attached[a] ? (uint8_t)b : 0u;
    } else if (strcmp(c, "ks_fault") == 0) {
        KS.fault[a] = b != 0;
    } else {
        return -1;
    }
    return 0;
}

static void ks_json(jw_t *j)
{
    jw_key(j, "keysim");
    jw_obj(j);
    jw_kbool(j, "apm", KS.apm_awake);
    jw_kint(j, "selected", KS.ch);
    jw_key(j, "ch");
    jw_arr(j);
    for (unsigned k = 0; k < WINSIM_NUM_CH; k++) {
        jw_obj(j);
        jw_kbool(j, "attached", KS.attached[k]);
        jw_kint(j, "pos", KS.pos[k]);
        jw_kbool(j, "fault", KS.fault[k]);
        jw_kint(j, "port_mv", (int64_t)(ks_port_mv(k) + 0.5f));
        jw_kint(j, "state", port_keysim_apm_state((uint8_t)k));
        jw_kbool(j, "sd_file", KSF.len[k] != 0u);
        jw_end_obj(j);
    }
    jw_end_arr(j);
    jw_end_obj(j);
}

void env_reset(void)
{
    memset(&E, 0, sizeof E);
    memset(samples, 0, sizeof samples);
    memset(&import_stage, 0, sizeof import_stage);
    E.batt_mv = 3900;
    E.sd_present = true;
    E.cur_session = -1;
    E.chip = 'V';
    E.screen = true;
    E.rtc_epoch = time(NULL);
    E.rtc_valid = E.rtc_epoch != (time_t)-1;
    preset_files();
    for (int ch = 0; ch < CH_NUM; ch++)
        bus_defaults(ch);
    ks_reset();
}

void env_set_key(key_id_t k, bool down) { E.key[k] = down; }

/* ---------- port.h：电源与外设 ---------- */
void port_pwr_hold(bool on)
{
    if (on && !E.powered) {
        E.powered = true;
        evt_post4(EV_BATT, 0, SRC_SYS, E.batt_mv, E.ext);   /* 上电后驱动读到的初始状态 */
        if (E.usb)
            evt_post4(EV_USB_ATTACH, 0, SRC_SYS, 0, 0);
        if (E.usb && E.host)
            evt_post4(EV_HOST_LINK, 0, SRC_SYS, 1, 0);
    } else if (!on && E.powered) {
        E.powered = false;
        E.v5 = false;
        E.msc = false;
        E.backlight = 0;
        E.mount_due = E.prealloc_due = E.close_due = E.format_due = 0;
        for (int ch = 0; ch < CH_NUM; ch++) {
            E.bus[ch].started = false;
            E.bus[ch].probing = false;
            E.bus[ch].txn = 0;
        }
    }
}

void port_5v(bool on) { E.v5 = on; }
void port_backlight(uint8_t pct) { E.backlight = pct; }
void port_chg_hi(bool on) { E.chg_hi = on; }
bool port_key_down(key_id_t k) { return E.key[k]; }
bool port_screen_present(void) { return E.screen; }
void port_screen_redraw(void) { /* Browser renderer has its own refresh loop. */ }
uint64_t port_capture_time_us(void) { return app_now_us(); }
char port_chip_rev(void) { return E.chip; }
uint32_t port_serial(void) { return 0x3A7C19E5u; }
bool port_rtc_valid(void) { return E.rtc_valid; }
void port_rtc_mark_synced(void)
{
    E.rtc_epoch = time(NULL);
    E.rtc_elapsed_ms = 0;
    E.rtc_valid = E.rtc_epoch != (time_t)-1;
}

/* ---------- port.h：CAN ---------- */
static void status_if_changed(int ch)
{
    bus_t *b = &E.bus[ch];
    if (b->tec != b->s_tec || b->rec != b->s_rec || b->busoff != b->s_busoff) {
        app_evt_t event={.type=EV_CAN_STATUS,.ch=(uint8_t)ch,.src=SRC_SYS,
            .a=b->tec,.b=(int32_t)b->rec|((int32_t)b->busoff<<16),.generation=b->generation};
        if (evt_post(&event)) {
            b->s_tec = b->tec;
            b->s_rec = b->rec;
            b->s_busoff = b->busoff;
        }
    }
}

uint32_t port_can_generation(int ch)
{
    return (unsigned)ch<CH_NUM?E.bus[ch].generation:0;
}
bool port_can_start(int ch, uint32_t nbr, uint32_t dbr, bool fd, can_mode_t mode)
{
    if ((unsigned)ch>=CH_NUM) return false;
    port_can_standby(ch);
    bus_t *b = &E.bus[ch];
    if (b->start_fail || (unsigned)mode>CAN_MODE_NORMAL) {
        evt_msg(MSG_BUS_ABNORMAL,(uint8_t)ch,0); return false;
    }
    b->started = true;
    b->mode = mode;
    b->c_nbr = nbr;
    b->c_dbr = dbr;
    b->c_fd = fd;
    b->tec = b->rec = 0;
    b->busoff = false;
    b->s_tec = b->s_rec = 0;
    b->s_busoff = false;
    b->txn = 0;
    b->probing = false;
    return true;
}

void port_can_standby(int ch)
{
    if ((unsigned)ch>=CH_NUM) return;
    bus_t *b = &E.bus[ch];
    b->generation++;if (!b->generation) b->generation++;
    b->started = false;
    b->probing = false;
    b->probe_ready = false;
    b->txn = 0;
}

bool port_can_tx(int ch, const can_frame_t *f)
{
    if ((unsigned)ch >= CH_NUM || !f) return false;
    bus_t *b = &E.bus[ch];
    if (!b->started || b->mode != CAN_MODE_NORMAL || b->txn >= 8) return false;
    b->txq[b->txn++] = *f;
    return true;
}

void port_can_tx_abort(int ch) { E.bus[ch].txn = 0; }

bool port_can_recover(int ch)
{
    if ((unsigned)ch>=CH_NUM) return false;
    bus_t *b = &E.bus[ch];
    return port_can_start(ch,b->c_nbr,b->c_dbr,b->c_fd,b->mode);
}

bool port_can_probe(int ch, uint32_t nbr, uint32_t dbr, bool data_phase, can_mode_t mode)
{
    if (!port_can_start(ch,nbr,data_phase?dbr:2000000u,true,mode)) return false;
    bus_t *b = &E.bus[ch];
    b->probing = true;
    b->started = true;
    b->p_nbr = nbr;
    b->p_dbr = dbr;
    b->p_data = data_phase;
    b->p_mode = mode;
    b->p_due = now() + PROBE_MS;
    b->txn = 0;
    return true;
}

/* ---------- port.h：SD ---------- */
bool port_sd_present(void) { return E.sd_present; }
void port_sd_mount(void) { E.mount_due = now() + MOUNT_MS; }
void port_sd_unmount(void) {}
void port_sd_reinit(void)
{
    if (E.sd_present && E.cur_session >= 0 && E.sd_werr == 0)
        post(EV_SD_WRITE_RECOVERED, 0, 0, 0);
}
bool port_sd_busy(void) { return E.sd_busy; }
uint32_t port_sd_total_kb(void) { return SD_TOTAL_KB; }

uint32_t port_sd_free_kb(void)
{
    uint64_t used_kb = E.sd_used / 1024u;
    return used_kb >= SD_TOTAL_KB ? 0 : (uint32_t)(SD_TOTAL_KB - used_kb);
}

uint32_t port_sd_next_session(void) { return E.session_max + 1; }

void port_sd_prealloc(uint32_t session, uint32_t seg, uint32_t seg_kb, uint32_t nbr0, uint32_t dbr0, bool fd0)
{
    (void)seg;
    E.prealloc_due = now() + PREALLOC_MS;
    if (E.sd_frag) {
        E.prealloc_res = SD_ERR_FRAG;
        return;
    }
    if (port_sd_free_kb() < seg_kb) {
        E.prealloc_res = SD_ERR_FULL;
        return;
    }
    int slot = free_sample_slot();
    if (E.nfiles >= MAX_FILES || slot < 0) {
        E.prealloc_res = SD_ERR_FULL;
        return;
    }
    char name[48];
    char stamp[16] = "NOTIME";
    if (E.rtc_valid) {
        time_t current = E.rtc_epoch + (time_t)(E.rtc_elapsed_ms / 1000u);
        const struct tm *calendar = localtime(&current);
        if (calendar && !strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", calendar))
            memcpy(stamp, "NOTIME", 7);
    }
    int written = snprintf(name, sizeof name, "%s_%s_%08u.log",
        E.rec_prefix[0] ? E.rec_prefix : "REC", stamp, (unsigned)session);
    if (written < 0 || (size_t)written >= sizeof name || name_exists(name, -1)) {
        E.prealloc_res = SD_ERR_IO;
        return;
    }
    E.prealloc_res = SD_OK;
    /* A host session may mix per-channel rates; do not attach CAN1's rate to
     * CAN2 playback. Real CFR headers need per-channel/segment configuration. */
    add_file(name, FK_CFR, 0, 0, 0, false, 2, 0, 0, true);
    E.cur_session = E.nfiles - 1;
    E.file_session[E.cur_session] = session;
    if (slot >= 0) {
        E.file_slot[E.cur_session] = (int8_t)slot;
        samples[slot].count = 0;
        E.files[E.cur_session].sample_available = true;
    }
    E.session_max = session;
    E.sd_dirty = true;
}

void port_sd_next_segment(uint32_t seg) { (void)seg; }

void port_sd_write(uint32_t bytes)
{
    if (!E.sd_present || !E.powered)
        return;
    E.sd_used += bytes;
    if (E.cur_session >= 0) {
        file_info_t *f = &E.files[E.cur_session];
        f->size_kb = (uint32_t)(f->size_kb + bytes / 1024u + (bytes % 1024u ? 1 : 0));
    }
    if (E.sd_werr == 1) {
        E.sd_werr = 0;
        post(EV_SD_WRITE_ERR, 0, 0, 0);
    } else if (E.sd_werr == 2 && now() - E.werr_last >= 200) {
        E.werr_last = now();
        post(EV_SD_WRITE_ERR, 0, 1, 0);
    }
    if (port_sd_free_kb() == 0 && !E.full_posted) {
        E.full_posted = true;
        post(EV_SD_FULL, 0, 0, 0);
    }
}

/* Real frame payloads for the host's retained recording sample. ring order
 * can interleave batches from two sources, so keep the bounded sample sorted. */
bool port_sd_record_frame(const can_frame_t *frame)
{
    if (!frame || !E.sd_present || E.cur_session < 0) return false;
    file_info_t *fi = &E.files[E.cur_session];
    if (!fi->frames) {
        E.first_record_us[E.cur_session] = E.last_record_us[E.cur_session] = frame->ts_us;
    } else {
        if (frame->ts_us < E.first_record_us[E.cur_session]) E.first_record_us[E.cur_session] = frame->ts_us;
        if (frame->ts_us > E.last_record_us[E.cur_session]) E.last_record_us[E.cur_session] = frame->ts_us;
    }
    uint64_t elapsed = (E.last_record_us[E.cur_session] - E.first_record_us[E.cur_session]) / 1000u;
    fi->duration_ms = elapsed > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed;
    fi->frames++;
    if (frame->flags & CANF_FD) fi->fd = true;
    int slot = E.file_slot[E.cur_session];
    if (!logfmt_frame_valid(frame) || slot < 0 || samples[slot].count >= ENV_LOG_MAX_FRAMES) {
        fi->sample_truncated = true;
        fi->sample_dropped++;
        return true;
    }
    log_sample_t *sample = &samples[slot];
    uint32_t at = sample->count;
    while (at && sample->frames[at - 1u].ts_us > frame->ts_us) {
        sample->frames[at] = sample->frames[at - 1u];
        at--;
    }
    sample->frames[at] = *frame;
    sample->count++;
    fi->sample_frames = sample->count;
    return true;
}

/* Presets are procedural fixtures, not parsers for CFR/ASC/CSV on a real SD. */
static uint32_t preset_source_count(const file_info_t *fi, uint8_t source)
{
    if (!fi->channels || source >= fi->channels || source >= CH_NUM) return 0;
    return fi->frames / fi->channels + (source < fi->frames % fi->channels ? 1u : 0u);
}

bool port_sd_replay_info(int idx, uint8_t source, uint32_t *frames, uint64_t *duration_us)
{
    if (!frames || !duration_us || !E.sd_present || idx < 0 || idx >= E.nfiles || source >= CH_NUM || idx == E.cur_session)
        return false;
    const file_info_t *fi = &E.files[idx];
    if (fi->kind == FK_UNKNOWN) return false;
    int slot = E.file_slot[idx];
    *frames = 0;
    *duration_us = 0;
    if (slot >= 0) {
        const log_sample_t *sample = &samples[slot];
        for (uint32_t i = 0; i < sample->count; i++) {
            if (sample->frames[i].ch == source) {
                (*frames)++;
                *duration_us = sample->frames[i].ts_us - sample->frames[0].ts_us;
            }
        }
    } else if (E.file_fixture[idx]) {
        *frames = preset_source_count(fi, source);
        *duration_us = (uint64_t)fi->duration_ms * 1000u;
    }
    return *frames != 0;
}

bool port_sd_read_frame(int idx, uint8_t source, uint32_t ordinal, can_frame_t *out)
{
    if (!out || !E.sd_present || idx < 0 || idx >= E.nfiles || source >= CH_NUM || idx == E.cur_session) return false;
    const file_info_t *fi = &E.files[idx];
    int slot = E.file_slot[idx];
    if (slot >= 0) {
        const log_sample_t *sample = &samples[slot];
        for (uint32_t i = 0, n = 0; i < sample->count; i++) {
            if (sample->frames[i].ch == source && n++ == ordinal) {
                *out = sample->frames[i];
                out->ts_us -= sample->frames[0].ts_us;
                return true;
            }
        }
        return false;
    }
    if (fi->kind == FK_UNKNOWN || !E.file_fixture[idx]) return false;
    uint32_t count = preset_source_count(fi, source);
    if (ordinal >= count) return false;
    *out = (can_frame_t){ .ch = source, .id = 0x100u + (ordinal % 8u) * 0x10u,
        .len = fi->fd ? 16 : 8, .flags = fi->fd ? CANF_FD | CANF_BRS : 0,
        .ts_us = (uint64_t)(ordinal + 1u) * fi->duration_ms * 1000u / count };
    for (unsigned i = 0; i < out->len; i++) out->data[i] = (uint8_t)(ordinal + i);
    return true;
}

static int import_error(env_log_result_t *result, int status, uint32_t line, uint32_t frames)
{
    if (result) *result = (env_log_result_t){ status, line, frames };
    return status;
}

int env_log_import(const char *name, const char *text, size_t size, env_log_result_t *result)
{
    if (!name || !text || !size || !name[0])
        return import_error(result, ENV_LOG_ERR_ARGUMENT, 0, 0);
    int name_result = sample_name(name);
    if (name_result != ENV_LOG_OK)
        return import_error(result, name_result == ENV_LOG_ERR_NAME ? ENV_LOG_ERR_ARGUMENT : name_result, 0, 0);
    if (size > ENV_LOG_MAX_TEXT || E.nfiles >= MAX_FILES)
        return import_error(result, ENV_LOG_ERR_LIMIT, 0, 0);
    if (!E.powered || power_get()->state != PWR_RUNNING || storage_get()->state != ST_READY ||
        !E.sd_present || E.sd_wp || E.sd_unsupported || E.msc || E.cur_session >= 0 || E.format_due)
        return import_error(result, ENV_LOG_ERR_BUSY, 0, 0);
    if ((size + 1023u) / 1024u > port_sd_free_kb())
        return import_error(result, ENV_LOG_ERR_LIMIT, 0, 0);
    if (name_exists(name, -1)) return import_error(result, ENV_LOG_ERR_EXISTS, 0, 0);
    int slot = free_sample_slot();
    if (slot < 0) return import_error(result, ENV_LOG_ERR_NO_SLOT, 0, 0);
    import_stage.count = 0;
    size_t pos = 0;
    uint32_t line_no = 0;
    bool fd = false;
    uint8_t channels = 0;
    while (pos < size) {
        size_t begin = pos;
        while (pos < size && text[pos] != '\n') pos++;
        size_t line_len = pos - begin;
        if (pos < size) pos++;
        line_no++;
        can_frame_t frame;
        logfmt_result_t parsed = logfmt_parse_line(text + begin, line_len, &frame);
        if (parsed == LOGFMT_SKIP) continue;
        if (parsed != LOGFMT_OK)
            return import_error(result, ENV_LOG_ERR_FORMAT, line_no, import_stage.count);
        if (import_stage.count == ENV_LOG_MAX_FRAMES)
            return import_error(result, ENV_LOG_ERR_LIMIT, line_no, import_stage.count);
        if (import_stage.count && frame.ts_us < import_stage.frames[import_stage.count - 1u].ts_us)
            return import_error(result, ENV_LOG_ERR_TIMESTAMP, line_no, import_stage.count);
        /* Keep vt_ms and file duration in their portable uint32_t range. */
        if (import_stage.count && frame.ts_us - import_stage.frames[0].ts_us > (uint64_t)UINT32_MAX * 1000u)
            return import_error(result, ENV_LOG_ERR_TIMESTAMP, line_no, import_stage.count);
        import_stage.frames[import_stage.count++] = frame;
        if (frame.flags & CANF_FD) fd = true;
        if (frame.ch + 1u > channels) channels = (uint8_t)(frame.ch + 1u);
    }
    if (!import_stage.count) return import_error(result, ENV_LOG_ERR_FORMAT, 0, 0);
    uint64_t duration = import_stage.frames[import_stage.count - 1u].ts_us - import_stage.frames[0].ts_us;
    int idx = E.nfiles;
    add_file(name, FK_CANDUMP, (uint32_t)((size + 1023u) / 1024u), 0, 0, fd,
             channels, import_stage.count, (uint32_t)((duration + 999u) / 1000u), false);
    samples[slot] = import_stage;
    E.file_slot[idx] = (int8_t)slot;
    E.files[idx].imported = true;
    E.files[idx].sample_available = true;
    E.files[idx].sample_frames = import_stage.count;
    E.sd_used += (uint64_t)E.files[idx].size_kb * 1024u;
    if (result) *result = (env_log_result_t){ ENV_LOG_OK, 0, import_stage.count };
    return idx;
}

int env_log_export(int idx, char *buf, size_t cap, size_t *needed)
{
    if (buf && cap) buf[0] = 0;
    if (needed) *needed = 0;
    if (idx < 0 || idx >= E.nfiles || !E.sd_present || E.file_slot[idx] < 0)
        return ENV_LOG_ERR_NOT_AVAILABLE;
    if (E.cur_session == idx) return ENV_LOG_ERR_BUSY;
    const log_sample_t *sample = &samples[E.file_slot[idx]];
    size_t total = 0;
    char line[LOGFMT_MAX_LINE + 1u];
    for (uint32_t i = 0; i < sample->count; i++) {
        int n = logfmt_format_line(&sample->frames[i], line, sizeof line);
        if (n < 0) return ENV_LOG_ERR_FORMAT;
        total += (size_t)n;
    }
    if (needed) *needed = total + 1u;
    if (!buf || cap <= total) return ENV_LOG_ERR_SPACE;
    size_t used = 0;
    for (uint32_t i = 0; i < sample->count; i++) {
        int n = logfmt_format_line(&sample->frames[i], buf + used, cap - used);
        if (n < 0) { buf[0] = 0; return ENV_LOG_ERR_FORMAT; }
        used += (size_t)n;
    }
    buf[used] = 0;
    return (int)used;
}

const char *env_log_error_name(int status)
{
    switch (status) {
    case ENV_LOG_OK: return "ok";
    case ENV_LOG_ERR_ARGUMENT: return "invalid name or argument";
    case ENV_LOG_ERR_LIMIT: return "input, file or frame capacity exceeded";
    case ENV_LOG_ERR_FORMAT: return "unsupported or malformed candump line";
    case ENV_LOG_ERR_TIMESTAMP: return "descending or excessive timestamp";
    case ENV_LOG_ERR_NO_SLOT: return "all retained sample slots are occupied";
    case ENV_LOG_ERR_NOT_AVAILABLE: return "no retained sample available";
    case ENV_LOG_ERR_SPACE: return "output buffer too small";
    case ENV_LOG_ERR_BUSY: return "storage unavailable, shared or recording";
    case ENV_LOG_ERR_EXISTS: return "a file with that name already exists";
    case ENV_LOG_ERR_READONLY: return "built-in or protected file cannot be renamed";
    case ENV_LOG_ERR_CHANGED: return "file has changed or moved; refresh the file list";
    case ENV_LOG_ERR_NAME: return "invalid UTF-8, path, reserved name or filename character";
    case ENV_LOG_ERR_EXTENSION: return "retained candump samples must use .log or .txt";
    }
    return "unknown error";
}

void port_sd_close(void) { E.close_due = now() + CLOSE_MS; }

static void remove_file(int idx)
{
    play_file_removed(idx);
    E.sd_used -= (uint64_t)E.files[idx].size_kb * 1024u > E.sd_used ? E.sd_used : (uint64_t)E.files[idx].size_kb * 1024u;
    for (int i = idx + 1; i < E.nfiles; i++) {
        E.files[i - 1] = E.files[i];
        E.file_token[i - 1] = E.file_token[i];
        E.file_session[i - 1] = E.file_session[i];
        E.file_slot[i - 1] = E.file_slot[i];
        E.file_fixture[i - 1] = E.file_fixture[i];
        E.first_record_us[i - 1] = E.first_record_us[i];
        E.last_record_us[i - 1] = E.last_record_us[i];
    }
    E.nfiles--;
    E.file_session[E.nfiles]=0;
    if (E.cur_session == idx)
        E.cur_session = -1;
    else if (E.cur_session > idx)
        E.cur_session--;
}

void port_sd_delete_oldest(void)
{
    for (int i = 0; i < E.nfiles; i++) {
        if (E.files[i].is_session && !E.files[i].protected_ && i != E.cur_session && !play_file_in_use(i)) {
            remove_file(i);
            E.full_posted = false;
            /* 模拟器里旧文件太小，删掉后再释放一点空间，免得马上又满 */
            E.sd_used = E.sd_used > 64u * 1024u * 1024u ? E.sd_used - 64u * 1024u * 1024u : 0;
            post(EV_SD_DELETE_DONE, 0, 1, 0);
            return;
        }
    }
    post(EV_SD_DELETE_DONE, 0, 0, 0);
}

void port_sd_format(void) { E.format_due = now() + FORMAT_MS; }

void port_sd_protect_toggle(int idx)
{
    if (idx >= 0 && idx < E.nfiles)
        E.files[idx].protected_ = !E.files[idx].protected_;
}

int port_sd_file_count(void) { return E.sd_present ? E.nfiles : 0; }

bool port_sd_file_info(int idx, file_info_t *out)
{
    if (!E.sd_present || idx < 0 || idx >= E.nfiles)
        return false;
    *out = E.files[idx];
    out->writing = idx == E.cur_session;
    return true;
}

/* ---------- port.h：USB ---------- */
void port_usb_msc(bool share) { E.msc = share; }

/* ---------- 节拍：总线与 SD 的异步动作 ---------- */
static bool rate_match(const bus_t *b)
{
    if (b->c_nbr != b->nbr)
        return false;
    if (b->fd && (!b->c_fd || b->c_dbr != b->dbr))
        return false;   /* 总线上有 FD 帧：控制器没开 FD 或数据段速率不对都会出错 */
    return true;
}

static void gen_frame(int ch, bus_t *b, can_frame_t *f, uint64_t ts)
{
    static const struct { uint32_t id; uint8_t flags, len; uint8_t w; } tbl[CH_NUM][5] = {
        { { 0x0C9, 0, 8, 10 }, { 0x18FEF100, CANF_EXT, 8, 2 }, { 0x3A1, CANF_FD | CANF_BRS, 64, 5 },
          { 0x1F0, 0, 8, 3 }, { 0x7E8, 0, 8, 1 } },
        { { 0x200, 0, 8, 6 }, { 0x215, CANF_FD | CANF_BRS, 12, 3 }, { 0x18DAF110, CANF_EXT, 8, 2 },
          { 0x330, 0, 4, 4 }, { 0x6A0, 0, 8, 1 } },
    };
    uint32_t total = 0;
    for (int i = 0; i < 5; i++)
        total += tbl[ch][i].w;
    uint32_t pick = (b->seq * 7u) % total, acc = 0;
    int k = 0;
    for (; k < 4; k++) {
        acc += tbl[ch][k].w;
        if (pick < acc)
            break;
    }
    memset(f, 0, sizeof *f);
    f->ts_us = ts;
    f->ch = (uint8_t)ch;
    f->id = tbl[ch][k].id;
    f->flags = tbl[ch][k].flags;
    f->len = tbl[ch][k].len;
    if (!b->fd && (f->flags & CANF_FD)) {      /* 经典 CAN 总线上没有 FD 帧 */
        f->flags &= (uint8_t)~(CANF_FD | CANF_BRS);
        f->len = 8;
    }
    uint32_t s = b->seq;
    for (int i = 0; i < f->len; i++)
        f->data[i] = (uint8_t)(0x10 * k + i);
    f->data[0] = (uint8_t)(s & 0xFF);                       /* 计数器字节，一直在变 */
    if (f->len > 3)
        f->data[3] = (uint8_t)((s / 97u) & 0xFF);           /* 慢变字节 */
    b->seq++;
}

static void push_err(int ch, uint64_t ts, uint8_t code)
{
    can_frame_t f;
    memset(&f, 0, sizeof f);
    f.ts_us = ts;
    f.ch = (uint8_t)ch;
    f.flags = CANF_ERR;
    f.len = 1;
    f.data[0] = code;      /* 1 填充 2 格式 3 ACK 4 位 5 CRC */
    ring_push(&f);
}

static void probe_done(int ch, bus_t *b)
{
    if (b->probe_ready) {
        if (evt_post(&b->probe_event)) b->probing=b->probe_ready=false;
        return;
    }
    bool traffic = b->connected && b->nodes >= 1 && b->fps > 0;
    int32_t frames = (int32_t)(b->fps * PROBE_MS / 1000u);
    bool rate_ok = b->p_data ? (b->c_nbr == b->nbr && b->fd && b->p_dbr == b->dbr) : (b->p_nbr == b->nbr);
    bool acked = b->nodes >= 2 || b->p_mode == CAN_MODE_RESTRICTED;
    int32_t valid = 0, errs = 0;
    if (traffic) {
        if (rate_ok && acked)
            valid = frames < 2 ? 2 : frames;
        else
            errs = frames < 3 ? 3 : frames;
    }
    if (!b->p_data)
        b->c_nbr = b->p_nbr;
    b->probe_event=(app_evt_t){.type=EV_CAN_PROBE_RESULT,.ch=(uint8_t)ch,.src=SRC_SYS,
        .a=valid,.b=errs|((int32_t)(traffic&&rate_ok&&b->fd)<<16),.generation=b->generation};
    b->probe_ready=true;
    if (evt_post(&b->probe_event)) b->probing=b->probe_ready=false;
}

static void bus_tick(int ch, uint32_t ms)
{
    bus_t *b = &E.bus[ch];
    uint64_t t0 = app_now_us();
    if (b->probing) {
        if ((int32_t)(now() - b->p_due) >= 0)
            probe_done(ch, b);
        return;
    }
    bool active = b->started && !b->busoff && E.v5;
    if (!active) {
        if (b->started && E.v5) status_if_changed(ch);
        return;
    }
    bool match = rate_match(b);

    /* 发送：有别的节点回 ACK 就发得出去；否则（空总线 / 速率不对）重发、TEC 上升 */
    if (b->txn > 0) {
        if (b->connected && b->nodes >= 1 && match) {
            for (int i = 0; i < b->txn; i++) {
                can_frame_t f = b->txq[i];
                f.ts_us = t0 + (uint64_t)i * 200u;
                f.flags |= CANF_TX;
                ring_push(&f);
                post(EV_CAN_TX_DONE, (uint8_t)ch, 0, 0);
                if (b->tec)
                    b->tec--;
            }
            b->txn = 0;
        } else if (b->connected && b->nodes >= 1) {
            b->tec = (uint16_t)(b->tec + 8);           /* 速率不对：位错误，可能 Bus-Off */
            push_err(ch, t0, 4);
        } else if (b->tec < 128) {
            b->tec = (uint16_t)(b->tec + 8 > 128 ? 128 : b->tec + 8);   /* 无 ACK：停在错误被动 */
        }
    }

    /* 接收 */
    if (b->connected && b->nodes >= 1 && b->fps) {
        b->acc_milli += b->fps * ms;
        uint32_t n = b->acc_milli / 1000u;
        b->acc_milli %= 1000u;
        bool acked = b->nodes >= 2 || b->mode != CAN_MODE_LISTEN;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t ts = t0 + (uint64_t)i * ms * 1000u / (n ? n : 1);
            if (match && acked) {
                can_frame_t f;
                gen_frame(ch, b, &f, ts);
                ring_push(&f);
                if (b->rec)
                    b->rec--;
            } else {
                if (i < 2)
                    push_err(ch, ts, match ? 3 : 1);    /* 只有一个节点：ACK 错；速率不对：填充错 */
                if (b->rec < 135)
                    b->rec++;
            }
        }
    }

    /* 注入的错误 */
    if (b->err_burst_ms) {
        uint32_t d = b->err_burst_ms < ms ? b->err_burst_ms : ms;
        b->err_burst_ms -= d;
        push_err(ch, t0, 5);
        if (b->mode == CAN_MODE_NORMAL)
            b->tec = (uint16_t)(b->tec + 8);
        else if (b->rec < 135)
            b->rec = (uint16_t)(b->rec + 2);
    }
    if (b->tec > 255) {
        b->busoff = true;
        b->tec = 256;
        b->txn = 0;
    }
    status_if_changed(ch);
}

static void sd_tick(void)
{
    uint32_t t = now();
    if (E.mount_due && (int32_t)(t - E.mount_due) >= 0) {
        E.mount_due = 0;
        if (E.sd_present) {
            sd_result_t r = E.sd_wp ? SD_ERR_WP : E.sd_unsupported ? SD_ERR_UNSUPPORTED : E.sd_dirty ? SD_OK_REPAIRED : SD_OK;
            if (r == SD_OK_REPAIRED) {
                E.sd_dirty = false;
                E.cur_session = -1;
            }
            post(EV_SD_MOUNT_DONE, 0, r, 0);
        }
    }
    if (E.prealloc_due && (int32_t)(t - E.prealloc_due) >= 0) {
        E.prealloc_due = 0;
        post(EV_SD_PREALLOC_DONE, 0, E.prealloc_res, 0);
    }
    if (E.close_due && (int32_t)(t - E.close_due) >= 0) {
        E.close_due = 0;
        E.sd_dirty = false;
        E.cur_session = -1;
        post(EV_SD_CLOSE_DONE, 0, 0, 0);
    }
    if (E.format_due && (int32_t)(t - E.format_due) >= 0) {
        E.format_due = 0;
        ks_files_clear();
        E.nfiles = 0;
        memset(E.file_session,0,sizeof E.file_session);
        E.sd_used = 0;
        E.sd_unsupported = false;
        E.full_posted = false;
        E.cur_session = -1;
        E.sd_dirty = false;
        post(EV_SD_FORMAT_DONE, 0, 0, 0);
    }
}

void env_tick(uint32_t ms)
{
    E.rtc_elapsed_ms += ms;
    if (!E.powered)
        return;
    ks_storage_tick();
    for (int ch = 0; ch < CH_NUM; ch++)
        bus_tick(ch, ms);
    sd_tick();
}

/* ---------- 控制命令（网页 / 测试用） ---------- */
static bool is(const char *a, const char *b) { return strcmp(a, b) == 0; }

int env_cmd(const char *c, int a, int b)
{
    if (strncmp(c, "ks_", 3) == 0)
        return ks_cmd(c, a, b);
    int ch = a < 0 ? 0 : a > 1 ? 1 : a;
    if (is(c, "batt")) {
        E.batt_mv = (uint16_t)a;
        post(EV_BATT, 0, E.batt_mv, E.ext);
    } else if (is(c, "ext")) {
        E.ext = a != 0;
        post(EV_BATT, 0, E.batt_mv, E.ext);
    } else if (is(c, "sd_insert")) {
        if (!E.sd_present) {
            E.sd_present = true;
            post(EV_SD_INSERT, 0, 0, 0);
        }
    } else if (is(c, "sd_remove")) {
        if (E.sd_present) {
            E.sd_present = false;
            E.mount_due = E.prealloc_due = E.close_due = E.format_due = 0;
            post(EV_SD_REMOVE, 0, 0, 0);
        }
    } else if (is(c, "sd_wp")) {
        E.sd_wp = a != 0;
    } else if (is(c, "sd_unsupported")) {
        E.sd_unsupported = a != 0;
    } else if (is(c, "sd_frag")) {
        E.sd_frag = a != 0;
    } else if (is(c, "sd_werr")) {
        E.sd_werr = (uint8_t)a;
    } else if (is(c, "sd_busy")) {
        E.sd_busy = a != 0;
    } else if (is(c, "sd_fill")) {
        E.sd_used = (uint64_t)SD_TOTAL_KB * 1024u - 2048u;
        E.full_posted = false;
    } else if (is(c, "usb")) {
        bool on = a != 0;
        if (on != E.usb) {
            E.usb = on;
            if (!on)
                E.host = false;
            post(on ? EV_USB_ATTACH : EV_USB_DETACH, 0, 0, 0);
        }
    } else if (is(c, "host")) {
        bool on = a != 0 && E.usb;
        if (on != E.host) {
            E.host = on;
            post(EV_HOST_LINK, 0, on, 0);
        }
    } else if (is(c, "eject")) {
        post(EV_USB_EJECT, 0, 0, 0);
    } else if (is(c, "can_start_fail")) {
        E.bus[ch].start_fail = b != 0;
    } else if (is(c, "bus_conn")) {
        E.bus[ch].connected = b != 0;
    } else if (is(c, "bus_nbr")) {
        E.bus[ch].nbr = (uint32_t)b;
    } else if (is(c, "bus_dbr")) {
        E.bus[ch].dbr = (uint32_t)b;
    } else if (is(c, "bus_fd")) {
        E.bus[ch].fd = b != 0;
    } else if (is(c, "bus_nodes")) {
        E.bus[ch].nodes = (uint8_t)b;
    } else if (is(c, "bus_fps")) {
        E.bus[ch].fps = (uint16_t)b;
    } else if (is(c, "bus_err")) {
        E.bus[ch].err_burst_ms = (uint32_t)b;
    } else if (is(c, "bus_overload")) {
        post(EV_CAN_DROPS, (uint8_t)ch, b, 0);   /* 模拟硬件 FIFO 溢出 */
    } else if (is(c, "chip")) {
        E.chip = (char)a;
    } else if (is(c, "screen")) {
        E.screen = a != 0;
    } else if (is(c, "rtc_lost")) {
        E.rtc_valid = false;
    } else {
        return -1;
    }
    return 0;
}

/* ---------- 快照 ---------- */
void env_json(jw_t *j)
{
    static const char *const modes[] = { "只听", "受限", "正常" };
    jw_obj(j);
    jw_kbool(j, "powered", E.powered);
    jw_kbool(j, "v5", E.v5);
    jw_kint(j, "backlight", E.backlight);
    jw_kbool(j, "chg_hi", E.chg_hi);
    jw_kint(j, "batt", E.batt_mv);
    jw_kbool(j, "ext", E.ext);
    jw_kbool(j, "usb", E.usb);
    jw_kbool(j, "host", E.host);
    jw_kbool(j, "msc", E.msc);
    jw_kstr(j, "chip", E.chip == 'Y' ? "Y" : "V");
    jw_kbool(j, "screen", E.screen);
    jw_kbool(j, "rtc", E.rtc_valid);
    jw_key(j, "sd");
    jw_obj(j);
    jw_kbool(j, "present", E.sd_present);
    jw_kbool(j, "wp", E.sd_wp);
    jw_kbool(j, "unsupported", E.sd_unsupported);
    jw_kbool(j, "frag", E.sd_frag);
    jw_kint(j, "werr", E.sd_werr);
    jw_kbool(j, "busy", E.sd_busy);
    jw_kbool(j, "dirty", E.sd_dirty);
    jw_kint(j, "used_mb", (int64_t)(E.sd_used / (1024u * 1024u)));
    jw_kint(j, "total_mb", SD_TOTAL_KB / 1024u);
    jw_end_obj(j);
    jw_key(j, "bus");
    jw_arr(j);
    for (int ch = 0; ch < CH_NUM; ch++) {
        const bus_t *b = &E.bus[ch];
        jw_obj(j);
        jw_kbool(j, "conn", b->connected);
        jw_kint(j, "nbr", b->nbr);
        jw_kint(j, "dbr", b->dbr);
        jw_kbool(j, "fd", b->fd);
        jw_kint(j, "nodes", b->nodes);
        jw_kint(j, "fps", b->fps);
        jw_kint(j, "err_ms", b->err_burst_ms);
        jw_kbool(j, "started", b->started);
        jw_kstr(j, "mode", modes[b->mode % 3]);
        jw_kint(j, "c_nbr", b->c_nbr);
        jw_kbool(j, "match", rate_match(b));
        jw_kint(j, "tec", b->tec);
        jw_kint(j, "rec", b->rec);
        jw_kbool(j, "busoff", b->busoff);
        jw_kint(j, "txq", b->txn);
        jw_kbool(j, "probing", b->probing);
        jw_end_obj(j);
    }
    jw_end_arr(j);
    jw_key(j, "keys");
    jw_arr(j);
    for (int k = 0; k < KEY_NUM; k++)
        jw_bool(j, E.key[k]);
    jw_end_arr(j);
    ks_json(j);
    jw_end_obj(j);
}

void port_can_flush_rx(void) { /* Host ingress already writes the broadcast ring. */ }
