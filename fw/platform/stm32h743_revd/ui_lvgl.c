/* LVGL adapter for the existing ui_json view model. No duplicate state machine:
 * screen touches and the on-screen keys enter the same app_touch/app_key_raw
 * paths as the browser/physical keys, including lock, drafts and confirmation.
 * Fixed widgets and bounded snapshots avoid per-refresh object allocation. */
#include "ui_lvgl.h"
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/jsonw.h"
#include "services/power/power_sm.h"
#include "power_reve.h"
#include "display.h"
#ifndef REVD_FREERTOS
#include "sim/local_files_ui.h"
#include "sim/tx_scope.h"
#include "sim/send_plan.h"
#include "services/core/app_timer.h"
#endif
#ifdef REVD_FREERTOS
#include "rtos/bridges.h"
#define app_touch revd_ui_touch
#define app_key_raw revd_ui_key
#define power_get revd_ui_power
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CANFD_REVD_TARGET
#define SNAP_BYTES 16384
#else
#define SNAP_BYTES 24576
#endif
#define TOKEN_MAX 2560
#define ROWS 6
#define MODAL_OPTIONS 7
#define COL_BG 0x0b1322
#define COL_BOX 0x17253c
#define COL_SEL 0x244e7b
#define COL_TEXT 0xeaf1ff
#define COL_DIM 0x94a9c8
#define COL_WARN 0xffc56d
#define COL_GOOD 0x79dfb7

/* This small parser only consumes our own bounded jsonw output, not USB input.
 * A token's next index skips its entire subtree, including container children. */
typedef struct { uint16_t start, end, next, children; } token_t;
static char snap[SNAP_BYTES];
static token_t tokens[TOKEN_MAX];
static int ntok;
static uint32_t snapshot_errors;
static size_t peak_snapshot_bytes;
static unsigned peak_snapshot_tokens;
static size_t pos, json_len;
static bool parse_error;

static void space(void)
{
    while (pos < json_len && (snap[pos] == ' ' || snap[pos] == '\n' || snap[pos] == '\r' || snap[pos] == '\t')) pos++;
}
static int parse_value(unsigned depth)
{
    space();
    if (parse_error || pos >= json_len || depth > 12 || ntok >= TOKEN_MAX) { parse_error = true; return -1; }
    int idx = ntok++;
    token_t *t = &tokens[idx];
    t->children = 0; t->start = (uint16_t)pos;
    char c = snap[pos++];
    if (c == '{' || c == '[') {
        char close = c == '{' ? '}' : ']';
        space();
        while (pos < json_len && snap[pos] != close) {
            if (snap[pos] == ',' || snap[pos] == ':') { pos++; continue; }
            if (parse_value(depth + 1) < 0) return -1;
            t->children++;
            space();
        }
        if (pos >= json_len) { parse_error = true; return -1; }
        pos++;
        t->end = (uint16_t)pos;
    } else if (c == '"') {
        t->start = (uint16_t)pos;
        while (pos < json_len && snap[pos] != '"') {
            if (snap[pos] == '\\') pos++;
            pos++;
        }
        if (pos >= json_len) { parse_error = true; return -1; }
        t->end = (uint16_t)pos++;
    } else {
        while (pos < json_len && snap[pos] != ',' && snap[pos] != '}' && snap[pos] != ']' && snap[pos] != ' ') pos++;
        t->end = (uint16_t)pos;
    }
    t->next = (uint16_t)ntok;
    return idx;
}
static bool equals(int t, const char *s)
{
    if (t < 0 || t >= ntok) return false;
    size_t n = tokens[t].end - tokens[t].start;
    return strlen(s) == n && !memcmp(snap + tokens[t].start, s, n);
}
static int field(int obj, const char *key)
{
    if (obj < 0 || obj >= ntok) return -1;
    for (int i = obj + 1; i < tokens[obj].next;) {
        int val = i + 1;
        if (val >= tokens[obj].next) break;
        if (equals(i, key)) return val;
        i = tokens[val].next;
    }
    return -1;
}
static int at(int arr, int n)
{
    if (arr < 0 || arr >= ntok || n < 0) return -1;
    int i = arr + 1;
    while (n-- && i < tokens[arr].next) i = tokens[i].next;
    return i < tokens[arr].next ? i : -1;
}
static int count(int arr)
{
    return arr >= 0 && arr < ntok ? tokens[arr].children : 0;
}
static uint32_t number(int t)
{
    return t >= 0 && t < ntok ? (uint32_t)strtoul(snap + tokens[t].start, NULL, 10) : 0;
}
static int hex_digit(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
}
static void string(int t, char *out, size_t cap)
{
    size_t n = 0;
    if (t >= 0 && t < ntok) {
        size_t i = tokens[t].start, end = tokens[t].end;
        while (i < end && n + 1 < cap) {
            char c = snap[i++];
            if (c == '\\' && i < end) {
                c = snap[i++];
                /* jsonw escapes only ASCII control codes as \\u00XX. */
                if (c == 'u' && i + 4 <= end) {
                    unsigned cp = 0;
                    for (int j = 0; j < 4; j++) cp = cp * 16u + (unsigned)hex_digit(snap[i++]);
                    c = cp < 0x20 ? ' ' : cp < 0x80 ? (char)cp : '?';
                } else if (c == 'n' || c == 'r' || c == 't') c = ' ';
            }
            out[n++] = c;
        }
        /* Keep UTF-8 complete when truncating a long filename/value. */
        if (i < end && n) {
            while (n && ((unsigned char)out[n - 1] & 0xc0u) == 0x80u) n--;
            if (n && ((unsigned char)out[n - 1] & 0x80u)) n--;
        }
    }
    if (cap) out[n] = '\0';
}

/* All widgets are allocated once. Page refresh only changes bounds/text/state. */
#define KEY_CELLS 40
#define KEY_ACTIONS 6
#define TX_SLOTS 8
static lv_obj_t *screen,*title,*status,*footer,*modal,*modal_text,*rec_button;
static lv_obj_t *home[8],*home_label[8],*rows[ROWS],*row_label[ROWS],*soft[5];
static lv_obj_t *choices[MODAL_OPTIONS],*choice_label[MODAL_OPTIONS];
static lv_obj_t *send_panel,*send_switch[2],*send_switch_label[2],*send_load,*send_save;
static lv_obj_t *send_head[2],*send_head_label[2],*send_loop[2],*send_loop_label[2];
static lv_obj_t *send_row[TX_SLOTS],*send_main[TX_SLOTS],*send_data[TX_SLOTS];
static lv_obj_t *send_one[TX_SLOTS],*send_one_label[TX_SLOTS],*send_time[TX_SLOTS],*send_time_label[TX_SLOTS];
static lv_obj_t *live_tabs[2],*live_tab_label[2],*live_mode,*live_pause,*live_filter,*live_record;
static lv_obj_t *editor_actions[3];
static lv_obj_t *input_value,*input_info,*input_matrix,*input_prev,*input_next;
static char matrix_labels[KEY_CELLS+KEY_ACTIONS][48];
static const char *matrix_map[KEY_CELLS+KEY_ACTIONS+12];

static uint32_t last_refresh,rendered_keyboard,action_notice_until;
static char action_notice[192];
static lv_indev_t *configured_pointer;
static page_t rendered_page;
static bool rendered_dialog,rendered_input,rendered_send,rendered_live;
static int rendered_cursor,input_page,last_input_cursor;
#ifdef REVD_FREERTOS
static uint32_t rendered_generation;
#endif

typedef enum { ACT_ITEM,ACT_SWITCH,ACT_LOAD,ACT_SAVE,ACT_LOOP,ACT_EDIT,ACT_SINGLE,ACT_TIMING,
               ACT_CHANNEL,ACT_VIEW,ACT_PAUSE,ACT_FILTER,ACT_LIVE_ROW,ACT_INPUT_PAGE,ACT_BACKSPACE,ACT_PLAN_FILE } action_t;
typedef struct {
    int index,a,b,pressed_index,pressed_a,pressed_b;
    action_t action,pressed_action;
    page_t page;
    uint32_t identity,pressed_identity,keyboard;
    bool dialog,valid;
#ifdef REVD_FREERTOS
    uint32_t generation;
#endif
} binding_t;
static binding_t home_binding[8],row_binding[ROWS],choice_binding[MODAL_OPTIONS];
static binding_t switch_binding[2],load_binding,save_binding,loop_binding[2];
static binding_t edit_binding[TX_SLOTS],one_binding[TX_SLOTS],time_binding[TX_SLOTS];
static binding_t tab_binding[2],mode_binding,pause_binding,filter_binding,editor_binding[3];
static binding_t input_binding[KEY_CELLS+KEY_ACTIONS],prev_binding,next_binding;
static binding_t matrix_press;
static unsigned matrix_count,matrix_pressed_slot;
static bool matrix_holding;
static uint32_t matrix_token;
static int matrix_page;
static struct { bool active,moved;int y;page_t page; } drag;

static void visible(lv_obj_t *o,bool on) { if(on)lv_obj_remove_flag(o,LV_OBJ_FLAG_HIDDEN);else lv_obj_add_flag(o,LV_OBJ_FLAG_HIDDEN); }
static void text(lv_obj_t *o,const char *s) { if(strcmp(lv_label_get_text(o),s))lv_label_set_text(o,s); }
static void token_text(lv_obj_t *o,int t) { char s[512];string(t,s,sizeof s);text(o,s); }
static void small(lv_obj_t *o) { lv_obj_set_style_text_font(o,&lv_font_montserrat_14,0); }
static lv_obj_t *label(lv_obj_t *parent,int x,int y,int w,int h,bool wrap)
{
    lv_obj_t *o=lv_label_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_label_set_long_mode(o,wrap?LV_LABEL_LONG_WRAP:LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(o,lv_color_hex(COL_TEXT),0);lv_obj_set_style_text_font(o,LV_FONT_DEFAULT,0);
    lv_obj_set_style_text_line_space(o,2,0);lv_label_set_text(o,"");return o;
}
static lv_obj_t *button(lv_obj_t *parent,int x,int y,int w,int h)
{
    lv_obj_t *o=lv_button_create(parent);lv_obj_set_pos(o,x,y);lv_obj_set_size(o,w,h);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_pad_all(o,0,0);lv_obj_set_style_radius(o,4,0);
    lv_obj_set_style_border_width(o,1,0);lv_obj_set_style_border_color(o,lv_color_hex(0x304967),0);
    lv_obj_set_style_bg_color(o,lv_color_hex(COL_BOX),0);lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_bg_color(o,lv_color_hex(COL_SEL),LV_STATE_PRESSED);return o;
}
static void style_selected(lv_obj_t *o,bool selected,unsigned flags)
{
    lv_obj_set_style_bg_color(o,lv_color_hex(selected?COL_SEL:COL_BOX),0);
    lv_obj_set_style_border_color(o,lv_color_hex(selected?0x74b9fa:0x304967),0);
    lv_obj_set_style_text_color(o,lv_color_hex(flags&4u?COL_WARN:flags&8u?COL_GOOD:flags&2u?COL_DIM:COL_TEXT),0);
}
static void enabled(lv_obj_t *o,bool on) { if(on)lv_obj_remove_state(o,LV_STATE_DISABLED);else lv_obj_add_state(o,LV_STATE_DISABLED); }
static void dispatch(binding_t *b)
{
#ifdef REVD_FREERTOS
    /* Preserve the existing event-only RTOS bridge. New product actions belong
     * to the bare-metal target; no mutable service is entered by its UI task. */
    if(b->action==ACT_ITEM)app_touch(b->dialog?EV_TOUCH_DLG:EV_TOUCH_ITEM,b->index);
#else
    int result=0;
    switch(b->action) {
    case ACT_ITEM:app_touch(b->dialog?EV_TOUCH_DLG:EV_TOUCH_ITEM,b->index);break;
    case ACT_SWITCH:result=ui_tx_channel_send(b->a,b->b!=0);break;
    case ACT_LOAD:(void)ui_plan_open(0,0);break;
    case ACT_SAVE:(void)ui_plan_open(2,0);break;
    case ACT_LOOP:(void)ui_local_open(0,b->a,0);break;
    case ACT_EDIT:(void)ui_tx_edit_open(b->a,b->b);break;
    case ACT_SINGLE:result=ui_tx_single(b->a,b->b);break;
    case ACT_TIMING:(void)ui_local_open(1,b->a,b->b);break;
    case ACT_CHANNEL:(void)ui_live_set_channel((unsigned)b->a);break;
    case ACT_VIEW:(void)ui_live_set_view((unsigned)b->a);break;
    case ACT_PAUSE:(void)ui_live_set_paused(b->a!=0);break;
    case ACT_FILTER:(void)ui_review_filter_open();break;
    case ACT_LIVE_ROW:if(b->b)(void)ui_live_open_detail_seq(b->identity);else(void)ui_live_open_detail((unsigned)b->index);break;
    case ACT_INPUT_PAGE:input_page+=b->a;if(input_page<0)input_page=0;break;
    case ACT_BACKSPACE:break; /* press/release owns repetition; never double delete */
    case ACT_PLAN_FILE:result=ui_plan_file_select((unsigned)b->index,b->identity);break;
    }
    app_tick(0);
    if(result<0){
        const char *reason=power_get()->locked?"请先解除按键锁":ui_draft_open()?"请先保存或取消草稿":ui_dialog_open()?"请先完成当前确认":b->action==ACT_PLAN_FILE?sp_result_text(result):scope_result_text(result);
        snprintf(action_notice,sizeof action_notice,"%s",reason);action_notice_until=app_now_ms()+2500u;
    }else action_notice[0]=0;
#endif
    last_refresh=0;
}
static bool context_valid(binding_t *b)
{
#ifdef REVD_FREERTOS
    return rendered_page==b->page&&rendered_dialog==b->dialog&&rendered_generation==b->generation;
#else
    return ui_page()==b->page&&ui_dialog_open()==b->dialog&&
        (!b->keyboard||b->keyboard==local_files_keyboard_token());
#endif
}
static void item_event(lv_event_t *e)
{
    binding_t *b=lv_event_get_user_data(e);lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED){
        b->pressed_index=b->index;b->pressed_a=b->a;b->pressed_b=b->b;b->pressed_action=b->action;b->pressed_identity=b->identity;
#ifdef REVD_FREERTOS
        b->page=rendered_page;b->dialog=rendered_dialog;b->generation=rendered_generation;
#else
        b->page=ui_page();b->dialog=ui_dialog_open();
#endif
        b->keyboard=rendered_keyboard;b->valid=true;
#ifndef REVD_FREERTOS
        if(b->action==ACT_BACKSPACE&&b->keyboard){(void)local_files_backspace_begin(b->keyboard);last_refresh=0;}
#endif
    } else if(code==LV_EVENT_PRESSING){
#ifndef REVD_FREERTOS
        if(b->valid&&b->pressed_action==ACT_BACKSPACE&&context_valid(b))(void)local_files_backspace_begin(b->keyboard);
#endif
    } else if(code==LV_EVENT_PRESS_LOST||code==LV_EVENT_RELEASED){
#ifndef REVD_FREERTOS
        if(b->pressed_action==ACT_BACKSPACE&&b->keyboard)(void)local_files_backspace_end(b->keyboard);
#endif
        if(code==LV_EVENT_PRESS_LOST)b->valid=false;
    } else if(code==LV_EVENT_CLICKED){
        if(b->valid&&b->pressed_index==b->index&&b->pressed_a==b->a&&b->pressed_b==b->b&&
           b->pressed_action==b->action&&b->pressed_identity==b->identity&&context_valid(b)&&
           !(b->action==ACT_LIVE_ROW&&drag.moved))dispatch(b);
        b->valid=false;
    }
}
static void bind(lv_obj_t *o,binding_t *b) { lv_obj_add_event_cb(o,item_event,LV_EVENT_ALL,b); }
static lv_obj_t *action_button(lv_obj_t *parent,int x,int y,int w,int h,const char *s,binding_t *b,lv_obj_t **out)
{
    lv_obj_t *o=button(parent,x,y,w,h),*l=label(o,2,5,w-4,h-7,false);
    lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);text(l,s);bind(o,b);if(out)*out=l;return o;
}
static void back_event(lv_event_t *e) { if(lv_event_get_code(e)==LV_EVENT_CLICKED){app_touch(EV_TOUCH_BACK,0);last_refresh=0;} }
static void key_event(lv_event_t *e)
{
    key_id_t k=(key_id_t)(uintptr_t)lv_event_get_user_data(e);
    switch(lv_event_get_code(e)){case LV_EVENT_PRESSED:app_key_raw(k,true);break;case LV_EVENT_RELEASED:case LV_EVENT_PRESS_LOST:app_key_raw(k,false);break;default:break;}
}
static void live_drag(int y,bool begin)
{
    if(begin){drag.active=true;drag.moved=false;drag.y=y;drag.page=rendered_page;return;}
    if(!drag.active||drag.page!=PG_LIVE||rendered_page!=PG_LIVE||rendered_dialog)return;
    int delta=drag.y-y;if(delta<15&&delta> -15)return;
    drag.moved=true;drag.y=y;
#ifndef REVD_FREERTOS
    int steps=delta/22;if(!steps)steps=delta>0?1:-1;
    if(steps>8)steps=8;
    if(steps< -8)steps= -8;
    (void)ui_live_scroll(steps);last_refresh=0;
#endif
}
static void live_gesture(lv_event_t *e)
{
    if(!rendered_live||rendered_dialog)return;
    lv_indev_t *indev=lv_indev_active();if(!indev)return;
    lv_point_t p;lv_indev_get_point(indev,&p);
    lv_event_code_t code=lv_event_get_code(e);
    if(code==LV_EVENT_PRESSED)live_drag(p.y,true);
    else if(code==LV_EVENT_PRESSING)live_drag(p.y,false);
    else if(code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)drag.active=false;
}

static void matrix_event(lv_event_t *e)
{
    lv_event_code_t code=lv_event_get_code(e);unsigned slot=lv_buttonmatrix_get_selected_button(input_matrix);
    if(code==LV_EVENT_PRESSED&&slot<matrix_count){
        matrix_press=input_binding[slot];matrix_pressed_slot=slot;matrix_holding=true;
        matrix_press.page=rendered_page;matrix_press.dialog=rendered_dialog;matrix_press.keyboard=rendered_keyboard;
#ifndef REVD_FREERTOS
        if(matrix_press.action==ACT_BACKSPACE)(void)local_files_backspace_begin(matrix_press.keyboard);
#endif
    }else if(code==LV_EVENT_PRESSING&&matrix_holding){
#ifndef REVD_FREERTOS
        if(matrix_press.action==ACT_BACKSPACE){
            if(slot==matrix_pressed_slot&&context_valid(&matrix_press))(void)local_files_backspace_begin(matrix_press.keyboard);
            else {(void)local_files_backspace_end(matrix_press.keyboard);matrix_holding=false;}
        }
#endif
    }else if(code==LV_EVENT_VALUE_CHANGED&&matrix_holding&&slot==matrix_pressed_slot&&context_valid(&matrix_press)){
        if(matrix_press.action!=ACT_BACKSPACE)dispatch(&matrix_press);
    }else if(code==LV_EVENT_PRESS_LOST||code==LV_EVENT_RELEASED){
#ifndef REVD_FREERTOS
        if(matrix_holding&&matrix_press.action==ACT_BACKSPACE)(void)local_files_backspace_end(matrix_press.keyboard);
#endif
        matrix_holding=false;
    }
    if(code==LV_EVENT_PRESSED||code==LV_EVENT_RELEASED||code==LV_EVENT_PRESS_LOST)last_refresh=0;
}

void ui_lvgl_init(lv_display_t *display)
{
    configured_pointer=NULL;screen=lv_display_get_screen_active(display);lv_obj_remove_flag(screen,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen,lv_color_hex(COL_BG),0);lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_set_style_pad_all(screen,0,0);lv_obj_set_style_border_width(screen,0,0);lv_obj_set_style_text_font(screen,LV_FONT_DEFAULT,0);
    lv_obj_t *back=button(screen,4,4,52,34);text(label(back,7,8,42,20,false),"返回");lv_obj_add_event_cb(back,back_event,LV_EVENT_CLICKED,NULL);
    lv_obj_t *clip=lv_obj_create(screen);lv_obj_set_pos(clip,66,4);lv_obj_set_size(clip,340,22);
    lv_obj_set_style_pad_all(clip,0,0);lv_obj_set_style_border_width(clip,0,0);lv_obj_set_style_bg_opa(clip,LV_OPA_TRANSP,0);
    lv_obj_remove_flag(clip,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_OVERFLOW_VISIBLE);title=label(clip,0,0,336,22,false);
    status=label(screen,66,25,340,19,false);lv_obj_set_style_text_color(status,lv_color_hex(COL_DIM),0);
    rec_button=button(screen,414,4,62,34);text(label(rec_button,11,8,45,20,false),"REC");lv_obj_add_event_cb(rec_button,key_event,LV_EVENT_ALL,(void *)(uintptr_t)KEY_REC);
    for(int i=0;i<8;i++){
        home[i]=action_button(screen,4+(i%4)*119,58+(i/4)*83,115,75,"",&home_binding[i],&home_label[i]);
        lv_obj_set_y(home_label[i],25);home_binding[i].index=i;
    }
    for(int i=0;i<ROWS;i++){
        rows[i]=button(screen,4,47+i*31,472,29);row_label[i]=label(rows[i],5,5,462,20,false);bind(rows[i],&row_binding[i]);
        lv_obj_add_event_cb(rows[i],live_gesture,LV_EVENT_ALL,NULL);
    }
    footer=label(screen,5,236,470,25,false);lv_obj_set_style_text_color(footer,lv_color_hex(COL_WARN),0);
    const key_id_t keys[]={KEY_UP,KEY_DOWN,KEY_LEFT,KEY_RIGHT,KEY_OK};const char *names[]={"↑","↓","←","→","OK"};
    for(int i=0;i<5;i++){
        soft[i]=button(screen,4+i*95,266,91,49);lv_obj_t *l=label(soft[i],3,14,85,22,false);text(l,names[i]);
        lv_obj_set_style_text_align(l,LV_TEXT_ALIGN_CENTER,0);lv_obj_add_event_cb(soft[i],key_event,LV_EVENT_ALL,(void *)(uintptr_t)keys[i]);
    }
    for(int ch=0;ch<2;ch++){
        switch_binding[ch]=(binding_t){.action=ACT_SWITCH,.a=ch};
        send_switch[ch]=action_button(screen,4+ch*155,43,151,31,"",&switch_binding[ch],&send_switch_label[ch]);
        tab_binding[ch]=(binding_t){.action=ACT_CHANNEL,.a=ch+1};
        live_tabs[ch]=action_button(screen,4+ch*76,43,72,29,ch?"CAN2":"CAN1",&tab_binding[ch],&live_tab_label[ch]);
    }
    load_binding.action=ACT_LOAD;save_binding.action=ACT_SAVE;
    send_load=action_button(screen,315,43,161,31,"加载文件",&load_binding,NULL);
    send_save=action_button(screen,350,287,126,29,"保存列表",&save_binding,NULL);
    send_panel=lv_obj_create(screen);lv_obj_set_pos(send_panel,0,78);lv_obj_set_size(send_panel,480,206);
    lv_obj_set_style_pad_all(send_panel,0,0);lv_obj_set_style_border_width(send_panel,0,0);lv_obj_set_style_bg_opa(send_panel,LV_OPA_TRANSP,0);
    lv_obj_set_scroll_dir(send_panel,LV_DIR_VER);lv_obj_set_scrollbar_mode(send_panel,LV_SCROLLBAR_MODE_AUTO);
    for(int ch=0;ch<2;ch++){
        int y=ch*266;send_head[ch]=button(send_panel,4,y,472,28);lv_obj_remove_flag(send_head[ch],LV_OBJ_FLAG_CLICKABLE);
        send_head_label[ch]=label(send_head[ch],7,5,312,21,false);
        loop_binding[ch]=(binding_t){.action=ACT_LOOP,.a=ch};
        send_loop[ch]=action_button(send_panel,342,y+1,128,26,"循环设置",&loop_binding[ch],&send_loop_label[ch]);
        for(int i=0;i<4;i++){
            int n=ch*4+i,row_y=y+30+i*58;
            edit_binding[n]=(binding_t){.action=ACT_EDIT,.a=ch,.b=i};
            send_row[n]=button(send_panel,4,row_y,472,56);bind(send_row[n],&edit_binding[n]);
            send_main[n]=label(send_row[n],5,5,326,22,false);send_data[n]=label(send_row[n],5,31,298,20,false);small(send_data[n]);
            one_binding[n]=(binding_t){.action=ACT_SINGLE,.a=ch,.b=i};
            send_one[n]=action_button(send_row[n],377,3,88,25,"单发",&one_binding[n],&send_one_label[n]);
            time_binding[n]=(binding_t){.action=ACT_TIMING,.a=ch,.b=i};
            send_time[n]=action_button(send_row[n],302,29,163,23,"",&time_binding[n],&send_time_label[n]);small(send_time_label[n]);
            lv_obj_set_y(send_time_label[n],3);
        }
    }
    mode_binding.action=ACT_VIEW;pause_binding.action=ACT_PAUSE;filter_binding.action=ACT_FILTER;
    live_mode=action_button(screen,156,43,100,29,"按 ID",&mode_binding,NULL);
    live_pause=action_button(screen,260,43,104,29,"暂停",&pause_binding,NULL);
    live_filter=action_button(screen,368,43,108,29,"筛选",&filter_binding,NULL);
    live_record=label(screen,5,269,470,23,false);
    const int editor_indices[]={12,10,11};const char *editor_names[]={"完整数据","保存报文","取消"};
    for(int i=0;i<3;i++){
        editor_binding[i]=(binding_t){.action=ACT_ITEM,.index=editor_indices[i]};
        editor_actions[i]=action_button(screen,4+i*159,43,154,29,editor_names[i],&editor_binding[i],NULL);
    }
    input_value=label(screen,8,45,464,56,true);small(input_value);input_info=label(screen,8,102,464,22,false);
    input_matrix=lv_buttonmatrix_create(screen);lv_obj_set_pos(input_matrix,4,122);lv_obj_set_size(input_matrix,472,158);
    lv_obj_remove_flag(input_matrix,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_pad_all(input_matrix,0,0);
    lv_obj_set_style_pad_row(input_matrix,3,0);lv_obj_set_style_pad_column(input_matrix,3,0);
    lv_obj_set_style_bg_color(input_matrix,lv_color_hex(COL_BG),0);lv_obj_set_style_border_width(input_matrix,0,0);
    lv_obj_set_style_bg_color(input_matrix,lv_color_hex(COL_BOX),LV_PART_ITEMS);
    lv_obj_set_style_bg_opa(input_matrix,LV_OPA_COVER,LV_PART_ITEMS);
    lv_obj_set_style_border_width(input_matrix,1,LV_PART_ITEMS);
    lv_obj_set_style_border_color(input_matrix,lv_color_hex(0x304967),LV_PART_ITEMS);
    lv_obj_set_style_bg_color(input_matrix,lv_color_hex(COL_SEL),LV_PART_ITEMS|LV_STATE_PRESSED);
    lv_obj_set_style_text_color(input_matrix,lv_color_hex(COL_TEXT),LV_PART_ITEMS);
    lv_obj_set_style_radius(input_matrix,4,LV_PART_ITEMS);
    lv_obj_add_event_cb(input_matrix,matrix_event,LV_EVENT_ALL,NULL);
    prev_binding=(binding_t){.action=ACT_INPUT_PAGE,.a= -1};next_binding=(binding_t){.action=ACT_INPUT_PAGE,.a=1};
    input_prev=action_button(screen,318,286,76,29,"上一页",&prev_binding,NULL);
    input_next=action_button(screen,400,286,76,29,"下一页",&next_binding,NULL);
    /* Full-screen modal is last in paint/hit order. */
    modal=lv_obj_create(screen);lv_obj_set_pos(modal,0,0);lv_obj_set_size(modal,480,320);
    lv_obj_set_style_bg_color(modal,lv_color_hex(0x111c30),0);lv_obj_set_style_bg_opa(modal,LV_OPA_COVER,0);
    lv_obj_set_style_pad_all(modal,0,0);lv_obj_set_style_border_width(modal,0,0);lv_obj_remove_flag(modal,LV_OBJ_FLAG_SCROLLABLE);
    modal_text=label(modal,15,16,450,180,true);
    for(int i=0;i<MODAL_OPTIONS;i++){
        choices[i]=action_button(modal,12,84+i*31,456,29,"",&choice_binding[i],&choice_label[i]);choice_binding[i].index=i;
    }
    visible(modal,false);rendered_page=PG_NUM;rendered_cursor= -1;rendered_dialog=false;
    rendered_input=rendered_send=rendered_live=false;rendered_keyboard=0;input_page=0;drag.active=false;last_refresh=0;
}
static void render_modal(int ui)
{
    int d=field(ui,"dlg");bool open=count(d)!=0;visible(modal,open);if(!open)return;
    int opts=field(d,"opts");bool menu=opts>=0;token_text(modal_text,field(d,"text"));lv_obj_set_height(modal_text,menu?60:208);
    int n=menu?count(opts):equals(field(d,"cancel"),"")?1:2,cur=(int)number(field(d,"cur"));
    for(int i=0;i<MODAL_OPTIONS;i++){
        visible(choices[i],i<n);if(i>=n)continue;
        lv_obj_set_pos(choices[i],menu?12:12+i*234,menu?82+i*32:247);
        lv_obj_set_size(choices[i],menu||n==1?456:222,menu?30:55);lv_obj_set_size(choice_label[i],menu||n==1?442:208,menu?22:42);
        if(menu)token_text(choice_label[i],at(opts,i));else token_text(choice_label[i],field(d,i?"cancel":"ok"));
        style_selected(choices[i],menu?i==cur:i==1,!menu&&i==0?4u:0u);choice_binding[i].index=i;choice_binding[i].dialog=true;
    }
}
static void render_home(int ui,int cursor)
{
    int items=field(ui,"items"),n=count(items),columns=n<=6?3:4,step=columns==3?159:119;
    for(int i=0;i<8;i++){
        visible(home[i],i<n);if(i>=n)continue;
        lv_obj_set_pos(home[i],4+(i%columns)*step,58+(i/columns)*83);lv_obj_set_size(home[i],step-4,75);
        lv_obj_set_width(home_label[i],step-16);token_text(home_label[i],field(at(items,i),"l"));style_selected(home[i],cursor==i,0);
    }
}
static void render_list(int ui,int cursor,bool live,bool editor)
{
    int container=live?field(ui,"live"):ui,arr=field(container,live?"rows":"items");
    int first=live?(cursor%12/ROWS)*ROWS:(cursor/ROWS)*ROWS;
    if(!live&&field(at(arr,0),"i")>=0){
        int absolute=first;first=0;
        while(first<count(arr)&&(int)number(field(at(arr,first),"i"))<absolute)first++;
    }
    for(int i=0;i<ROWS;i++){
        int row=at(arr,first+i);visible(rows[i],row>=0);if(row<0)continue;
        lv_obj_set_pos(rows[i],4,(live?99:editor?77:47)+i*(live?28:editor?26:31));lv_obj_set_height(rows[i],live?27:editor?25:29);
        lv_obj_set_y(row_label[i],live||editor?3:5);lv_obj_set_height(row_label[i],21);
        char s[512],a[200],b[280];unsigned flags=0;int abs_index=first+i;
        if(live){
            abs_index=(int)number(field(row,"i"));string(field(row,"type"),a,sizeof a);string(field(row,"data"),b,sizeof b);
            snprintf(s,sizeof s,"%s %08lX %-6s %2luB %s",equals(field(row,"tx"),"true")?"TX":"RX",
                     (unsigned long)number(field(row,"id")),a,(unsigned long)number(field(row,"len")),b);
            flags=equals(field(row,"err"),"true")||equals(field(row,"timeout"),"true")?4u:0u;
            row_binding[i].action=ACT_LIVE_ROW;row_binding[i].b=equals(field(container,"view"),"scroll");
            row_binding[i].identity=number(field(row,"seq"));
        }else{
            string(field(row,"l"),a,sizeof a);string(field(row,"v"),b,sizeof b);snprintf(s,sizeof s,"%s%s%s",a,b[0]?" : ":"",b);
            flags=number(field(row,"f"));if(field(row,"i")>=0)abs_index=(int)number(field(row,"i"));
            if(rendered_page==PG_PROJECTFILES&&field(row,"plan_token")>=0){row_binding[i].action=ACT_PLAN_FILE;row_binding[i].identity=number(field(row,"plan_token"));}
            else {row_binding[i].action=ACT_ITEM;row_binding[i].identity=0;}
        }
        text(row_label[i],s);lv_obj_set_style_text_color(row_label[i],lv_color_hex(flags&4u?COL_WARN:flags&8u?COL_GOOD:flags&2u?COL_DIM:COL_TEXT),0);
        style_selected(rows[i],abs_index==cursor,flags);row_binding[i].index=abs_index;
    }
}
static void render_send(int ui)
{
    int channels=field(ui,"tx_channels");
    for(int ch=0;ch<2;ch++){
        int channel=at(channels,ch),entries=field(channel,"entries");
        bool running=equals(field(channel,"running"),"true"),pending=equals(field(channel,"pending"),"true"),busy=running||pending;
        char s[160];snprintf(s,sizeof s,"CAN%d · %s",ch+1,running?"发送中":pending?"等待确认":"已停止");
        text(send_switch_label[ch],s);switch_binding[ch].b=!running;enabled(send_switch[ch],!pending);
        lv_obj_set_style_bg_color(send_switch[ch],lv_color_hex(running?0x205748:pending?0x5a482b:COL_BOX),0);
        lv_obj_set_style_border_color(send_switch[ch],lv_color_hex(running?0x67b094:pending?0xb08d51:0x43566f),0);
        snprintf(s,sizeof s,"CAN%d  %s · 已发 %lu",ch+1,running?"发送中":pending?"准备中":"已停止",(unsigned long)number(field(channel,"sent")));
        text(send_head_label[ch],s);enabled(send_loop[ch],!busy);
        for(int i=0;i<4;i++){
            int n=ch*4+i,row=at(entries,i);bool present=row>=0;visible(send_row[n],present);if(!present)continue;
            uint32_t flags=number(field(row,"flags")),length=number(field(row,"len"));
            snprintf(s,sizeof s,"%d  %0*lX  %s %s  %luB",i+1,flags&CANF_EXT?8:3,(unsigned long)number(field(row,"id")),
                     flags&CANF_EXT?"扩展":"标准",flags&CANF_RTR?"RTR":flags&CANF_FD?flags&CANF_BRS?"FD/B":"FD":"CAN",(unsigned long)length);
            text(send_main[n],s);int bytes=field(row,"data");size_t used=0;s[0]=0;
            for(unsigned b=0;b<length&&b<8&&!(flags&CANF_RTR);b++)
                used+=(size_t)snprintf(s+used,sizeof s-used,"%02lX%s",(unsigned long)number(at(bytes,(int)b)),b+1<length&&b<7?" ":"");
            if(length>8&&!(flags&CANF_RTR))snprintf(s+used,sizeof s-used," ...");
            if(flags&CANF_RTR)snprintf(s,sizeof s,"RTR (no data)");
            text(send_data[n],s);
            if(equals(field(row,"enabled"),"true"))snprintf(s,sizeof s,"%lux / %lums / %lums",(unsigned long)number(field(row,"repeat_count")),
                (unsigned long)number(field(row,"frame_gap_ms")),(unsigned long)number(field(row,"cycle_ms")));
            else snprintf(s,sizeof s,"设置次数与时间");
            text(send_time_label[n],s);enabled(send_row[n],!busy);enabled(send_one[n],!busy);enabled(send_time[n],!busy);
            style_selected(send_row[n],rendered_cursor==n,0);
        }
    }
    char source[128],s[256];string(field(ui,"tx_loaded_name"),source,sizeof source);
    if(source[0])snprintf(s,sizeof s,"已加载：%s",source);else snprintf(s,sizeof s,"两路各用自己的列表；点报文编辑");
    int toast=field(ui,"toast");
    if(toast>=0&&!equals(toast,""))token_text(footer,toast);else text(footer,s);
}
static void render_keyboard(int ui)
{
    int keyboard=field(ui,"keyboard"),keys=field(keyboard,"keys"),n=count(keys);
    uint32_t token=number(field(keyboard,"token"));
    if(rendered_keyboard!=token){input_page=0;rendered_keyboard=token;last_input_cursor=rendered_cursor;}
    bool plain=equals(field(keyboard,"mode"),"text"),payload=equals(field(keyboard,"mode"),"payload");
    lv_obj_set_style_text_font(input_value,payload?&lv_font_montserrat_14:LV_FONT_DEFAULT,0);
    lv_obj_set_height(input_value,payload?76:56);lv_obj_set_y(input_info,payload?121:102);
    lv_obj_set_pos(input_matrix,4,payload?148:122);lv_obj_set_size(input_matrix,472,payload?132:158);
    char value[512],info[128];string(field(keyboard,"value"),value,sizeof value);text(input_value,value[0]?value:"_");
    snprintf(info,sizeof info,"%s · %u / %lu 字节",plain?"名称":equals(field(keyboard,"mode"),"decimal")?"十进制":equals(field(keyboard,"mode"),"payload")?"完整十六进制数据":"十六进制",(unsigned)strlen(value),(unsigned long)number(field(keyboard,"max_bytes")));
    text(input_info,info);
    int body=n;
    if(plain)for(int i=0;i<n;i++)if(equals(field(at(keys,i),"kind"),"action")){body=i;break;}
    if(plain&&rendered_cursor!=last_input_cursor&&rendered_cursor<body)input_page=rendered_cursor/KEY_CELLS;
    last_input_cursor=rendered_cursor;
    int pages=plain?(body+KEY_CELLS-1)/KEY_CELLS:1;if(pages<1)pages=1;if(input_page>=pages)input_page=pages-1;
    int start=plain?input_page*KEY_CELLS:0,columns=plain?10:(int)number(field(keyboard,"columns"));
    if(columns<1||columns>10)columns=5;
    if(matrix_token!=token||matrix_page!=input_page){
        matrix_holding=false;matrix_count=0;unsigned maps=0;
        for(int i=0;i<KEY_CELLS;i++){
            int index=start+i,k=at(keys,index);if(k<0||(plain&&index>=body))break;
            if(i&&i%columns==0)matrix_map[maps++]="\n";
            string(field(k,"label"),matrix_labels[matrix_count],sizeof matrix_labels[0]);
            matrix_map[maps++]=matrix_labels[matrix_count];
            input_binding[matrix_count]=(binding_t){.action=equals(field(k,"label"),"退格")?ACT_BACKSPACE:ACT_ITEM,
                .index=(int)number(field(k,"index")),.identity=token};matrix_count++;
        }
        if(plain){
            matrix_map[maps++]="\n";
            for(int i=0;i<KEY_ACTIONS;i++){
                int k=at(keys,body+i);if(k<0)break;
                string(field(k,"label"),matrix_labels[matrix_count],sizeof matrix_labels[0]);
                if(equals(field(k,"label"),"保留默认名"))snprintf(matrix_labels[matrix_count],sizeof matrix_labels[0],"默认名");
                if(equals(field(k,"label"),"方案后缀"))snprintf(matrix_labels[matrix_count],sizeof matrix_labels[0],"后缀");
                matrix_map[maps++]=matrix_labels[matrix_count];
                input_binding[matrix_count]=(binding_t){.action=equals(field(k,"label"),"退格")?ACT_BACKSPACE:ACT_ITEM,
                    .index=(int)number(field(k,"index")),.identity=token};matrix_count++;
            }
        }
        matrix_map[maps]="";lv_buttonmatrix_set_map(input_matrix,matrix_map);
        lv_buttonmatrix_set_button_ctrl_all(input_matrix,LV_BUTTONMATRIX_CTRL_CLICK_TRIG|LV_BUTTONMATRIX_CTRL_NO_REPEAT);
        matrix_token=token;matrix_page=input_page;
    }
    if(!matrix_holding){
        unsigned selected=LV_BUTTONMATRIX_BUTTON_NONE;
        for(unsigned i=0;i<matrix_count;i++)if(input_binding[i].index==rendered_cursor)selected=i;
        lv_buttonmatrix_set_selected_button(input_matrix,selected);
    }
    visible(input_prev,plain&&pages>1);visible(input_next,plain&&pages>1);
    enabled(input_prev,input_page>0);enabled(input_next,input_page+1<pages);
    prev_binding.identity=next_binding.identity=token;
    lv_obj_set_width(footer,plain?305:470);lv_obj_set_y(footer,286);lv_obj_set_height(footer,31);
    int notice=field(ui,"local_notice");token_text(footer,notice);
    if(equals(notice,"")){
        int toast=field(ui,"toast");
        if(toast>=0&&!equals(toast,"")){token_text(footer,toast);return;}
        snprintf(info,sizeof info,plain?"字符页 %d/%d · 名称可保存或取消":"确定更新草稿，外层保存后生效",input_page+1,pages);text(footer,info);}
}
static void show_views(bool is_home,bool live,bool send,bool input,bool editor)
{
    for(int i=0;i<8;i++)visible(home[i],is_home);
    for(int i=0;i<ROWS;i++)visible(rows[i],!is_home&&!send&&!input);
    for(int i=0;i<5;i++)visible(soft[i],!live&&!send&&!input);
    for(int i=0;i<2;i++){
        visible(send_switch[i],send);visible(live_tabs[i],live);
    }
    visible(send_panel,send);visible(send_load,send);visible(send_save,send);
    visible(live_mode,live);visible(live_pause,live);visible(live_filter,live);visible(live_record,live);
    for(int i=0;i<3;i++)visible(editor_actions[i],editor&&!input);
    visible(input_value,input);visible(input_info,input);visible(input_matrix,input);
    if(!input){visible(input_prev,false);visible(input_next,false);matrix_holding=false;}
}
void ui_lvgl_task(uint32_t now_ms)
{
    if(!screen)return;
    /* Touch sampling is independent from the 100 ms model/render cadence.
     * LVGL otherwise uses LV_DEF_REFR_PERIOD for its input read timer too. */
    for(lv_indev_t *input=lv_indev_get_next(NULL);input;input=lv_indev_get_next(input)){
        if(lv_indev_get_type(input)==LV_INDEV_TYPE_POINTER&&lv_indev_get_display(input)==lv_obj_get_display(screen)){
            lv_timer_t *timer=lv_indev_get_read_timer(input);if(timer&&configured_pointer!=input){lv_timer_set_period(timer,20u);configured_pointer=input;}
        }
    }
#ifdef REVD_FREERTOS
    if((uint32_t)(now_ms-last_refresh)<100u)return;
    last_refresh=now_ms;
    revd_ui_view_t view;
    if(!revd_ui_snapshot(snap,sizeof snap,&json_len,&view)){snapshot_errors++;text(footer,"UI snapshot too large; use physical BACK");return;}
#else
    bool changed=ui_page()!=rendered_page||ui_cursor()!=rendered_cursor||ui_dialog_open()!=rendered_dialog;
    if(!changed&&last_refresh&&(uint32_t)(now_ms-last_refresh)<100u)return;
    last_refresh=now_ms;
    jw_t j;jw_init(&j,snap,sizeof snap);jw_obj(&j);ui_json(&j);jw_end_obj(&j);json_len=jw_finish(&j);
    if(j.overflow){snapshot_errors++;text(footer,"UI snapshot too large; use physical BACK");return;}
#endif
    if(json_len>peak_snapshot_bytes)peak_snapshot_bytes=json_len;
    ntok=0;pos=0;parse_error=false;
    if(parse_value(0)<0||parse_error){snapshot_errors++;text(footer,"UI snapshot too large; use physical BACK");return;}
    if((unsigned)ntok>peak_snapshot_tokens)peak_snapshot_tokens=(unsigned)ntok;
    int ui=field(0,"ui");if(ui<0){snapshot_errors++;return;}
#ifdef REVD_FREERTOS
    rendered_page=view.page;rendered_cursor=view.cursor;rendered_dialog=view.dialog;rendered_generation=view.generation;
#else
    page_t previous=rendered_page;rendered_page=ui_page();rendered_cursor=(int)number(field(ui,"cursor"));rendered_dialog=ui_dialog_open();
    if(previous!=rendered_page&&rendered_page==PG_TX)lv_obj_scroll_to_y(send_panel,0,LV_ANIM_OFF);
#endif
    bool is_home=rendered_page==PG_HOME,editor=rendered_page==PG_TXEDIT;
    rendered_input=field(ui,"keyboard")>=0;rendered_live=rendered_page==PG_LIVE&&!rendered_input;
    rendered_send=rendered_page==PG_TX&&!rendered_input&&field(ui,"tx_channels")>=0;
#ifdef REVD_FREERTOS
    /* Existing RTOS event bridge remains buildable; do not call new core APIs
     * from its UI task until that separate target gains matching commands. */
    rendered_send=false;rendered_input=false;
#endif
    if(!rendered_input)rendered_keyboard=0;
    unsigned record_state=number(field(field(ui,"rec_controls"),"state"));
    lv_obj_set_style_bg_color(rec_button,lv_color_hex(record_state==2?0x205748:record_state==1||record_state==3?0x5a482b:COL_BOX),0);
    token_text(title,field(ui,"title"));show_views(is_home&&!rendered_input,rendered_live,rendered_send,rendered_input,editor);
    char s[160];int live=field(ui,"live");
    if(rendered_live){
        unsigned channel=number(field(live,"channel"));bool byid=equals(field(live,"view"),"byid"),paused=equals(field(live,"paused"),"true");
        snprintf(s,sizeof s,"CAN%u | %s %s | %lu frames",channel,byid?"BY ID":"SCROLL",paused?"PAUSED":"LIVE",(unsigned long)number(field(live,"total")));
        for(int i=0;i<2;i++)style_selected(live_tabs[i],channel==(unsigned)i+1u,0);
        mode_binding.a=!byid;pause_binding.a=!paused;
        text(lv_obj_get_child(live_mode,0),byid?"滚动":"按 ID");text(lv_obj_get_child(live_pause,0),paused?"继续":"暂停");
        int rec=field(ui,"rec_controls");unsigned state=number(field(rec,"state"));
        char record[160];snprintf(record,sizeof record,"REC %s · %lu帧 · 丢%lu · 短按启停",state==2?"录制中":state==1?"准备中":state==3?"保存中":"已停止",(unsigned long)number(field(rec,"frames")),(unsigned long)number(field(rec,"drops")));
        text(live_record,record);
    }else {
        const char *context=rendered_input?(equals(field(ui,"page"),"local_record_save")?"内容已保存 · 修改名称或保留默认名":equals(field(ui,"page"),"local_plan_name")?"保存报文列表 · 编辑文件名":"本地编辑 · 确定返回草稿"):rendered_send?"两路独立启停 · 加载后不自动发送":
            editor?"报文草稿 · 保存后生效":rendered_page==PG_PROJECTFILES?"选择文件，直接载入报文":
            rendered_page==PG_TXSET||rendered_page==PG_TXTIMING?"循环时序 · 保存后生效":
            rendered_page==PG_FILES?"本机 SD 文件":rendered_page==PG_REC?"录制设置 · REC 短按启停":
            rendered_page>=PG_KEYSIM&&rendered_page<=PG_KEYSCAN?"APM 按键模拟 · 同一时刻只接通一路":"CAN FD · 本机操作";
        if(record_state&&!rendered_input)snprintf(s,sizeof s,"REC %s · %lu 帧",record_state==2?"录制中":record_state==1?"准备中":"保存中",(unsigned long)number(field(field(ui,"rec_controls"),"frames")));
        else snprintf(s,sizeof s,"%s%s",power_get()->locked?"已锁定 · ":"",context);
    }
#ifdef CANFD_REVE_TARGET
    if(is_home && !rendered_input && !record_state) {
        const reve_power_diag_t *pd=reve_power_diagnostics();
        static const char *const modes[]={"BAT","USB 100mA","USB 500mA","USB-C ILIM","12V ILIM","USB SUSPEND"};
        if(pd->monitor_valid) snprintf(s,sizeof s,"%s | BAT %u.%02uV | %+ldmA",modes[pd->mode],pd->cell_mv/1000u,pd->cell_mv%1000u/10u,(long)pd->battery_ma);
        else snprintf(s,sizeof s,"%s | BAT monitor unavailable",modes[pd->mode]);
    }
#endif
    text(status,s);lv_obj_set_pos(footer,5,rendered_live?295:rendered_send?289:editor?238:236);lv_obj_set_size(footer,rendered_send?337:470,24);
    int ft=field(ui,"toast");if(equals(ft,""))ft=field(ui,"hint");token_text(footer,ft);
    if(rendered_input)render_keyboard(ui);
    else if(is_home)render_home(ui,rendered_cursor);
    else if(rendered_send)render_send(ui);
    else render_list(ui,rendered_cursor,rendered_live,editor);
#ifndef REVD_FREERTOS
    if(!rendered_input&&action_notice[0]&&(int32_t)(action_notice_until-now_ms)>0)text(footer,action_notice);
#endif
#ifdef CANFD_REVE_TARGET
    if(!display_diagnostics()->touch_present) text(footer,"Touch offline | BACK / REC / POWER still work");
#endif
    render_modal(ui);
#ifdef REVD_FREERTOS
    revd_ui_commit_view(&view);
#endif
}
uint32_t ui_lvgl_snapshot_errors(void){return snapshot_errors;}
