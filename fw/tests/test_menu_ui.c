/* Device-only menu journeys after combining recording with live display. */
#include "app/app.h"
#include "app/ui/core/ui_sm.h"
#include "services/core/app_evt.h"
#include "services/core/port.h"
#include "services/disp/disp_pipe.h"
#include "services/power/power_sm.h"
#include "services/recorder/rec_sm.h"
#include "services/storage/storage_sm.h"
#include "services/tx_sched/tx_sm.h"
#include "sim/env_sim.h"
#include "sim/local_files_ui.h"
#include "sim/send_plan.h"
#include "sim/sim_api.h"
#include "sim/tx_scope.h"
#include "sim/tx_sequence.h"
#include "services/keysim/keysim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks;
#define CHECK(x) do {++checks;if(!(x)){fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x);exit(1);}}while(0)
static void run(unsigned ms){for(unsigned i=0;i<ms;i++)sim_tick(1);}
static void post(ev_type_t type,int ch,int a,int b){CHECK(evt_post4(type,(uint8_t)ch,SRC_UI,a,b));app_tick(0);}
static void key(key_id_t k){post(EV_KEY,k,GEST_SHORT,0);}
static void touch(int row){post(EV_TOUCH_ITEM,0,row,0);}
static const char *json(void){static char data[65536];jw_t j;jw_init(&j,data,sizeof data);jw_obj(&j);ui_json(&j);jw_end_obj(&j);CHECK(jw_finish(&j)>0&&!j.overflow);return data;}
static void boot(void){sim_reset();sim_key(KEY_POWER,1);run(1200);sim_key(KEY_POWER,0);run(600);env_cmd("bus_fps",0,0);env_cmd("bus_fps",1,0);CHECK(power_running()&&storage_get()->state==ST_READY);}
static int row(const char *label){
 const char *at=strstr(json(),"\"items\":[");CHECK(at!=NULL);int index=0;char expected[160];snprintf(expected,sizeof expected,"\"l\":\"%s\"",label);
 while((at=strstr(at,"\"l\":"))){if(!strncmp(at,expected,strlen(expected)))return index;index++;at++;}
 CHECK(false);return -1;
}
static void tap(const char *label){touch(row(label));}
static void home_and_send_routes(void){
 boot();CHECK(PG_SENDMODE==18&&PG_PROJECTS==19&&PG_PROJECTFILES==20);
 CHECK(row("实时显示")==0&&row("发送报文")==1&&row("系统设置")==5);
 CHECK(!strstr(json(),"\"l\":\"录制设置\"")&&!strstr(json(),"\"l\":\"手动编辑报文\""));
 const int columns=keysim_available()?4:3,last=keysim_available()?6:5;
 if(keysim_available())CHECK(row("按键模拟")==6);
 key(KEY_DOWN);CHECK(ui_cursor()==columns);key(KEY_OK);CHECK(ui_page()==(columns==3?PG_FILES:PG_CHSET));
 key(KEY_BACK);key(KEY_UP);CHECK(ui_cursor()==0);key(KEY_RIGHT);key(KEY_OK);CHECK(ui_page()==PG_TX);
 CHECK(!strstr(json(),"现有报文")&&!strstr(json(),"手动编辑报文")&&!strstr(json(),"采集日志回放"));
 tap("加载文件");CHECK(ui_page()==PG_PROJECTFILES);key(KEY_BACK);CHECK(ui_page()==PG_TX);
 key(KEY_BACK);CHECK(ui_page()==PG_HOME);for(unsigned i=0;i<12;i++)key(KEY_RIGHT);CHECK(ui_cursor()==last);
 key(KEY_DOWN);CHECK(ui_cursor()==last);key(KEY_UP);CHECK(ui_cursor()==last-columns);
}
static void project_files_and_unified_inventory(void){
 boot();sp_file_t files[3];memset(files,0,sizeof files);send_plan_t plan;CHECK(sp_capture(1,&plan)==0);
 const char *names[]={"A_one.canplan.json","B_one.canplan.json","A_two.canplan.json"};
 for(unsigned i=0;i<3;i++){snprintf(files[i].name,sizeof files[i].name,"%s",names[i]);snprintf(files[i].project,sizeof files[i].project,"%s",i==1?"台架 B":"台架 A");files[i].plan=plan;}
 CHECK(sp_catalog_replace(files,3)==0);
 local_db_frame_t frame={.id=0x123,.length=8};snprintf(frame.name,sizeof frame.name,"Motor");CHECK(local_db_store("motor.dbc",&frame,1)>=0);
 CHECK(ui_review_navigate(PG_SENDMODE)==0&&ui_page()==PG_TX);tap("加载文件");CHECK(ui_page()==PG_PROJECTFILES);
 CHECK(row(names[0])==0&&row(names[1])==1&&row(names[2])==2);
 CHECK(!strstr(json(),"台架 A")&&!strstr(json(),"台架 B"));
 key(KEY_DOWN);key(KEY_DOWN);key(KEY_OK);CHECK(ui_page()==PG_TX&&strstr(json(),"\"tx_scope_mask\":1"));
 CHECK(!tx_any_active());
 CHECK(ui_review_navigate(PG_FILES)==0);tap(names[2]);CHECK(ui_page()==PG_PLANDETAIL&&strstr(json(),names[2]));
 files[2].plan.channels[0].entries[0].data[0]^=0xff;CHECK(sp_catalog_replace(files,3)==0);
 uint32_t previous=tx_get()->e[0][0].f.id;touch(4);CHECK(tx_get()->e[0][0].f.id==previous&&!tx_any_active());
 CHECK(strstr(ui_last_toast(),"变化")!=NULL);key(KEY_BACK);CHECK(ui_page()==PG_FILES);
 CHECK(ui_review_navigate(PG_SENDMODE)==0);tap("加载文件");key(KEY_BACK);CHECK(ui_page()==PG_TX);
 CHECK(ui_review_navigate(PG_PROJECTS)==UI_REVIEW_RANGE&&ui_review_navigate(PG_LOGPLAY)==UI_REVIEW_RANGE);
 CHECK(ui_review_navigate(PG_FILES)==0);file_info_t first;CHECK(port_sd_file_info(0,&first));
 CHECK(row(first.name)==0&&row(names[0])==port_sd_file_count());
 CHECK(row("motor.dbc")==port_sd_file_count()+3);
 CHECK(row("进入 U 盘模式")==port_sd_file_count()+4&&row("格式化 SD 卡")==port_sd_file_count()+5);
 CHECK(!strstr(json(),"\"l\":\"SD 卡\"")&&!strstr(json(),"\"l\":\"USB\""));
 tap("motor.dbc");CHECK(ui_dialog_open()&&strstr(json(),"只读预览"));key(KEY_BACK);
 touch(0);CHECK(ui_page()==PG_FILEDETAIL&&strstr(json(),"内置样例只读"));tap("修改文件名");CHECK(!ui_draft_open());
 key(KEY_BACK);CHECK(ui_page()==PG_FILES);tap(names[0]);CHECK(ui_page()==PG_PLANDETAIL);key(KEY_BACK);CHECK(ui_page()==PG_FILES);
 tap("格式化 SD 卡");CHECK(ui_dialog_ask()==ASK_FORMAT&&strstr(json(),"已保存发送方案保留"));key(KEY_BACK);CHECK(sp_catalog_count()==3);
}
static void recording_returns_to_paused_live(void){
 boot();CHECK(ui_review_navigate(PG_LIVE)==0);CHECK(ui_live_set_paused(true)==0);
 CHECK(ui_review_navigate(PG_REC)==0);key(KEY_BACK);CHECK(ui_page()==PG_LIVE&&disp_paused());
 CHECK(ui_review_navigate(PG_REC)==0);touch(0);CHECK(ui_page()==PG_LIVE&&disp_paused());run(200);
 CHECK(rec_get()->state==REC_RUNNING&&strstr(json(),"\"can_stop\":true")&&strstr(json(),"\"can_mark\":true"));
 int count=port_sd_file_count();uint32_t session=rec_get()->session;
 CHECK(sim_rec_action(2)==0&&rec_get()->markers==1&&ui_page()==PG_LIVE&&disp_paused()); /* Explicit legacy mark API. */
 CHECK(ui_review_navigate(PG_FILES)==0);file_info_t info;CHECK(port_sd_file_info(count-1,&info)&&info.writing);
 tap(info.name);CHECK(ui_page()==PG_FILEDETAIL&&strstr(json(),"正在写入"));touch(4);CHECK(!ui_dialog_open()&&!ui_draft_open());
 CHECK(rec_get()->state==REC_RUNNING);CHECK(ui_review_navigate(PG_LIVE)==0);CHECK(ui_live_set_paused(true)==0);
 post(EV_KEY,KEY_REC,GEST_LONG,0);run(300);CHECK(rec_get()->state==REC_IDLE&&ui_page()==PG_LIVE&&disp_paused());
 CHECK(ui_draft_open()&&strstr(json(),"local_record_save"));key(KEY_BACK);CHECK(!ui_draft_open());
 key(KEY_REC);run(200);CHECK(rec_get()->state==REC_RUNNING&&rec_get()->session!=session&&port_sd_file_count()==count+1);
 post(EV_KEY,KEY_REC,GEST_LONG,0);run(300);
}
static void scope_is_separate_and_save_is_complete(void){
 boot();CHECK(ui_review_navigate(PG_TX)==0);CHECK(ui_tx_select(1,0)==0);CHECK(ui_tx_scope_select(3)==0);
 CHECK(strstr(json(),"\"tx_channel\":1")&&strstr(json(),"\"tx_scope_mask\":3"));
 touch(CH_NUM*TX_ENTRIES+5);CHECK(strstr(json(),"\"tx_scope_mask\":1")&&strstr(json(),"\"tx_channel\":1"));
 key(KEY_LEFT);CHECK(strstr(json(),"\"tx_scope_mask\":3"));
 uint8_t a[1]={0x11},b[1]={0x22};CHECK(tx_entry_update(0,0,0x111,0,1,a,0)==TX_OK);CHECK(tx_entry_update(1,0,0x222,0,1,b,0)==TX_OK);
 touch(CH_NUM*TX_ENTRIES+3);CHECK(ui_draft_open()&&strstr(json(),"保存两路发送方案"));
 tap("保存");sp_io_request_t request;CHECK(sp_io_peek(&request)&&request.plan.channel_mask==3);
 CHECK(request.plan.channels[0].entries[0].data[0]==0x11&&request.plan.channels[1].entries[0].data[0]==0x22);
 CHECK(sp_io_complete(request.id,SP_ERR_IO,"模拟写盘失败")==SP_ERR_IO);
 CHECK(ui_review_navigate(PG_TX)==0);CHECK(ui_tx_scope_select(1)==0);CHECK(ui_tx_select(1,0)==0);
 touch(CH_NUM*TX_ENTRIES+3);tap("保存");CHECK(sp_io_peek(&request)&&request.plan.channel_mask==2);
 CHECK(sp_io_complete(request.id,SP_ERR_IO,"模拟写盘失败")==SP_ERR_IO);CHECK(ui_review_navigate(PG_TX)==0);
 CHECK(ui_tx_select(1,0)==0&&ui_tx_scope_select(3)==0);key(KEY_OK);CHECK(ui_dialog_ask()==ASK_SCOPE_START);
 CHECK(strstr(json(),"CAN1 + CAN2")&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);key(KEY_BACK);run(10);
 CHECK(!tx_any_active()&&tx_get()->echo_count[0]==0&&tx_get()->echo_count[1]==0);
 CHECK(ui_tx_edit_open(1,0));CHECK(ui_tx_scope_select(1)==UI_REVIEW_DRAFT);key(KEY_BACK);
 CHECK(ui_tx_scope_select(0)==UI_REVIEW_RANGE);post(EV_COMBO,0,COMBO_LOCK,0);CHECK(ui_tx_scope_select(1)==UI_REVIEW_LOCKED);
}
int main(void){home_and_send_routes();project_files_and_unified_inventory();recording_returns_to_paused_live();scope_is_separate_and_save_is_complete();printf("Device menu / flat files / live recording / independent send scope: %u checks passed\n",checks);return 0;}
