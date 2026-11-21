#include "app/ui/core/ui_sm.h"
#include "services/can_core/chan_sm.h"
#include "services/config/cfg.h"
#include "services/core/app_evt.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
#include "services/frame_ring/frame_ring.h"
#include "services/player/play_sm.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/stats/stats.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/tx_sequence.h"
#include "sim/local_files_ui.h"
#include "sim/send_plan.h"
#include "sim/tx_scope.h"
#include "services/device/file_port.h"
#include "services/keysim/keysim.h"

#include <stdio.h>
#include <string.h>

#define TOAST_MS     2500u
#define LOG_LEN      24
#define DLG_QUEUE    4
#define LIST_WINDOW  12
#define TXT          160
#ifdef CANFD_REVD_TARGET
#define TX_EXTRA_COUNT 8
#define TX_LOAD_FILE_INDEX (CH_NUM*TX_ENTRIES+7)
#else
#define TX_EXTRA_COUNT 9
#define TX_LOAD_FILE_INDEX (CH_NUM*TX_ENTRIES+8)
#endif

/* 条目标志（给绘制用） */
#define IF_DISABLED 0x02
#define IF_WARN     0x04
#define IF_ACTIVE   0x08
#define IF_ACTION   0x10

typedef enum { DK_ASK, DK_MENU, DK_INFO, DK_LOCAL } dlg_kind_t;
typedef enum { MENU_LIVE_FILTER,MENU_LIVE_ACTIONS } menu_id_t;
typedef enum { LOCAL_FACTORY_RESET } local_id_t;

typedef struct {
    dlg_kind_t kind;
    ask_t      ask;
    uint8_t    ch;
    int32_t    b;
    src_t      src;
    int        id;          /* menu_id_t / local_id_t */
    int        cur;
    bool       tx_bound;
    uint32_t   tx_revision; /* confirmation applies to this exact entry version */
    char       text[TXT];
} dlg_t;

static page_t   page;
static int      cursor[PG_NUM];
static uint8_t  live_view;          /* 0 滚动，1 按 ID */
static uint8_t  live_channel;       /* 1 CAN1 / 2 CAN2; 0 internal all-channel view */
static int      live_cursors[3][2];
static dlg_t    dlgq[DLG_QUEUE];
static int      n_dlg;
static char     toast[TXT];
static uint32_t toast_until;
static struct { uint32_t t; char s[TXT]; } logbuf[LOG_LEN];
static int      log_n, log_pos;
typedef struct {
    can_frame_t frame;
    uint32_t period, revision;
    int ch, idx;
    uint8_t byte;
    bool open, dirty;
} tx_editor_t;
static tx_editor_t editor;
static struct {
    txseq_entry_config_t entries[TX_ENTRIES], original[TX_ENTRIES], entry_original;
    uint32_t id_gap_ms, original_gap_ms, revision, frame_revisions[TX_ENTRIES];
    int ch, entry;
    bool open, dirty;
} local_tx;
static uint8_t local_tx_channel;
static uint32_t tx_scope_mask;
static uint8_t plan_save_channel;
static send_plan_t plan_save_snapshot;
static bool plan_save_valid;
static struct { bool open,payload;page_t page;int row,ch,entry,base;uint32_t maximum,revision; } field_input;
static page_t plan_save_return;
static sp_file_t plan_selected;
static unsigned plan_selected_index;
static uint8_t plan_entry_ch,plan_entry_index;
static page_t plan_detail_return;
static page_t send_files_return;
static int log_selected_index;
static uint32_t log_selected_token;
static struct { bool pending; uint32_t session,token; } record_save;
#define PLAN_FILES_START 9
#define PLAN_ENTRIES_START 9
static disp_filter_t filter_draft;
typedef struct { uint8_t view,channel; int cursor; bool paused; } live_context_t;
static live_context_t detail_origin, filter_origin,rec_origin;
static bool detail_valid, filter_origin_valid;
static bool rec_origin_valid;
static struct {
    disp_row_t frame;
    uint32_t count, avg_us, min_us, max_us, seq;
    bool has_sequence;
    bool has_summary;
    int index;
} detail;
static bool numeric;
static uint8_t numeric_digit;
static uint32_t numeric_original;
static int8_t   host_edit_ch = -1;  /* 上位机刚改了正在编辑的这一路：随后的"已改为"提示只记日志，不盖掉冲突提示 */

#define PLAN_PAGE_TITLE "发送方案"
static const char *const page_title[PG_NUM] = {
    "主页", "实时显示", "发送报文", "录制设置", PLAN_PAGE_TITLE, "总线诊断", "文件管理", "通道设置", "系统设置", "工程菜单", "编辑发送报文", "显示筛选", "报文详情", "本地循环设置", "本地单 ID 时序", "已移除", "发送方案预览", "方案报文预览", "发送报文", "已移除", "现有报文文件", "文件详情",
    "按键模拟", "按键模拟", "学习键值", "门限扫描"
};
static const char *const page_id[PG_NUM] = {
    "home", "live", "tx", "rec", "play", "diag", "files", "chset", "sysset", "eng", "txedit", "filter", "detail", "txset", "txtiming", "removed_logplay", "plan_detail", "plan_entry", "send", "removed_projects", "send_files", "file_detail",
    "keysim", "keych", "keylearn", "keyscan"
};
/* 第 7 格“按键模拟”只在板上有这部分硬件时出现；当前 Rev D/E 目标仍是 6 格。 */
static const page_t home_tiles[7] = { PG_LIVE, PG_TX, PG_DIAG, PG_FILES, PG_CHSET, PG_SYSSET, PG_KEYSIM };
static int home_count(void) { return keysim_available() ? 7 : 6; }
#define HOME_COUNT home_count()
#define HOME_COLUMNS (HOME_COUNT==6?3:4)

static const uint32_t nbr_presets[] = { 10000, 20000, 33333, 50000, 83333, 100000, 125000, 250000, 500000, 800000, 1000000 };
static const uint32_t dbr_presets[] = { 1000000, 2000000, 4000000, 5000000 };
static const uint16_t screen_off_opts[] = { 15, 30, 60, 0 };
static const uint16_t auto_off_opts[] = { 1, 5, 10, 0 };
static const uint8_t  backlight_opts[] = { 20, 40, 60, 80, 100 };
#define N_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

page_t ui_page(void) { return page; }
const char *ui_base_page_id(void){return page_id[page];}
int ui_cursor(void) {
    if(local_files_active()) return local_files_cursor();
    return cursor[page];
}
bool ui_dialog_open(void) { return n_dlg > 0; }
int ui_dialog_ask(void) { return n_dlg && dlgq[0].kind == DK_ASK ? (int)dlgq[0].ask : -1; }
const char *ui_last_toast(void) { return toast; }

/* ---------- 文字 ---------- */
static void fmt_rate(char *buf, size_t n, uint32_t bps)
{
    if (bps >= 1000000 && bps % 1000000 == 0)
        snprintf(buf, n, "%uM", (unsigned)(bps / 1000000));
    else if (bps % 1000 == 0)
        snprintf(buf, n, "%uk", (unsigned)(bps / 1000));
    else
        snprintf(buf, n, "%u.%uk", (unsigned)(bps / 1000), (unsigned)(bps % 1000 / 100));
}

static void fmt_ch_rate(char *buf, size_t n, int ch)
{
    const cfg_t *c = cfg_get();
    char a[16], b[16];
    fmt_rate(a, sizeof a, c->nbr[ch]);
    fmt_rate(b, sizeof b, c->dbr[ch]);
    if (c->fd[ch])
        snprintf(buf, n, "%.10s/%.10s", a, b);
    else
        snprintf(buf, n, "%s", a);
}

static const char *mode_name(can_mode_t m)
{
    return m == CAN_MODE_NORMAL ? "正常" : m == CAN_MODE_RESTRICTED ? "受限" : "只听";
}

static void msg_text(char *o, size_t n, msg_t m, int ch, int32_t b)
{
    char r[24];
    int c = ch + 1;
    switch (m) {
    case MSG_LOCKED:            snprintf(o, n, "按键已锁定（同时长按 ←→ 2 秒解锁）"); break;
    case MSG_UNLOCKED:          snprintf(o, n, "按键已解锁"); break;
    case MSG_LOCKED_HINT:       snprintf(o, n, "按键已锁定：同时长按 ←→ 2 秒解锁"); break;
    case MSG_LOW_BATT:          snprintf(o, n, "电量低（%d mV），请充电", (int)b); break;
    case MSG_CHIP_REV_Y:        snprintf(o, n, "芯片为 rev Y：两路同时接收可能读错数据（ES0392 2.24.2）"); break;
    case MSG_NO_SD:             snprintf(o, n, "未插 SD 卡"); break;
    case MSG_SD_IN_MSC:         snprintf(o, n, "SD 卡正作为 U 盘使用"); break;
    case MSG_SD_ERROR:          snprintf(o, n, "SD 卡异常"); break;
    case MSG_SD_REPAIRED:       snprintf(o, n, "已修复上次未正常结束的录制文件"); break;
    case MSG_SD_WP:             snprintf(o, n, "SD 卡写保护，请拨开写保护开关"); break;
    case MSG_SD_UNSUPPORTED:    snprintf(o, n, "SD 卡文件系统不支持，请格式化"); break;
    case MSG_SD_FRAG:           snprintf(o, n, "SD 卡碎片过多，无法预分配，建议格式化"); break;
    case MSG_SD_FULL_NOREC:     snprintf(o, n, "SD 卡空间不足，无法开始录制"); break;
    case MSG_SD_REMOVED:        snprintf(o, n, "SD 卡已拔出"); break;
    case MSG_REC_STARTED:       snprintf(o, n, "开始录制（会话 %08d）", (int)b); break;
    case MSG_REC_STOPPED:       snprintf(o, n, "录制已停止，共 %d 帧", (int)b); break;
    case MSG_REC_MARK:          snprintf(o, n, "已打标记 #%d", (int)b); break;
    case MSG_REC_SEG_CFG:       snprintf(o, n, "CAN%d 参数已改，已切到新分段 %d", c, (int)b); break;
    case MSG_REC_SD_REMOVED:    snprintf(o, n, "录制中 SD 卡不可用，录制已停止；重新插卡后自动修复文件"); break;
    case MSG_REC_WRITE_RETRY:   snprintf(o, n, "SD 写入出错，重新初始化后继续（第 %d 次）", (int)b); break;
    case MSG_REC_WRITE_FAIL:    snprintf(o, n, "SD 写入多次失败，录制已停止，请换卡"); break;
    case MSG_REC_SD_FULL:       snprintf(o, n, "SD 卡已满，录制已停止"); break;
    case MSG_REC_DELETED_OLDEST:snprintf(o, n, "SD 卡已满，已删除最旧的未保护会话"); break;
    case MSG_REC_BUSY_CFG:      snprintf(o, n, "录制中不能修改录制设置"); break;
    case MSG_CH_LOW_RATE:       snprintf(o, n, "CAN%d 速率低于 50k，只能接收，不能发送", c); break;
    case MSG_CH_MODE:           snprintf(o, n, "CAN%d 已切到%s模式", c, mode_name((can_mode_t)b)); break;
    case MSG_CH_RATE:           fmt_ch_rate(r, sizeof r, ch); snprintf(o, n, "CAN%d 速率已改为 %s", c, r); break;
    case MSG_CH_BUSOFF:         snprintf(o, n, "CAN%d 进入 Bus-Off", c); break;
    case MSG_CH_RECOVERED:      snprintf(o, n, "CAN%d 已从 Bus-Off 恢复", c); break;
    case MSG_CH_BUSOFF_GIVEUP:  snprintf(o, n, "CAN%d 多次 Bus-Off，已停止自动恢复，请检查总线", c); break;
    case MSG_CH_NO_RATE:        snprintf(o, n, "CAN%d 未设置速率，请到诊断页自动识别", c); break;
    case MSG_AB_START:          snprintf(o, n, "CAN%d 开始自动识别（%s）", c, mode_name((can_mode_t)b)); break;
    case MSG_AB_OK:             fmt_ch_rate(r, sizeof r, ch); snprintf(o, n, "CAN%d 识别成功：%s", c, r); break;
    case MSG_AB_NO_TRAFFIC:     snprintf(o, n, "CAN%d 未识别：总线上没有流量，检查接线和终端电阻", c); break;
    case MSG_AB_FAIL:           snprintf(o, n, "CAN%d 未识别，检查速率范围和终端电阻", c); break;
    case MSG_AB_DENY_REC:       snprintf(o, n, "录制中不能自动识别波特率"); break;
    case MSG_AB_ABORTED:        snprintf(o, n, "CAN%d 自动识别已取消", c); break;
    case MSG_BUS_ABNORMAL:      snprintf(o, n, "CAN%d 错误很多但没有有效帧：速率可能不对，或只有一个发送节点（可试受限模式）", c); break;
    case MSG_TX_NOACK:          snprintf(o, n, "CAN%d 无应答，检查对端节点或终端电阻；已暂停 %d 条周期发送", c, (int)b); break;
    case MSG_TX_PAUSED:         snprintf(o, n, "CAN%d 不在正常模式，已暂停 %d 条周期发送", c, (int)b); break;
    case MSG_TX_LOAD_WARN:      snprintf(o, n, "CAN%d 周期发送负载 %d.%d%%，偏高", c, (int)b / 10, (int)b % 10); break;
    case MSG_TX_LOAD_DENY:      snprintf(o, n, "CAN%d 周期发送负载将达 %d.%d%%，超过 100%%，拒绝启动", c, (int)b / 10, (int)b % 10); break;
    case MSG_TX_DENY_BUSOFF:    snprintf(o, n, "CAN%d 处于 Bus-Off，不能发送", c); break;
    case MSG_PLAY_STARTED:      snprintf(o, n, "CAN%d 开始回放", c); break;
    case MSG_PLAY_DONE:         snprintf(o, n, "CAN%d 回放完成，共发送 %d 帧", c, (int)b); break;
    case MSG_PLAY_STOPPED:      snprintf(o, n, "CAN%d 回放已停止", c); break;
    case MSG_PLAY_SD_REMOVED:   snprintf(o, n, "SD 卡不可用，回放已停止"); break;
    case MSG_PLAY_BAD_FILE:     snprintf(o, n, "无法识别的文件格式（首行标识不对）"); break;
    case MSG_PLAY_FD_SKIPPED:   snprintf(o, n, "CAN%d 不是 FD 通道，跳过了 %d 个 FD 帧", c, (int)b); break;
    case MSG_PLAY_BUSY:         snprintf(o, n, "CAN%d 已在回放", c); break;
    case MSG_PLAY_PAUSED_MODE:  snprintf(o, n, "CAN%d 不在正常模式，回放已暂停", c); break;
    case MSG_MSC_ON:            snprintf(o, n, "SD 卡已交给电脑（U 盘模式）"); break;
    case MSG_MSC_OFF:           snprintf(o, n, "SD 卡已收回"); break;
    case MSG_MSC_DENY:          snprintf(o, n, "录制中或文件正在使用，不能进入 U 盘模式"); break;
    case MSG_MSC_NO_USB:        snprintf(o, n, "未连接 USB"); break;
    case MSG_FORMAT_DONE:       snprintf(o, n, "格式化完成"); break;
    case MSG_FORMAT_DENY:       snprintf(o, n, "录制中或文件正在使用，不能格式化"); break;
    case MSG_HOST_CFG_CHANGED:  snprintf(o, n, "CAN%d 设置已被上位机修改", c); break;
    case MSG_HOST_DENIED:       snprintf(o, n, "已拒绝上位机的请求"); break;
    case MSG_HOST_ON:           snprintf(o, n, "上位机已连接"); break;
    case MSG_HOST_OFF:          snprintf(o, n, "上位机已断开"); break;
    case MSG_FACTORY_RESET:     snprintf(o, n, "已恢复出厂设置"); break;
    case MSG_SELFTEST_OK:       snprintf(o, n, "自检完成：全部通过（模拟）"); break;
    case MSG_TIME_SYNCED:       snprintf(o, n, "时间已同步"); break;
    default:                    snprintf(o, n, "提示 %d", (int)m); break;
    }
}

static void ask_text(dlg_t *d)
{
    char r[24];
    int c = d->ch + 1;
    bool host = d->src == SRC_HOST || d->src == SRC_CLI;
    const char *who = d->src == SRC_CLI ? "命令行" : "上位机";
    switch (d->ask) {
    case ASK_CH_NORMAL:
        if (host)
            snprintf(d->text, TXT, "%s请求把 CAN%d 切到正常模式（会往总线发送）。允许？", who, c);
        else
            snprintf(d->text, TXT, "把 CAN%d 切到正常模式？正常模式会回 ACK、可以发送报文。", c);
        break;
    case ASK_TX_NORMAL:
        snprintf(d->text, TXT, "CAN%d 当前是%s模式。切到正常模式并发送这一帧？", c, mode_name(chan_get(d->ch)->mode));
        break;
    case ASK_CYC_NORMAL:
        snprintf(d->text, TXT, "CAN%d 当前是%s模式。切到正常模式并启动周期发送？", c, mode_name(chan_get(d->ch)->mode));
        break;
    case ASK_TX_FIRST:
        snprintf(d->text, TXT, "CAN%d 刚切到正常模式，确认发送第一帧？", c);
        break;
    case ASK_SEQ_NORMAL:
        snprintf(d->text, TXT, "CAN%d 当前是%s模式。切到正常模式并启动所选ID的循环发送？", c, mode_name(chan_get(d->ch)->mode));
        break;
    case ASK_SEQ_FIRST:
        snprintf(d->text, TXT, "CAN%d 刚切到正常模式。确认按已保存的次数和间隔循环发送所选ID？", c);
        break;
    case ASK_PLAN_START:
        snprintf(d->text, TXT, "执行方案 %.47s？\n%s · %s。确认发送？",
                 sp_state()->loaded_name,sp_state()->loaded_mask==3?"两路 CAN":sp_state()->loaded_mask==2?"CAN2":"CAN1",
                 sp_state()->run_once?"执行一轮后自动停止":"开始循环");
        break;
    case ASK_SCOPE_START:
        if(scope_state()->kind==SCOPE_SINGLE)
            snprintf(d->text,TXT,"确认向%s发送各自第%u条报文？所选通道使用各自的数据；将一起确认正常模式与首次发送权限。",
                     scope_state()->mask==3?"CAN1 + CAN2":scope_state()->mask==2?"CAN2":"CAN1",(unsigned)scope_state()->entry+1u);
        else
            snprintf(d->text,TXT,"确认启动%s的循环发送？所选通道各用自己的报文列表和时序，全部就绪后一起开始。",
                     scope_state()->mask==3?"CAN1 + CAN2":scope_state()->mask==2?"CAN2":"CAN1");
        break;
    case ASK_AB_RESTRICTED:
        snprintf(d->text, TXT, "CAN%d 只听模式识别失败，可能总线上只有一个发送节点。用受限模式再试一次？（会回 ACK）", c);
        break;
    case ASK_PLAY_CHMAP:
        snprintf(d->text, TXT, "文件里有 %d 个通道，只回放前 2 个（CH1→CAN1，CH2→CAN2），其余忽略。继续？", (int)d->b);
        break;
    case ASK_PLAY_RATE:
        fmt_rate(r, sizeof r, (uint32_t)d->b);
        snprintf(d->text, TXT, "文件速率 %s 和 CAN%d 当前设置不同。", r, c);
        break;
    case ASK_PLAY_NORMAL:
        if (chan_can_tx(d->ch))
            snprintf(d->text, TXT, "CAN%d 刚切到正常模式，确认开始文件回放并发送第一帧？", c);
        else
            snprintf(d->text, TXT, "回放要把 CAN%d 切到正常模式（会往总线发送）。继续？", c);
        break;
    case ASK_REC_STOP:
        snprintf(d->text, TXT, "%s请求停止正在进行的录制。允许？", who);
        break;
    case ASK_MSC:
        if (host)
            snprintf(d->text, TXT, "%s请求进入 U 盘模式（期间不能录制）。允许？", who);
        else
            snprintf(d->text, TXT, "把 SD 卡交给电脑当 U 盘？期间不能录制。");
        break;
    case ASK_FORMAT:
#ifdef CANFD_REVD_TARGET
        snprintf(d->text,TXT,"格式化 SD 卡？所有录制文件、发送方案与命名设置都会被删除。确认继续？");
#else
        snprintf(d->text,TXT,"格式化 SD 卡？清空模拟日志与数据库；已保存发送方案保留。确认继续？");
#endif
        break;
    default:
        snprintf(d->text, TXT, "确认？");
        break;
    }
}

static void log_add(const char *s)
{
    logbuf[log_pos].t = app_now_ms();
    snprintf(logbuf[log_pos].s, TXT, "%s", s);
    log_pos = (log_pos + 1) % LOG_LEN;
    if (log_n < LOG_LEN)
        log_n++;
}

static void show_toast(const char *s)
{
    snprintf(toast, sizeof toast, "%s", s);
    toast_until = app_now_ms() + TOAST_MS;
    log_add(s);
}

static void toast_msg(msg_t m, int ch, int32_t b)
{
    char t[TXT];
    msg_text(t, sizeof t, m, ch, b);
    show_toast(t);
}

/* ---------- 对话框 ---------- */
static void dlg_push(const dlg_t *d)
{
    if (n_dlg >= DLG_QUEUE)
        return;
    dlgq[n_dlg++] = *d;
    power_wake();                        /* 要用户确认的事，把屏幕点亮 */
}

static void dlg_pop(void)
{
    if (!n_dlg)
        return;
    for (int i = 1; i < n_dlg; i++)
        dlgq[i - 1] = dlgq[i];
    n_dlg--;
}

static const char *menu_opts[] = { "全部通道", "只看 CAN1", "只看 CAN2", "全部类型", "只看 FD", "只看错误", "只看发送" };
#define MENU_N 7
static const char *live_menu_opts[]={"切换滚动 / 按 ID","显示筛选","录制设置"};
#define LIVE_MENU_N 3
static int menu_count(const dlg_t *dialog){return dialog->id==MENU_LIVE_ACTIONS?LIVE_MENU_N:MENU_N;}

static void ask_confirm(const dlg_t *d)
{
    uint8_t ch = d->ch;
    src_t src = d->src;
    if (d->tx_bound) {
        int idx = d->ask == ASK_TX_FIRST ? d->b & 0xFF : d->b;
        if (ch >= CH_NUM || idx < 0 || idx >= TX_ENTRIES ||
            tx_get()->e[ch][idx].revision != d->tx_revision) {
            show_toast("发送条目已修改，请重新检查报文后发起发送");
            return;
        }
    }
    switch (d->ask) {
    case ASK_CH_NORMAL:
        evt_post4(EV_REQ_CH_MODE, ch, src, CAN_MODE_NORMAL, 1);
        break;
    case ASK_TX_NORMAL:
        evt_post4(EV_REQ_CH_MODE, ch, src, CAN_MODE_NORMAL, 2);
        evt_post4(EV_REQ_TX_SEND, ch, src, d->b, 1);
        break;
    case ASK_CYC_NORMAL:
        evt_post4(EV_REQ_CH_MODE, ch, src, CAN_MODE_NORMAL, 2);
        evt_post4(EV_REQ_TX_CYC, ch, src, d->b, 3);
        break;
    case ASK_TX_FIRST:
        if (d->b & 0x100)
            evt_post4(EV_REQ_TX_CYC, ch, src, d->b & 0xFF, 3);
        else
            evt_post4(EV_REQ_TX_SEND, ch, src, d->b, 1);
        break;
    case ASK_SEQ_NORMAL: case ASK_SEQ_FIRST: {
        int result = txseq_answer(ch, (uint32_t)d->b, true);
        if (result < 0) show_toast(txseq_result_text(result));
        break;
    }
    case ASK_PLAN_START: {
        int result=sp_answer((uint32_t)d->b,true);
        if(result<0)show_toast(sp_result_text(result));
        break;
    }
    case ASK_SCOPE_START: {
        int result=scope_answer((uint32_t)d->b,true);
        if(result<0)show_toast(scope_result_text(result));
        break;
    }
    case ASK_AB_RESTRICTED:
        evt_post4(EV_REQ_AUTOBAUD, ch, src, CAN_MODE_RESTRICTED, 1);
        break;
    case ASK_PLAY_CHMAP: case ASK_PLAY_RATE: case ASK_PLAY_NORMAL:
        evt_post4(EV_REQ_PLAY_ANSWER, ch, SRC_UI, d->ask, 1);
        break;
    case ASK_REC_STOP:
        evt_post4(EV_REQ_REC_STOP, 0, src, 0, 1);
        break;
    case ASK_MSC:
        evt_post4(EV_REQ_MSC, 0, src, 1, 1);
        break;
    case ASK_FORMAT:
        evt_post4(EV_REQ_FORMAT, 0, src, 0, 1);
        break;
    default:
        break;
    }
}

static void ask_cancel(const dlg_t *d)
{
    switch (d->ask) {
    case ASK_SEQ_NORMAL: case ASK_SEQ_FIRST:
        (void)txseq_answer(d->ch, (uint32_t)d->b, false);
        break;
    case ASK_PLAN_START:
        (void)sp_answer((uint32_t)d->b,false);
        break;
    case ASK_SCOPE_START:
        (void)scope_answer((uint32_t)d->b,false);
        break;
    case ASK_PLAY_CHMAP: case ASK_PLAY_RATE: case ASK_PLAY_NORMAL:
        evt_post4(EV_REQ_PLAY_ANSWER, d->ch, SRC_UI, d->ask, 0);
        break;
    default:
        if (d->src == SRC_HOST || d->src == SRC_CLI)
            toast_msg(MSG_HOST_DENIED, d->ch, 0);
        break;
    }
}

static void dlg_answer(bool ok, int opt)
{
    dlg_t d = dlgq[0];
    dlg_pop();
    switch (d.kind) {
    case DK_ASK:
        if (ok)
            ask_confirm(&d);
        else
            ask_cancel(&d);
        break;
    case DK_MENU:
        if(ok&&d.id==MENU_LIVE_ACTIONS){
            int selected=opt>=0?opt:d.cur;
            if(selected==0)(void)ui_live_set_view(live_view^1u);
            else if(selected==1)ui_filter_open();
            else if(selected==2)(void)ui_review_navigate(PG_REC);
        } else if (ok && d.id == MENU_LIVE_FILTER) {
            int sel = opt >= 0 ? opt : d.cur;
            disp_ch_filter_t fc;
            disp_type_filter_t ft;
            disp_get_filter(&fc, &ft);
            if (sel <= 2)
                fc = (disp_ch_filter_t)sel;
            else
                ft = (disp_type_filter_t)(sel - 3);
            disp_set_filter(fc, ft);
            cursor[PG_LIVE] = 0;
        }
        break;
    case DK_LOCAL:
        if (ok && d.id == LOCAL_FACTORY_RESET) {
            cfg_defaults();
            toast_msg(MSG_FACTORY_RESET, 0, 0);
        }
        break;
    case DK_INFO:
    default:
        break;
    }
}

/* ---------- 页面切换 ---------- */
static bool is_keysim_page(page_t p)
{
    return p == PG_KEYSIM || p == PG_KEYCH || p == PG_KEYLEARN || p == PG_KEYSCAN;
}

static void go(page_t p)
{
    /* Normal page navigation supersedes a non-editing catalogue overlay.
     * Keyboard drafts are guarded before their callers reach this function. */
    local_files_close();
    /* 进出按键模拟的任一页都先停学习 / 扫描并松开，接口回到高阻。 */
    if (p != page && (is_keysim_page(page) || is_keysim_page(p)))
        keysim_leave();
    page = p;
    numeric = false;
    if (p == PG_LIVE)
        disp_pause(false);
}

static void go_home(void)
{
    if (is_keysim_page(page))
        keysim_leave();
    disp_pause(false);
    editor.open = false;
    local_tx.open = false;
    local_files_close();
    detail_valid = false; filter_origin_valid = false;
    numeric = false;
    page = PG_HOME;
}

/* 每页条目数（给光标限幅） */
static int file_count(void) { return port_sd_file_count(); }

/* 按键模拟通道页的行布局：接口、按法、各键（没有键值表时一行提示）、手动、松开、学习、扫描、串联电阻。 */
typedef struct { int keys0, nkeys, manual, release, learn, scan, series, table, total; } keych_rows_t;
static keych_rows_t keych_rows(void)
{
    const keysim_t *k = keysim_get();
    const ks_table_t *t = &k->tables[k->ch];
    keych_rows_t r;
    r.keys0 = 2;
    r.nkeys = t->valid ? t->n : 0;
    r.manual = r.keys0 + (r.nkeys ? r.nkeys : 1);
    r.release = r.manual + 1;
    r.learn = r.release + 1;
    r.scan = r.learn + 1;
    r.series = r.scan + 1;
    r.table = r.series + 1;
    r.total = r.table + 1;
    return r;
}
static bool live_row_at(unsigned channel,unsigned ordinal,disp_row_t *out,uint32_t *source_back)
{
    if(!channel){if(source_back)*source_back=ordinal;return disp_row(ordinal,out);}
    for(uint32_t back=0;back<disp_rows_available();back++){
        disp_row_t row;if(!disp_row(back,&row))return false;
        if(row.ch+1u==channel&&ordinal--==0){*out=row;if(source_back)*source_back=back;return true;}
    }
    return false;
}
static const disp_id_t *live_id_at(unsigned channel,unsigned ordinal)
{
    if(!channel)return disp_id_at((int)ordinal);
    for(int index=0;index<disp_id_count();index++){
        const disp_id_t *entry=disp_id_at(index);
        if(entry&&entry->ch+1u==channel&&ordinal--==0)return entry;
    }
    return NULL;
}
static int live_count(unsigned channel,unsigned view)
{
    if(!channel)return view?disp_id_count():(int)disp_rows_available();
    int count=0;
    if(view){
        for(int index=0;index<disp_id_count();index++){
            const disp_id_t *entry=disp_id_at(index);if(entry&&entry->ch+1u==channel)count++;
        }
    } else for(uint32_t back=0;back<disp_rows_available();back++){
        disp_row_t row;if(disp_row(back,&row)&&row.ch+1u==channel)count++;
    }
    return count;
}
static bool live_index_for_back(unsigned channel,uint32_t source_back,unsigned *ordinal)
{
    disp_row_t row;
    if(!ordinal||!disp_row(source_back,&row)||(channel&&row.ch+1u!=channel))return false;
    if(!channel){*ordinal=source_back;return true;}
    unsigned count=0;
    for(uint32_t back=0;back<source_back;back++)
        if(disp_row(back,&row)&&row.ch+1u==channel)count++;
    *ordinal=count;return true;
}
static int files_tail_start(void)
{
    int count=file_count()+(int)sp_catalog_count()+(int)local_db_count();
    return count?count:1;
}

static int item_count(page_t p)
{
    switch (p) {
    case PG_HOME:   return HOME_COUNT;
    case PG_TX:
        return CH_NUM * TX_ENTRIES + TX_EXTRA_COUNT;
    case PG_TXEDIT:
        return 13;
    case PG_TXSET: return 12;
    case PG_TXTIMING: return 8;
    case PG_FILTER: return 9;
    case PG_DETAIL: return 5 + ((detail.frame.flags & CANF_RTR) ? 0 : ((detail.frame.len < DISP_DATA ? detail.frame.len : DISP_DATA) + 7) / 8);
    case PG_REC:    return 9;
    case PG_PLAY:
        return PLAN_FILES_START + (sp_catalog_count() ? (int)sp_catalog_count() : 1);
    case PG_LOGPLAY:return 0;
    case PG_SENDMODE:return 2;
    case PG_PROJECTFILES:return (sp_catalog_count()?(int)sp_catalog_count():1)+1;
    case PG_FILEDETAIL:return 7;
    case PG_PLANDETAIL:
        return PLAN_ENTRIES_START + (plan_selected.plan.channel_mask==3?2:1)*TX_ENTRIES;
    case PG_PLANENTRY:
        return 8 + ((plan_selected.plan.channels[plan_entry_ch].entries[plan_entry_index].flags&CANF_RTR)?0:
                    (int)(plan_selected.plan.channels[plan_entry_ch].entries[plan_entry_index].length+7u)/8);
    case PG_DIAG:   return 13;
    case PG_FILES:
        return files_tail_start()+2;
    case PG_CHSET:  return 9;
    case PG_SYSSET:
        return 11;
    case PG_ENG:    return 6;
    case PG_LIVE:
        return live_count(live_channel,live_view);
    case PG_KEYSIM: return keysim_available() ? keysim_get()->channels + 2 : 1;
    case PG_KEYCH:  return keych_rows().total;
    case PG_KEYLEARN: return 4 + keysim_get()->learn_n;
    case PG_KEYSCAN:  return 3 + keysim_get()->scan_n;
    default:        return 1;
    }
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static int find_u32(const uint32_t *a, int n, uint32_t v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v)
            return i;
    return 0;
}

/* ---------- 各页按键 ---------- */
static live_context_t live_context(void)
{
    return (live_context_t){.view=live_view,.channel=live_channel,.cursor=cursor[PG_LIVE],.paused=disp_paused()};
}
static void restore_live(live_context_t context)
{
    page = PG_LIVE; numeric = false;
    live_view = context.view;live_channel=context.channel;cursor[PG_LIVE] = context.cursor;
    int total = item_count(PG_LIVE);
    cursor[PG_LIVE] = clampi(cursor[PG_LIVE], 0, total > 0 ? total - 1 : 0);
    live_cursors[live_channel][live_view]=cursor[PG_LIVE];
    disp_pause(context.paused);
}
static const disp_id_t *summary_for(const disp_row_t *frame)
{
    if (frame->flags & CANF_ERR) return NULL;
    for (int i = 0; i < disp_id_count(); ++i) {
        const disp_id_t *entry = disp_id_at(i);
        if (entry->id == frame->id && entry->ch == frame->ch &&
            ((entry->flags ^ frame->flags) & (CANF_EXT|CANF_RTR|CANF_TX|CANF_FD)) == 0) return entry;
    }
    return NULL;
}
static bool detail_capture(int index)
{
    disp_row_t frame;
    const disp_id_t *summary;
    uint32_t source_back=0;
    if (live_view == 0) {
        if (!live_row_at(live_channel,(unsigned)index,&frame,&source_back)) return false;
        summary = summary_for(&frame);
    } else {
        summary = live_id_at(live_channel,(unsigned)index);
        if (!summary) return false;
        frame = (disp_row_t){.ts_us=summary->last_ts,.id=summary->id,
            .dt_same_us=summary->per_avg_us,.ch=summary->ch,.flags=summary->flags,.len=summary->len};
        memcpy(frame.data, summary->data, sizeof frame.data);
    }
    memset(&detail, 0, sizeof detail);
    detail.frame = frame; detail.index = index; detail.has_summary = summary != NULL;
    detail.has_sequence = live_view == 0 && disp_row_seq(source_back, &detail.seq);
    if (summary) {
        detail.count=summary->count; detail.avg_us=summary->per_avg_us;
        detail.min_us=summary->per_min_us; detail.max_us=summary->per_max_us;
    }
    return true;
}
static void info_frame(void)
{
    if (!detail_capture(cursor[PG_LIVE])) { show_toast("暂无可查看的报文"); return; }
    detail_origin = live_context(); detail_valid = true;
    disp_pause(true); cursor[PG_DETAIL] = 0; go(PG_DETAIL);
}
static void detail_back(void)
{
    live_context_t saved = detail_origin;
    detail_valid = false;
    restore_live(saved);
}
static void key_detail(key_id_t key, gesture_t gesture)
{
    if (key == KEY_BACK) { detail_back(); return; }
    if (gesture == GEST_LONG) return;
    if (key == KEY_UP || key == KEY_DOWN) {
        cursor[PG_DETAIL] = clampi(cursor[PG_DETAIL] + (key == KEY_DOWN ? 1 : -1), 0, item_count(PG_DETAIL)-1);
    } else if (key == KEY_LEFT || key == KEY_RIGHT) {
        int next = detail.index + (key == KEY_LEFT ? -1 : 1);
        if (next >= 0 && next < live_count(live_channel,live_view) && detail_capture(next)) {
            detail_origin.cursor = next; cursor[PG_DETAIL] = 0;
        }
    }
}
static int review_guard(void)
{
    if (!power_running() || !power_get()->screen_on) return UI_REVIEW_NOT_READY;
    if (power_get()->locked) return UI_REVIEW_LOCKED;
    if (n_dlg) return UI_REVIEW_DIALOG;
    return UI_REVIEW_OK;
}

bool ui_draft_open(void)
{
    if (editor.open || page == PG_FILTER) return true;
    return record_save.pending || local_tx.open || local_files_draft();
}

static void local_tx_load(int ch, int entry)
{
    const txseq_channel_t *q = txseq_get(ch);
    memset(&local_tx,0,sizeof local_tx);
    memcpy(local_tx.entries,q->entries,sizeof local_tx.entries);
    memcpy(local_tx.original,q->entries,sizeof local_tx.original);
    local_tx.entry_original = q->entries[entry];
    local_tx.id_gap_ms = local_tx.original_gap_ms = q->id_gap_ms;
    local_tx.revision = q->revision;
    for (int i=0;i<TX_ENTRIES;i++) local_tx.frame_revisions[i] = tx_get()->e[ch][i].revision;
    local_tx.ch = ch; local_tx.entry = entry; local_tx.open = true;
    local_tx_channel = (uint8_t)ch;
}
static void local_tx_changed(void)
{
    local_tx.dirty = local_tx.id_gap_ms != local_tx.original_gap_ms;
    for (int i=0;i<TX_ENTRIES;i++) {
        const txseq_entry_config_t *a=&local_tx.entries[i], *b=&local_tx.original[i];
        if (a->enabled!=b->enabled || a->repeat_count!=b->repeat_count ||
            a->frame_gap_ms!=b->frame_gap_ms || a->cycle_ms!=b->cycle_ms) local_tx.dirty = true;
    }
}
static void local_tx_close(void)
{
    cursor[PG_TX] = local_tx.ch * TX_ENTRIES + local_tx.entry;
    local_tx.open = false; numeric = false; go(PG_TX);
}

int ui_local_open(int kind, int ch, int idx)
{
    int result=review_guard(); if(result) return result;
    if (ui_draft_open()) return UI_REVIEW_DRAFT;
    if (kind==0 || kind==1) {
        if (ch<0 || ch>=CH_NUM || idx<0 || idx>=TX_ENTRIES) return UI_REVIEW_RANGE;
        local_files_close();
        local_tx_load(ch,idx);
        cursor[PG_TXSET]=0; cursor[PG_TXTIMING]=0;
        go(kind==0 ? PG_TXSET : PG_TXTIMING);
        return UI_REVIEW_OK;
    }
    if (kind>=2 && kind<=5) return local_files_open(kind,ch,idx,0);
    return UI_REVIEW_RANGE;
}

static const char *plan_name_saved(const char *name,void *context)
{
    (void)context;
    if(!plan_save_valid)return "保存快照已取消，请返回发送页重新保存";
    int result=sp_request_save(name,&plan_save_snapshot);
    if(result<0)return sp_result_text(result);
    plan_save_valid=false;
    send_files_return=PG_TX;
    page=PG_PROJECTFILES;cursor[PG_PROJECTFILES]=0;numeric=false;
    return NULL;
}
static void plan_name_cancelled(void *context)
{
    (void)context;
    plan_save_valid=false;
    if(page==PG_PLAY){page=plan_save_return;numeric=false;}
}
static int plan_save_open(uint32_t mask)
{
    int result=sp_capture(mask,&plan_save_snapshot);
    if(result<0){show_toast(sp_result_text(result));return result;}
    char name[SP_NAME_BYTES+1u];
    sp_default_name(mask,name,sizeof name);
    plan_save_return=page;plan_save_valid=true;
    go(PG_PLAY);
    result=local_text_open(mask==3?"保存两路发送方案":"保存当前屏发送方案",name,SP_NAME_BYTES,
                           plan_name_saved,plan_name_cancelled,NULL);
    if(result<0){plan_save_valid=false;go(plan_save_return);show_toast(local_files_result_text(result));}
    return result;
}

int ui_plan_open(int mode,int ch)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(mode<0||mode>2||ch<0||ch>=CH_NUM)return UI_REVIEW_RANGE;
    plan_save_channel=(uint8_t)ch;
    if(mode==1||mode==2)return plan_save_open(mode==2?3u:1u<<ch);
    send_files_return=PG_TX;
    cursor[PG_PROJECTFILES]=0;go(PG_PROJECTFILES);
    return UI_REVIEW_OK;
}

int ui_tx_select(int ch, int idx)
{
    int result = review_guard(); if (result) return result;
    if(ui_draft_open()) return UI_REVIEW_DRAFT;
    if (page != PG_TX) return UI_REVIEW_PAGE;
    if (ch < 0 || ch >= CH_NUM || idx < 0 || idx >= TX_ENTRIES) return UI_REVIEW_RANGE;
    cursor[PG_TX] = ch * TX_ENTRIES + idx;
    local_tx_channel = (uint8_t)ch;
    return UI_REVIEW_OK;
}
int ui_tx_scope_select(uint32_t mask)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_TX)return UI_REVIEW_PAGE;
    if(mask<1u||mask>3u)return UI_REVIEW_RANGE;
    if(tx_scope_mask!=mask){scope_cancel();tx_scope_mask=mask;}
    return UI_REVIEW_OK;
}
int ui_tx_target_select(uint32_t mask)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_TX)return UI_REVIEW_PAGE;
    if(mask<1u||mask>3u)return UI_REVIEW_RANGE;
    scope_cancel();tx_scope_mask=mask;
    if(mask==1u)local_tx_channel=0;
    else if(mask==2u)local_tx_channel=1;
    else if(local_tx_channel>=CH_NUM)local_tx_channel=0;
    cursor[PG_TX]=local_tx_channel*TX_ENTRIES;
    return UI_REVIEW_OK;
}
int ui_tx_channel_send(int ch,bool enabled)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_TX)return UI_REVIEW_PAGE;
    if(ch<0||ch>=CH_NUM)return UI_REVIEW_RANGE;
    uint32_t mask=1u<<(unsigned)ch;
    const txseq_channel_t *sequence=txseq_get(ch);
    const scope_status_t *scope=scope_state();
    bool running=sequence->state==TXSEQ_RUNNING;
    for(int index=0;index<TX_ENTRIES;index++)running=running||tx_get()->e[ch][index].active;
    bool scope_pending=scope->state!=SCOPE_IDLE&&(scope->mask&mask)!=0;
    bool sequence_pending=sequence->state==TXSEQ_CONFIRMING||sequence->state==TXSEQ_ARMING;
    if(enabled){
        if(running)return TXSEQ_OK;
        if(scope_pending)return scope->kind==SCOPE_GROUP?TXSEQ_WAIT_CONFIRM:TXSEQ_ERR_ACTIVE;
        if(sequence_pending)return TXSEQ_WAIT_CONFIRM;
        return scope_start(mask);
    }
    if(running||scope_pending||sequence_pending)scope_stop(mask);
    return TXSEQ_OK;
}
int ui_tx_single(int ch,int index)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_TX)return UI_REVIEW_PAGE;
    if(ch<0||ch>=CH_NUM||index<0||index>=TX_ENTRIES)return UI_REVIEW_RANGE;
    return scope_single(1u<<(unsigned)ch,(uint32_t)index);
}
int ui_review_navigate(page_t target)
{
    int result = review_guard(); if (result) return result;
    if(target==PG_SENDMODE)target=PG_TX;
    if ((unsigned)target >= PG_NUM || (target > PG_SYSSET
        && target!=PG_SENDMODE && target!=PG_PROJECTFILES && target!=PG_KEYSIM
        )) return UI_REVIEW_RANGE;
    if (target == PG_KEYSIM && !keysim_available()) return UI_REVIEW_RANGE;
    if (ui_draft_open()) {
        show_toast("请先保存或取消当前草稿"); return UI_REVIEW_DRAFT;
    }
    local_files_close();
    if(target==PG_PROJECTFILES&&page!=PG_PROJECTFILES)send_files_return=PG_TX;
    if(target==PG_PLAY && page!=PG_PLAY && page!=PG_LOGPLAY && page!=PG_PLANDETAIL && page!=PG_PLANENTRY)
        plan_save_channel=local_tx_channel;
    if (target == page) return UI_REVIEW_OK;
    if(target==PG_LIVE&&page==PG_REC&&rec_origin_valid){restore_live(rec_origin);rec_origin_valid=false;return UI_REVIEW_OK;}
    if(target==PG_REC){rec_origin=live_context();rec_origin_valid=true;}
    if (page == PG_DETAIL && detail_valid) {
        detail_back();
        if (target == PG_LIVE) return UI_REVIEW_OK;
    }
    if (target != PG_LIVE && target!=PG_REC) disp_pause(false);
    if (target == PG_HOME) go_home(); else go(target);
    return UI_REVIEW_OK;
}
int ui_review_filter_open(void)
{
    int result = review_guard(); if (result) return result;
    if (page != PG_LIVE) return UI_REVIEW_PAGE;
    ui_filter_open(); return UI_REVIEW_OK;
}
int ui_live_set_view(unsigned view)
{
    int result = review_guard(); if (result) return result;
    if (page != PG_LIVE) return UI_REVIEW_PAGE;
    if (view > 1u) return UI_REVIEW_RANGE;
    if (live_view != view) {
        live_cursors[live_channel][live_view]=cursor[PG_LIVE];live_view=(uint8_t)view;
        int count=live_count(live_channel,live_view);
        cursor[PG_LIVE]=clampi(live_cursors[live_channel][live_view],0,count>0?count-1:0);
    }
    return UI_REVIEW_OK;
}
int ui_live_set_channel(unsigned channel)
{
    int result=review_guard();if(result)return result;
    if(page!=PG_LIVE)return UI_REVIEW_PAGE;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(channel>2u)return UI_REVIEW_RANGE;
    if(live_channel!=channel){
        live_cursors[live_channel][live_view]=cursor[PG_LIVE];live_channel=(uint8_t)channel;
        int count=live_count(live_channel,live_view);
        cursor[PG_LIVE]=clampi(live_cursors[live_channel][live_view],0,count>0?count-1:0);
    }
    return UI_REVIEW_OK;
}
int ui_live_scroll(int rows)
{
    int result=review_guard();if(result)return result;
    if(page!=PG_LIVE)return UI_REVIEW_PAGE;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(rows < -128 || rows > 128)return UI_REVIEW_RANGE;
    if(!rows)return UI_REVIEW_OK;
    int count=live_count(live_channel,live_view);
    if(!live_view&&rows>0&&count>0)disp_pause(true);
    cursor[PG_LIVE]=clampi(cursor[PG_LIVE]+rows,0,count>0?count-1:0);
    live_cursors[live_channel][live_view]=cursor[PG_LIVE];return UI_REVIEW_OK;
}
int ui_live_set_paused(bool paused)
{
    int result = review_guard(); if (result) return result;
    if (page != PG_LIVE) return UI_REVIEW_PAGE;
    disp_pause(paused);
    if (!paused) cursor[PG_LIVE]=0;
    return UI_REVIEW_OK;
}
int ui_live_open_detail(unsigned index)
{
    int result = review_guard(); if (result) return result;
    if (page != PG_LIVE) return UI_REVIEW_PAGE;
    if (index >= (unsigned)item_count(PG_LIVE)) return UI_REVIEW_RANGE;
    cursor[PG_LIVE]=(int)index; info_frame();
    return detail_valid ? UI_REVIEW_OK : UI_REVIEW_RANGE;
}
int ui_live_open_detail_seq(uint32_t sequence)
{
    int result = review_guard(); if (result) return result;
    if (page != PG_LIVE || live_view != 0) return UI_REVIEW_PAGE;
    uint32_t back;unsigned ordinal;
    if (!disp_row_back_for_seq(sequence,&back)) return UI_REVIEW_RANGE;
    if(!live_index_for_back(live_channel,back,&ordinal))return UI_REVIEW_RANGE;
    return ui_live_open_detail(ordinal);
}

static void key_live(key_id_t k, gesture_t g)
{
    int *c = &cursor[PG_LIVE];
    int n = item_count(PG_LIVE);
    switch (k) {
    case KEY_LEFT: case KEY_RIGHT:
        if(g==GEST_SHORT)(void)ui_live_set_channel(k==KEY_LEFT?1u:2u);
        break;
    case KEY_UP:
        if (live_view == 0) {
            if (n > 0 && *c < n - 1)
                (*c)++;
            disp_pause(true);            /* 往回翻就暂停跟随（F5.1） */
        } else if (*c > 0) {
            (*c)--;
        }
        break;
    case KEY_DOWN:
        if (live_view == 0) {
            if (*c > 0)
                (*c)--;
        } else if (*c < n - 1) {
            (*c)++;
        }
        break;
    case KEY_OK:
        if (g == GEST_LONG) {
            dlg_t menu={.kind=DK_MENU,.id=MENU_LIVE_ACTIONS};
            snprintf(menu.text,sizeof menu.text,"CAN%u 显示与录制操作",live_channel?live_channel:1u);dlg_push(&menu);
        } else if (g == GEST_SHORT) {
            info_frame();
        }
        break;
    case KEY_BACK:
        if (g == GEST_SHORT) {
            if (disp_paused()) {
                disp_pause(false);       /* 暂停中先恢复跟随 */
                *c = 0;
            } else {
                go_home();
            }
        }
        break;
    default:
        break;
    }
}

void ui_filter_open(void)
{
    filter_origin = live_context(); filter_origin_valid = true;
    disp_filter_get_ex(&filter_draft);
    cursor[PG_FILTER] = 0;
    go(PG_FILTER);
}

bool ui_play_select(int ch, int idx)
{
    (void)ch;(void)idx;
    return false; /* Captured-log replay has been removed from the device UI. */
}

bool ui_tx_edit_open(int ch, int idx)
{
    if (ui_draft_open() || n_dlg || ch < 0 || ch >= CH_NUM || idx < 0 || idx >= TX_ENTRIES) return false;
    const tx_entry_t *e = &tx_get()->e[ch][idx];
    local_tx_channel=(uint8_t)ch;
    memset(&editor, 0, sizeof editor);
    editor.frame = e->f;
    editor.period = e->period_ms;
    editor.revision = e->revision;
    editor.ch = ch;
    editor.idx = idx;
    editor.open = true;
    cursor[PG_TXEDIT] = 0;
    go(PG_TXEDIT);
    return true;
}

int ui_detail_to_tx(int ch,int idx)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_DETAIL||!detail_valid)return UI_REVIEW_PAGE;
    if(ch<0||ch>=CH_NUM||idx<0||idx>=TX_ENTRIES)return UI_REVIEW_RANGE;
    const disp_row_t *source=&detail.frame;
    if((source->flags&CANF_ERR)||(!(source->flags&CANF_RTR)&&source->len>DISP_DATA)) {
        show_toast("错误事件或未完整缓存的记录，不能复制为发送报文");return UI_REVIEW_RANGE;
    }
    can_frame_t draft={.ch=(uint8_t)ch,.id=source->id,.len=source->len,
        .flags=(uint8_t)(source->flags&(CANF_EXT|CANF_FD|CANF_BRS|CANF_RTR))};
    if(!(draft.flags&CANF_RTR))memcpy(draft.data,source->data,draft.len);
    if(tx_validate_frame(ch,&draft,false)!=TX_OK){show_toast("此记录不是可发送的 CAN 报文");return UI_REVIEW_RANGE;}
    if(!ui_tx_edit_open(ch,idx))return UI_REVIEW_DRAFT;
    editor.frame=draft;editor.period=0;editor.dirty=true;
    cursor[PG_TX]=ch*TX_ENTRIES+idx;
    detail_valid=false;disp_pause(false);
    show_toast("已复制到本地发送草稿；尚未保存、未发送");
    return UI_REVIEW_OK;
}

void ui_tx_edit_cancel(void)
{
    editor.open = false;
    numeric = false;
    if (page == PG_TXEDIT) go(PG_TX);
}

/* Numeric fields share digit editing: OK enters/exits, left/right moves the
 * digit and up/down changes it. This avoids thousands of presses for an ID. */
static bool numeric_field(uint32_t *value, uint32_t *maximum, int *base, int *digits)
{
    int c = cursor[page];
    *base = 16; *digits = 8; *maximum = 0x1FFFFFFFu;
    if (page==PG_TXSET && c==1) {
        *value=local_tx.id_gap_ms; *maximum=TXSEQ_MAX_TIME_MS; *base=10; *digits=7; return true;
    }
    if (page==PG_TXTIMING && c>=2 && c<=4) {
        const txseq_entry_config_t *q=&local_tx.entries[local_tx.entry];
        *value=c==2?q->repeat_count:c==3?q->frame_gap_ms:q->cycle_ms;
        *maximum=c==2?TXSEQ_MAX_COUNT:TXSEQ_MAX_TIME_MS; *base=10; *digits=c==2?5:7; return true;
    }
    if (page == PG_FILTER && (c == 2 || c == 3)) {
        *value = c == 2 ? filter_draft.id : filter_draft.mask;
        return true;
    }
    if (page != PG_TXEDIT) return false;
    if (c == 0) {
        *value = editor.frame.id;
        if (!(editor.frame.flags & CANF_EXT)) { *maximum = 0x7FFu; *digits = 3; }
    }
    else if(c==5){*value=editor.frame.len;*maximum=(editor.frame.flags&CANF_FD)?64:8;*base=10;*digits=2;}
    else if (c == 6) {
        *value = editor.period; *maximum = UINT16_MAX; *base = 10; *digits = 5;
    } else if (c == 7) {
        *value = editor.byte; *maximum = editor.frame.len ? editor.frame.len - 1u : 0;
        *base = 10; *digits = 2;
    } else if (c == 8 && editor.frame.len && !(editor.frame.flags & CANF_RTR)) {
        *value = editor.frame.data[editor.byte]; *maximum = 255; *digits = 2;
    } else return false;
    return true;
}

static void numeric_write(uint32_t value)
{
    int c = cursor[page];
    if (page==PG_TXSET) {
        local_tx.id_gap_ms=value; local_tx_changed(); return;
    }
    if (page==PG_TXTIMING) {
        txseq_entry_config_t *q=&local_tx.entries[local_tx.entry];
        if(c==2)q->repeat_count=value; else if(c==3)q->frame_gap_ms=value; else if(c==4)q->cycle_ms=value;
        local_tx_changed(); return;
    }
    if (page == PG_FILTER) {
        if (c == 2) filter_draft.id = value;
        else filter_draft.mask = value;
    } else {
        if (c == 0) editor.frame.id = value;
        else if(c==5){editor.frame.len=(uint8_t)value;if(editor.byte>=value)editor.byte=value?(uint8_t)(value-1):0;}
        else if (c == 6) editor.period = value;
        else if (c == 7) editor.byte = (uint8_t)value;
        else editor.frame.data[editor.byte] = (uint8_t)value;
        editor.dirty = true;
    }
}

static bool numeric_key(key_id_t k, gesture_t g)
{
    uint32_t value, maximum;
    int base, digits;
    if (!numeric || !numeric_field(&value, &maximum, &base, &digits)) return false;
    if (g == GEST_LONG) return true;
    if (k == KEY_OK) numeric = false;
    else if (k == KEY_BACK) { numeric_write(numeric_original); numeric = false; }
    else if (k == KEY_LEFT && numeric_digit) numeric_digit--;
    else if (k == KEY_RIGHT && numeric_digit < digits - 1) numeric_digit++;
    else if (k == KEY_UP || k == KEY_DOWN) {
        uint32_t step = 1;
        for (int i = numeric_digit + 1; i < digits; i++) step *= (uint32_t)base;
        if (k == KEY_UP) value = value > maximum - (step > maximum ? maximum : step) ? maximum : value + step;
        else value = value < step ? 0 : value - step;
        if (value > maximum) value = maximum;
        numeric_write(value);
    }
    return true;
}

static bool local_tx_current(void);
static void hexstr(char *o,size_t n,const uint8_t *data,int length,int maximum);
static bool field_current(void)
{
    if(!field_input.open||page!=field_input.page||cursor[page]!=field_input.row||review_guard())return false;
    if(page==PG_TXEDIT)return editor.open&&editor.ch==field_input.ch&&editor.idx==field_input.entry&&editor.revision==field_input.revision&&tx_get()->e[editor.ch][editor.idx].revision==editor.revision;
    if(page==PG_TXSET||page==PG_TXTIMING)return local_tx.open&&local_tx.ch==field_input.ch&&local_tx.entry==field_input.entry&&local_tx.revision==field_input.revision&&local_tx_current()&&txseq_get(local_tx.ch)->state==TXSEQ_IDLE;
    return page==PG_FILTER;
}
static int input_hex(unsigned char c){return c>='0'&&c<='9'?c-'0':c>='A'&&c<='F'?c-'A'+10:c>='a'&&c<='f'?c-'a'+10:-1;}
static const char *field_saved(const char *text,void *context)
{
    (void)context;
    if(!field_current())return "原报文或设置已变化；请取消输入后重新打开，草稿保留";
    if(field_input.payload){
        uint8_t bytes[64]={0};unsigned count=0;int high=-1;
        for(const unsigned char *p=(const unsigned char*)text;*p;p++){
            if(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')continue;
            int digit=input_hex(*p);if(digit<0)return "数据只能填写完整十六进制字节";
            if(high<0)high=digit;else{if(count>=64)return "数据最多64字节，草稿保留";bytes[count++]=(uint8_t)(high*16+digit);high=-1;}
        }
        if(high>=0||count!=editor.frame.len)return "数据字节数必须与当前长度完全一致，草稿保留";
        memcpy(editor.frame.data,bytes,sizeof bytes);editor.dirty=true;field_input.open=false;return NULL;
    }
    if(!*text)return "请输入数值，空输入不会修改草稿";
    uint32_t value=0;
    for(const unsigned char *p=(const unsigned char*)text;*p;p++){
        int digit=input_hex(*p);
        if(digit<0||digit>=field_input.base||(uint32_t)digit>field_input.maximum||value>(field_input.maximum-(uint32_t)digit)/(uint32_t)field_input.base)
            return "数值超出此字段允许范围，草稿保留";
        value=value*(uint32_t)field_input.base+(uint32_t)digit;
    }
    if(page==PG_TXEDIT&&field_input.row==5){can_frame_t frame=editor.frame;frame.len=(uint8_t)value;if(tx_validate_frame(editor.ch,&frame,false)!=TX_OK)return "长度须为当前帧格式的合法DLC，草稿保留";}
    if(page==PG_TXTIMING){
        txseq_entry_config_t row=local_tx.entries[local_tx.entry];
        if(field_input.row==2)row.repeat_count=value;else if(field_input.row==3)row.frame_gap_ms=value;else row.cycle_ms=value;
        if(!row.repeat_count||!row.cycle_ms||(row.repeat_count>1&&!row.frame_gap_ms)||row.cycle_ms<=(uint64_t)(row.repeat_count-1)*row.frame_gap_ms)
            return "次数至少1；总周期须大于(次数-1)×同ID间隔，草稿保留";
    }
    numeric_write(value);field_input.open=false;return NULL;
}
static void field_cancelled(void *context){(void)context;field_input.open=false;}
static bool field_begin(bool payload)
{
    uint32_t value=0,maximum=0;int base=16,digits=0;
    if(payload){if(page!=PG_TXEDIT||!editor.open)return false;if(editor.frame.flags&CANF_RTR){show_toast("RTR帧没有数据载荷");return true;}}
    else if(!numeric_field(&value,&maximum,&base,&digits))return false;
    char initial[LOCAL_INPUT_MAX_BYTES+1u],title[48];
    if(payload){hexstr(initial,sizeof initial,editor.frame.data,editor.frame.len,64);snprintf(title,sizeof title,"完整数据（%u字节）",editor.frame.len);}
    else {
        snprintf(initial,sizeof initial,base==16?"%X":"%u",(unsigned)value);
        const char *name=page==PG_FILTER?(cursor[page]==2?"匹配 ID":"ID 掩码"):
            page==PG_TXSET?"不同 ID 间隔（ms）":page==PG_TXTIMING?(cursor[page]==2?"每轮次数":cursor[page]==3?"同 ID 间隔（ms）":"单 ID 总周期（ms）"):
            cursor[page]==0?"报文 ID（十六进制）":cursor[page]==5?"报文长度（字节）":cursor[page]==6?"独立周期（ms）":cursor[page]==7?"数据字节位置":"数据字节值（十六进制）";
        snprintf(title,sizeof title,"%s",name);
    }
    field_input.page=page;field_input.row=cursor[page];field_input.payload=payload;field_input.maximum=maximum;field_input.base=base;
    field_input.ch=page==PG_TXEDIT?editor.ch:local_tx.ch;field_input.entry=page==PG_TXEDIT?editor.idx:local_tx.entry;
    field_input.revision=page==PG_TXEDIT?editor.revision:local_tx.revision;field_input.open=true;
    int result=local_input_open(payload?LOCAL_INPUT_PAYLOAD:base==16?LOCAL_INPUT_HEX:LOCAL_INPUT_DECIMAL,title,initial,payload?LOCAL_INPUT_MAX_BYTES:base==16?8u:10u,field_saved,field_cancelled,NULL);
    if(result){field_input.open=false;show_toast(local_files_result_text(result));}
    return true;
}
static bool numeric_begin(void)
{
    return field_begin(false);
}

static void key_editor(key_id_t k, gesture_t g)
{
    if (numeric_key(k, g)) return;
    if (g == GEST_LONG) return;
    int *c = &cursor[PG_TXEDIT];
    if (k == KEY_BACK) { ui_tx_edit_cancel(); return; }
    if (k == KEY_UP || k == KEY_DOWN) { *c = clampi(*c + (k == KEY_DOWN ? 1 : -1), 0, item_count(page)-1); return; }
    if (k != KEY_OK && k != KEY_LEFT && k != KEY_RIGHT) return;
    if(k==KEY_OK&&*c==12){(void)field_begin(true);return;}
    if (k == KEY_OK && numeric_begin()) return;
    int d = k == KEY_LEFT ? -1 : 1;
    can_frame_t *f = &editor.frame;
    if (*c == 1) {
        if ((f->flags & CANF_EXT) && f->id > 0x7FF) { show_toast("请先把 ID 改为 7FF 以内再选择标准帧"); return; }
        f->flags ^= CANF_EXT;
    } else if (*c == 2) {
        f->flags ^= CANF_FD;
        if (f->flags & CANF_FD) f->flags &= (uint8_t)~CANF_RTR;
        else { f->flags &= (uint8_t)~CANF_BRS; if (f->len > 8) f->len = 8; }
    } else if (*c == 3) {
        if (!(f->flags & CANF_FD)) { show_toast("BRS 需要先开启 CAN FD"); return; }
        f->flags ^= CANF_BRS;
    } else if (*c == 4) {
        f->flags ^= CANF_RTR;
        if (f->flags & CANF_RTR) { f->flags &= (uint8_t)~(CANF_FD | CANF_BRS); if (f->len > 8) f->len = 8; }
    } else if (*c == 5) {
        static const uint8_t sizes[] = { 0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64 };
        int i = 0, n = (f->flags & CANF_FD) ? 16 : 9;
        while (i < n - 1 && sizes[i] < f->len) i++;
        f->len = sizes[clampi(i + d, 0, n - 1)];
    } else if (*c == 9 && k == KEY_OK) {
        show_toast("保存后按输入字节发送；原计数器和校验字段将关闭"); return;
    } else if (*c == 10 && k == KEY_OK) {
        tx_result_t r = tx_get()->e[editor.ch][editor.idx].revision != editor.revision ? TX_ERR_CHANGED :
            tx_entry_update(editor.ch, editor.idx, f->id, f->flags, f->len, f->data, editor.period);
        if (r != TX_OK) { show_toast(tx_result_text(r)); return; }
        ui_tx_edit_cancel();
        show_toast("发送条目已保存；数据按输入值发送");
        return;
    } else if (*c == 11 && k == KEY_OK) { ui_tx_edit_cancel(); return; }
    else return;
    if (editor.byte >= f->len) editor.byte = f->len ? (uint8_t)(f->len - 1) : 0;
    editor.dirty = true;
}

static void key_filter(key_id_t k, gesture_t g)
{
    if (numeric_key(k, g)) return;
    if (g == GEST_LONG) return;
    int *c = &cursor[PG_FILTER];
    if (k == KEY_BACK) { restore_live(filter_origin); filter_origin_valid=false; return; }
    if (k == KEY_UP || k == KEY_DOWN) { *c = clampi(*c + (k == KEY_DOWN ? 1 : -1), 0, 8); return; }
    if (k != KEY_OK && k != KEY_LEFT && k != KEY_RIGHT) return;
    if (k == KEY_OK && numeric_begin()) return;
    int d = k == KEY_LEFT ? -1 : 1;
    if (*c == 0) filter_draft.channel = (disp_ch_filter_t)((filter_draft.channel + d + DF_CH_NUM) % DF_CH_NUM);
    else if (*c == 1) filter_draft.type = (disp_type_filter_t)((filter_draft.type + d + DF_T_NUM) % DF_T_NUM);
    else if (*c == 4) filter_draft.direction = (disp_direction_filter_t)((filter_draft.direction + d + DF_DIR_NUM) % DF_DIR_NUM);
    else if (*c == 5) filter_draft.format = (disp_format_filter_t)((filter_draft.format + d + DF_FMT_NUM) % DF_FMT_NUM);
    else if (*c == 6 && k == KEY_OK) {
        disp_filter_t previous; disp_filter_get_ex(&previous);
        bool changed = previous.channel != filter_draft.channel || previous.type != filter_draft.type ||
            previous.id != filter_draft.id || previous.mask != filter_draft.mask ||
            previous.direction != filter_draft.direction || previous.format != filter_draft.format;
        (void)disp_filter_set_ex(&filter_draft);
        live_context_t saved=filter_origin;
        if (changed) { saved.cursor=0; saved.paused=false; }
        restore_live(saved); filter_origin_valid=false;
        show_toast(changed ? "筛选已应用并恢复跟随；录制保留原始报文" : "显示筛选未变更");
    } else if (*c == 7 && k == KEY_OK) memset(&filter_draft, 0, sizeof filter_draft);
    else if (*c == 8 && k == KEY_OK) { restore_live(filter_origin); filter_origin_valid=false; }
}

static bool local_tx_writable(void)
{
    if(txseq_get(local_tx.ch)->state==TXSEQ_IDLE) return true;
    show_toast("请先停止本路循环，再修改本地设置"); return false;
}
static bool local_tx_current(void)
{
    if(txseq_get(local_tx.ch)->revision!=local_tx.revision) return false;
    for(int i=0;i<TX_ENTRIES;i++)
        if(tx_get()->e[local_tx.ch][i].revision!=local_tx.frame_revisions[i]) return false;
    return true;
}
static void local_tx_save(void)
{
    if(!local_tx_current()) {show_toast("设置或发送条目已变化；草稿保留，请取消后重新打开");return;}
    int result=txseq_configure(local_tx.ch,local_tx.entries,local_tx.id_gap_ms,local_tx.revision);
    if(result<0){show_toast(txseq_result_text(result));return;}
    local_tx_close(); show_toast("本地循环设置已保存；尚未启动发送");
}
static void local_tx_defaults(void)
{
    if(!local_tx_writable()) return;
    for(int i=0;i<TX_ENTRIES;i++) {
        txseq_entry_config_t *q=&local_tx.entries[i];
        uint64_t minimum=(uint64_t)(q->repeat_count ? q->repeat_count-1 : 0)*TXSEQ_DEFAULT_FRAME_GAP_MS+1;
        q->frame_gap_ms=TXSEQ_DEFAULT_FRAME_GAP_MS;
        q->cycle_ms=(uint32_t)(minimum>TXSEQ_DEFAULT_CYCLE_MS?minimum:TXSEQ_DEFAULT_CYCLE_MS);
    }
    local_tx.id_gap_ms=TXSEQ_DEFAULT_ID_GAP_MS; local_tx_changed();
    show_toast("本路默认时间已填入草稿；次数、启用和报文数据保留");
}
static void local_tx_defaults_all(void)
{
    if(local_tx.dirty){show_toast("请先保存或取消当前草稿，再恢复两路默认时间");return;}
    if(!local_tx_current()){show_toast("设置已变化，请取消后重新打开");return;}
    uint32_t revisions[CH_NUM];
    for(int ch=0;ch<CH_NUM;ch++) {
        const txseq_channel_t *q=txseq_get(ch);
        if(q->state!=TXSEQ_IDLE){show_toast("请先停止两路循环，再恢复全部默认时间");return;}
        revisions[ch]=q->revision;
    }
    for(int ch=0;ch<CH_NUM;ch++) (void)txseq_defaults(ch,revisions[ch]);
    int ch=local_tx.ch,entry=local_tx.entry;local_tx_load(ch,entry);
    show_toast("两路默认时间已保存；次数、启用、ID 和数据保留");
}
static void key_local_tx(key_id_t k,gesture_t g)
{
    if(numeric_key(k,g)) return;
    if(g==GEST_LONG){if(k==KEY_BACK)show_toast("短按 BACK 返回或取消本地草稿");return;}
    int *c=&cursor[page];
    if(k==KEY_BACK) {
        if(page==PG_TXTIMING){cursor[PG_TXSET]=local_tx.entry+2;go(PG_TXSET);}
        else local_tx_close();
        return;
    }
    if(k==KEY_UP||k==KEY_DOWN){*c=clampi(*c+(k==KEY_DOWN?1:-1),0,item_count(page)-1);return;}
    if(k!=KEY_OK&&k!=KEY_LEFT&&k!=KEY_RIGHT)return;
    int direction=k==KEY_LEFT?-1:1;
    if(page==PG_TXSET) {
        if(*c==0) {
            if(local_tx.dirty){show_toast("请先保存或取消本路草稿，再切换通道");return;}
            int ch=(local_tx.ch+direction+CH_NUM)%CH_NUM;local_tx_load(ch,0);
        } else if(*c==1) {if(local_tx_writable()&&k==KEY_OK)(void)numeric_begin();}
        else if(*c>=2&&*c<2+TX_ENTRIES&&k==KEY_OK) {
            local_tx.entry=*c-2;local_tx.entry_original=local_tx.entries[local_tx.entry];
            cursor[PG_TXTIMING]=0;go(PG_TXTIMING);
        } else if(k==KEY_OK) {
            if(*c==6)local_tx_save();
            else if(*c==7)local_tx_defaults();
            else if(*c==8)local_tx_defaults_all();
            else if(*c==9) {
                if(local_tx.dirty){show_toast("本地循环草稿尚未保存");return;}
                if(!local_tx_current()){show_toast("设置已变化，请取消后重新打开");return;}
                int result=txseq_start(local_tx.ch,local_tx.revision);
                show_toast(result<0?txseq_result_text(result):result==1?"请确认通道工作模式与发送权限":"本路分组循环已启动");
            } else if(*c==10) {
                txseq_stop(local_tx.ch);
                for(int i=0;i<TX_ENTRIES;i++)evt_post4(EV_REQ_TX_CYC,(uint8_t)local_tx.ch,SRC_UI,i,0);
                show_toast("本路发送已停止；另一通道保持原状态");
            } else if(*c==11)local_tx_close();
        }
    } else {
        if(*c==0) {
            local_tx.entry=(local_tx.entry+direction+TX_ENTRIES)%TX_ENTRIES;
            local_tx.entry_original=local_tx.entries[local_tx.entry];
        } else if(*c==1) {
            if(local_tx_writable()) {local_tx.entries[local_tx.entry].enabled=!local_tx.entries[local_tx.entry].enabled;local_tx_changed();}
        } else if(*c>=2&&*c<=4) {if(local_tx_writable()&&k==KEY_OK)(void)numeric_begin();}
        else if(k==KEY_OK) {
            if(*c==5){cursor[PG_TXSET]=local_tx.entry+2;go(PG_TXSET);}
            else if(*c==6)local_tx_save();
            else if(*c==7){local_tx.entries[local_tx.entry]=local_tx.entry_original;local_tx_changed();cursor[PG_TXSET]=local_tx.entry+2;go(PG_TXSET);}
        }
    }
}

static void key_tx(key_id_t k, gesture_t g)
{
    int c = cursor[PG_TX];
#ifdef CANFD_REVD_TARGET
    /* DBC/DBF import belongs on the PC; its legacy menu row is absent here.
     * Keep the remaining host action numbers as an internal dispatch table. */
    if(c>=CH_NUM*TX_ENTRIES+2)c++;
#endif
    bool is_entry = c < CH_NUM * TX_ENTRIES;
    int ch = c / TX_ENTRIES, idx = c % TX_ENTRIES;
    if (g == GEST_LONG) {
        if (k == KEY_OK && is_entry) ui_tx_edit_open(ch, idx);
        return;
    }
    if (k == KEY_OK) {
        if (is_entry) {
            int result=scope_single(tx_scope_mask,(uint32_t)idx);
            if(result<0)show_toast(scope_result_text(result));
        }
        else if(c==CH_NUM*TX_ENTRIES)
            evt_post4(EV_REQ_TX_STOP_ALL, 0, SRC_UI, 0, 0);
        else if(c==CH_NUM*TX_ENTRIES+1) {
            (void)ui_local_open(0,local_tx_channel,0);
        } else if(c==CH_NUM*TX_ENTRIES+2) {
            (void)ui_local_open(2,local_tx_channel,0);
        } else if(c==CH_NUM*TX_ENTRIES+3) {
            (void)ui_plan_open(tx_scope_mask==3?2:1,local_tx_channel);
        }
        else if(c==CH_NUM*TX_ENTRIES+4)local_tx_channel^=1u;
        else if(c==CH_NUM*TX_ENTRIES+5)(void)ui_tx_scope_select(tx_scope_mask%3u+1u);
        else if(c==CH_NUM*TX_ENTRIES+6){int result=scope_start(tx_scope_mask);if(result<0)show_toast(scope_result_text(result));}
        else if(c==CH_NUM*TX_ENTRIES+7){scope_stop(tx_scope_mask);show_toast("已停止发送范围内的通道");}
        else if(c==CH_NUM*TX_ENTRIES+8)(void)ui_plan_open(0,local_tx_channel);
    } else if (k == KEY_RIGHT && is_entry && g == GEST_SHORT) {
        bool on = !tx_get()->e[ch][idx].active;
        if (on) {
            tx_result_t valid = tx_validate_frame(ch, &tx_get()->e[ch][idx].f, true);
            if (valid != TX_OK) { show_toast(tx_result_text(valid)); return; }
        }
        evt_post4(EV_REQ_TX_CYC, (uint8_t)ch, SRC_UI, idx, on ? 1 : 0);
    } else if (k == KEY_LEFT && g == GEST_SHORT) {
        if(c==CH_NUM*TX_ENTRIES+4)local_tx_channel^=1u;
        else if(c==CH_NUM*TX_ENTRIES+5)(void)ui_tx_scope_select((tx_scope_mask+1u)%3u+1u);
        else
        evt_post4(EV_REQ_TX_STOP_ALL, 0, SRC_UI, 0, 0);
    }
    else if(k==KEY_RIGHT&&g==GEST_SHORT&&c==CH_NUM*TX_ENTRIES+4)local_tx_channel^=1u;
    else if(k==KEY_RIGHT&&g==GEST_SHORT&&c==CH_NUM*TX_ENTRIES+5)(void)ui_tx_scope_select(tx_scope_mask%3u+1u);
}

static void key_rec(key_id_t k, gesture_t g)
{
    int c = cursor[PG_REC];
    cfg_t *cf = cfg_mut();
    bool idle = rec_get()->state == REC_IDLE;
    if(k==KEY_OK&&g==GEST_SHORT&&c==8){(void)ui_local_open(3,0,0);return;}
    if (k == KEY_OK && g == GEST_SHORT && c == 0) {
        if(rec_get()->state==REC_STOPPING)return;
        if (idle)
            evt_post4(EV_REQ_REC_START, 0, SRC_UI, 0, 0);
        else
            evt_post4(EV_REQ_REC_STOP, 0, SRC_UI, 0, 1);
        if(rec_origin_valid)restore_live(rec_origin);else go(PG_LIVE);
        return;
    }
    if ((k != KEY_LEFT && k != KEY_RIGHT) || g == GEST_LONG || c < 1 || c > 3)
        return;
    if (!idle) {
        toast_msg(MSG_REC_BUSY_CFG, 0, 0);
        return;
    }
    int d = k == KEY_RIGHT ? 1 : -1;
    if (c == 1) {
        static const uint8_t masks[] = { 3, 1, 2 };
        int i = cf->rec_ch_mask == 3 ? 0 : cf->rec_ch_mask == 1 ? 1 : 2;
        cf->rec_ch_mask = masks[(i + d + 3) % 3];
    } else if (c == 2) {
        cf->rec_seg_idx = (uint8_t)((cf->rec_seg_idx + d + REC_SEG_OPTIONS) % REC_SEG_OPTIONS);
    } else {
        cf->rec_full_delete = !cf->rec_full_delete;
    }
}

static bool plan_entry_at(int ordinal,int *channel,int *entry_index)
{
    for(int ch=0;ch<CH_NUM;ch++)if(plan_selected.plan.channel_mask&(1u<<ch))
        for(int index=0;index<TX_ENTRIES;index++)if(ordinal--==0){*channel=ch;*entry_index=index;return true;}
    return false;
}
static void plan_start_result(bool once)
{
    int result=once?sp_start_once():sp_start();
    show_toast(result<0?sp_result_text(result):result==SP_WAIT_CONFIRM?"请在设备屏幕确认方案发送权限":
               once?"正在执行发送方案的一整轮":"已开始循环执行发送方案");
}
static void plan_select(unsigned index,page_t return_page)
{
    if(!sp_catalog_get(index,&plan_selected)){show_toast("方案目录已变化，请刷新后重选");return;}
    plan_selected_index=index;plan_detail_return=return_page;cursor[PG_PLANDETAIL]=4;go(PG_PLANDETAIL);
}
static bool plan_load_ready(void)
{
    if(scope_state()->state!=SCOPE_IDLE||sp_state()->io_pending){show_toast(sp_result_text(SP_ERR_BUSY));return false;}
    return true;
}
static void plan_loaded_ui(uint32_t mask)
{
    scope_cancel();tx_scope_mask=mask;
    if(!(mask&(1u<<local_tx_channel)))local_tx_channel=(uint8_t)((mask&1u)?0:1);
    plan_save_channel=local_tx_channel;cursor[PG_TX]=local_tx_channel*TX_ENTRIES;
}
static int plan_load_into_tx(unsigned index,uint32_t expected_token)
{
    if(!plan_load_ready())return SP_ERR_BUSY;
    sp_file_t file;
    if(!sp_catalog_get(index,&file)||(expected_token&&file.token!=expected_token)){
        show_toast("方案目录已变化，请刷新后重选");return SP_ERR_CHANGED;
    }
    int result=sp_load(index,file.token);
    if(result<0){show_toast(sp_result_text(result));return result;}
    plan_selected=file;plan_selected_index=index;
    plan_loaded_ui(file.plan.channel_mask);go(PG_TX);
    show_toast("发送文件已加载，所含通道已替换；尚未发送");
    return UI_REVIEW_OK;
}
int ui_plan_file_select(unsigned index,uint32_t token)
{
    int result=review_guard();if(result)return result;
    if(ui_draft_open())return UI_REVIEW_DRAFT;
    if(page!=PG_PROJECTFILES)return UI_REVIEW_PAGE;
    if(!token)return UI_REVIEW_RANGE;
    return plan_load_into_tx(index,token);
}
static void key_send_mode(key_id_t key,gesture_t gesture)
{
    if(key!=KEY_OK||gesture!=GEST_SHORT)return;
    if(cursor[PG_SENDMODE]==0)(void)ui_plan_open(0,local_tx_channel);
    else if(cursor[PG_SENDMODE]==1)go(PG_TX);
}
static void key_send_files(key_id_t key,gesture_t gesture)
{
    if(key!=KEY_OK||gesture!=GEST_SHORT)return;
    int selected=cursor[PG_PROJECTFILES],count=(int)sp_catalog_count(),tail=count?count:1;
    if(selected<count)(void)plan_load_into_tx((unsigned)selected,0);
    else if(selected==tail){
        int result=sp_request_refresh();show_toast(result<0?sp_result_text(result):"正在刷新SD中的发送文件");
    }
}
static void key_plan(key_id_t key,gesture_t gesture)
{
    if(gesture==GEST_LONG)return;
    int selected=cursor[PG_PLAY];
    if(selected==0&&(key==KEY_LEFT||key==KEY_RIGHT||key==KEY_OK)){plan_save_channel^=1;return;}
    if(key!=KEY_OK)return;
    if(selected==1 || selected==2){
        if(sp_state()->io_pending){show_toast("文件操作尚未完成，请稍后再保存");return;}
        (void)plan_save_open(selected==2?3u:1u<<plan_save_channel);
    } else if(selected==3){
        int result=sp_request_refresh();show_toast(result<0?sp_result_text(result):"正在重新读取SD中的发送方案");
    } else if(selected==5)plan_start_result(true);
    else if(selected==6)plan_start_result(false);
    else if(selected==7){sp_stop();show_toast("已停止已加载方案的相关通道，其他通道不变");}
    else if(selected>=PLAN_FILES_START){
        unsigned index=(unsigned)(selected-PLAN_FILES_START);
        plan_select(index,PG_PLAY);
    }
}
static void key_plan_detail(key_id_t key,gesture_t gesture)
{
    if(key!=KEY_OK||gesture!=GEST_SHORT)return;
    int selected=cursor[PG_PLANDETAIL];
    if(selected==4||selected==5||selected==6){
        if(!plan_load_ready())return;
        int result=sp_load(plan_selected_index,plan_selected.token);
        if(result<0){show_toast(sp_result_text(result));return;}
        plan_loaded_ui(plan_selected.plan.channel_mask);
        if(selected==4)show_toast("方案已加载到手动发送列表，尚未发送");
        else plan_start_result(selected==5);
    } else if(selected==7){sp_stop();show_toast("已停止已加载方案的相关通道");}
    else if(selected==8)go(plan_detail_return);
    else if(selected>=PLAN_ENTRIES_START){
        int channel,entry_index;
        if(!plan_entry_at(selected-PLAN_ENTRIES_START,&channel,&entry_index))return;
        plan_entry_ch=(uint8_t)channel;plan_entry_index=(uint8_t)entry_index;
        cursor[PG_PLANENTRY]=0;go(PG_PLANENTRY);
    }
}
static bool selected_log(file_info_t *info)
{
    for(int index=0;index<file_count();index++)
        if(file_port_token(index)==log_selected_token&&port_sd_file_info(index,info)){
            log_selected_index=index;return true;
        }
    return false;
}
static bool log_file_busy(const file_info_t *info)
{
    return storage_get()->state!=ST_READY||info->writing||play_file_in_use(log_selected_index);
}
static void key_file_detail(key_id_t key,gesture_t gesture)
{
    if(key!=KEY_OK||gesture!=GEST_SHORT)return;
    int selected=cursor[PG_FILEDETAIL];
    if(selected==6){go(PG_FILES);return;}
    file_info_t info;
    if(!selected_log(&info)){show_toast("文件已变化，请返回文件列表重新选择");return;}
    if(selected<4)return;
    if(log_file_busy(&info)){show_toast("文件正忙或SD不可用，稍后重试");return;}
    if(selected==4)evt_post4(EV_REQ_PROTECT,0,SRC_UI,log_selected_index,0);
    else if(selected==5){
        int result=local_files_open(4,0,log_selected_index,log_selected_token);
        if(result<0)show_toast(local_files_result_text(result));
    }
}

static void key_diag(key_id_t k, gesture_t g)
{
    int c = cursor[PG_DIAG];
    if (k != KEY_OK || g != GEST_SHORT)
        return;
    switch (c) {
    case 8: case 9:   evt_post4(EV_REQ_AUTOBAUD, (uint8_t)(c - 8), SRC_UI, CAN_MODE_LISTEN, 0); break;
    case 10: case 11: evt_post4(EV_REQ_CH_RECOVER, (uint8_t)(c - 10), SRC_UI, 0, 0); break;
    case 12:          stats_clear(0); stats_clear(1); disp_clear_ids(); break;
    default:          break;
    }
}

static void key_files(key_id_t k, gesture_t g)
{
    int c = cursor[PG_FILES];
    if (k != KEY_OK || g != GEST_SHORT)
        return;
    int logs=file_count(),plans=(int)sp_catalog_count(),databases=(int)local_db_count(),tail=files_tail_start();
    if(c<logs){
        log_selected_index=c;log_selected_token=file_port_token(c);cursor[PG_FILEDETAIL]=4;go(PG_FILEDETAIL);
    } else if(c<logs+plans)plan_select((unsigned)(c-logs),PG_FILES);
    else if(c<logs+plans+databases){
        char name[48];uint32_t frames;
        if(local_db_file_info((unsigned)(c-logs-plans),name,sizeof name,&frames)){
            dlg_t dialog={.kind=DK_INFO};
            snprintf(dialog.text,TXT,"%.47s：数据库包含 %u 帧定义，只读预览。可从手动编辑报文的本地 SD 数据库选帧。",name,(unsigned)frames);
            dlg_push(&dialog);
        }
    } else if(c==tail)evt_post4(EV_REQ_MSC,0,SRC_UI,storage_get()->state==ST_MSC?0:1,0);
    else if(c==tail+1)evt_post4(EV_REQ_FORMAT,0,SRC_UI,0,0);
}

static void key_chset(key_id_t k, gesture_t g)
{
    int c = cursor[PG_CHSET];
    if (g == GEST_LONG || (k != KEY_LEFT && k != KEY_RIGHT && k != KEY_OK))
        return;
    int d = k == KEY_LEFT ? -1 : 1;
    const cfg_t *cf = cfg_get();
    if (c == 8) {
        cfg_mut()->busoff_auto = !cf->busoff_auto;
        return;
    }
    int ch = c / 4, item = c % 4;
    const chan_t *s = chan_get(ch);
    switch (item) {
    case 0: {
        int m = ((int)s->mode + d + 3) % 3;
        evt_post4(EV_REQ_CH_MODE, (uint8_t)ch, SRC_UI, m, 0);
        break;
    }
    case 1: {
        int i = find_u32(nbr_presets, N_OF(nbr_presets), cf->nbr[ch]);
        i = clampi(i + d, 0, N_OF(nbr_presets) - 1);
        evt_post4(EV_REQ_CH_RATE, (uint8_t)ch, SRC_UI, (int32_t)nbr_presets[i], 0);
        break;
    }
    case 2:
        evt_post4(EV_REQ_CH_FD, (uint8_t)ch, SRC_UI, !cf->fd[ch], 0);
        break;
    case 3: {
        int i = find_u32(dbr_presets, N_OF(dbr_presets), cf->dbr[ch]);
        i = clampi(i + d, 0, N_OF(dbr_presets) - 1);
        evt_post4(EV_REQ_CH_RATE, (uint8_t)ch, SRC_UI, 0, (int32_t)dbr_presets[i]);
        break;
    }
    default:
        break;
    }
}

static int find_u16(const uint16_t *a, int n, uint16_t v)
{
    for (int i = 0; i < n; i++)
        if (a[i] == v)
            return i;
    return 0;
}

static void key_sysset(key_id_t k, gesture_t g)
{
    int c = cursor[PG_SYSSET];
    cfg_t *cf = cfg_mut();
    if (g == GEST_LONG)
        return;
    int d = k == KEY_LEFT ? -1 : 1;
    bool change = k == KEY_LEFT || k == KEY_RIGHT || k == KEY_OK;
    if (!change)
        return;
    switch (c) {
    case 0: cf->screen_off_s = screen_off_opts[(find_u16(screen_off_opts, 4, cf->screen_off_s) + d + 4) % 4]; power_activity(); break;
    case 1: cf->auto_off_min = auto_off_opts[(find_u16(auto_off_opts, 4, cf->auto_off_min) + d + 4) % 4]; power_activity(); break;
    case 2: {
        int i = 0;
        for (int j = 0; j < N_OF(backlight_opts); j++)
            if (backlight_opts[j] == cf->backlight_pct)
                i = j;
        cf->backlight_pct = backlight_opts[(i + d + N_OF(backlight_opts)) % N_OF(backlight_opts)];
        port_backlight(cf->backlight_pct);
        break;
    }
    case 3:
        if (k == KEY_OK) {
            port_rtc_mark_synced();      /* 固件里是时间编辑器；模拟器里直接按"已校时"处理 */
            toast_msg(MSG_TIME_SYNCED, 0, 0);
        }
        break;
    case 7:
        if (k == KEY_OK) {
            dlg_t dl = { .kind = DK_LOCAL, .id = LOCAL_FACTORY_RESET };
            snprintf(dl.text, TXT, "恢复出厂设置？会清除通道、过滤、发送列表等配置；序列号、校准值和录制文件保留。");
            dlg_push(&dl);
        }
        break;
    case 8:
        if (k == KEY_OK)
            go(PG_ENG);
        break;
    case 10:
        if(k==KEY_OK)(void)ui_review_navigate(PG_REC);
        break;
    default:
        break;
    }
}

static void key_eng(key_id_t k, gesture_t g)
{
    int c = cursor[PG_ENG];
    if (k != KEY_OK || g != GEST_SHORT)
        return;
    if (c == 0)
        toast_msg(MSG_SELFTEST_OK, 0, 0);
    else if (c == 5)
        go_home();
}

/* ---------- 按键模拟 ---------- */
static void fmt_volt(char *o, size_t n, uint16_t mv)
{
    snprintf(o, n, "%u.%02u V", (unsigned)(mv / 1000u), (unsigned)(mv % 1000u / 10u));
}

/* 设备字库没有 Ω，阻值写成“欧 / 千欧”。 */
static void fmt_ohm_cn(char *o, size_t n, float ohm)
{
    if (ohm != ohm)
        snprintf(o, n, "—");
    else if (ohm > 1e7f)
        snprintf(o, n, "开路");
    else if (ohm >= 1000.0f)
        snprintf(o, n, "%.2f 千欧", (double)(ohm / 1000.0f));
    else
        snprintf(o, n, "%.0f 欧", (double)ohm);
}

static void ks_report(winsim_status_t rc, const char *ok_text)
{
    switch (rc) {
    case WINSIM_OK:              if (ok_text) show_toast(ok_text); break;
    case WINSIM_ERR_OVERVOLTAGE: show_toast("接口过压，已断开电阻阵；排除后在总览里重新检测"); break;
    case WINSIM_ERR_NO_SUPPLY:   show_toast("APM 未上电：先在发送报文页发唤醒报文"); break;
    case WINSIM_ERR_VERIFY_FAIL: show_toast("回读和目标相差超过 80 mV，检查接线"); break;
    case WINSIM_ERR_CALIBRATION: show_toast("串联电阻标定失败，检查保险丝和接线"); break;
    default:                     show_toast("现在不能执行这一步"); break;
    }
}

static void key_keysim(key_id_t k, gesture_t g)
{
    const keysim_t *ks = keysim_get();
    int c = cursor[PG_KEYSIM];
    char t[96];
    if (k != KEY_OK || g != GEST_SHORT || !ks->available)
        return;
    if (c < ks->channels) {
        winsim_status_t rc = keysim_select((uint8_t)c);
        if (rc == WINSIM_OK) {
            snprintf(t, sizeof t, "已接通 K%d，串联电阻 %.1f 欧", c + 1, (double)ks->r_series[c]);
            show_toast(t);
        } else if (rc == WINSIM_ERR_NO_SUPPLY) {
            snprintf(t, sizeof t, "已接通 K%d；APM 未上电，串联电阻没有标定", c + 1);
            show_toast(t);
        } else {
            ks_report(rc, NULL);
        }
        if (rc != WINSIM_ERR_OVERVOLTAGE && rc != WINSIM_ERR_PARAM) {
            cursor[PG_KEYCH] = 2;
            go(PG_KEYCH);
        }
    } else if (c == ks->channels) {
        if (ks->ovp)
            ks_report(keysim_clear_ovp(), "电压正常，已解除过压锁定");
    } else {
        keysim_release();
        show_toast("已全部松开，接口回到高阻");
    }
}

static void key_keych(key_id_t k, gesture_t g)
{
    const keysim_t *ks = keysim_get();
    keych_rows_t r = keych_rows();
    int c = cursor[PG_KEYCH];
    char t[96];
    if (!ks->available)
        return;
    if (k == KEY_LEFT || k == KEY_RIGHT) {
        if (g == GEST_LONG)
            return;
        if (c == 1)
            keysim_set_press_mode(ks->press_mode == KS_TAP ? KS_LATCH : KS_TAP);
        else if (c == r.manual)
            keysim_manual_adjust(k == KEY_RIGHT ? 100 : -100);
        return;
    }
    if (k != KEY_OK || g != GEST_SHORT)
        return;
    if (c == 1) {
        keysim_set_press_mode(ks->press_mode == KS_TAP ? KS_LATCH : KS_TAP);
    } else if (c >= r.keys0 && c < r.keys0 + r.nkeys) {
        ks_report(keysim_press(c - r.keys0), NULL);
    } else if (c == r.keys0 && r.nkeys == 0) {
        show_toast("先学习或扫描，得到这一路的键值表");
    } else if (c == r.manual) {
        ks_report(keysim_manual_toggle(), NULL);
    } else if (c == r.release) {
        keysim_release();
        show_toast("已全部松开，接口回到高阻");
    } else if (c == r.learn) {
        cursor[PG_KEYLEARN] = 0;
        go(PG_KEYLEARN);
    } else if (c == r.scan) {
        cursor[PG_KEYSCAN] = 0;
        go(PG_KEYSCAN);
    } else if (c == r.table) {
        if (keysim_resave())
            show_toast("正在重新写入 SD 卡");
        else if (ks->tables[ks->ch].valid && ks->sd_saved[ks->ch])
            show_toast("这张键值表已经在 SD 卡上");
    } else if (c == r.series) {
        winsim_status_t rc = keysim_recalibrate();
        if (rc == WINSIM_OK) {
            snprintf(t, sizeof t, "K%u 串联电阻 %.1f 欧", (unsigned)ks->ch + 1u, (double)ks->r_series[ks->ch]);
            show_toast(t);
        } else {
            ks_report(rc, NULL);
        }
    }
}

static void key_keylearn(key_id_t k, gesture_t g)
{
    const keysim_t *ks = keysim_get();
    int c = cursor[PG_KEYLEARN], n = ks->learn_n;
    char t[96];
    if (k != KEY_OK || g != GEST_SHORT || !ks->available)
        return;
    if (c == 0) {
        if (ks->activity == KS_LEARNING) {
            keysim_learn_stop();
            show_toast("已暂停学习，记录保留");
        } else {
            ks_report(keysim_learn_start(), "开始学习：按住原车开关的一个档位");
        }
    } else if (c >= 2 && c < 2 + n) {
        keysim_learn_rename(c - 2);
    } else if (c == 2 + n) {
        keysim_learn_clear();
        show_toast("已清空，重新学习");
    } else if (c == 3 + n) {
        if (keysim_learn_save()) {
            snprintf(t, sizeof t, "已保存 %d 个键值到 K%u，正在写入 SD 卡", n, (unsigned)ks->ch + 1u);
            show_toast(t);
            cursor[PG_KEYCH] = 2;
            go(PG_KEYCH);
        } else {
            show_toast("还没有记录到键值");
        }
    }
}

static void key_keyscan(key_id_t k, gesture_t g)
{
    const keysim_t *ks = keysim_get();
    int c = cursor[PG_KEYSCAN], n = ks->scan_n;
    char t[96];
    if (k != KEY_OK || g != GEST_SHORT || !ks->available)
        return;
    if (c == 0) {
        if (ks->activity == KS_SCANNING) {
            keysim_scan_stop();
            show_toast("已中止扫描，接口回到高阻");
        } else {
            winsim_status_t rc = keysim_scan_start();
            if (rc == WINSIM_ERR_PARAM && !keysim_state_known())
                show_toast("读不到 APM 开关状态：先配置 CAN 开关状态信号");
            else
                ks_report(rc, "开始扫描，约 4 秒");
        }
    } else if (c == 2 + n) {
        if (keysim_scan_save()) {
            snprintf(t, sizeof t, "已保存 %u 个键值到 K%u（每档取中点），正在写入 SD 卡",
                     (unsigned)ks->tables[ks->ch].n, (unsigned)ks->ch + 1u);
            show_toast(t);
            cursor[PG_KEYCH] = 2;
            go(PG_KEYCH);
        } else {
            show_toast(ks->activity == KS_SCANNING ? "扫描还没完成" : "还没有扫描结果");
        }
    }
}

static void activate_home(void)
{
    go(home_tiles[clampi(cursor[PG_HOME],0,HOME_COUNT-1)]);
}

static void page_key(key_id_t k, gesture_t g)
{
    if(record_save.pending&&!editor.open&&!local_tx.open&&page!=PG_FILTER&&!local_files_draft()){
        show_toast("正在关闭录制文件，请稍候命名保存");return;
    }
    if(local_files_active()) {
        local_files_key(k,g);
        if(!local_files_active() && local_files_notice()[0]) show_toast(local_files_notice());
        return;
    }
    if(page==PG_TXSET||page==PG_TXTIMING){key_local_tx(k,g);return;}
    int *c = &cursor[page];
    int n = item_count(page);

    if (k == KEY_BACK && g == GEST_LONG) {
        go_home();
        return;
    }
    if (page == PG_TXEDIT) { key_editor(k, g); return; }
    if (page == PG_FILTER) { key_filter(k, g); return; }
    if (page == PG_DETAIL) { key_detail(k, g); return; }
    if (page == PG_LIVE) {
        key_live(k, g);
        return;
    }
    if (k == KEY_BACK) {
        if (g == GEST_SHORT && page != PG_HOME) {
            if(page==PG_PLANENTRY){go(PG_PLANDETAIL);return;}
            if(page==PG_PLANDETAIL){go(plan_detail_return);return;}
            if(page==PG_FILEDETAIL){go(PG_FILES);return;}
            if(page==PG_KEYCH){go(PG_KEYSIM);return;}
            if(page==PG_KEYLEARN||page==PG_KEYSCAN){go(PG_KEYCH);return;}
            if(page==PG_PROJECTFILES){go(send_files_return);return;}
            if(page==PG_TX){go_home();return;}
            if(page==PG_REC){if(rec_origin_valid)restore_live(rec_origin);else go(PG_LIVE);rec_origin_valid=false;return;}
            go(page == PG_ENG ? PG_SYSSET : PG_HOME);
        }
        return;
    }
    if (page == PG_HOME) {
        if (g == GEST_LONG)
            return;
        switch (k) {
        case KEY_LEFT:  *c = clampi(*c - 1, 0, HOME_COUNT-1); break;
        case KEY_RIGHT: *c = clampi(*c + 1, 0, HOME_COUNT-1); break;
        case KEY_UP:    if (*c >= HOME_COLUMNS) *c -= HOME_COLUMNS; break;
        case KEY_DOWN:  if (*c+HOME_COLUMNS<HOME_COUNT) *c += HOME_COLUMNS; break;
        case KEY_OK:    activate_home(); break;
        default: break;
        }
        return;
    }
    if (k == KEY_UP) {
        *c = clampi(*c - 1, 0, n - 1);
        if(page==PG_TX&&*c<CH_NUM*TX_ENTRIES)local_tx_channel=(uint8_t)(*c/TX_ENTRIES);
        return;
    }
    if (k == KEY_DOWN) {
        *c = clampi(*c + 1, 0, n - 1);
        if(page==PG_TX&&*c<CH_NUM*TX_ENTRIES)local_tx_channel=(uint8_t)(*c/TX_ENTRIES);
        return;
    }
    switch (page) {
    case PG_TX:     key_tx(k, g); break;
    case PG_REC:    key_rec(k, g); break;
    case PG_PLAY:
        key_plan(k,g);break;
    case PG_SENDMODE:key_send_mode(k,g);break;
    case PG_PROJECTFILES:key_send_files(k,g);break;
    case PG_FILEDETAIL:key_file_detail(k,g);break;
    case PG_PLANDETAIL:key_plan_detail(k,g);break;
    case PG_PLANENTRY:
        if(k==KEY_OK&&g==GEST_SHORT&&cursor[PG_PLANENTRY]==item_count(PG_PLANENTRY)-1)go(PG_PLANDETAIL);
        break;
    case PG_DIAG:   key_diag(k, g); break;
    case PG_FILES:  key_files(k, g); break;
    case PG_CHSET:  key_chset(k, g); break;
    case PG_SYSSET: key_sysset(k, g); break;
    case PG_ENG:    key_eng(k, g); break;
    case PG_KEYSIM: key_keysim(k, g); break;
    case PG_KEYCH:  key_keych(k, g); break;
    case PG_KEYLEARN: key_keylearn(k, g); break;
    case PG_KEYSCAN:  key_keyscan(k, g); break;
    default: break;
    }
}

static bool rec_key(gesture_t g)
{
    if(g!=GEST_SHORT&&g!=GEST_LONG)return false;
    rec_state_t s = rec_get()->state;
    if(s==REC_IDLE)return evt_post4(EV_REQ_REC_START,0,SRC_UI,0,0);
    if(s==REC_RUNNING||s==REC_PREPARING)return evt_post4(EV_REQ_REC_STOP,0,SRC_UI,0,1);
    return false;
}

static void record_save_arm(void)
{
    const rec_t *rec=rec_get();
    if(record_save.pending||!power_running()||rec->state!=REC_STOPPING||rec->last_result!=MSG_REC_STOPPED)return;
    uint32_t token=file_port_record_token(rec->session);
    if(!token)return;
    record_save.pending=true;record_save.session=rec->session;record_save.token=token;
    if(page==PG_REC){if(rec_origin_valid)restore_live(rec_origin);else go(PG_LIVE);}
}
static void record_save_poll(void)
{
    if(!record_save.pending)return;
    const rec_t *rec=rec_get();
    if(!power_running()||rec->session!=record_save.session){record_save.pending=false;return;}
    if(rec->state!=REC_IDLE)return;
    if(rec->last_result!=MSG_REC_STOPPED){record_save.pending=false;return;}
    /* Existing edits must finish first. The pending flag still blocks a new
     * recording and navigation, while their own save/cancel keys remain usable. */
    if(!power_get()->screen_on||power_get()->locked||n_dlg||editor.open||local_tx.open||page==PG_FILTER||local_files_draft())return;
    int result=local_record_save_open(record_save.session,record_save.token);
    if(result==0){record_save.pending=false;return;}
    if(result==-1||result==-2||result==-3||result==-4||result==LOCAL_FILES_ERR_SD)return;
    record_save.pending=false;show_toast("未打开录制命名，默认文件保留；可到文件管理检查");
}

static void dlg_key(key_id_t k, gesture_t g)
{
    dlg_t *d = &dlgq[0];
    if (g == GEST_LONG)
        return;
    switch (k) {
    case KEY_OK:   dlg_answer(true, -1); break;
    case KEY_BACK: dlg_answer(false, -1); break;
    case KEY_UP:   if (d->kind == DK_MENU && d->cur > 0) d->cur--; break;
    case KEY_DOWN: if (d->kind == DK_MENU && d->cur < menu_count(d) - 1) d->cur++; break;
    default: break;
    }
}

/* ---------- 事件入口 ---------- */
void ui_init(void)
{
    page = PG_HOME;
    memset(cursor, 0, sizeof cursor);
    live_view = 0;
    live_channel=1;memset(live_cursors,0,sizeof live_cursors);
    n_dlg = 0;
    toast[0] = '\0';
    toast_until = 0;
    host_edit_ch = -1;
    memset(&editor, 0, sizeof editor);
    memset(&local_tx,0,sizeof local_tx);local_tx_channel=0;tx_scope_mask=1;
    local_files_init();
    plan_save_channel=0;plan_save_valid=false;plan_save_return=PG_TX;
    memset(&plan_selected,0,sizeof plan_selected);plan_selected_index=0;plan_entry_ch=plan_entry_index=0;
    plan_detail_return=PG_PROJECTFILES;send_files_return=PG_TX;memset(&record_save,0,sizeof record_save);memset(&field_input,0,sizeof field_input);
    log_selected_index=-1;log_selected_token=0;
    rec_origin_valid=false;
    detail_valid = filter_origin_valid = false; memset(&detail, 0, sizeof detail);
    numeric = false;
}

void ui_handle(const app_evt_t *e)
{
    local_files_handle_event(e);
    if(e->type==EV_SYS_STOP_ALL)record_save.pending=false;
    record_save_poll();
    switch (e->type) {
    case EV_REQ_REC_STOP:
        if(e->src==SRC_UI)record_save_arm();
        return;
    case EV_SYS_STOP_ALL:
        local_tx.open=false;
        if(page==PG_TXSET||page==PG_TXTIMING){numeric=false;page=PG_TX;}
        break;
    case EV_SYS_STARTED:
        local_tx.open=false;
        page = e->a ? PG_ENG : PG_HOME;
        for (int ch = 0; ch < CH_NUM; ch++)
            if (!cfg_get()->rate_saved[ch])
                toast_msg(MSG_CH_NO_RATE, ch, 0);
        if (!port_rtc_valid())
            show_toast("时间未同步（RTC 无效），可到系统设置里校时");
        return;
    case EV_NTF_MSG:
        if (host_edit_ch == e->ch && (e->a == MSG_CH_RATE || e->a == MSG_CH_MODE)) {
            char t[TXT];
            msg_text(t, sizeof t, (msg_t)e->a, e->ch, e->b);
            log_add(t);
            host_edit_ch = -1;
            return;
        }
        toast_msg((msg_t)e->a, e->ch, e->b);
        return;
    case EV_NTF_ASK: {
        dlg_t d = { .kind = DK_ASK, .ask = (ask_t)e->a, .ch = e->ch, .b = e->b, .src = (src_t)e->src };
        if (d.ask == ASK_TX_NORMAL || d.ask == ASK_CYC_NORMAL || d.ask == ASK_TX_FIRST) {
            int idx = d.ask == ASK_TX_FIRST ? d.b & 0xFF : d.b;
            if (d.ch < CH_NUM && idx >= 0 && idx < TX_ENTRIES) {
                d.tx_bound = true;
                d.tx_revision = tx_get()->e[d.ch][idx].revision;
            }
        }
        ask_text(&d);
        dlg_push(&d);
        return;
    }
    case EV_HOST_LINK:
        toast_msg(e->a ? MSG_HOST_ON : MSG_HOST_OFF, 0, 0);
        return;
    case EV_REQ_CH_RATE: case EV_REQ_CH_MODE: case EV_REQ_CH_FD:
        /* 屏幕正在改这一路的设置时被上位机改了：提示并刷新（需求 3.5 第 1 条） */
        if (e->src == SRC_HOST && page == PG_CHSET && cursor[PG_CHSET] / 4 == e->ch) {
            toast_msg(MSG_HOST_CFG_CHANGED, e->ch, 0);
            host_edit_ch = (int8_t)e->ch;
        }
        return;
    default:
        break;
    }

    if (!power_running())
        return;

    if (e->type == EV_KEY) {
        key_id_t k = (key_id_t)e->ch;
        gesture_t g = (gesture_t)e->a;
        if (k == KEY_POWER)
            return;                      /* 电源状态机处理 */
        if (power_get()->locked) {
            if (g != GEST_REPEAT)
                toast_msg(MSG_LOCKED_HINT, 0, 0);
            return;
        }
        if (n_dlg)
            dlg_key(k, g);
        else if (k == KEY_REC) {
            rec_state_t state=rec_get()->state;
            if(record_save.pending||((state!=REC_RUNNING&&state!=REC_PREPARING)&&ui_draft_open())){show_toast("请先完成当前草稿或录制命名，再开始录制");return;}
            bool acted=rec_key(g);
            if(acted&&!ui_draft_open()&&page!=PG_LIVE&&page!=PG_DETAIL){
                if(page==PG_REC&&rec_origin_valid)restore_live(rec_origin);else go(PG_LIVE);
            }
        } else
            page_key(k, g);
        record_save_poll();
        return;
    }
    if (power_get()->locked && (e->type == EV_TOUCH_ITEM || e->type == EV_TOUCH_BACK || e->type == EV_TOUCH_DLG)) {
        toast_msg(MSG_LOCKED_HINT, 0, 0);
        return;
    }
    switch (e->type) {
    case EV_TOUCH_DLG:
        if (n_dlg)
            dlg_answer(dlgq[0].kind == DK_MENU ? true : e->a == 0, dlgq[0].kind == DK_MENU ? e->a : -1);
        break;
    case EV_TOUCH_BACK:
        if (n_dlg)
            dlg_answer(false, -1);
        else
            page_key(KEY_BACK, GEST_SHORT);
        break;
    case EV_TOUCH_ITEM:
        if (n_dlg)
            break;
        if(record_save.pending&&!editor.open&&!local_tx.open&&page!=PG_FILTER&&!local_files_draft()){
            show_toast("正在关闭录制文件，请稍候命名保存");break;
        }
        if(local_files_active()) {
            local_files_touch(e->a);
            if(!local_files_active()&&local_files_notice()[0])show_toast(local_files_notice());
            break;
        }
        numeric = false;
        cursor[page] = clampi(e->a, 0, item_count(page) > 0 ? item_count(page) - 1 : 0);
        if (page == PG_TX && cursor[PG_TX] < CH_NUM * TX_ENTRIES)
            ui_tx_edit_open(cursor[PG_TX] / TX_ENTRIES, cursor[PG_TX] % TX_ENTRIES);
        else if (page == PG_HOME)
            activate_home();
        else if (page == PG_LIVE)
            info_frame();
        else if (page == PG_DETAIL)
            break;
        else
            page_key(KEY_OK, GEST_SHORT);
        break;
    default:
        break;
    }
    record_save_poll();
}

/* ---------- 视图模型 ---------- */
static struct {int next,start,total;bool bounded;} item_window;
static void plan_item(jw_t *j,const char *l,const char *v,int f,uint32_t token)
{
    int index=item_window.next++;
    if(item_window.bounded&&(index<item_window.start||index>=item_window.start+LIST_WINDOW))return;
    jw_obj(j);
    jw_kint(j,"i",index);
    jw_kstr(j, "l", l);
    jw_kstr(j, "v", v);
    jw_kint(j, "f", f);
    if(token)jw_kint(j,"plan_token",token);
    jw_end_obj(j);
}
static void item(jw_t *j,const char *l,const char *v,int f){plan_item(j,l,v,f,0);}

static void hexstr(char *o, size_t n, const uint8_t *d, int len, int max)
{
    size_t pos = 0;
    o[0] = '\0';
    int m = len < max ? len : max;
    for (int i = 0; i < m && pos + (i?4u:3u) <= n; i++)
        pos += (size_t)snprintf(o + pos, n - pos, i ? " %02X" : "%02X", d[i]);
    if (len > max && pos + 4 < n)
        snprintf(o + pos, n - pos, " …");
}

static void type_str(char *o, size_t n, uint8_t flags, uint32_t id)
{
    (void)id;
    if (flags & CANF_ERR) {
        snprintf(o, n, "ERR");
        return;
    }
    snprintf(o, n, "%s%s%s%s", (flags & CANF_EXT) ? "EXT" : "STD", (flags & CANF_FD) ? " FD" : "",
             (flags & CANF_BRS) ? " BRS" : "", (flags & CANF_RTR) ? " RTR" : "");
}

static const char *const filter_channels[] = { "全部通道", "CAN1", "CAN2" };
static const char *const filter_types[] = { "全部类型", "仅 FD", "仅错误", "仅发送" };
static const char *const filter_directions[] = { "接收与发送", "仅接收", "仅发送" };
static const char *const filter_formats[] = { "标准与扩展", "仅标准 ID", "仅扩展 ID" };

static void items_editor(jw_t *j)
{
    const can_frame_t *f = &editor.frame;
    char v[TXT];
    snprintf(v, sizeof v, "0x%0*X（OK 键盘输入）", (f->flags & CANF_EXT) ? 8 : 3, (unsigned)f->id);
    item(j, "报文 ID", v, 0);
    item(j, "ID 格式", (f->flags & CANF_EXT) ? "扩展 29 位" : "标准 11 位", 0);
    item(j, "帧协议", (f->flags & CANF_FD) ? "CAN FD" : "Classic CAN", 0);
    item(j, "BRS", (f->flags & CANF_BRS) ? "开启数据段变速" : "关闭", (f->flags & CANF_FD) ? 0 : IF_DISABLED);
    item(j, "RTR", (f->flags & CANF_RTR) ? "远程请求（无数据）" : "数据帧", 0);
    snprintf(v, sizeof v, "%u 字节（←→ 合法 DLC）", f->len);
    item(j, "长度", v, 0);
    snprintf(v, sizeof v, "%u ms（0 停用独立周期；分组循环另设）", (unsigned)editor.period);
    item(j, "单条独立周期", v, 0);
    snprintf(v, sizeof v, "%u / %u（从 0 编号）", editor.byte, f->len ? f->len - 1 : 0);
    item(j, "数据字节位置", v, f->len && !(f->flags & CANF_RTR) ? 0 : IF_DISABLED);
    snprintf(v, sizeof v, "%02X（OK 键盘输入）", f->data[editor.byte]);
    item(j, "数据字节值", v, f->len && !(f->flags & CANF_RTR) ? 0 : IF_DISABLED);
    item(j, "动态字段", "保存后关闭计数器 / 校验，按输入数据发送", IF_WARN);
    item(j, "保存", "仅保存条目；发送仍需返回列表并确认", IF_ACTION);
    item(j, "取消", "放弃草稿，返回发送列表", IF_ACTION);
    item(j,"完整数据","打开十六进制键盘，一次编辑全部有效字节",f->flags&CANF_RTR?IF_DISABLED:IF_ACTION);
}

static void items_filter(jw_t *j)
{
    char v[64];
    item(j, "通道", filter_channels[filter_draft.channel], 0);
    item(j, "类型", filter_types[filter_draft.type], 0);
    snprintf(v, sizeof v, "0x%08X（OK 键盘输入）", (unsigned)filter_draft.id);
    item(j, "匹配 ID", v, 0);
    snprintf(v, sizeof v, "0x%08X（0 为不过滤 ID）", (unsigned)filter_draft.mask);
    item(j, "ID 掩码", v, 0);
    item(j, "方向", filter_directions[filter_draft.direction], 0);
    item(j, "ID 格式", filter_formats[filter_draft.format], 0);
    item(j, "应用显示筛选", "仅影响屏幕；SD 录制保留原始数据", IF_ACTION);
    item(j, "重置草稿", "全部通道 / ID / 类型", IF_ACTION);
    item(j, "取消", "返回实时显示", IF_ACTION);
}

static void items_local_tx(jw_t *j)
{
    char label[64],value[TXT];
    bool busy=txseq_get(local_tx.ch)->state!=TXSEQ_IDLE;
    const txseq_entry_config_t *q=&local_tx.entries[local_tx.entry];
    if(page==PG_TXSET) {
        snprintf(value,sizeof value,"CAN%d（←→ 切换；两路独立）",local_tx.ch+1);
        item(j,"发送通道",value,0);
        snprintf(value,sizeof value,"%u ms（OK 键盘输入）",(unsigned)local_tx.id_gap_ms);
        item(j,"不同 ID 间隔",value,busy?IF_DISABLED:0);
        for(int i=0;i<TX_ENTRIES;i++) {
            const txseq_entry_config_t *e=&local_tx.entries[i];
            const can_frame_t *f=&tx_get()->e[local_tx.ch][i].f;
            snprintf(label,sizeof label,"第%d条 0x%0*X",i+1,f->flags&CANF_EXT?8:3,(unsigned)f->id);
            snprintf(value,sizeof value,"%s · %u 次 / %u ms（OK 设置）",e->enabled?"启用":"不参与",(unsigned)e->repeat_count,(unsigned)e->cycle_ms);
            item(j,label,value,IF_ACTION);
        }
        item(j,"保存本路设置",local_tx.dirty?"保存草稿并返回发送页":"保存并返回发送页",busy?IF_DISABLED:IF_ACTION);
        item(j,"本路时间恢复默认","填入草稿；次数、选择与报文保留",busy?IF_DISABLED:IF_ACTION);
        item(j,"两路时间恢复默认","立即保存两路时间；不修改次数/数据",(busy||local_tx.dirty)?IF_DISABLED:IF_ACTION);
        item(j,"开始本路循环",local_tx.dirty?"请先保存当前草稿":"使用已保存的本路时序",(busy||local_tx.dirty)?IF_DISABLED:IF_ACTION);
        item(j,"停止本路发送","另一通道不受影响",IF_ACTION);
        item(j,"取消并返回","放弃本路未保存的草稿",IF_ACTION);
    } else {
        const can_frame_t *f=&tx_get()->e[local_tx.ch][local_tx.entry].f;
        snprintf(value,sizeof value,"CAN%d 第%d条 · 0x%0*X（←→ 选槽）",local_tx.ch+1,local_tx.entry+1,f->flags&CANF_EXT?8:3,(unsigned)f->id);
        item(j,"发送槽",value,0);
        item(j,"参与分组循环",q->enabled?"启用":"关闭",busy?IF_DISABLED:0);
        snprintf(value,sizeof value,"%u 次（OK 键盘输入）",(unsigned)q->repeat_count);
        item(j,"每轮次数",value,busy?IF_DISABLED:0);
        snprintf(value,sizeof value,"%u ms（同组重复发送间隔）",(unsigned)q->frame_gap_ms);
        item(j,"同 ID 间隔",value,busy?IF_DISABLED:0);
        snprintf(value,sizeof value,"%u ms（从本组首帧开始计算）",(unsigned)q->cycle_ms);
        item(j,"单 ID 总周期",value,busy?IF_DISABLED:0);
        item(j,"返回循环总览","保留本路草稿，继续设置其他 ID",IF_ACTION);
        item(j,"保存本路设置","保存草稿并返回发送页",busy?IF_DISABLED:IF_ACTION);
        item(j,"取消本条修改","回滚本条草稿，返回循环总览",IF_ACTION);
    }
}

static void local_tx_json(jw_t *j)
{
    jw_key(j,"local_tx");jw_obj(j);
    jw_kbool(j,"open",local_tx.open);
    if(local_tx.open) {
        jw_kint(j,"ch",local_tx.ch);jw_kint(j,"entry",local_tx.entry);
        jw_kint(j,"revision",local_tx.revision);jw_kbool(j,"dirty",local_tx.dirty);
        jw_kbool(j,"stale",!local_tx_current());jw_kint(j,"id_gap_ms",local_tx.id_gap_ms);
        jw_key(j,"entries");jw_arr(j);
        for(int i=0;i<TX_ENTRIES;i++) {
            const txseq_entry_config_t *e=&local_tx.entries[i];
            jw_obj(j);jw_kbool(j,"enabled",e->enabled);jw_kint(j,"repeat_count",e->repeat_count);
            jw_kint(j,"frame_gap_ms",e->frame_gap_ms);jw_kint(j,"cycle_ms",e->cycle_ms);jw_end_obj(j);
        }
        jw_end_arr(j);
    }
    jw_end_obj(j);
}

static void filter_json(jw_t *j, const char *key, const disp_filter_t *f)
{
    jw_key(j, key); jw_obj(j);
    jw_kint(j, "channel", f->channel); jw_kint(j, "type", f->type);
    jw_kint(j, "id", f->id); jw_kint(j, "mask", f->mask);
    jw_kint(j, "direction", f->direction); jw_kint(j, "format", f->format);
    jw_end_obj(j);
}

static void items_tx(jw_t *j)
{
    const tx_t *t = tx_get();
    char l[64], v[TXT], hex[64];
    for (int ch = 0; ch < CH_NUM; ch++) {
        for (int i = 0; i < TX_ENTRIES; i++) {
            const tx_entry_t *e = &t->e[ch][i];
            char ty[24];
            type_str(ty, sizeof ty, e->f.flags, e->f.id);
            snprintf(l, sizeof l, "CAN%d 0x%X %s %dB", ch + 1, (unsigned)e->f.id, ty, e->f.len);
            hexstr(hex, sizeof hex, e->f.data, (e->f.flags & CANF_RTR) ? 0 : e->f.len, 8);
            char per[24] = "仅单发";
            if (e->period_ms)
                snprintf(per, sizeof per, "%ums%s", e->period_ms, e->active ? " ▶" : "");
            snprintf(v, sizeof v, "%s | %s%s%s | 已发 %u 丢 %u", hex, per, e->dyn.cnt_bits ? " 计数" : "",
                     e->dyn.chk ? " 校验" : "", (unsigned)e->sent, (unsigned)e->missed);
            item(j, l, v, e->active ? IF_ACTIVE : 0);
        }
    }
    item(j, "全部停止", "← 键也可以", IF_ACTION);
    item(j,"本地循环设置","通道 / 次数 / 间隔 / 总周期 / 默认时间",IF_ACTION);
#ifndef CANFD_REVD_TARGET
    item(j,"从本地 SD 数据库选帧","DBC / DBF 定义；加入本路发送槽",IF_ACTION);
#endif
    snprintf(v,sizeof v,"保存 CAN%d 所有报文、数据与时序",local_tx_channel+1);
    item(j,tx_scope_mask==3?"保存两路列表为发送方案":"保存这一屏为发送方案",tx_scope_mask==3?"分别保存 CAN1 / CAN2 各自列表与时序":v,IF_ACTION);
    snprintf(v,sizeof v,"CAN%d（编辑操作不改变发送范围）",local_tx_channel+1);
    item(j,"当前编辑通道",v,IF_ACTION);
    item(j,"发送范围",tx_scope_mask==3?"CAN1 + CAN2（各用自己的报文列表）":tx_scope_mask==2?"仅 CAN2":"仅 CAN1",IF_ACTION);
    item(j,"按范围启动循环","全部就绪后一起开始，各自使用本路时序",IF_ACTION);
    item(j,"停止范围内发送","只停止当前发送范围内的通道",IF_ACTION);
    item(j,"加载文件","选择后替换文件所含通道，直接显示报文；不发送",IF_ACTION);
}

static void items_rec(jw_t *j)
{
    const rec_t *r = rec_get();
    const cfg_t *c = cfg_get();
    char v[TXT];
    bool idle = r->state == REC_IDLE;
    uint32_t s = r->elapsed_ms / 1000u;
    snprintf(v, sizeof v, "%s%s", rec_state_name(r->state), idle ? "（OK 开始）" : r->state == REC_RUNNING ? "（OK 停止）" : "");
    item(j, "状态", v, r->state == REC_RUNNING ? IF_ACTIVE : IF_ACTION);
    item(j, "录制通道", c->rec_ch_mask == 3 ? "两路" : c->rec_ch_mask == 1 ? "CAN1" : "CAN2", idle ? 0 : IF_DISABLED);
    item(j, "分段大小", cfg_rec_seg_name(c->rec_seg_idx), idle ? 0 : IF_DISABLED);
    item(j, "卡满时", c->rec_full_delete ? "删除最旧会话" : "停止录制", idle ? 0 : IF_DISABLED);
    snprintf(v, sizeof v, "%02u:%02u:%02u  %u 帧", (unsigned)(s / 3600u), (unsigned)(s / 60u % 60u), (unsigned)(s % 60u), (unsigned)r->frames);
    item(j, "时长 / 帧数", v, 0);
    snprintf(v, sizeof v, "%llu KB  会话 %08u 分段 %u", (unsigned long long)(r->bytes / 1024u), (unsigned)r->session, (unsigned)r->seg);
    item(j, "大小", v, 0);
    snprintf(v, sizeof v, "丢帧 %u  标记 %u", (unsigned)r->drops, (unsigned)r->markers);
    item(j, "丢帧 / 标记", v, r->drops ? IF_WARN : 0);
    snprintf(v, sizeof v, "%s  剩余 %u MB", storage_state_name(storage_get()->state), (unsigned)(port_sd_free_kb() / 1024u));
    item(j, "SD 卡", v, storage_get()->state == ST_READY ? 0 : IF_WARN);
    item(j,"本地录制名称","设备键盘编辑下次录制前缀",idle?IF_ACTION:IF_DISABLED);
}

static const char *kind_name(file_kind_t k)
{
    static const char *const n[] = { "录制", "ASC", "candump", "发送列表", "未知" };
    return k <= FK_UNKNOWN ? n[k] : "?";
}

static const char *plan_mask_name(uint32_t mask)
{
    return mask==3?"CAN1 + CAN2":mask==2?"CAN2":"CAN1";
}
static void items_send_mode(jw_t *j)
{
    item(j,"现有报文","直接选择上位机导入或本机保存的发送文件",IF_ACTION);
    item(j,"手动编辑报文","编辑本机报文，选择 CAN1 / CAN2 / 两路发送",IF_ACTION);
}
static void items_send_files(jw_t *j)
{
    char value[TXT];unsigned count=sp_catalog_count();
    for(unsigned index=0;index<count;index++){
        sp_file_t file;if(!sp_catalog_get(index,&file))continue;
        snprintf(value,sizeof value,"加载并替换 %s 列表；不发送",plan_mask_name(file.plan.channel_mask));
        plan_item(j,file.name,value,IF_ACTION,file.token);
    }
    if(!count)item(j,"暂无发送文件","从上位机导入 .canplan.json，或保存手动报文",IF_DISABLED);
    const sp_status_t *state=sp_state();
    item(j,"刷新发送文件",state->io_pending?"文件操作处理中":state->message[0]?state->message:"重新读取SD中的 .canplan.json",state->io_pending?IF_DISABLED:IF_ACTION);
}
static void items_plan(jw_t *j)
{
    const sp_status_t *state=sp_state();
    bool pending=state->io_pending!=0;
    char value[TXT];
    snprintf(value,sizeof value,"CAN%d（←→ 切换保存范围）",plan_save_channel+1);
    item(j,"当前屏通道",value,0);
    item(j,"保存当前屏为方案","报文数据 + 次数 + 所有间隔/周期",pending?IF_DISABLED:IF_ACTION);
    item(j,"两路一起保存为方案","同时保存 CAN1 / CAN2 的完整配置",pending?IF_DISABLED:IF_ACTION);
    item(j,"刷新方案文件列表",pending?"正在处理文件，请稍候":"读取SD卡中的 .canplan.json",pending?IF_DISABLED:IF_ACTION);
    if(state->loaded_mask)
        snprintf(value,sizeof value,"%.47s · %s%s",state->loaded_name,plan_mask_name(state->loaded_mask),
                 state->state==SP_RUNNING?" · 发送中":state->state==SP_CONFIRMING?" · 等待确认":state->state==SP_ARMING?" · 正在准备":" · 已加载未发送");
    else snprintf(value,sizeof value,"尚未加载；请在下面选择文件");
    item(j,"已加载方案",value,state->state==SP_RUNNING?IF_ACTIVE:0);
    bool no_start=!state->loaded_mask||pending||state->state==SP_CONFIRMING||state->state==SP_ARMING||state->state==SP_RUNNING;
    item(j,"执行已加载方案一轮","每个 ID 按保存的次数执行一整轮",no_start?IF_DISABLED:IF_ACTION);
    item(j,"循环执行已加载方案","持续按保存的条目顺序与时序执行",no_start?IF_DISABLED:IF_ACTION);
    item(j,"停止方案发送","只停止已加载方案包含的通道",state->loaded_mask?IF_ACTION:IF_DISABLED);
    item(j,"文件操作结果",state->io_pending?"文件读写处理中，配置快照已固定":state->message[0]?state->message:"保存和加载都不会自动发送",state->last_error<0?IF_WARN:0);
    unsigned count=sp_catalog_count();
    for(unsigned index=0;index<count;index++){
        sp_file_t file;
        if(!sp_catalog_get(index,&file))continue;
        snprintf(value,sizeof value,"%s · OK 预览数据和时序",plan_mask_name(file.plan.channel_mask));
        item(j,file.name,value,IF_ACTION);
    }
    if(!count)item(j,"暂无发送方案文件","可保存当前屏，或向SD放入统一模板文件",IF_DISABLED);
}
static void items_plan_detail(jw_t *j)
{
    char value[TXT],label[64];
    item(j,"方案文件",plan_selected.name,0);
    item(j,"发送通道",plan_mask_name(plan_selected.plan.channel_mask),0);
    snprintf(value,sizeof value,"%u 条槽（启用选择与完整载荷均已保存）",(plan_selected.plan.channel_mask==3?2u:1u)*TX_ENTRIES);
    item(j,"发送条目",value,0);
    item(j,"复现内容","ID/格式/数据/动态初值/次数/间隔/周期",0);
    const sp_status_t *state=sp_state();
    bool pending=state->io_pending!=0||state->state==SP_RUNNING||state->state==SP_ARMING||state->state==SP_CONFIRMING;
    item(j,"仅加载到手动发送列表","恢复本方案包含的通道配置；不会发送",pending?IF_DISABLED:IF_ACTION);
    item(j,"加载并执行一轮","显式确认后，每个 ID 执行保存的次数",pending?IF_DISABLED:IF_ACTION);
    item(j,"加载并循环执行","显式确认后，重复完整的发送列表",pending?IF_DISABLED:IF_ACTION);
    snprintf(value,sizeof value,"%.47s · %s",state->loaded_name,plan_mask_name(state->loaded_mask));
    item(j,"停止已加载方案",state->loaded_mask?value:"尚无已加载方案",state->loaded_mask?IF_ACTION:IF_DISABLED);
    item(j,"返回方案列表","不修改当前发送配置",IF_ACTION);
    for(int ch=0;ch<CH_NUM;ch++)if(plan_selected.plan.channel_mask&(1u<<ch)){
        for(int index=0;index<TX_ENTRIES;index++){
            const send_plan_entry_t *entry=&plan_selected.plan.channels[ch].entries[index];
            snprintf(label,sizeof label,"CAN%d 第%d条 · 0x%0*lX",ch+1,index+1,entry->flags&CANF_EXT?8:3,(unsigned long)entry->id);
            snprintf(value,sizeof value,"%s · %luB · %lu次 / %lums · OK 数据",entry->enabled?"启用":"未启用",(unsigned long)entry->length,
                     (unsigned long)entry->repeat_count,(unsigned long)entry->cycle_ms);
            item(j,label,value,IF_ACTION);
        }
    }
}
static void items_file_detail(jw_t *j)
{
    file_info_t info;char value[TXT];
    if(!selected_log(&info)){
        item(j,"文件已变化","请返回文件列表重新选择",IF_WARN);
        for(int index=1;index<6;index++)item(j,"—","",IF_DISABLED);
        item(j,"返回文件列表","",IF_ACTION);return;
    }
    bool busy=log_file_busy(&info);
    item(j,"文件名",info.name,0);
    snprintf(value,sizeof value,"%s%s%s",kind_name(info.kind),info.writing?" · 正在写入":play_file_in_use(log_selected_index)?" · 正在使用":" · 已关闭",info.protected_?" · 已保护":"");
    item(j,"类型 / 状态",value,busy?IF_WARN:0);
#ifdef CANFD_REVD_TARGET
    if(!info.sample_available)snprintf(value,sizeof value,"%u KB · 帧数未索引",(unsigned)info.size_kb);
    else
#endif
    snprintf(value,sizeof value,"%u KB · %u 帧",(unsigned)info.size_kb,(unsigned)info.frames);
    item(j,"大小 / 帧数",value,0);
#ifdef CANFD_REVD_TARGET
    if(!info.sample_available)snprintf(value,sizeof value,"时长与通道未索引");
    else snprintf(value,sizeof value,"%u ms · %u 路",(unsigned)info.duration_ms,info.channels);
#else
    snprintf(value,sizeof value,"%u ms · %u 路%s",(unsigned)info.duration_ms,info.channels,info.sample_available?"":" · 内置只读样例");
#endif
    item(j,"时长 / 通道",value,0);
    item(j,info.protected_?"取消文件保护":"保护文件",busy?"文件正忙或SD不可用":"仅修改此日志保护状态",busy?IF_DISABLED:IF_ACTION);
#ifdef CANFD_REVD_TARGET
    item(j,"修改文件名",info.protected_?"请先取消保护":"本机名称键盘",busy||info.protected_?IF_DISABLED:IF_ACTION);
#else
    item(j,"修改文件名",info.protected_?"请先取消保护":!info.sample_available?"内置样例只读":"本机名称键盘",busy||info.protected_||!info.sample_available?IF_DISABLED:IF_ACTION);
#endif
    item(j,"返回文件列表","",IF_ACTION);
}
static void items_plan_entry(jw_t *j)
{
    const send_plan_channel_t *channel=&plan_selected.plan.channels[plan_entry_ch];
    const send_plan_entry_t *entry=&channel->entries[plan_entry_index];
    char value[TXT],label[48];
    snprintf(value,sizeof value,"CAN%d 第%d条 · 0x%0*lX",plan_entry_ch+1,plan_entry_index+1,entry->flags&CANF_EXT?8:3,(unsigned long)entry->id);
    item(j,"通道 / ID",value,0);
    item(j,"帧格式",entry->flags&CANF_RTR?"RTR（无载荷）":entry->flags&CANF_FD?entry->flags&CANF_BRS?"CAN FD / BRS":"CAN FD":"Classic CAN",0);
    snprintf(value,sizeof value,"%s · %lu 字节",entry->flags&CANF_EXT?"扩展29位":"标准11位",(unsigned long)entry->length);
    item(j,"ID格式 / 长度",value,0);
    snprintf(value,sizeof value,"%s · 每轮 %lu 次 · 同ID间隔 %lu ms",entry->enabled?"参与循环":"不参与循环",(unsigned long)entry->repeat_count,(unsigned long)entry->frame_gap_ms);
    item(j,"次数 / 间隔",value,0);
    snprintf(value,sizeof value,"本ID %lu ms · 不同ID %lu ms",(unsigned long)entry->cycle_ms,(unsigned long)channel->id_gap_ms);
    item(j,"保存的循环时间",value,0);
    snprintf(value,sizeof value,"%lu ms（0为停用独立周期）",(unsigned long)entry->period_ms);
    item(j,"单条独立周期",value,0);
    if(entry->dynamic[1]||entry->dynamic[4])snprintf(value,sizeof value,"计数位 %lu · 初值 %lu · 校验类型 %lu",(unsigned long)entry->dynamic[1],(unsigned long)entry->dynamic[3],(unsigned long)entry->dynamic[4]);
    else snprintf(value,sizeof value,"关闭；下列字节按原值发送");
    item(j,"动态字段快照",value,0);
    if(!(entry->flags&CANF_RTR))for(uint32_t offset=0;offset<entry->length;offset+=8){
        snprintf(label,sizeof label,"DATA %02lX",(unsigned long)offset);
        size_t used=0;value[0]=0;
        for(uint32_t n=offset;n<entry->length&&n<offset+8;n++)
            used+=(size_t)snprintf(value+used,sizeof value-used,"%02X%s",entry->data[n],n+1<entry->length&&n+1<offset+8?" ":"");
        item(j,label,value,0);
    }
    item(j,"返回方案预览","只查看，不修改和发送",IF_ACTION);
}

static void items_diag(jw_t *j)
{
    char l[48], v[TXT], r[24];
    static const char *const es[] = { "主动", "警告", "被动", "Bus-Off" };
    for (int ch = 0; ch < CH_NUM; ch++) {
        const chan_t *s = chan_get(ch);
        const ch_stats_t *st = stats_get(ch);
        fmt_ch_rate(r, sizeof r, ch);
        snprintf(l, sizeof l, "CAN%d 状态", ch + 1);
        if (s->state == CH_AUTOBAUD)
            snprintf(v, sizeof v, "自动识别中 %u%%", chan_ab_progress(ch));
        else
            snprintf(v, sizeof v, "%s  %s%s", chan_state_name(s->state), r, cfg_get()->fd[ch] ? " FD" : "");
        item(j, l, v, s->state == CH_BUSOFF ? IF_WARN : s->state == CH_NORMAL ? IF_ACTIVE : 0);
        snprintf(l, sizeof l, "CAN%d TEC/REC", ch + 1);
        snprintf(v, sizeof v, "%u / %u  %s", s->tec, s->rec, es[s->errst]);
        item(j, l, v, s->errst >= ERRST_PASSIVE ? IF_WARN : 0);
        snprintf(l, sizeof l, "CAN%d 负载", ch + 1);
        snprintf(v, sizeof v, "%u.%u%%  峰值 %u.%u%%  %u 帧/s", st->load_permille / 10, st->load_permille % 10,
                 st->load_peak / 10, st->load_peak % 10, (unsigned)st->fps);
        item(j, l, v, 0);
        snprintf(l, sizeof l, "CAN%d 错误", ch + 1);
        snprintf(v, sizeof v, "%u 次（%u/s）丢帧 %u；ACK/位错误仅发送时有效", (unsigned)st->err_total, st->err_rate,
                 (unsigned)st->hw_drops);
        item(j, l, v, st->err_rate ? IF_WARN : 0);
    }
    item(j, "自动识别 CAN1", "OK 开始（只听）", IF_ACTION);
    item(j, "自动识别 CAN2", "OK 开始（只听）", IF_ACTION);
    item(j, "恢复 CAN1 Bus-Off", chan_get(0)->state == CH_BUSOFF ? "OK 恢复" : "—", chan_get(0)->state == CH_BUSOFF ? IF_ACTION : IF_DISABLED);
    item(j, "恢复 CAN2 Bus-Off", chan_get(1)->state == CH_BUSOFF ? "OK 恢复" : "—", chan_get(1)->state == CH_BUSOFF ? IF_ACTION : IF_DISABLED);
    item(j, "清零统计", "OK", IF_ACTION);
}

static void items_files(jw_t *j)
{
    const storage_t *s = storage_get();
    char v[TXT];
    int count=0;
    for(int index=0;index<file_count();index++){
        file_info_t info;if(!port_sd_file_info(index,&info))continue;
        bool busy=info.writing||play_file_in_use(index);
        snprintf(v,sizeof v,"%s · %u KB%s%s",kind_name(info.kind),(unsigned)info.size_kb,
#ifdef CANFD_REVD_TARGET
                 busy?" · 忙":!info.sample_available?" · 帧数未索引":" · 已关闭",
#else
                 busy?" · 忙":!info.sample_available?" · 只读样例":" · 已关闭",
#endif
                 info.protected_?" · 已保护":"");
        item(j,info.name,v,IF_ACTION|(busy?IF_WARN:0));count++;
    }
    for(unsigned index=0;index<sp_catalog_count();index++){
        sp_file_t file;if(!sp_catalog_get(index,&file))continue;
        bool busy=sp_state()->loaded_mask&&sp_state()->state!=SP_LOADED&&sp_state()->state!=SP_IDLE&&!strcmp(sp_state()->loaded_name,file.name);
        snprintf(v,sizeof v,"发送方案 · %s%s",plan_mask_name(file.plan.channel_mask),busy?" · 忙（可预览）":" · 可预览");
        item(j,file.name,v,IF_ACTION|(busy?IF_WARN:0));count++;
    }
    for(unsigned index=0;index<local_db_count();index++){
        char name[48];uint32_t frames;
        if(!local_db_file_info(index,name,sizeof name,&frames))continue;
        snprintf(v,sizeof v,"数据库 · %u 帧定义 · 只读",(unsigned)frames);
        item(j,name,v,IF_ACTION);count++;
    }
    if(!count)item(j,"暂无文件","保存录制日志，或导入发送方案",IF_DISABLED);
    snprintf(v,sizeof v,"%s · USB %s%s",storage_state_name(s->state),s->usb_attached?"已连接":"未连接",s->state==ST_MSC?" · 请先在电脑弹出":"");
    item(j,s->state==ST_MSC?"收回 SD 卡":"进入 U 盘模式",v,IF_ACTION);
#ifdef CANFD_REVD_TARGET
    item(j,"格式化 SD 卡","删除所有录制文件、发送方案与命名设置",IF_ACTION|IF_WARN);
#else
    item(j,"格式化 SD 卡","清空模拟日志与数据库；发送方案保留",IF_ACTION|IF_WARN);
#endif
}

static void items_chset(jw_t *j)
{
    const cfg_t *c = cfg_get();
    char l[48], v[48];
    for (int ch = 0; ch < CH_NUM; ch++) {
        const chan_t *s = chan_get(ch);
        snprintf(l, sizeof l, "CAN%d 工作模式", ch + 1);
        if (s->state == CH_STANDBY || s->state == CH_BUSOFF || s->state == CH_AUTOBAUD)
            snprintf(v, sizeof v, "%s", chan_state_name(s->state));
        else
            snprintf(v, sizeof v, "%s", mode_name(s->mode));
        item(j, l, v, s->mode == CAN_MODE_NORMAL ? IF_ACTIVE : 0);
        snprintf(l, sizeof l, "CAN%d 标称速率", ch + 1);
        fmt_rate(v, sizeof v, c->nbr[ch]);
        item(j, l, v, 0);
        snprintf(l, sizeof l, "CAN%d CAN FD", ch + 1);
        item(j, l, c->fd[ch] ? "开" : "关", 0);
        snprintf(l, sizeof l, "CAN%d 数据段速率", ch + 1);
        fmt_rate(v, sizeof v, c->dbr[ch]);
        item(j, l, v, c->fd[ch] ? 0 : IF_DISABLED);
    }
    item(j, "Bus-Off 恢复", c->busoff_auto ? "自动（退避，最多 5 次）" : "手动", 0);
}

static void items_sysset(jw_t *j)
{
    const cfg_t *c = cfg_get();
    char v[64];
    if (c->screen_off_s) snprintf(v, sizeof v, "%u 秒", c->screen_off_s); else snprintf(v, sizeof v, "从不");
    item(j, "自动熄屏", v, 0);
    if (c->auto_off_min) snprintf(v, sizeof v, "%u 分钟", c->auto_off_min); else snprintf(v, sizeof v, "从不");
    item(j, "自动关机", v, 0);
    snprintf(v, sizeof v, "%u%%", c->backlight_pct);
    item(j, "背光亮度", v, 0);
    item(j, "时间", port_rtc_valid() ? "已同步" : "未同步（OK 校时）", port_rtc_valid() ? 0 : IF_WARN);
    snprintf(v, sizeof v, "rev %c%s", port_chip_rev(), port_chip_rev() == 'Y' ? "（双路同时接收有风险）" : "");
    item(j, "芯片版本", v, port_chip_rev() == 'Y' ? IF_WARN : 0);
    snprintf(v, sizeof v, "%08X", (unsigned)port_serial());
    item(j, "序列号", v, 0);
    item(j, "语言", "中文（英文 P1）", IF_DISABLED);
    item(j, "恢复出厂设置", "保留序列号、校准值和录制文件", IF_ACTION | IF_WARN);
    item(j, "工程菜单", "OK 进入", IF_ACTION);
    item(j, "关于", "固件 v0.1（状态机模拟）", 0);
    item(j,"录制设置","通道、分段、卡满处理与下次录制名称",IF_ACTION);
}

static void items_eng(jw_t *j)
{
    static const char *const reason[] = { "—", "长按 POWER", "低电", "自动关机", "开机误触", "上位机" };
    char v[48];
    item(j, "自检（模拟）", "OK 运行", IF_ACTION);
    snprintf(v, sizeof v, "%u", (unsigned)evt_dropped());
    item(j, "事件队列溢出", v, 0);
    snprintf(v, sizeof v, "%u", (unsigned)ring_dropped());
    item(j, "环形缓冲丢帧", v, 0);
    snprintf(v, sizeof v, "%u", (unsigned)power_get()->boot_count);
    item(j, "开机次数", v, 0);
    item(j, "上次关机原因", reason[power_get()->reason % 6], 0);
    item(j, "返回主页", "OK", IF_ACTION);
}

static void payload_json(jw_t *j, const uint8_t *data, unsigned len, unsigned flags)
{
    unsigned available = flags & CANF_RTR ? 0u : (len < DISP_DATA ? len : DISP_DATA);
    jw_kint(j, "available_len", available);
    jw_kbool(j, "truncated", !(flags & CANF_RTR) && available < len);
    jw_key(j, "bytes"); jw_arr(j);
    for (unsigned b = 0; b < available; ++b) jw_int(j, data[b]);
    jw_end_arr(j);
}

static void live_json(jw_t *j)
{
    disp_ch_filter_t fc;
    disp_type_filter_t ft;
    static const char *const fcn[] = { "全部通道", "CAN1", "CAN2" };
    static const char *const ftn[] = { "全部类型", "仅 FD", "仅错误", "仅发送" };
    disp_get_filter(&fc, &ft);
    int c = cursor[PG_LIVE];
    jw_key(j, "live");
    jw_obj(j);
    jw_kstr(j, "view", live_view ? "byid" : "scroll");
    jw_kint(j,"channel",live_channel);
    jw_kbool(j, "paused", disp_paused());
    jw_kint(j, "view_time_us", (int64_t)disp_view_time_us());
    jw_kint(j, "hidden", disp_hidden_while_paused());
    jw_kint(j, "sampled", disp_sampled_n());
    char f[48];
    snprintf(f, sizeof f, "%s · %s", fcn[fc], ftn[ft]);
    jw_kstr(j, "filter", f);
    disp_filter_t fcfg;
    disp_filter_get_ex(&fcfg);
    filter_json(j, "filter_cfg", &fcfg);
    jw_kint(j, "cur", c);
    jw_kint(j, "others", disp_id_others());
    jw_kint(j,"cached_total",live_view?disp_id_count():(int)disp_rows_available());
    jw_key(j, "rows");
    jw_arr(j);
    int start = (c / LIST_WINDOW) * LIST_WINDOW;
    char hex[64], ty[24], t[32], dt[24];
    for (int i = start; i < start + LIST_WINDOW; i++) {
        if (live_view == 0) {
            disp_row_t r;
            uint32_t source_back;
            if (!live_row_at(live_channel,(unsigned)i,&r,&source_back))
                break;
            type_str(ty, sizeof ty, r.flags, r.id);
            hexstr(hex, sizeof hex, r.data, (r.flags & CANF_RTR) ? 0 : r.len, 8);
            snprintf(t, sizeof t, "%llu.%06llu", (unsigned long long)(r.ts_us / 1000000u), (unsigned long long)(r.ts_us % 1000000u));
            if (r.dt_same_us)
                snprintf(dt, sizeof dt, "%u.%03u", (unsigned)(r.dt_same_us / 1000u), (unsigned)(r.dt_same_us % 1000u));
            else
                snprintf(dt, sizeof dt, "—");
            jw_obj(j);
            jw_kint(j, "i", i);
            uint32_t sequence=0; (void)disp_row_seq(source_back,&sequence);
            jw_kint(j, "seq", sequence);
            jw_kbool(j, "selected", i == c);
            jw_kint(j, "flags", r.flags);
            jw_kint(j, "ts_us", (int64_t)r.ts_us);
            jw_kint(j, "dt_us", r.dt_same_us);
            payload_json(j, r.data, r.len, r.flags);
            jw_kint(j, "ch", r.ch + 1);
            jw_kint(j, "id", r.id);
            jw_kbool(j, "ext", r.flags & CANF_EXT);
            jw_kstr(j, "type", ty);
            jw_kint(j, "len", r.len);
            jw_kstr(j, "data", hex);
            jw_kstr(j, "t", t);
            jw_kstr(j, "dt", dt);
            jw_kbool(j, "tx", r.flags & CANF_TX);
            jw_kstr(j,"direction",r.flags&CANF_TX?"TX":"RX");
            jw_kbool(j, "err", r.flags & CANF_ERR);
            jw_end_obj(j);
        } else {
            const disp_id_t *x = live_id_at(live_channel,(unsigned)i);
            if (!x)
                break;
            type_str(ty, sizeof ty, x->flags, x->id);
            hexstr(hex, sizeof hex, x->data, (x->flags & CANF_RTR) ? 0 : x->len, 8);
            uint64_t now = disp_view_time_us();
            uint64_t age = now >= x->last_ts ? now - x->last_ts : 0;
            uint64_t thr = (uint64_t)x->per_avg_us * 3u < 100000u ? 100000u : (uint64_t)x->per_avg_us * 3u;
            jw_obj(j);
            jw_kint(j, "i", i);
            jw_kbool(j, "selected", i == c);
            jw_kint(j, "flags", x->flags);
            jw_kint(j, "ts_us", (int64_t)x->last_ts);
            jw_kint(j, "period_avg_us", x->per_avg_us);
            jw_kint(j, "period_min_us", x->per_min_us);
            jw_kint(j, "period_max_us", x->per_max_us);
            payload_json(j, x->data, x->len, x->flags);
            jw_key(j, "changed"); jw_arr(j);
            for (unsigned b = 0; b < ((x->flags & CANF_RTR) ? 0u : (x->len < DISP_DATA ? x->len : DISP_DATA)); ++b) jw_bool(j, disp_id_byte_changed(x,b));
            jw_end_arr(j);
            jw_kint(j, "ch", x->ch + 1);
            jw_kint(j, "id", x->id);
            jw_kbool(j, "ext", x->flags & CANF_EXT);
            jw_kbool(j,"tx",x->flags&CANF_TX);
            jw_kbool(j,"err",x->flags&CANF_ERR);
            jw_kstr(j,"direction",x->flags&CANF_TX?"TX":"RX");
            jw_kstr(j, "type", ty);
            jw_kint(j, "len", x->len);
            jw_kstr(j, "data", hex);
            jw_kint(j, "chg", x->chg_mask);
            jw_kint(j, "count", x->count);
            snprintf(dt, sizeof dt, "%u.%u", (unsigned)(x->per_avg_us / 1000u), (unsigned)(x->per_avg_us % 1000u / 100u));
            jw_kstr(j, "per", dt);
            jw_kbool(j, "timeout", x->count > 1 && age > thr);
            jw_end_obj(j);
        }
    }
    jw_end_arr(j);
    jw_kint(j, "total", item_count(PG_LIVE));
    jw_end_obj(j);
}

static void detail_json(jw_t *j)
{
    const disp_row_t *f = &detail.frame;
    unsigned available = f->flags & CANF_RTR ? 0u : (f->len < DISP_DATA ? f->len : DISP_DATA);
    char type[24]; type_str(type, sizeof type, f->flags, f->id);
    jw_key(j, "detail"); jw_obj(j);
    jw_kbool(j, "valid", detail_valid);
    jw_kstr(j, "source", detail_origin.view ? "byid" : "scroll");
    jw_kstr(j, "source_view", detail_origin.view ? "byid" : "scroll");
    jw_kint(j,"source_channel",detail_origin.channel);
    jw_kint(j, "source_index", detail.index);
    jw_kint(j, "seq", detail.seq); jw_kbool(j, "has_sequence", detail.has_sequence);
    jw_kbool(j, "source_paused", detail_origin.paused);
    jw_kbool(j, "snapshot", true);
    jw_kint(j, "ch", f->ch + 1); jw_kint(j, "id", f->id);
    jw_kint(j, "flags", f->flags); jw_kstr(j, "type", type);
    jw_kbool(j, "ext", (f->flags & CANF_EXT) != 0);
    jw_kbool(j, "tx", (f->flags & CANF_TX) != 0);
    jw_kbool(j, "err", (f->flags & CANF_ERR) != 0);
    jw_kbool(j, "rtr", (f->flags & CANF_RTR) != 0);
    jw_kint(j, "len", f->len); jw_kint(j, "available_len", available);
    jw_kbool(j, "truncated", !(f->flags & CANF_RTR) && available < f->len);
    jw_kint(j, "ts_us", (int64_t)f->ts_us);
    jw_kint(j, "dt_us", detail_origin.view ? 0 : f->dt_same_us);
    jw_kbool(j, "has_interval", !detail_origin.view && f->dt_same_us > 0);
    jw_kstr(j, "dt_kind", detail_origin.view ? "not_retained" : "same_id_interval");
    jw_kbool(j, "has_summary", detail.has_summary);
    jw_kint(j, "count", detail.count);
    jw_kstr(j, "period_kind", "ewma");
    jw_kint(j, "period_avg_us", detail.avg_us); jw_kint(j, "period_min_us", detail.min_us);
    jw_kint(j, "period_max_us", detail.max_us);
    jw_key(j, "data"); jw_arr(j);
    for (unsigned b=0;b<available;++b) jw_int(j,f->data[b]);
    jw_end_arr(j); jw_end_obj(j);
    /* Generic item fallback keeps the view model usable by the existing LCD
     * adapter. Desktop renderer may present the structured 64-byte grid. */
    jw_key(j, "items"); jw_arr(j);
    char label[48], value[96];
    snprintf(value,sizeof value,"CAN%u · %s · %s",f->ch+1,(f->flags&CANF_TX)?"TX":"RX",type);
    item(j,"通道 / 类型",value,0);
    snprintf(value,sizeof value,(f->flags&CANF_EXT)?"0x%08lX":"0x%03lX",(unsigned long)f->id);
    item(j,"仲裁 ID",value,0);
    if (f->flags & CANF_RTR) snprintf(value,sizeof value,"RTR DLC %u · 无数据载荷",f->len);
    else snprintf(value,sizeof value,"%u B · 已保留 %u B%s",f->len,available,
                  available < f->len ? "（截断）" : "");
    item(j,"载荷长度",value,available < f->len && !(f->flags & CANF_RTR) ? IF_WARN : 0);
    snprintf(value,sizeof value,"%llu.%06llu s",(unsigned long long)(f->ts_us/1000000u),(unsigned long long)(f->ts_us%1000000u));
    item(j,"时间戳",value,0);
    if (detail.has_summary) snprintf(value,sizeof value,"%lu 次 · 平滑周期 %lu.%03lu ms",(unsigned long)detail.count,(unsigned long)(detail.avg_us/1000u),(unsigned long)(detail.avg_us%1000u));
    else snprintf(value,sizeof value,"错误帧不进入 ID 聚合统计");
    item(j,"打开时 ID 统计",value,0);
    for (unsigned base=0;base<available;base+=8) {
        snprintf(label,sizeof label,"字节 %02u–%02u",base,base+7u<available?base+7u:available-1u);
        hexstr(value,sizeof value,f->data+base,available-base,8); item(j,label,value,0);
    }
    jw_end_arr(j);
}

static void tx_channels_json(jw_t *j)
{
    static const char *const states[]={"空闲","等待确认","正在准备","正在发送"};
    jw_kstr(j,"tx_loaded_name",sp_state()->loaded_name);
    jw_kstr(j,"tx_plan_message",sp_state()->message);
    jw_key(j,"tx_channels");jw_arr(j);
    for(int ch=0;ch<CH_NUM;ch++){
        const txseq_channel_t *sequence=txseq_get(ch);
        const scope_status_t *scope=scope_state();
        bool running=sequence->state==TXSEQ_RUNNING;
        bool pending=sequence->state==TXSEQ_CONFIRMING||sequence->state==TXSEQ_ARMING||
            (scope->state!=SCOPE_IDLE&&(scope->mask&(1u<<(unsigned)ch))!=0);
        for(int i=0;i<TX_ENTRIES;i++)running|=tx_get()->e[ch][i].active;
        jw_obj(j);jw_kint(j,"ch",ch);jw_kbool(j,"running",running);jw_kbool(j,"pending",pending);
        jw_kint(j,"state",sequence->state);jw_kstr(j,"state_name",states[sequence->state]);
        jw_kint(j,"sent",sequence->sent);jw_kint(j,"missed",sequence->missed);jw_kint(j,"rounds",sequence->rounds);
        jw_kint(j,"id_gap_ms",sequence->id_gap_ms);jw_key(j,"entries");jw_arr(j);
        for(int i=0;i<TX_ENTRIES;i++){
            const tx_entry_t *entry=&tx_get()->e[ch][i];const txseq_entry_config_t *timing=&sequence->entries[i];
            jw_obj(j);jw_kint(j,"index",i);jw_kint(j,"id",entry->f.id);jw_kint(j,"flags",entry->f.flags);
            jw_kint(j,"len",entry->f.len);jw_kint(j,"period_ms",entry->period_ms);jw_kbool(j,"active",entry->active);
            jw_kbool(j,"enabled",timing->enabled);jw_kint(j,"repeat_count",timing->repeat_count);
            jw_kint(j,"frame_gap_ms",timing->frame_gap_ms);jw_kint(j,"cycle_ms",timing->cycle_ms);
            jw_key(j,"data");jw_arr(j);for(int byte=0;byte<64;byte++)jw_int(j,entry->f.data[byte]);jw_end_arr(j);jw_end_obj(j);
        }
        jw_end_arr(j);jw_end_obj(j);
    }
    jw_end_arr(j);
}

static void record_controls_json(jw_t *j)
{
    jw_key(j,"rec_controls");jw_obj(j);
    const rec_t *rec=rec_get();
    bool common=power_running()&&power_get()->screen_on&&!power_get()->locked&&!n_dlg&&!ui_draft_open();
    msg_t storage_reason;
    jw_kint(j,"state",rec->state);jw_kstr(j,"state_name",rec_state_name(rec->state));
    jw_kint(j,"channel_mask",cfg_get()->rec_ch_mask);jw_kint(j,"session",rec->session);jw_kint(j,"segment",rec->seg);
    jw_kint(j,"elapsed_ms",rec->elapsed_ms);jw_kint(j,"frames",rec->frames);jw_kint(j,"drops",rec->drops);jw_kint(j,"markers",rec->markers);
    jw_kbool(j,"can_start",common&&rec->state==REC_IDLE&&storage_writable(&storage_reason));
    jw_kbool(j,"can_stop",common&&(rec->state==REC_PREPARING||rec->state==REC_RUNNING));
    jw_kbool(j,"can_mark",common&&rec->state==REC_RUNNING);jw_kstr(j,"settings_page","rec");
    jw_end_obj(j);
}

/* ---------- 按键模拟：视图 ---------- */
static void items_keysim(jw_t *j)
{
    const keysim_t *ks = keysim_get();
    char l[16], v[TXT], a[24];
    if (!ks->available) {
        item(j, "本板没有按键模拟硬件", "需要 Hardware-C 原理图第 12 页", IF_DISABLED);
        return;
    }
    for (int c = 0; c < ks->channels; c++) {
        const ks_table_t *t = &ks->tables[c];
        bool on = c == ks->ch;
        const char *state = on ? (ks->pressed != -1 ? "按下中" : "已接通") : "断开";
        snprintf(l, sizeof l, "K%d", c + 1);
        if (t->valid)
            snprintf(v, sizeof v, "%s · %s · %u 个键值 · %s", state, t->name, (unsigned)t->n,
                     ks->sd_saved[c] ? "已存 SD" : "未存 SD");
        else
            snprintf(v, sizeof v, "%s · 未学习", state);
        item(j, l, v, on ? IF_ACTIVE : 0);
    }
    fmt_volt(a, sizeof a, ks->port_mv);
    if (ks->ovp)
        snprintf(v, sizeof v, "K%u 接口 %s，过压已断开 · OK 重新检测", (unsigned)ks->ch + 1u, a);
    else if (ks->supply_ok)
        snprintf(v, sizeof v, "已上电 · K%u 接口 %s", (unsigned)ks->ch + 1u, a);
    else
        snprintf(v, sizeof v, "未上电：先在发送报文页发唤醒报文");
    item(j, "APM 供电", v, ks->ovp || !ks->supply_ok ? IF_WARN : 0);
    item(j, "全部松开", "回到高阻", ks->pressed != -1 ? IF_ACTION : IF_DISABLED);
}

static void items_keych(jw_t *j)
{
    const keysim_t *ks = keysim_get();
    const ks_table_t *t = &ks->tables[ks->ch];
    keych_rows_t r = keych_rows();
    char v[TXT], a[24];
    int st = keysim_apm_state();
    fmt_volt(a, sizeof a, ks->port_mv);
    if (ks->ovp)
        snprintf(v, sizeof v, "%s · 过压，已断开", a);
    else
        snprintf(v, sizeof v, "%s · APM 判定：%s", a, st >= 0 ? keysim_state_name(st) : "读不到");
    item(j, "接口", v, ks->ovp ? IF_WARN : ks->pressed != -1 ? IF_ACTIVE : 0);
    item(j, "按法", ks->press_mode == KS_TAP ? "点按 300 ms（左右键切换）" : "保持：再按一次松开（左右键切换）", 0);
    if (!r.nkeys)
        item(j, "还没有键值表", "先学习或扫描", IF_DISABLED);
    for (int i = 0; i < r.nkeys; i++) {
        const ks_key_t *key = &t->keys[i];
        if (key->mv == 0u)
            snprintf(a, sizeof a, "直通 0 欧");
        else
            fmt_volt(a, sizeof a, key->mv);
        snprintf(v, sizeof v, "%s%s", a, ks->pressed == i ? " · 按下中" : "");
        item(j, key->name, v, ks->pressed == i ? IF_ACTIVE : IF_ACTION);
    }
    fmt_volt(a, sizeof a, ks->manual_mv);
    snprintf(v, sizeof v, "%s · 左右键调 0.1 V · OK %s", a, ks->pressed == KS_MANUAL ? "松开" : "按下");
    item(j, "手动电压", v, ks->pressed == KS_MANUAL ? IF_ACTIVE : IF_ACTION);
    item(j, "全部松开", "回到高阻", ks->pressed != -1 ? IF_ACTION : IF_DISABLED);
    item(j, "学习键值", "原车开关接着时用：设备只测量", IF_ACTION);
    item(j, "门限扫描", "没有原车开关时用：需要 APM 开关状态", IF_ACTION);
    snprintf(v, sizeof v, "%.1f 欧（%s）· OK 重新标定", (double)ks->r_series[ks->ch],
             ks->calibrated[ks->ch] ? "已标定" : "未标定，用默认值");
    item(j, "串联电阻", v, ks->calibrated[ks->ch] ? IF_ACTION : IF_WARN);
    int16_t err = ks->sd_error[ks->ch];
    if (!t->valid)
        item(j, "键值表", "还没有", IF_DISABLED);
    else if (ks->sd_saved[ks->ch]) {
        snprintf(v, sizeof v, "%s · 已存 SD 卡 APMKEY/K%u.TXT", t->name, (unsigned)ks->ch + 1u);
        item(j, "键值表", v, 0);
    } else if (err == KS_IO_ERR_UNAVAILABLE)
        item(j, "键值表", "只在内存：SD 卡不可用，插卡后自动保存", IF_WARN);
    else if (err)
        item(j, "键值表", "写入 SD 卡失败 · OK 重试", IF_WARN);
    else
        item(j, "键值表", "正在写入 SD 卡", IF_ACTIVE);
    (void)r;
}

static void items_keylearn(jw_t *j)
{
    const keysim_t *ks = keysim_get();
    char v[TXT], a[24], b[24], o[32], l[48];
    bool on = ks->activity == KS_LEARNING;
    if (on)
        snprintf(v, sizeof v, "进行中：按住原车开关一个档位（稳定 %u%%）· OK 暂停", (unsigned)ks->learn_progress);
    else
        snprintf(v, sizeof v, "OK 开始 · 设备只测量，不下拉");
    item(j, "学习", v, on ? IF_ACTIVE : IF_ACTION);
    fmt_volt(a, sizeof a, ks->port_mv);
    fmt_volt(b, sizeof b, ks->learn_idle_mv);
    if (ks->learn_idle_mv)
        snprintf(v, sizeof v, "%s · 空闲 %s", a, b);
    else
        snprintf(v, sizeof v, "%s", a);
    item(j, "接口电压", v, 0);
    for (int i = 0; i < ks->learn_n; i++) {
        const ks_key_t *key = &ks->learn_keys[i];
        fmt_volt(a, sizeof a, key->mv);
        fmt_ohm_cn(o, sizeof o, key->ohm);
        snprintf(l, sizeof l, "第 %d 档 %s", i + 1, key->name);
        snprintf(v, sizeof v, "%s · %s · OK 改名", a, o);
        item(j, l, v, 0);
    }
    item(j, "重学", "清空已记录的档位", ks->learn_n ? IF_ACTION : IF_DISABLED);
    snprintf(v, sizeof v, "存为 K%u 的键值表", (unsigned)ks->ch + 1u);
    item(j, "保存", v, ks->learn_n ? IF_ACTION : IF_DISABLED);
}

static void items_keyscan(jw_t *j)
{
    const keysim_t *ks = keysim_get();
    char v[TXT], a[24], b[24], l[48], from[16];
    int st = keysim_apm_state();
    if (ks->activity == KS_SCANNING) {
        fmt_volt(a, sizeof a, ks->scan_mv);
        snprintf(v, sizeof v, "进行中：%s · OK 中止", a);
    } else if (ks->scan_done) {
        snprintf(v, sizeof v, "完成：%u 个门限 · OK 重新扫描", (unsigned)ks->scan_n);
    } else {
        snprintf(v, sizeof v, "OK 开始 · 原车开关要先断开 · 约 4 秒");
    }
    item(j, "扫描", v, ks->activity == KS_SCANNING ? IF_ACTIVE : IF_ACTION);
    if (st >= 0)
        snprintf(v, sizeof v, "APM 上报 · 当前 %s", keysim_state_name(st));
    else
        snprintf(v, sizeof v, "读不到：需要配置 CAN 开关状态信号");
    item(j, "开关状态来源", v, st >= 0 ? 0 : IF_WARN);
    for (int i = 0; i < ks->scan_n; i++) {
        const ks_edge_t *e = &ks->scan_edges[i];
        snprintf(from, sizeof from, "%s", keysim_state_name(e->from));
        snprintf(l, sizeof l, "%s → %s", from, keysim_state_name(e->to));
        fmt_volt(a, sizeof a, e->below_mv);
        fmt_volt(b, sizeof b, e->above_mv);
        snprintf(v, sizeof v, "%s – %s", a, b);
        item(j, l, v, 0);
    }
    item(j, "保存", "每档取中点存为键值表", ks->scan_done && ks->scan_n ? IF_ACTION : IF_DISABLED);
}

static const char *keysim_title(void)
{
    static char t[24];
    if (page != PG_KEYCH)
        return page_title[page];
    snprintf(t, sizeof t, "K%u 按键模拟", (unsigned)keysim_get()->ch + 1u);
    return t;
}

static const char *keysim_hint(void)
{
    switch (page) {
    case PG_KEYSIM:   return "OK 接通这一路并进入 · 同一时刻只接通一路";
    case PG_KEYCH:    return "OK 按键 · 左右键切换按法或调电压 · BACK 松开并返回";
    case PG_KEYLEARN: return "按住原车开关一个档位 0.5 秒自动记录 · BACK 退出，未保存的保留到下次";
    case PG_KEYSCAN:  return "设备代替开关从 4.8 V 往下扫 · BACK 中止并回到高阻";
    default:          return "";
    }
}

void ui_json(jw_t *j)
{
    memset(&item_window,0,sizeof item_window);
    jw_key(j, "ui");
    jw_obj(j);
    record_controls_json(j);
    if(local_files_active())local_files_json(j);
    else
    {
    item_window.total=item_count(page);
#ifdef CANFD_REVD_TARGET
    item_window.bounded=page==PG_FILES||page==PG_PROJECTFILES||page==PG_PLAY;
    if(item_window.bounded){
        cursor[page]=clampi(cursor[page],0,item_window.total>0?item_window.total-1:0);
        item_window.start=(cursor[page]/6)*6;
    }
#endif
    jw_kstr(j, "page", page_id[page]);
    jw_kstr(j, "title", keysim_title());
    jw_kint(j, "cursor", cursor[page]);
    jw_kint(j,"items_start",item_window.start);jw_kint(j,"items_total",item_window.total);
    jw_kint(j,"tx_channel",local_tx_channel);
    jw_kint(j,"tx_scope_mask",tx_scope_mask);
    jw_kint(j,"tx_load_file_index",TX_LOAD_FILE_INDEX);
    jw_kstr(j,"send_files_return",page_id[send_files_return]);
    jw_kint(j,"home_columns",HOME_COLUMNS);
    jw_key(j,"files_state");jw_obj(j);
    jw_kstr(j,"status",storage_state_name(storage_get()->state));jw_kbool(j,"usb",storage_get()->usb_attached);
    jw_kbool(j,"read_only",storage_get()->state!=ST_READY);jw_kint(j,"free_kb",port_sd_free_kb());jw_kint(j,"total_kb",port_sd_total_kb());
    jw_end_obj(j);
    if(page==PG_TX)tx_channels_json(j);
    const char *hint = page == PG_TX ? "OK 按范围单发 · 长按 OK 编辑 · → 本槽独立周期 · ← 全停" :
        page == PG_LIVE ? "← CAN1 / → CAN2 · OK详情 · 长OK菜单：滚动/ID、筛选、录制设置" :
        page == PG_DETAIL ? "←→ 上一条 / 下一条 · ↑↓ 查看 · BACK 返回原列表" :
        page==PG_TXSET ? (numeric?"←→ 选位 · ↑↓ 改值 · OK 完成 · BACK 撤销字段":"↑↓ 选择 · OK 设置/保存 · BACK 取消本路草稿") :
        page==PG_TXTIMING ? (numeric?"←→ 选位 · ↑↓ 改值 · OK 完成 · BACK 撤销字段":"↑↓ 选字段 · OK 编辑 · BACK 回总览（保留草稿）") :
        page==PG_SENDMODE ? "现有发送文件或本机手动编辑 · BACK 返回" :
        page==PG_PROJECTFILES ? "选文件即加载到手动报文页 · 替换所含通道 · 不自动发送" :
        page==PG_FILEDETAIL ? "文件信息与操作 · BACK 返回文件列表" :
        page==PG_PLAY ? "保存当前屏为文件 · 选择方案预览 · 加载不自动发送" :
        page==PG_PLANDETAIL ? "OK 加载/执行 · 可逐条查看完整数据 · BACK 返回" :
        page==PG_PLANENTRY ? "只读快照 · ↑↓ 查看完整载荷 · BACK 返回方案" :
        (page == PG_TXEDIT || page == PG_FILTER) ?
        (numeric ? "←→ 选择位 · ↑↓ 改值 · OK 完成 · BACK 撤销本字段" : "↑↓ 选字段 · ←→ 切换 · OK 编辑数值 / 执行 · BACK 取消") : keysim_hint();
    jw_kstr(j, "hint", hint);
    jw_kbool(j, "numeric", numeric);
    jw_kint(j, "digit", numeric_digit);
    uint32_t numeric_value,numeric_maximum;int numeric_base,numeric_digits;
    jw_kint(j,"numeric_digits",numeric&&numeric_field(&numeric_value,&numeric_maximum,&numeric_base,&numeric_digits)?numeric_digits:0);
    local_tx_json(j);
    jw_key(j,"plan_view");jw_obj(j);
    jw_kint(j,"save_channel",plan_save_channel);
    jw_kint(j,"selected_index",plan_selected_index);jw_kint(j,"selected_token",plan_selected.token);
    jw_kstr(j,"selected_name",plan_selected.name);jw_kint(j,"selected_mask",plan_selected.plan.channel_mask);
    jw_kint(j,"entry_channel",plan_entry_ch);jw_kint(j,"entry_index",plan_entry_index);
    jw_end_obj(j);
    disp_filter_t current_filter;
    disp_filter_get_ex(&current_filter);
    filter_json(j, "filter_cfg", page == PG_FILTER ? &filter_draft : &current_filter);
    jw_key(j, "editor"); jw_obj(j);
    jw_kbool(j, "open", editor.open);
    if (editor.open) {
        jw_kint(j, "ch", editor.ch); jw_kint(j, "entry", editor.idx);
        jw_kint(j, "id", editor.frame.id); jw_kint(j, "flags", editor.frame.flags);
        jw_kint(j, "len", editor.frame.len); jw_kint(j, "period_ms", editor.period);
        jw_kint(j, "byte", editor.byte); jw_kbool(j, "dirty", editor.dirty);
        jw_key(j, "data"); jw_arr(j);
        for (int b = 0; b < 64; b++) jw_int(j, editor.frame.data[b]);
        jw_end_arr(j);
    }
    jw_end_obj(j);
    if (page == PG_LIVE) {
        live_json(j);
    } else if (page == PG_DETAIL) {
        detail_json(j);
    } else {
        jw_key(j, "items");
        jw_arr(j);
        switch (page) {
        case PG_HOME:
            for (int i = 0; i < HOME_COUNT; i++)
                item(j, page_title[home_tiles[i]], "", 0);
            break;
        case PG_TX:     items_tx(j); break;
        case PG_TXEDIT: items_editor(j); break;
        case PG_TXSET: case PG_TXTIMING: items_local_tx(j); break;
        case PG_FILTER: items_filter(j); break;
        case PG_REC:    items_rec(j); break;
        case PG_PLAY:
            items_plan(j);break;
        case PG_SENDMODE:items_send_mode(j);break;
        case PG_PROJECTFILES:items_send_files(j);break;
        case PG_FILEDETAIL:items_file_detail(j);break;
        case PG_PLANDETAIL:items_plan_detail(j);break;
        case PG_PLANENTRY:items_plan_entry(j);break;
        case PG_DIAG:   items_diag(j); break;
        case PG_FILES:  items_files(j); break;
        case PG_CHSET:  items_chset(j); break;
        case PG_SYSSET: items_sysset(j); break;
        case PG_ENG:    items_eng(j); break;
        case PG_KEYSIM: items_keysim(j); break;
        case PG_KEYCH:  items_keych(j); break;
        case PG_KEYLEARN: items_keylearn(j); break;
        case PG_KEYSCAN:  items_keyscan(j); break;
        default: break;
        }
        jw_end_arr(j);
    }
    }
    jw_key(j, "dlg");
    if (n_dlg) {
        const dlg_t *d = &dlgq[0];
        static const char *const kinds[] = { "ask", "menu", "info", "ask" };
        jw_obj(j);
        jw_kstr(j, "kind", kinds[d->kind]);
        jw_kstr(j, "text", d->text);
        bool rate = d->kind == DK_ASK && d->ask == ASK_PLAY_RATE;
        jw_kstr(j, "ok", d->kind == DK_INFO ? "关闭" : rate ? "套用文件参数" : "确认");
        jw_kstr(j, "cancel", d->kind == DK_INFO ? "" : rate ? "按当前参数" : "取消");
        jw_kint(j, "cur", d->cur);
        jw_kint(j, "queued", n_dlg - 1);
        if (d->kind == DK_MENU) {
            jw_key(j, "opts");
            jw_arr(j);
            for (int i = 0; i < menu_count(d); i++)
                jw_str(j,d->id==MENU_LIVE_ACTIONS?live_menu_opts[i]:menu_opts[i]);
            jw_end_arr(j);
        }
        jw_end_obj(j);
    } else {
        jw_obj(j);
        jw_end_obj(j);
    }
    jw_kstr(j, "toast", (int32_t)(toast_until - app_now_ms()) > 0 ? toast : "");
    jw_key(j, "log");
    jw_arr(j);
    for (int i = 0; i < log_n; i++) {
        int idx = (log_pos + LOG_LEN - log_n + i) % LOG_LEN;
        jw_obj(j);
        jw_kint(j, "t", logbuf[idx].t);
        jw_kstr(j, "s", logbuf[idx].s);
        jw_end_obj(j);
    }
    jw_end_arr(j);
    jw_end_obj(j);
}
