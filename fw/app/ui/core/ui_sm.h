/* 界面状态机：页面导航、按键 / 触摸到功能的映射、确认对话框、提示条。
 * 只维护"界面状态"并产出视图模型（ui_json），不画图：
 * 固件里由 LVGL 按视图模型绘制，模拟器里由网页绘制。
 *
 * 页面：主页 → 6 个功能页（实时显示 / 发送报文 / 总线诊断 / 文件管理 / 通道设置 / 系统设置），
 *       板上有按键模拟硬件时再加第 7 个“按键模拟”，
 *       系统设置里进工程菜单。BACK 短按返回，长按回主页。
 * 对话框：服务层发来的确认请求（EV_NTF_ASK）排队显示；OK 确认，BACK 取消。
 * 全局键：REC 开始 / 停止录制；POWER 由电源状态机处理。 */
#ifndef UI_SM_H
#define UI_SM_H

#include "services/core/app_types.h"
#include "services/core/jsonw.h"

typedef enum {
    PG_HOME, PG_LIVE, PG_TX, PG_REC, PG_PLAY, PG_DIAG, PG_FILES, PG_CHSET, PG_SYSSET, PG_ENG, PG_TXEDIT, PG_FILTER, PG_DETAIL,
    PG_TXSET, PG_TXTIMING, PG_LOGPLAY, PG_PLANDETAIL, PG_PLANENTRY,
    PG_SENDMODE, PG_PROJECTS, PG_PROJECTFILES, PG_FILEDETAIL,
    /* APM 按键模拟（Hardware-C 第 12 页）。追加在末尾，保持既有页面编号不变。 */
    PG_KEYSIM, PG_KEYCH, PG_KEYLEARN, PG_KEYSCAN, PG_NUM
} page_t;

/* Long OK / touch on a TX row opens a local draft. Host edits use tx_entry_update
 * directly; saving a stale device draft is rejected rather than overwriting it. */
bool        ui_tx_edit_open(int ch, int idx);
void        ui_tx_edit_cancel(void);
void        ui_filter_open(void);
/* Retained ABI only: captured-log replay has been removed, returns false. */
bool        ui_play_select(int ch, int idx);
typedef enum {
    UI_REVIEW_OK = 0, UI_REVIEW_NOT_READY = -1, UI_REVIEW_LOCKED = -2,
    UI_REVIEW_DIALOG = -3, UI_REVIEW_DRAFT = -4, UI_REVIEW_PAGE = -5, UI_REVIEW_RANGE = -6
} ui_review_result_t;
/* Navigation/review only: no transmission, recording or destructive action. */
int         ui_review_navigate(page_t target);
int         ui_tx_select(int ch, int idx); /* Select only; never edits or sends. */
/* Manual-send scope: independent of the channel currently being edited. */
int         ui_tx_scope_select(uint32_t mask);
/* Normal send target: atomically selects visible list(s) and send mask. */
int         ui_tx_target_select(uint32_t mask);
/* Explicit single-channel switch. Actual running state determines idempotence. */
int         ui_tx_channel_send(int ch,bool enabled);
/* Explicit row action: sends only this row's channel, preserving list scope. */
int         ui_tx_single(int ch,int index);
/* Local device menus: 0 grouped-loop overview, 1 one-slot timing editor.
 * Further file/name kinds are provided by the local simulated SD catalogue. */
int         ui_local_open(int kind, int ch, int idx);
/* Send-plan local entry: 0 flat file list (selection loads into manual TX;
 * BACK returns to its sender), 1 save this channel,
 * 2 save both channels. Legacy captured-log replay mode is rejected. */
int         ui_plan_open(int mode, int ch);
/* Touch selection binds the displayed catalogue identity; stale rows reject. */
int         ui_plan_file_select(unsigned index,uint32_t expected_token);
bool        ui_draft_open(void);
/* Copy the fixed full-detail snapshot into a local, unsaved TX editor. */
int         ui_detail_to_tx(int ch, int idx);
int         ui_review_filter_open(void);
int         ui_live_set_view(unsigned view); /* 0 scroll, 1 ID summary */
/* Presentation only: 1 CAN1 / 2 CAN2; 0 retains the internal all-channel view.
 * Does not change acquisition filters, recorder mask or cached history. */
int         ui_live_set_channel(unsigned channel);
int         ui_live_scroll(int rows); /* Signed row delta; preserves acquisition and selected channel. */
int         ui_live_set_paused(bool paused);
int         ui_live_open_detail(unsigned index);
int         ui_live_open_detail_seq(uint32_t sequence); /* Stable scroll selection. */
void        ui_init(void);
void        ui_handle(const app_evt_t *e);
void        ui_json(jw_t *j);
page_t      ui_page(void);
const char *ui_base_page_id(void); /* Underlying page, excluding keyboard overlays. */
int         ui_cursor(void);
bool        ui_dialog_open(void);
int         ui_dialog_ask(void);       /* 当前对话框对应的 ask_t，没有则 -1 */
const char *ui_last_toast(void);

#endif
