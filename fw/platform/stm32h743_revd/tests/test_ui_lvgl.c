/* Host execution of the same embedded widgets, view model and application.
 * Including the adapter makes fixed widget bindings inspectable for this test. */
#include "../ui_lvgl.c"
#include "sim/sim_api.h"
#include "services/core/app_timer.h"
#include "services/core/port.h"
#include "services/can_core/chan_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "services/frame_ring/frame_ring.h"
#include "services/recorder/rec_sm.h"
#include "services/disp/disp_pipe.h"
#include "sim/env_sim.h"
#include "sim/send_plan.h"
#include "sim/tx_sequence.h"
#include "services/keysim/keysim.h"
#include <assert.h>
#include "src/widgets/buttonmatrix/lv_buttonmatrix_private.h"

/* A named port double for the actual board catalogue limit. Normal interaction
 * tests forward to env_sim; the boundary pass exposes 128 closed log records. */
int ui_test_env_file_count(void);
bool ui_test_env_file_info(int index,file_info_t *out);
static bool largest_catalogue;
int port_sd_file_count(void){return largest_catalogue?128:ui_test_env_file_count();}
bool port_sd_file_info(int index,file_info_t *out)
{
    if(!largest_catalogue)return ui_test_env_file_info(index,out);
    if(index<0||index>=128||!out)return false;
    memset(out,0,sizeof *out);
    for(unsigned n=0;n<13;n++)memcpy(out->name+n*3,"测",3);
    snprintf(out->name+39,sizeof out->name-39,"%04u.LOG",(unsigned)index);
    out->kind=FK_CFR;out->size_kb=UINT32_MAX;out->frames=UINT32_MAX;out->duration_ms=UINT32_MAX;
    out->channels=3;out->is_session=true;out->sample_available=true;out->protected_=true;return true;
}
static uint8_t pixels[480u * 320u * 2u], partial[480u * 20u * 2u];
static unsigned flushes, checks;
#define CHECK(e) do { ++checks; if (!(e)) { fprintf(stderr,"FAIL %s:%d %s\n",__FILE__,__LINE__,#e); exit(1); } } while (0)
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    size_t width = (size_t)(a->x2 - a->x1 + 1);
    CHECK(a->x1 >= 0 && a->x2 < 480 && a->y1 >= 0 && a->y2 < 320);
    for (int y = a->y1; y <= a->y2; y++) {
        memcpy(pixels + ((size_t)y * 480u + a->x1) * 2u, p, width * 2u);
        p += width * 2u;
    }
    flushes++;
    lv_display_flush_ready(d);
}
static void run(unsigned ms)
{
    for (unsigned t = 0; t < ms; t += 10) {
        sim_tick(10);
        ui_lvgl_task(app_now_ms());
        (void)lv_timer_handler();
    }
}
static void click(lv_obj_t *o)
{
    lv_obj_send_event(o, LV_EVENT_PRESSED, NULL);
    lv_obj_send_event(o, LV_EVENT_CLICKED, NULL);
    lv_obj_send_event(o, LV_EVENT_RELEASED, NULL);
    run(120);
}
static void key(key_id_t k, unsigned ms)
{
    sim_key(k, 1); run(ms); sim_key(k, 0); run(120);
}
static void dump(const char *prefix, const char *name)
{
    if (!prefix) return;
    lv_refr_now(lv_display_get_default());
    char path[512]; snprintf(path,sizeof path,"%s/%s.ppm",prefix,name);
    FILE *f = fopen(path,"wb"); CHECK(f != NULL);
    fprintf(f,"P6\n480 320\n255\n");
    for(size_t i=0;i<sizeof pixels;i+=2) {
        uint16_t v=(uint16_t)(pixels[i] | ((uint16_t)pixels[i+1]<<8));
        uint8_t rgb[]={(uint8_t)(((v>>11)&31u)*255u/31u),
                       (uint8_t)(((v>>5)&63u)*255u/63u),(uint8_t)((v&31u)*255u/31u)};
        fwrite(rgb,1,sizeof rgb,f);
    }
    fclose(f);
}
static void glyph_check(const char *path)
{
    FILE *f=fopen(path,"rb"); CHECK(f != NULL);
    unsigned char s[32768]; size_t len=fread(s,1,sizeof s,f); CHECK(!ferror(f)&&feof(f)); fclose(f);
    for(size_t i=0;i<len;) {
        uint32_t cp=s[i++];
        if(cp < 0x20) continue;
        unsigned left=0;
        if(cp>=0xF0){cp&=7u;left=3;} else if(cp>=0xE0){cp&=15u;left=2;} else if(cp>=0xC0){cp&=31u;left=1;}
        while(left-- && i<len) cp=(cp<<6)|(s[i++]&63u);
        lv_font_glyph_dsc_t d;
        CHECK(lv_font_get_glyph_dsc(LV_FONT_DEFAULT,&d,cp,0) && !d.is_placeholder);
    }
}
static void pointer(int x,int y,bool down,unsigned ms);
static void hold_input(const char *caption,unsigned duration)
{
    int ui=field(0,"ui"),keys=field(field(ui,"keyboard"),"keys"),index= -1;
    for(int i=0;i<count(keys);i++)if(equals(field(at(keys,i),"label"),caption))index=(int)number(field(at(keys,i),"index"));
    CHECK(index>=0);
    for(unsigned i=0;i<matrix_count;i++)if(input_binding[i].index==index){
        const lv_area_t *a=&((lv_buttonmatrix_t *)input_matrix)->button_areas[i];lv_area_t obj;lv_obj_get_coords(input_matrix,&obj);
        int x=obj.x1+(a->x1+a->x2)/2,y=obj.y1+(a->y1+a->y2)/2;
        pointer(x,y,true,duration);pointer(x,y,false,120);return;
    }
    CHECK(false);
}
static void click_input(const char *caption){hold_input(caption,70);}
static void refresh(void){last_refresh=0;run(120);}
static void normal(int ch)
{
    app_evt_t e={.type=EV_REQ_CH_MODE,.src=SRC_UI,.ch=(uint8_t)ch,.a=CAN_MODE_NORMAL,.b=2};app_post(&e);run(120);
}
static void configure(int ch)
{
    txseq_entry_config_t entries[TX_ENTRIES];memcpy(entries,txseq_get(ch)->entries,sizeof entries);
    for(int i=0;i<TX_ENTRIES;i++)entries[i].enabled=i==0;
    entries[0].repeat_count=2;entries[0].frame_gap_ms=5;entries[0].cycle_ms=40;
    CHECK(txseq_configure(ch,entries,3,txseq_get(ch)->revision)==0);
}
static bool pointer_down;
static lv_point_t pointer_point;
static void pointer_read(lv_indev_t *indev,lv_indev_data_t *data)
{
    (void)indev;data->point=pointer_point;data->state=pointer_down?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
}
static void pointer(int x,int y,bool down,unsigned ms)
{
    pointer_point=(lv_point_t){.x=x,.y=y};pointer_down=down;run(ms);
}
static void stress_live_payloads(void)
{
    disp_pause(false);disp_clear_ids();
    for(int ch=0;ch<2;ch++)for(unsigned i=0;i<16;i++){
        can_frame_t f={.ch=(uint8_t)ch,.id=0x300+i/2,.flags=(uint8_t)(CANF_FD|CANF_BRS|((i&1u)?CANF_TX:0)),.len=64,.ts_us=app_now_us()+i*10};
        for(unsigned b=0;b<64;b++)f.data[b]=(uint8_t)(b+i);CHECK(ring_push(&f));
    }
    app_capture_poll();refresh();
}
int main(int argc,char **argv)
{
    const char *out=argc>1?argv[1]:NULL;
    sim_reset();sim_key(KEY_POWER,1);for(int i=0;i<120;i++)sim_tick(10);
    sim_key(KEY_POWER,0);for(int i=0;i<50;i++)sim_tick(10);CHECK(power_running());
    env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);
    lv_init();lv_tick_set_cb(app_now_ms);lv_display_t *d=lv_display_create(480,320);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_RGB565);lv_display_set_buffers(d,partial,NULL,sizeof partial,LV_DISPLAY_RENDER_MODE_PARTIAL);lv_display_set_flush_cb(d,flush);
    ui_lvgl_init(d);run(200);
    lv_indev_t *touch=lv_indev_create();lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);lv_indev_set_display(touch,d);lv_indev_set_read_cb(touch,pointer_read);
    CHECK(ui_page()==PG_HOME&&!strcmp(lv_label_get_text(title),"主页"));
    const int home_n=keysim_available()?7:6;   /* 7th tile only where key-simulation hardware exists */
    for(int i=0;i<8;i++)CHECK(lv_obj_has_flag(home[i],LV_OBJ_FLAG_HIDDEN)==(i>=home_n));
    CHECK(flushes>0);dump(out,"lvgl-home");glyph_check("platform/stm32h743_revd/fonts/ui_font_chars.txt");
    const page_t pages[]={PG_LIVE,PG_TX,PG_DIAG,PG_FILES,PG_CHSET,PG_SYSSET};
    for(int i=0;i<6;i++){
        click(home[i]);CHECK(ui_page()==pages[i]&&lv_obj_has_flag(home[i],LV_OBJ_FLAG_HIDDEN));
        if(i==0){CHECK(strstr(lv_label_get_text(status),"frames"));dump(out,"lvgl-live");}
        app_touch(EV_TOUCH_BACK,0);run(120);CHECK(ui_page()==PG_HOME);
    }
    if(keysim_available()){
        /* Key simulation renders through the generic list: overview, K2 page, learning. */
        click(home[6]);CHECK(ui_page()==PG_KEYSIM&&!lv_obj_has_flag(rows[0],LV_OBJ_FLAG_HIDDEN));
        CHECK(strstr(lv_label_get_text(row_label[0]),"K1")!=NULL);run(400);dump(out,"lvgl-keysim");
        click(rows[1]);CHECK(ui_page()==PG_KEYCH&&strstr(lv_label_get_text(title),"K2"));dump(out,"lvgl-keych-empty");
        click(rows[5]);CHECK(ui_page()==PG_KEYLEARN);click(rows[0]);CHECK(keysim_get()->activity==KS_LEARNING);
        for(int pos=1;pos<=3;pos++){env_cmd("ks_switch",1,pos);run(900);env_cmd("ks_switch",1,0);run(300);}
        CHECK(keysim_get()->learn_n==3u);refresh();dump(out,"lvgl-keylearn");
        app_touch(EV_TOUCH_BACK,0);run(120);CHECK(ui_page()==PG_KEYCH);
        app_touch(EV_TOUCH_BACK,0);run(120);CHECK(ui_page()==PG_KEYSIM);
        app_touch(EV_TOUCH_BACK,0);run(120);CHECK(ui_page()==PG_HOME);
    }
    click(home[1]);CHECK(rendered_send&&ui_page()==PG_TX);
    for(int i=0;i<8;i++)CHECK(!lv_obj_has_flag(send_row[i],LV_OBJ_FLAG_HIDDEN));
    CHECK(!lv_obj_has_flag(send_switch[0],LV_OBJ_FLAG_HIDDEN)&&!lv_obj_has_flag(send_switch[1],LV_OBJ_FLAG_HIDDEN));
    CHECK(strstr(lv_label_get_text(send_switch_label[0]),"已停止"));dump(out,"lvgl-send-can1");
    /* Native pointer scroll moves to the second channel without editing a row. */
    pointer(210,237,true,70);pointer(210,100,true,100);pointer(210,100,false,120);
    CHECK(ui_page()==PG_TX&&lv_obj_get_scroll_y(send_panel)>0);dump(out,"lvgl-send-can2");
    lv_obj_scroll_to_y(send_panel,0,LV_ANIM_OFF);refresh();
    click(send_switch[0]);CHECK(ui_dialog_open()&&tx_get()->e[0][0].sent==0);
    CHECK(!lv_obj_has_flag(modal,LV_OBJ_FLAG_HIDDEN));dump(out,"lvgl-confirm");click(choices[1]);CHECK(!ui_dialog_open());
    normal(0);normal(1);
    txseq_entry_config_t empty[TX_ENTRIES];memcpy(empty,txseq_get(0)->entries,sizeof empty);
    for(unsigned i=0;i<TX_ENTRIES;i++)empty[i].enabled=false;
    CHECK(txseq_configure(0,empty,3,txseq_get(0)->revision)==0);refresh();click(send_switch[0]);
    CHECK(txseq_get(0)->state==TXSEQ_IDLE&&strstr(lv_label_get_text(footer),"至少选择"));
    configure(0);configure(1);refresh();
    click(send_switch[0]);CHECK(txseq_get(0)->state==TXSEQ_RUNNING&&strstr(lv_label_get_text(send_switch_label[0]),"发送中"));
    click(send_switch[1]);CHECK(txseq_get(1)->state==TXSEQ_RUNNING);uint32_t sent1=txseq_get(1)->sent;
    click(send_switch[0]);CHECK(txseq_get(0)->state==TXSEQ_IDLE&&txseq_get(1)->state==TXSEQ_RUNNING&&txseq_get(1)->sent>sent1);
    click(send_switch[1]);CHECK(txseq_get(1)->state==TXSEQ_IDLE);dump(out,"lvgl-send-stopped");
    click(send_row[0]);CHECK(ui_page()==PG_TXEDIT);CHECK(!lv_obj_has_flag(editor_actions[0],LV_OBJ_FLAG_HIDDEN));
    click(rows[0]);CHECK(rendered_input&&equals(field(field(field(0,"ui"),"keyboard"),"mode"),"hex"));
    click_input("清空");click_input("4");click_input("5");click_input("6");
    CHECK(!strcmp(lv_label_get_text(input_value),"456"));dump(out,"lvgl-keyboard-hex");
    pointer(440,20,true,70);pointer(440,20,false,120);CHECK(rec_get()->state==REC_IDLE&&rendered_input&&strstr(lv_label_get_text(footer),"草稿"));
    can_frame_t prior=tx_get()->e[0][0].f;click_input("确定");CHECK(ui_page()==PG_TXEDIT&&!memcmp(&prior,&tx_get()->e[0][0].f,sizeof prior));
    click(editor_actions[1]);CHECK(ui_page()==PG_TX&&tx_get()->e[0][0].f.id==0x456);
    uint8_t payload[64];for(unsigned i=0;i<64;i++)payload[i]=(uint8_t)i;
    CHECK(tx_entry_update(0,0,0x456,CANF_FD|CANF_BRS,64,payload,0)==TX_OK);refresh();click(send_row[0]);click(editor_actions[0]);
    CHECK(equals(field(field(field(0,"ui"),"keyboard"),"mode"),"payload"));
    char complete[192];for(unsigned i=0;i<64;i++)snprintf(complete+i*3,sizeof complete-i*3,"%02X%s",i,i<63?" ":"");
    CHECK(strlen(complete)==191&&local_files_keyboard_replace(rendered_keyboard,complete)==0);refresh();
    CHECK(strstr(lv_label_get_text(input_value),"3D 3E 3F"));dump(out,"lvgl-keyboard-payload64");
    click_input("确定");click(editor_actions[1]);CHECK(tx_get()->e[0][0].f.len==64&&tx_get()->e[0][0].f.data[63]==0x3f);
    click(send_loop[0]);CHECK(ui_page()==PG_TXSET);click(rows[1]);CHECK(equals(field(field(field(0,"ui"),"keyboard"),"mode"),"decimal"));
    click_input("清空");click_input("1");click_input("7");dump(out,"lvgl-keyboard-decimal");click_input("取消");
    key(KEY_BACK,70);CHECK(ui_page()==PG_TX);
    /* A file selection restores its stored mask/frame format without a second
     * format or channel menu, and does not send a frame. */
    send_plan_t plan;CHECK(sp_capture(3,&plan)==0);sp_file_t file;memset(&file,0,sizeof file);file.plan=plan;snprintf(file.name,sizeof file.name,"Both.canplan.json");
    CHECK(sp_catalog_replace(&file,1)==0);uint32_t before0=tx_get()->echo_count[0],before1=tx_get()->echo_count[1];
    click(send_load);CHECK(ui_page()==PG_PROJECTFILES&&!rendered_send);dump(out,"lvgl-send-files");
    lv_obj_send_event(rows[0],LV_EVENT_PRESSED,NULL);file.plan.channels[0].entries[0].id^=1u;CHECK(sp_catalog_replace(&file,1)==0);
    lv_obj_send_event(rows[0],LV_EVENT_CLICKED,NULL);CHECK(ui_page()==PG_PROJECTFILES&&tx_get()->e[0][0].f.id==0x456);
    file.plan.channels[0].entries[0].id^=1u;CHECK(sp_catalog_replace(&file,1)==0);refresh();click(rows[0]);
    CHECK(ui_page()==PG_TX&&rendered_send&&tx_get()->e[0][0].f.len==64&&tx_get()->echo_count[0]==before0&&tx_get()->echo_count[1]==before1);
    click(send_save);CHECK(rendered_input&&equals(field(field(field(0,"ui"),"keyboard"),"mode"),"text"));
    CHECK(!lv_obj_has_flag(input_next,LV_OBJ_FLAG_HIDDEN));click(input_next);CHECK(input_page==1);dump(out,"lvgl-save-plan");
    size_t name_length=strlen(lv_label_get_text(input_value));hold_input("退格",740);CHECK(strlen(lv_label_get_text(input_value))+4<=name_length);
    size_t after_hold=strlen(lv_label_get_text(input_value));run(500);CHECK(strlen(lv_label_get_text(input_value))==after_hold);
    click_input("取消");CHECK(ui_page()==PG_TX);
    key(KEY_BACK,70);CHECK(ui_page()==PG_HOME);click(home[0]);stress_live_payloads();
    CHECK(rendered_live&&number(field(field(field(0,"ui"),"live"),"channel"))==1);dump(out,"lvgl-live-can1-rxtx");
    click(live_tabs[1]);CHECK(number(field(field(field(0,"ui"),"live"),"channel"))==2);dump(out,"lvgl-live-can2-rxtx");
    int old_cursor=ui_cursor();pointer(220,235,true,70);pointer(220,150,true,100);pointer(220,150,false,120);
    CHECK(ui_page()==PG_LIVE&&ui_cursor()>old_cursor&&disp_paused());dump(out,"lvgl-live-scroll-paused");
    click(live_mode);CHECK(equals(field(field(field(0,"ui"),"live"),"view"),"byid"));
    CHECK(number(field(field(field(0,"ui"),"live"),"total"))==16);refresh();
    int summary_rows=field(field(field(0,"ui"),"live"),"rows");CHECK(count(summary_rows)==12);
    for(int i=0;i<12;i++)CHECK(number(field(at(summary_rows,i),"available_len"))==64);
    dump(out,"lvgl-live-byid");
    pointer(440,20,true,70);pointer(440,20,false,120);run(200);CHECK(rec_get()->state==REC_RUNNING&&rec_get()->markers==0);pointer(440,20,true,70);pointer(440,20,false,120);run(200);
    CHECK(rec_get()->state==REC_IDLE&&rec_get()->markers==0&&rendered_input&&equals(field(field(0,"ui"),"page"),"local_record_save"));
    dump(out,"lvgl-record-save-name");click_input("保留默认名");CHECK(!ui_draft_open()&&ui_page()==PG_LIVE);
    /* A maximum-byte-value display window also exercises JSON byte lengths;
     * all 12 ID rows retain 64 bytes, including their change-flag arrays. */
    CHECK(ui_live_set_paused(false)==0);disp_clear_ids();
    for(unsigned i=0;i<24;i++){
        can_frame_t f={.ch=1,.id=0x1fffff00u+i,.flags=CANF_EXT|CANF_FD|CANF_BRS,.len=64,.ts_us=app_now_us()};
        memset(f.data,255,sizeof f.data);CHECK(ring_push(&f));
    }
    app_capture_poll();refresh();summary_rows=field(field(field(0,"ui"),"live"),"rows");CHECK(count(summary_rows)==12);
    for(int i=0;i<12;i++)CHECK(count(field(at(summary_rows,i),"bytes"))==64&&count(field(at(summary_rows,i),"changed"))==64);
    /* Worst supported catalogue and eight complete FD entries, rather than a
     * startup list containing shorter classic frames. No user SD files exist. */
    CHECK(ui_review_navigate(PG_TX)==0);uint8_t maximum_data[64];memset(maximum_data,255,sizeof maximum_data);
    for(int ch=0;ch<2;ch++){
        txseq_entry_config_t timing[TX_ENTRIES];
        for(int slot=0;slot<TX_ENTRIES;slot++){
            CHECK(tx_entry_update(ch,slot,0x1fffffffu-(unsigned)slot,CANF_EXT|CANF_FD|CANF_BRS,64,maximum_data,UINT16_MAX)==TX_OK);
            timing[slot]=(txseq_entry_config_t){.enabled=true,.repeat_count=10000,.frame_gap_ms=300,.cycle_ms=3600000};
        }
        CHECK(txseq_configure(ch,timing,3600000,txseq_get(ch)->revision)==0);
    }
    send_plan_t maximum;CHECK(sp_capture(3,&maximum)==0);sp_file_t catalogue[SP_MAX_FILES];memset(catalogue,0,sizeof catalogue);
    for(unsigned index=0;index<SP_MAX_FILES;index++){
        for(unsigned c=0;c<10;c++)memcpy(catalogue[index].name+c*3,"测",3);
        snprintf(catalogue[index].name+30,sizeof catalogue[index].name-30,"%04u.canplan.json",index);
        CHECK(strlen(catalogue[index].name)==47);catalogue[index].plan=maximum;
    }
    CHECK(sp_catalog_replace(catalogue,SP_MAX_FILES)==0);refresh();
    CHECK(ui_plan_open(0,0)==0);refresh();
#ifdef CANFD_REVD_TARGET
    CHECK(count(field(field(0,"ui"),"items"))==12&&number(field(field(0,"ui"),"items_total"))==17);
#else
    CHECK(count(field(field(0,"ui"),"items"))==17);
#endif
    dump(out,"lvgl-files-long-names");
    app_touch(EV_TOUCH_BACK,0);refresh();CHECK(ui_page()==PG_TX);
    CHECK(ui_review_navigate(PG_FILES)==0);largest_catalogue=true;refresh();
#ifdef CANFD_REVD_TARGET
    CHECK(count(field(field(0,"ui"),"items"))<=12);
    CHECK(number(field(field(0,"ui"),"items_total"))==128+SP_MAX_FILES+2);
#else
    CHECK(count(field(field(0,"ui"),"items"))==128+SP_MAX_FILES+2);
#endif
    dump(out,"lvgl-files-capacity-128-plus-16");
    for(unsigned i=0;i<128;i++){app_evt_t down={.type=EV_KEY,.ch=KEY_DOWN,.a=GEST_SHORT,.src=SRC_UI};app_post(&down);app_tick(0);}
    refresh();CHECK(ui_cursor()==128&&row_binding[2].index==128);click(rows[2]);CHECK(ui_page()==PG_PLANDETAIL);
    largest_catalogue=false;CHECK(ui_review_navigate(PG_TX)==0);refresh();
    CHECK(ui_tx_channel_send(0,false)==0);app_evt_t listen={.type=EV_REQ_CH_MODE,.src=SRC_UI,.ch=0,.a=CAN_MODE_LISTEN,.b=2};app_post(&listen);run(20);
    CHECK(ui_tx_channel_send(0,true)==1);app_tick(0);refresh();CHECK(ui_dialog_open());dump(out,"lvgl-full-fd-confirm");click(choices[1]);
    /* A delayed press may not activate a newly navigated page. */
    CHECK(ui_review_navigate(PG_HOME)==0);refresh();lv_obj_send_event(home[0],LV_EVENT_PRESSED,NULL);
    CHECK(ui_review_navigate(PG_DIAG)==0);refresh();lv_obj_send_event(home[0],LV_EVENT_CLICKED,NULL);refresh();CHECK(ui_page()==PG_DIAG);
    /* Token exhaustion is detected without writing past the fixed array. The
     * next complete view recovers; no truncation is presented as a valid view. */
    size_t generated=0;snap[generated++]='[';
    for(unsigned i=0;i<TOKEN_MAX+1u;i++){snap[generated++]='0';snap[generated++]=',';}
    snap[generated-1]=']';json_len=generated;ntok=0;pos=0;parse_error=false;
    CHECK(parse_value(0)<0&&parse_error&&ntok==TOKEN_MAX);refresh();
    CHECK(snapshot_errors==0&&peak_snapshot_bytes<SNAP_BYTES&&peak_snapshot_tokens<TOKEN_MAX);
    CHECK(lv_mem_test()==LV_RESULT_OK);lv_mem_monitor_t mem;lv_mem_monitor(&mem);
    printf("LVGL %s: %u checks / %u flushes; snapshot peak %zu bytes %u tokens; heap peak %zu / %zu\n",
#ifdef CANFD_REVD_TARGET
           "TARGET CORE + simulated ports (not hardware)",
#else
           "host model",
#endif
           checks,flushes,peak_snapshot_bytes,peak_snapshot_tokens,(size_t)mem.max_used,(size_t)mem.total_size);
    lv_indev_delete(touch);lv_display_delete(d);lv_deinit();return 0;
}
