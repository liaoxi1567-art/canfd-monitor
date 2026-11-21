/* Real FatFs + production storage_sd/diskio, host HAL card in RAM.
 * No real volume, peripheral, USB device or external disk is accessed.
 */
#include "stm32h7xx_hal.h"
#include "storage_sd.h"
#include "ff.h"
#include "diskio.h"
#include "services/core/port.h"
#include "services/core/app_evt.h"
#include "services/player/play_sm.h"
#include "services/config/cfg.h"
#include "services/device/file_port.h"
#include "send_plan_json.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CARD_BLOCKS 32768u
static uint8_t ram_card[CARD_BLOCKS * 512u];
GPIO_TypeDef mock_gpio_c, mock_gpio_d;
static bool inserted = true, fail_write, fail_read;
static uint32_t tick, writes, pumps, dma_starts, dma_aborts;
static bool dma_hang, dma_pending, irq_enabled, old_irq_pending, old_hw_flag;
static uint32_t resets, late_callbacks, card_inits;
static unsigned dma_delay;
static void (*capture_during_wait)(void);
static struct { SD_HandleTypeDef *handle; uint8_t *data;
    uint32_t lba, count; bool writing; } pending;
static unsigned checks, events[EV_COUNT];
static int32_t last_result[EV_COUNT];
static unsigned reject_events;
static sp_io_request_t plan_request;
static sp_file_t plan_catalog[SP_MAX_FILES];
static unsigned plan_count,plan_done;
static int plan_result;
static char plan_message[192];
static sp_status_t plan_status;
bool sp_io_peek(sp_io_request_t *out){if(!plan_request.kind)return false;*out=plan_request;return true;}
int sp_io_complete(uint32_t id,int result,const char *message)
{if(id!=plan_request.id)return SP_ERR_CHANGED;snprintf(plan_message,sizeof plan_message,"%s",message);plan_request.kind=0;plan_result=result;plan_done++;return result;}
unsigned sp_catalog_count(void){return plan_count;}
bool sp_catalog_get(unsigned index,sp_file_t *out){if(index>=plan_count)return false;*out=plan_catalog[index];return true;}
int sp_catalog_replace(const sp_file_t *source,uint32_t count)
{if(count>SP_MAX_FILES)return SP_ERR_FILE;for(unsigned i=0;i<count;i++)if(sp_validate(&source[i].plan))return SP_ERR_FORMAT;if(count)memcpy(plan_catalog,source,count*sizeof *source);plan_count=count;return 0;}
const sp_status_t *sp_state(void){return &plan_status;}
static player_t players[CH_NUM];
static int removed_index = -1;

#define CHECK(condition) do { checks++; if (!(condition)) { \
    fprintf(stderr, "storage test failed line %d: %s\n", __LINE__, #condition); exit(1); \
} } while (0)

uint32_t HAL_GetTick(void) { return tick++; }
#ifdef CANFD_REVE_TARGET
static bool sd_power,sd_pc_hiz,sd_pd_hiz;
static uint32_t sd_ready_at,sd_rail_cycles;
void HAL_Delay(uint32_t ms){tick+=ms;}
void HAL_GPIO_WritePin(GPIO_TypeDef *gpio,uint16_t pin,GPIO_PinState state)
{
    CHECK(gpio==GPIOD && pin==GPIO_PIN_9);
    if(state==GPIO_PIN_RESET){CHECK(sd_pc_hiz && sd_pd_hiz);sd_power=false;}
    else {sd_power=true;sd_ready_at=tick+5;sd_rail_cycles++;}
}
void HAL_GPIO_Init(GPIO_TypeDef *gpio,const GPIO_InitTypeDef *init)
{
    if((gpio==GPIOC && (init->Pin&GPIO_PIN_8)) || (gpio==GPIOD && init->Pin==GPIO_PIN_2)) {
        CHECK(init->Pull==GPIO_NOPULL);
        bool hiz=init->Mode==GPIO_MODE_INPUT;
        if(!hiz){CHECK(sd_power);CHECK((int32_t)(tick-sd_ready_at)>=0);}
        if(gpio==GPIOC)sd_pc_hiz=hiz;else sd_pd_hiz=hiz;
    }
}
#else
void HAL_GPIO_Init(GPIO_TypeDef *gpio, const GPIO_InitTypeDef *init) { (void)gpio; (void)init; }
#endif
GPIO_PinState HAL_GPIO_ReadPin(GPIO_TypeDef *gpio, uint16_t pin)
{ (void)gpio; (void)pin; return inserted ? GPIO_PIN_RESET : GPIO_PIN_SET; }
HAL_StatusTypeDef HAL_SD_Init(SD_HandleTypeDef *handle)
{
#ifdef CANFD_REVE_TARGET
 CHECK(sd_power && !sd_pc_hiz && !sd_pd_hiz); CHECK(sd_rail_cycles>0);
#endif
 CHECK(handle->Instance == SDMMC1); CHECK(handle->Init.ClockDiv == 4); card_inits++; return inserted ? HAL_OK : HAL_ERROR;
}
HAL_StatusTypeDef HAL_SD_DeInit(SD_HandleTypeDef *handle)
{ CHECK(handle->Instance == SDMMC1); return HAL_OK; }
HAL_StatusTypeDef HAL_SD_ConfigWideBusOperation(SD_HandleTypeDef *handle, uint32_t width)
{ (void)handle; CHECK(width == SDMMC_BUS_WIDE_4B); return HAL_OK; }
HAL_StatusTypeDef HAL_SD_GetCardInfo(const SD_HandleTypeDef *handle, HAL_SD_CardInfoTypeDef *info)
{ (void)handle; info->LogBlockSize = 512; info->LogBlockNbr = CARD_BLOCKS; return HAL_OK; }
HAL_SD_CardStateTypeDef HAL_SD_GetCardState(SD_HandleTypeDef *handle)
{ (void)handle; return inserted ? HAL_SD_CARD_TRANSFER : HAL_SD_CARD_DISCONNECTED; }
void HAL_NVIC_SetPriority(int irq, uint32_t preempt, uint32_t sub)
{ CHECK(irq == SDMMC1_IRQn && preempt == 5 && sub == 0); }
void HAL_NVIC_EnableIRQ(int irq)
{ CHECK(irq == SDMMC1_IRQn); CHECK(!old_irq_pending && !old_hw_flag); irq_enabled = true; }
void HAL_NVIC_DisableIRQ(int irq) { CHECK(irq == SDMMC1_IRQn); irq_enabled = false; }
void HAL_NVIC_ClearPendingIRQ(int irq) { CHECK(irq == SDMMC1_IRQn); old_irq_pending = false; }
void test_sd_force_reset(void)
{ CHECK(!irq_enabled); old_hw_flag = false; dma_pending = false; resets++; }
static HAL_StatusTypeDef dma_start(SD_HandleTypeDef *handle, uint8_t *data,
                                   uint32_t lba, uint32_t count, bool writing)
{
    CHECK(!dma_pending);
    CHECK(((uintptr_t)data & 31u) == 0);
    CHECK(count <= 8u);
    if (!inserted || !count || lba >= CARD_BLOCKS || count > CARD_BLOCKS - lba)
        return HAL_ERROR;
    pending.handle = handle; pending.data = data;
    pending.lba = lba; pending.count = count; pending.writing = writing;
    dma_delay = 3; dma_pending = true; dma_starts++;
    return HAL_OK;
}
HAL_StatusTypeDef HAL_SD_ReadBlocks_DMA(SD_HandleTypeDef *handle, uint8_t *data,
                                       uint32_t lba, uint32_t count)
{ return dma_start(handle, data, lba, count, false); }
HAL_StatusTypeDef HAL_SD_WriteBlocks_DMA(SD_HandleTypeDef *handle, const uint8_t *data,
                                        uint32_t lba, uint32_t count)
{ return dma_start(handle, (uint8_t *)data, lba, count, true); }
HAL_StatusTypeDef HAL_SD_Abort_IT(SD_HandleTypeDef *handle)
{
    (void)handle;
    CHECK(!irq_enabled);
    /* Model a DATAEND already latched when Abort_IT returns. */
    old_irq_pending = old_hw_flag = true;
    dma_pending = false; dma_aborts++;
    return HAL_OK;
}
void HAL_SD_IRQHandler(SD_HandleTypeDef *handle)
{
    CHECK(dma_pending && handle == pending.handle);
    dma_pending = false;
    if (pending.writing ? fail_write : fail_read) { HAL_SD_ErrorCallback(handle); return; }
    uint8_t *card = ram_card + (size_t)pending.lba * 512u;
    size_t bytes = (size_t)pending.count * 512u;
    if (pending.writing) {
        memcpy(card, pending.data, bytes); writes += pending.count;
        HAL_SD_TxCpltCallback(handle);
    } else { memcpy(pending.data, card, bytes); HAL_SD_RxCpltCallback(handle); }
}
extern void SDMMC1_IRQHandler(void);
void revd_io_wait_poll(void)
{
    pumps++;
    if (capture_during_wait) capture_during_wait();
    if (irq_enabled && (old_irq_pending || old_hw_flag)) {
        late_callbacks++;
        old_irq_pending = old_hw_flag = false;
        HAL_SD_RxCpltCallback(pending.handle);
    }
    if (dma_pending && !dma_hang && --dma_delay == 0) SDMMC1_IRQHandler();
}

bool evt_post4(ev_type_t type, uint8_t ch, src_t src, int32_t a, int32_t b)
{ (void)ch; (void)src; (void)b; if(reject_events){reject_events--;return false;} events[type]++; last_result[type] = a; return true; }
void evt_msg(msg_t message, uint8_t ch, int32_t b) { (void)message; (void)ch; (void)b; }
const player_t *play_get(int ch) { return &players[ch]; }
bool play_any_active(void)
{ return players[0].state != PL_IDLE || players[1].state != PL_IDLE; }
bool play_file_in_use(int index)
{ return (players[0].state != PL_IDLE && players[0].file == index) ||
         (players[1].state != PL_IDLE && players[1].file == index); }
void play_file_removed(int index) { removed_index = index; }

static void await_event(ev_type_t type, unsigned before)
{
    for (unsigned i = 0; i < 10000 && events[type] == before; i++) storage_sd_task();
    if (events[type] == before) fprintf(stderr, "await event stalled type=%d before=%u tick=%u\n", type,before,tick);
    CHECK(events[type] > before);
}
static void mount_sync(void)
{ unsigned before = events[EV_SD_MOUNT_DONE]; port_sd_mount(); await_event(EV_SD_MOUNT_DONE, before); }
static void format_sync(void)
{ unsigned before = events[EV_SD_FORMAT_DONE]; port_sd_format(); await_event(EV_SD_FORMAT_DONE, before); }
static void prealloc_sync(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, bool f)
{ unsigned before = events[EV_SD_PREALLOC_DONE]; port_sd_prealloc(a,b,c,d,e,f); await_event(EV_SD_PREALLOC_DONE, before); }
static void request_close(void) { port_sd_close(); }
static void request_delete(void) { port_sd_delete_oldest(); }
static void request_protect(int index) { port_sd_protect_toggle(index); }
static void close_sync(void)
{ unsigned before = events[EV_SD_CLOSE_DONE]; port_sd_close(); await_event(EV_SD_CLOSE_DONE, before); }
static void delete_sync(void)
{ unsigned before = events[EV_SD_DELETE_DONE]; port_sd_delete_oldest(); await_event(EV_SD_DELETE_DONE, before); }
static void tasks(unsigned count) { while (count--) storage_sd_task(); }
static void unmount_sync(void) { port_sd_unmount(); tasks(32); }
static void protect_sync(int index) { port_sd_protect_toggle(index); tasks(32); }
static void reinit_sync(void) { port_sd_reinit(); tasks(32); }
#define port_sd_mount mount_sync
#define port_sd_format format_sync
#define port_sd_prealloc prealloc_sync
#define port_sd_close close_sync
#define port_sd_delete_oldest delete_sync
#define port_sd_unmount unmount_sync
#define port_sd_protect_toggle protect_sync
#define port_sd_reinit reinit_sync

static can_frame_t frame(unsigned ordinal)
{
    can_frame_t f = { .ts_us = 1000000u + (uint64_t)ordinal * 500u,
                      .id = 0x123, .ch = ordinal % 2u, .len = 8 };
    if (ordinal % 3u == 0) { f.flags = CANF_FD | CANF_BRS; f.len = 64; }
    if (ordinal % 5u == 0) { f.flags |= CANF_EXT; f.id = 0x1234567; }
    for (unsigned i = 0; i < f.len; i++) f.data[i] = (uint8_t)(ordinal + i);
    return f;
}

static unsigned injected, injection_limit;
static bool injection_ok;
static void inject_capture(void)
{
    if (injected >= injection_limit) return;
    storage_sd_task(); /* Guard must reject recursion while FatFs owns state. */
    can_frame_t incoming = frame(100u + injected);
    if (!port_sd_record_frame(&incoming)) injection_ok = false;
    injected++;
}

static int find_name(const char *part)
{
    for (int i = 0; i < port_sd_file_count(); i++) {
        file_info_t info;
        CHECK(port_sd_file_info(i, &info));
        if (strstr(info.name, part)) return i;
    }
    return -1;
}

static void small_record(uint32_t session)
{
    port_sd_prealloc(session, 1, 64, 500000, 2000000, true);
    CHECK(last_result[EV_SD_PREALLOC_DONE] == SD_OK);
    can_frame_t f = frame(0);
    port_sd_record_frame(&f);
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);
}

static int test_json_read(void *context,uint8_t *buffer,size_t capacity)
{UINT read=0;return f_read((FIL *)context,buffer,(UINT)capacity,&read)==FR_OK?(int)read:-1;}
static void plan_io(unsigned kind,const char *name,const send_plan_t *plan)
{
    static unsigned serial;unsigned previous=plan_done;
    memset(&plan_request,0,sizeof plan_request);plan_request.kind=kind;plan_request.id=++serial;
    if(name)snprintf(plan_request.name,sizeof plan_request.name,"%s",name);
    if(plan)plan_request.plan=*plan;
    for(unsigned i=0;i<1000&&plan_done==previous;i++)storage_sd_task();
    CHECK(plan_done==previous+1);
}
static void create_bytes(const char *path,const char *data,size_t length)
{
    FIL file;UINT written=0;CHECK(f_open(&file,path,FA_CREATE_NEW|FA_WRITE)==FR_OK);
    CHECK(f_write(&file,data,(UINT)length,&written)==FR_OK&&written==length);
    CHECK(f_sync(&file)==FR_OK);CHECK(f_close(&file)==FR_OK);
}
typedef struct {const uint8_t *data;size_t size,position,chunk;} memory_json_t;
static int memory_json_read(void *context,uint8_t *buffer,size_t capacity)
{
    memory_json_t *input=context;size_t n=input->size-input->position;
    if(n>capacity)n=capacity;if(n>input->chunk)n=input->chunk;
    memcpy(buffer,input->data+input->position,n);input->position+=n;return (int)n;
}
static void codec_boundaries(const send_plan_t *valid)
{
    static char text[SP_JSON_OUTPUT_BYTES],changed[SP_JSON_OUTPUT_BYTES+1024];size_t length;
    CHECK(sp_json_encode(valid,"项目:测试",text,sizeof text,&length)==0);
    memory_json_t input={(const uint8_t *)text,length,0,1};send_plan_t parsed=*valid;char project[48],error[96];
    CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==0);
    CHECK(!memcmp(&parsed,valid,sizeof parsed)&&!strcmp(project,"项目:测试"));
    char tiny[16]="old";CHECK(sp_json_encode(valid,"项目",tiny,sizeof tiny,&length)==SP_ERR_IO&&!tiny[0]);
    CHECK(sp_json_encode(valid,"\xC2\x80",text,sizeof text,&length)==SP_ERR_FORMAT);
    CHECK(sp_json_encode(valid,"\xED\xA0\x80",text,sizeof text,&length)==SP_ERR_FORMAT);
    CHECK(sp_json_encode(valid,"项目",text,sizeof text,&length)==0);
    snprintf(changed,sizeof changed,"{\"version\":1,%s",text+1);
    input=(memory_json_t){(const uint8_t *)changed,strlen(changed),0,3};
    CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==SP_ERR_FORMAT);
    CHECK(strstr(error,"version")&&strstr(error,"line"));CHECK(!memcmp(&parsed,valid,sizeof parsed));
    snprintf(changed,sizeof changed,"{\"unknown\":true,%s",text+1);
    input=(memory_json_t){(const uint8_t *)changed,strlen(changed),0,7};
    CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==SP_ERR_FORMAT);
    CHECK(strstr(error,"unknown"));
    snprintf(changed,sizeof changed,"{\"description\":\"\\uD800\",%s",text+1);
    input=(memory_json_t){(const uint8_t *)changed,strlen(changed),0,1};
    CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==SP_ERR_FORMAT);
    memcpy(changed,"\xEF\xBB\xBF",3);memcpy(changed+3,text,length);
    input=(memory_json_t){(const uint8_t *)changed,length+3,0,1};
    CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==0);
    static uint8_t large[SP_JSON_MAX_BYTES+1u];memcpy(large,text,length);memset(large+length,' ',sizeof large-length);
    input=(memory_json_t){large,SP_JSON_MAX_BYTES,0,512};CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==0);
    input=(memory_json_t){large,sizeof large,0,512};CHECK(sp_json_decode(memory_json_read,&input,&parsed,project,error)==SP_ERR_FORMAT);
    CHECK(!memcmp(&parsed,valid,sizeof parsed));
}

#ifdef CANFD_WINSIM_HW
/* APM key tables (/APMKEY/K1.TXT … K4.TXT): the keysim service is replaced by
 * this one-slot request double, like the send-plan double above. */
#include "services/keysim/keysim_file.h"
static ks_io_request_t key_request;
static bool key_pending;
static int key_result=1;
static unsigned key_done,key_loaded_mask;
static ks_table_t key_loaded[WINSIM_NUM_CH];
bool keysim_io_peek(ks_io_request_t *out){if(!key_pending)return false;*out=key_request;return true;}
void keysim_io_complete(uint32_t id,int result){if(!key_pending||id!=key_request.id)return;key_pending=false;key_result=result;key_done++;}
void keysim_table_loaded(uint8_t ch,const ks_table_t *table){if(ch<WINSIM_NUM_CH){key_loaded[ch]=*table;key_loaded_mask|=1u<<ch;}}
static void key_io(unsigned kind,uint8_t ch,const ks_table_t *table)
{
    static unsigned serial;unsigned before=key_done;
    memset(&key_request,0,sizeof key_request);key_request.id=++serial;key_request.kind=(uint8_t)kind;key_request.ch=ch;
    if(table)key_request.table=*table;
    key_pending=true;key_loaded_mask=0;
    for(unsigned i=0;i<1000&&key_done==before;i++)storage_sd_task();
    CHECK(key_done==before+1);
}
static ks_table_t key_table(const char *name,unsigned n)
{
    ks_table_t t;memset(&t,0,sizeof t);t.valid=true;snprintf(t.name,sizeof t.name,"%s",name);
    for(unsigned i=0;i<n;i++){snprintf(t.keys[i].name,sizeof t.keys[i].name,"档%u",i+1);t.keys[i].mv=(uint16_t)(4000u-700u*i);t.keys[i].ohm=1000.0f*(float)(i+1);}
    t.n=(uint8_t)n;return t;
}
static bool read_text(const char *path,char *buffer,size_t capacity)
{
    FIL file;UINT count=0;if(f_open(&file,path,FA_READ)!=FR_OK)return false;
    FRESULT result=f_read(&file,buffer,(UINT)(capacity-1u),&count);buffer[count]=0;
    return f_close(&file)==FR_OK&&result==FR_OK;
}
static unsigned key_dir_entries(void)
{
    DIR dir;FILINFO info;unsigned n=0;CHECK(f_opendir(&dir,"0:/APMKEY")==FR_OK);
    while(f_readdir(&dir,&info)==FR_OK&&info.fname[0])n++;
    CHECK(f_closedir(&dir)==FR_OK);return n;
}
static void keysim_files(void)
{
    char text[KS_FILE_MAX_BYTES+1];FILINFO info;
    ks_table_t learned=key_table("学习 · K2",3),scanned=key_table("扫描 · K2",5);
    key_io(KS_IO_SAVE,1,&learned);CHECK(key_result==0);
    CHECK(read_text("0:/APMKEY/K2.TXT",text,sizeof text));
    CHECK(!strncmp(text,"CANFD-APMKEY 1\nchannel=2\nname=学习 · K2\n",strlen("CANFD-APMKEY 1\nchannel=2\nname=学习 · K2\n")));
    CHECK(strstr(text,"key=档1,4000,1000\n")&&key_dir_entries()==1);
    /* Replacing keeps exactly one file: no .BAK or temporary left behind. */
    key_io(KS_IO_SAVE,1,&scanned);CHECK(key_result==0);
    CHECK(read_text("0:/APMKEY/K2.TXT",text,sizeof text)&&strstr(text,"name=扫描 · K2")&&key_dir_entries()==1);
    key_io(KS_IO_LOAD,0,NULL);CHECK(key_result==0&&key_loaded_mask==2u);
    CHECK(key_loaded[1].valid&&key_loaded[1].n==5&&!strcmp(key_loaded[1].name,"扫描 · K2")&&key_loaded[1].keys[4].mv==1200);
    /* A hand-broken or oversized file is skipped; other channels still load. */
    create_bytes("0:/APMKEY/K3.TXT","garbage\n",8);
    static char big[KS_FILE_MAX_BYTES+64];memset(big,'#',sizeof big);memcpy(big,"CANFD-APMKEY 1\n",15);
    create_bytes("0:/APMKEY/K4.TXT",big,sizeof big);
    key_io(KS_IO_LOAD,0,NULL);CHECK(key_result==0&&key_loaded_mask==2u);
    /* Power lost between the two renames: the backup is restored on load. */
    CHECK(f_rename("0:/APMKEY/K2.TXT","0:/APMKEY/K2.BAK")==FR_OK);
    key_io(KS_IO_LOAD,0,NULL);CHECK(key_result==0&&key_loaded_mask==2u&&!strcmp(key_loaded[1].name,"扫描 · K2"));
    CHECK(f_stat("0:/APMKEY/K2.TXT",&info)==FR_OK&&f_stat("0:/APMKEY/K2.BAK",&info)==FR_NO_FILE);
    /* Failed write keeps the previous table. */
    fail_write=true;key_io(KS_IO_SAVE,1,&learned);fail_write=false;
    CHECK(key_result==KS_IO_ERR_IO);
    CHECK(read_text("0:/APMKEY/K2.TXT",text,sizeof text)&&strstr(text,"name=扫描 · K2"));
    /* Invalid table is refused before any file is touched. */
    ks_table_t bad=learned;bad.keys[0].mv=6000;
    key_io(KS_IO_SAVE,1,&bad);CHECK(key_result==KS_FILE_ERR_NAME);
    CHECK(read_text("0:/APMKEY/K2.TXT",text,sizeof text)&&strstr(text,"name=扫描 · K2"));
    /* Read-only file is not replaced. */
    CHECK(f_chmod("0:/APMKEY/K2.TXT",AM_RDO,AM_RDO)==FR_OK);
    key_io(KS_IO_SAVE,1,&learned);CHECK(key_result==KS_IO_ERR_IO);
    CHECK(f_chmod("0:/APMKEY/K2.TXT",0,AM_RDO)==FR_OK);
    key_io(KS_IO_SAVE,0,&learned);CHECK(key_result==0&&f_stat("0:/APMKEY/K1.TXT",&info)==FR_OK);
    /* Without a mounted card the request completes as unavailable. */
    port_sd_unmount();key_io(KS_IO_SAVE,1,&learned);CHECK(key_result==KS_IO_ERR_UNAVAILABLE);
    port_sd_mount();key_io(KS_IO_LOAD,0,NULL);CHECK(key_result==0&&key_loaded_mask==3u);
}
#endif
static void device_file_operations(void)
{
    CHECK(!file_port_rec_prefix_get()[0]);
    CHECK(file_port_rec_prefix_set("道路A")==FILE_PORT_OK);
    CHECK(!strcmp(file_port_rec_prefix_get(),"道路A"));
    CHECK(file_port_rec_prefix_set("../bad")==FILE_PORT_ERR_NAME);
    port_sd_unmount();port_sd_mount();
    CHECK(!strcmp(file_port_rec_prefix_get(),"道路A"));
    port_sd_prealloc(1,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    uint32_t token=file_port_record_token(1);CHECK(token!=0&&file_port_record_token(99)==0);
    int first=port_sd_file_count()-1;CHECK(file_port_token(first)==token);
    CHECK(file_port_rec_prefix_set("busy")==FILE_PORT_ERR_BUSY);
    CHECK(file_port_rename(first,token,"busy.log")==FILE_PORT_ERR_BUSY);
    can_frame_t input=frame(4);CHECK(port_sd_record_frame(&input));
    port_sd_next_segment(2);tasks(16);input=frame(5);CHECK(port_sd_record_frame(&input));port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE]==SD_OK);
    CHECK(port_sd_file_count()==2&&file_port_token(first+1)==token);
    CHECK(file_port_rename(first,token+99,"stale.log")==FILE_PORT_ERR_CHANGED);
    CHECK(file_port_rename(first,token,"完整会话.log")==FILE_PORT_OK);
    FILINFO info;
    CHECK(f_stat("0:/CANLOG/S0000001_完整会话.log/00000001.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000001_完整会话.log/00000002.LOG",&info)==FR_OK);
    can_frame_t restored;CHECK(port_sd_read_frame(first,0,0,&restored)&&restored.data[0]==4);
    CHECK(port_sd_read_frame(first+1,1,0,&restored)&&restored.data[0]==5);
    tasks(2);
    CHECK(f_stat("0:/CANLOG/S0000001_道路A",&info)==FR_NO_FILE);
    file_info_t file;CHECK(port_sd_file_info(first,&file)&&file.is_session&&!strcmp(file.name,"完整会话.log"));
    CHECK(file_port_token(first)==token&&file_port_token(first+1)==token);
    CHECK(f_mkdir("0:/CANLOG/S0000001_exists.log")==FR_OK);
    CHECK(file_port_rename(first,token,"exists.log")==FILE_PORT_ERR_EXISTS);
    CHECK(port_sd_file_info(first,&file)&&!strcmp(file.name,"完整会话.log"));
    CHECK(f_chmod("0:/CANLOG/S0000001_完整会话.log/00000002.LOG",AM_RDO,AM_RDO)==FR_OK);
    CHECK(file_port_rename(first,token,"protected.log")==FILE_PORT_ERR_READONLY);
    CHECK(f_chmod("0:/CANLOG/S0000001_完整会话.log/00000002.LOG",0,AM_RDO)==FR_OK);
    fail_write=true;CHECK(file_port_rec_prefix_set("失败")==FILE_PORT_ERR_IO);fail_write=false;
    CHECK(!strcmp(file_port_rec_prefix_get(),"道路A"));
    fail_write=true;CHECK(file_port_rename(first,token,"io-failure.log")==FILE_PORT_ERR_IO);fail_write=false;
    CHECK(port_sd_file_info(first,&file)&&!strcmp(file.name,"完整会话.log"));
    port_sd_unmount();port_sd_mount();
    CHECK(!strcmp(file_port_rec_prefix_get(),"道路A"));
    CHECK(file_port_token(first)!=token); /* Mount generation invalidates old handles. */
    CHECK(file_port_rename(first,token,"wrong.log")==FILE_PORT_ERR_CHANGED);
    CHECK(f_stat("0:/CANLOG/S0000001_完整会话.log/00000001.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000001_完整会话.log/00000002.LOG",&info)==FR_OK);
    CHECK(port_sd_read_frame(first,0,0,&restored)&&restored.data[0]==4);
    CHECK(port_sd_read_frame(first+1,1,0,&restored)&&restored.data[0]==5);tasks(2);
    CHECK(f_mkdir("0:/CANLOG/S0000042")==FR_OK);
    const char *legacy="# REVD1 nbr=500000 dbr=2000000 fd=1\n(0.000000) can0 123#AA\n";
    create_bytes("0:/CANLOG/S0000042/00000001.LOG",legacy,strlen(legacy));
    create_bytes("0:/CANLOG/S0000042/00000002.LOG",legacy,strlen(legacy));
    port_sd_unmount();port_sd_mount();
    int legacy_first=find_name("S0000042/00000001.LOG"),legacy_second=find_name("S0000042/00000002.LOG");
    CHECK(legacy_first>=0&&legacy_second>=0);
    uint32_t indexed;uint64_t duration;
    CHECK(port_sd_replay_info(legacy_first,0,&indexed,&duration)&&indexed==1);
    CHECK(port_sd_replay_info(legacy_second,0,&indexed,&duration)&&indexed==1);tasks(2);
    uint32_t legacy_token=file_port_token(legacy_first);CHECK(legacy_token&&file_port_token(legacy_second)==legacy_token);
    CHECK(file_port_record_token(42)==0); /* Historic file is not the current recording. */
    CHECK(file_port_rename(legacy_first,legacy_token,"legacy.log")==FILE_PORT_OK);
    CHECK(f_stat("0:/CANLOG/S0000042_legacy.log/00000001.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000042_legacy.log/00000002.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000042",&info)==FR_NO_FILE);

    /* Import the actual PC template into the real FatFs RAM filesystem. */
    FILE *source=fopen("sim/web/examples/pc-template.canplan.json","rb");CHECK(source!=NULL);
    static char template[16384];size_t length=fread(template,1,sizeof template,source);CHECK(length>0&&length<sizeof template);CHECK(!ferror(source));fclose(source);
    create_bytes("0:/SEND/项目A.canplan.json",template,length);
    plan_io(SP_IO_REFRESH,NULL,NULL);CHECK(plan_result==SP_OK&&plan_count==1);
    CHECK(!strcmp(plan_catalog[0].name,"项目A.canplan.json"));
    CHECK(!strcmp(plan_catalog[0].project,"示例项目 - 单路台架"));
    CHECK(plan_catalog[0].plan.channel_mask==2&&plan_catalog[0].plan.channels[1].entries[1].length==64);
    send_plan_t captured=plan_catalog[0].plan;captured.channels[1].entries[0].data[0]=0xA5;
    codec_boundaries(&captured);
    snprintf(plan_status.loaded_name,sizeof plan_status.loaded_name,"%s",plan_catalog[0].name);
    plan_io(SP_IO_SAVE,"本机发送.canplan.json",&captured);CHECK(plan_result==SP_OK&&plan_count==2);
    FIL saved;CHECK(f_open(&saved,"0:/SEND/本机发送.canplan.json",FA_READ)==FR_OK);
    send_plan_t parsed;char project[48],error[96];
    CHECK(sp_json_decode(test_json_read,&saved,&parsed,project,error)==SP_OK);CHECK(f_close(&saved)==FR_OK);
    CHECK(!memcmp(&parsed,&captured,sizeof parsed)&&!strcmp(project,"示例项目 - 单路台架"));
    static sp_file_t old[SP_MAX_FILES];memcpy(old,plan_catalog,sizeof old);
    plan_io(SP_IO_SAVE,"本机发送.canplan.json",&captured);CHECK(plan_result==SP_ERR_NAME&&plan_count==2);
    CHECK(!memcmp(old,plan_catalog,sizeof old));
    fail_write=true;plan_io(SP_IO_SAVE,"失败.canplan.json",&captured);fail_write=false;
    CHECK(plan_result==SP_ERR_IO&&!memcmp(old,plan_catalog,sizeof old));
    CHECK(strstr(plan_message,"失败.canplan.json")&&strstr(plan_message,"FatFs"));
    port_sd_unmount();port_sd_mount();CHECK(plan_count==2);
    CHECK(f_stat("0:/SEND/失败.canplan.json",&info)==FR_NO_FILE);
    const char *invalid="{\"format\":\"canfd-send-plan\",\"version\":1,\"version\":1,\"channel_mask\":2,\"channels\":[]}";
    create_bytes("0:/SEND/bad.canplan.json",invalid,strlen(invalid));
    memcpy(old,plan_catalog,sizeof old);plan_io(SP_IO_REFRESH,NULL,NULL);CHECK(plan_result==SP_ERR_FORMAT);
    CHECK(strstr(plan_message,"bad.canplan.json")&&strstr(plan_message,"version"));
    CHECK(!memcmp(old,plan_catalog,sizeof old));CHECK(f_unlink("0:/SEND/bad.canplan.json")==FR_OK);
    plan_io(SP_IO_REFRESH,NULL,NULL);CHECK(plan_result==SP_OK&&plan_count==2);

    unsigned completed=events[EV_SD_PREALLOC_DONE];reject_events=10;
    port_sd_prealloc(2,1,64,500000,2000000,true);
    CHECK(events[EV_SD_PREALLOC_DONE]==completed+1); /* await helper observed retried completion */
    storage_sd_stats_t state;storage_sd_stats(&state);
    CHECK(state.event_deferred>0&&state.event_high_water>0&&state.event_fallback==0);
    port_sd_close();
}
static void full_catalog_record_name(void)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    const char *line="(0.000000) can0 123#AA\n";
    for(unsigned i=0;i<128;i++){char path[40];snprintf(path,sizeof path,"0:/OLD%03u.LOG",i);create_bytes(path,line,strlen(line));}
    port_sd_unmount();port_sd_mount();CHECK(port_sd_file_count()==128);
    unsigned metadata_dma=dma_starts;
    for(int i=0;i<128;i++){file_info_t cached;CHECK(port_sd_file_info(i,&cached)&&!cached.sample_available);}
    CHECK(dma_starts==metadata_dma); /* Listing 128 old logs performs no data I/O. */
    uint32_t evicted=file_port_token(126),pinned=file_port_token(127);CHECK(evicted&&pinned);
    players[0].state=PL_PLAYING;players[0].file=127;
    small_record(1);uint32_t token=file_port_record_token(1);CHECK(token);
    CHECK(file_port_token(126)==token&&file_port_token(127)==pinned);
    CHECK(file_port_rename(126,evicted,"stale.log")==FILE_PORT_ERR_CHANGED);
    players[0].state=PL_IDLE;tasks(2);
    CHECK(file_port_rename(126,token,"容量满后命名.log")==FILE_PORT_OK);
    FILINFO info;CHECK(f_stat("0:/OLD126.LOG",&info)==FR_OK); /* Cache eviction never deletes media. */
    CHECK(f_stat("0:/CANLOG/S0000001_容量满后命名.log/00000001.LOG",&info)==FR_OK);
    port_sd_prealloc(2,1,64,500000,2000000,true);CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    token=file_port_record_token(2);CHECK(token);
    for(unsigned segment=1;segment<=256;segment++){
        if(segment>1){port_sd_next_segment(segment);tasks(4);}
        can_frame_t value=frame(segment);CHECK(port_sd_record_frame(&value));tasks(2);
        CHECK(file_port_record_token(2)==token);
    }
    port_sd_close();CHECK(last_result[EV_SD_CLOSE_DONE]==SD_OK&&port_sd_file_count()==128);
    int index=-1;for(int i=0;i<port_sd_file_count();i++)if(file_port_token(i)==token){index=i;break;}
    CHECK(index>=0&&file_port_rename(index,token,"完整256段.log")==FILE_PORT_OK);
    CHECK(f_stat("0:/CANLOG/S0000002_完整256段.log/00000001.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000002_完整256段.log/00000256.LOG",&info)==FR_OK);
    CHECK(f_stat("0:/OLD126.LOG",&info)==FR_OK&&f_stat("0:/OLD127.LOG",&info)==FR_OK);
    storage_sd_stats_t state;storage_sd_stats(&state);CHECK(state.catalog_limited&&state.catalog_omitted>=256);
}

/* Continue the 256-segment fixture above. Its 128-slot cache contains imported
 * root logs and only a representative of each recently recorded session. */
static void reclaim_uncached_segments(void)
{
    const char *session1 = "0:/CANLOG/S0000001_容量满后命名.log";
    const char *session2 = "0:/CANLOG/S0000002_完整256段.log";
    char path[96];
    FILINFO info;
    CHECK(f_chmod(session1, AM_RDO, AM_RDO) == FR_OK);
    snprintf(path, sizeof path, "%s/00000001.LOG", session2);
    CHECK(f_chmod(path, AM_RDO, AM_RDO) == FR_OK);
    const char *header = "# REVD1 nbr=500000 dbr=2000000 fd=1\n(0.000000) can0 123#AA\n";
    snprintf(path, sizeof path, "%s/USER.LOG", session2);
    create_bytes(path, header, strlen(header));
    snprintf(path, sizeof path, "%s/00000000.LOG", session2);
    create_bytes(path, header, strlen(header));
    const char *foreign = "(0.000000) can0 123#AA\n";
    snprintf(path, sizeof path, "%s/00000257.LOG", session2);
    create_bytes(path, foreign, strlen(foreign));

    /* An I/O failure anywhere in discovery must not erase a newer candidate. */
    fail_read = true;
    port_sd_delete_oldest();
    fail_read = false;
    CHECK(last_result[EV_SD_DELETE_DONE] == 0);
    snprintf(path, sizeof path, "%s/00000002.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK);

    /* A controller I/O failure requires the existing explicit remount path. */
    port_sd_unmount();port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE] == SD_OK);

    port_sd_prealloc(3, 1, 64, 500000, 2000000, true);
    CHECK(last_result[EV_SD_PREALLOC_DONE] == SD_OK);
    can_frame_t value = frame(1);
    CHECK(port_sd_record_frame(&value));tasks(2);
    uint32_t token = file_port_record_token(3);
    CHECK(token);
    uint32_t tokens[128];
    CHECK(port_sd_file_count() == 128);
    for (int i=0;i<128;i++) tokens[i] = file_port_token(i);

    unsigned before_pumps = pumps;
    for (unsigned segment=2;segment<=6;segment++) {
        port_sd_delete_oldest();
        CHECK(last_result[EV_SD_DELETE_DONE] == 1);
        snprintf(path, sizeof path, "%s/%08u.LOG", session2, segment);
        CHECK(f_stat(path, &info) == FR_NO_FILE);
        /* Deleting an uncached segment cannot shift cached UI identities. */
        CHECK(port_sd_file_count() == 128 && file_port_record_token(3) == token);
        for (int i=0;i<128;i++) CHECK(file_port_token(i) == tokens[i]);
    }
    CHECK(pumps > before_pumps + 256u);
    snprintf(path, sizeof path, "%s/00000001.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK && (info.fattrib & AM_RDO));
    snprintf(path, sizeof path, "%s/00000256.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK); /* Newest closed segment is preserved. */
    snprintf(path, sizeof path, "%s/00000257.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK); /* Foreign payload with numeric name. */
    snprintf(path, sizeof path, "%s/USER.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK); /* Header alone does not confer ownership. */
    snprintf(path, sizeof path, "%s/00000000.LOG", session2);
    CHECK(f_stat(path, &info) == FR_OK);
    CHECK(f_stat("0:/OLD127.LOG", &info) == FR_OK);
    CHECK(f_stat("0:/CANLOG/S0000003/00000001.LOG", &info) == FR_OK);
    CHECK(f_chmod(session2, AM_RDO, AM_RDO) == FR_OK);
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 0); /* Only protected or active remain. */
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 1 && port_sd_file_count() == 127);
    CHECK(f_stat("0:/CANLOG/S0000003/00000001.LOG", &info) == FR_NO_FILE);
}

static void reclaim_latency_fixture(void)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE] == SD_OK);
    CHECK(f_mkdir("0:/CANLOG/S0000001") == FR_OK);
    const char *header = "# REVD1 nbr=500000 dbr=2000000 fd=1\n(0.000000) can0 123#AA\n";
    for (unsigned i=1;i<=256;i++) {
        char path[64];snprintf(path,sizeof path,"0:/CANLOG/S0000001/%08u.LOG",i);
        create_bytes(path,header,strlen(header));
    }
    unsigned before=events[EV_SD_DELETE_DONE],calls=0,max_step=0;
    uint32_t started=tick;
    request_delete();
    while(events[EV_SD_DELETE_DONE]==before && calls<10000) {
        uint32_t step=tick;storage_sd_task();step=tick-step;
        if(step>max_step)max_step=step;
        calls++;
    }
    uint32_t total = tick-started;
    CHECK(events[EV_SD_DELETE_DONE]==before+1 && last_result[EV_SD_DELETE_DONE]==1);
    CHECK(calls>1 && calls<100 && max_step<128u);
    FILINFO info;
    CHECK(f_stat("0:/CANLOG/S0000001/00000001.LOG",&info)==FR_NO_FILE);
    CHECK(f_stat("0:/CANLOG/S0000001/00000256.LOG",&info)==FR_OK);
    printf("reclaim latency: 256 segments, calls=%u max_step_model_ms=%u total_model_ms=%u\n",calls,max_step,(unsigned)total);

    /* A stop is handled at the next task boundary instead of waiting for the
     * remaining hundreds of directory entries. No candidate is deleted. */
    port_sd_prealloc(2,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    before=events[EV_SD_DELETE_DONE];request_delete();storage_sd_task();
    CHECK(events[EV_SD_DELETE_DONE]==before && port_sd_busy());
    unsigned close_before=events[EV_SD_CLOSE_DONE];
    request_close();calls=0;
    while(events[EV_SD_CLOSE_DONE]==close_before && calls<10) {storage_sd_task();calls++;}
    CHECK(events[EV_SD_CLOSE_DONE]==close_before+1 && last_result[EV_SD_CLOSE_DONE]==SD_OK);
    CHECK(calls<=2 && events[EV_SD_DELETE_DONE]==before+1 && !last_result[EV_SD_DELETE_DONE]);
    CHECK(f_stat("0:/CANLOG/S0000001/00000002.LOG",&info)==FR_OK);
    printf("reclaim cancel: close serviced in %u task call(s)\n",calls);

    /* Playback can start between scan steps. Recheck the final candidate. */
    port_sd_unmount();port_sd_mount();
    int candidate=find_name("S0000001/00000002.LOG");CHECK(candidate>=0);
    before=events[EV_SD_DELETE_DONE];request_delete();storage_sd_task();
    CHECK(events[EV_SD_DELETE_DONE]==before);
    players[0].state=PL_PLAYING;players[0].file=candidate;
    await_event(EV_SD_DELETE_DONE,before);
    CHECK(!last_result[EV_SD_DELETE_DONE]);
    CHECK(f_stat("0:/CANLOG/S0000001/00000002.LOG",&info)==FR_OK);
    players[0].state=PL_IDLE;

    /* Card removal discards the open directory cursors before a new volume
     * can be mounted. The next deletion starts a fresh scan. */
    before=events[EV_SD_DELETE_DONE];request_delete();storage_sd_task();
    CHECK(events[EV_SD_DELETE_DONE]==before);
    unsigned removed=events[EV_SD_REMOVE];inserted=false;storage_sd_task();tick+=31;storage_sd_task();
    CHECK(events[EV_SD_REMOVE]==removed+1 && !port_sd_busy());
    inserted=true;storage_sd_task();tick+=31;storage_sd_task();port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE]==SD_OK);
    CHECK(f_stat("0:/CANLOG/S0000001/00000002.LOG",&info)==FR_OK);
    port_sd_delete_oldest();CHECK(last_result[EV_SD_DELETE_DONE]==1);
    CHECK(f_stat("0:/CANLOG/S0000001/00000002.LOG",&info)==FR_NO_FILE);

    /* Full-card reclamation bypasses queued frame fences, but STOP must still
     * cancel discovery promptly and report an incomplete close honestly. */
    port_sd_prealloc(3,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    FIL filler;
    CHECK(f_open(&filler,"0:/FILLER.BIN",FA_CREATE_NEW|FA_WRITE)==FR_OK);
    CHECK(f_lseek(&filler,UINT32_MAX)==FR_OK && f_tell(&filler)>15000000u);
    CHECK(f_close(&filler)==FR_OK);
    unsigned full_before=events[EV_SD_FULL];
    for(unsigned i=0;i<200;i++){can_frame_t value=frame(i);CHECK(port_sd_record_frame(&value));}
    tasks(64);CHECK(events[EV_SD_FULL]>full_before);
    storage_sd_stats_t scan_before,scan_after;storage_sd_stats(&scan_before);
    port_sd_next_segment(2); /* This fenced ROTATE precedes the urgent DELETE. */
    before=events[EV_SD_DELETE_DONE];request_delete();tasks(3);
    CHECK(events[EV_SD_DELETE_DONE]==before);
    storage_sd_stats(&scan_after);
    CHECK(scan_after.segments_opened==scan_before.segments_opened &&
          scan_after.segments_closed==scan_before.segments_closed);
    CHECK(f_stat("0:/CANLOG/S0000003/00000002.LOG",&info)==FR_NO_FILE);
    close_before=events[EV_SD_CLOSE_DONE];request_close();calls=0;
    while(events[EV_SD_CLOSE_DONE]==close_before && calls<10){storage_sd_task();calls++;}
    CHECK(events[EV_SD_CLOSE_DONE]==close_before+1 && last_result[EV_SD_CLOSE_DONE]==SD_ERR_IO);
    CHECK(calls<=2 && events[EV_SD_DELETE_DONE]==before+1 && !last_result[EV_SD_DELETE_DONE]);
    CHECK(f_stat("0:/CANLOG/S0000001/00000003.LOG",&info)==FR_OK);
    CHECK(f_unlink("0:/FILLER.BIN")==FR_OK);
    printf("reclaim cancel: full-card close serviced in %u task call(s), reported SD_ERR_IO\n",calls);
}

static void removal_cancels_pending_close(void)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    port_sd_prealloc(1,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    CHECK(file_port_record_token(1));
    /* Begin removal debounce, then let STOP become pending before the task
     * which confirms removal. Its entry snapshot still says "closing". */
    inserted=false;storage_sd_task();
    unsigned closed=events[EV_SD_CLOSE_DONE],removed=events[EV_SD_REMOVE];
    request_close();tick+=31;storage_sd_task();
    CHECK(events[EV_SD_REMOVE]==removed+1);
    CHECK(events[EV_SD_CLOSE_DONE]==closed);
    CHECK(!file_port_record_token(1) && port_sd_file_count()==0);
    tasks(4);CHECK(events[EV_SD_CLOSE_DONE]==closed);
    inserted=true;storage_sd_task();tick+=31;storage_sd_task();port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE]==SD_OK);
}

/* A failed close invalidates the optimistic capture metadata. Re-index only
 * frames actually persisted before the fault, including per-channel totals. */
static void failed_close_reindexes_persisted_frames(void)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    port_sd_prealloc(1,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    for(unsigned i=0;i<8;i++){can_frame_t value=frame(i);CHECK(port_sd_record_frame(&value));}
    tick+=1001;tasks(4); /* Persist eight records and their directory size. */
    int index=find_name("S0000001/00000001.LOG");CHECK(index>=0);
    file_info_t info;CHECK(port_sd_file_info(index,&info)&&info.frames==8);
    can_frame_t lost=frame(8);CHECK(port_sd_record_frame(&lost));
    fail_write=true;port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE]==SD_ERR_IO);
    fail_write=false;
    uint32_t count=0;uint64_t duration=0;
    CHECK(port_sd_replay_info(index,0,&count,&duration));
    CHECK(count==4&&duration==3000);
    CHECK(port_sd_replay_info(index,1,&count,&duration));
    CHECK(count==4&&duration==3500);
    CHECK(port_sd_file_info(index,&info)&&info.frames==8&&info.sample_frames==8);
    can_frame_t actual;
    CHECK(port_sd_read_frame(index,0,3,&actual)&&actual.data[0]==6);
    CHECK(!port_sd_read_frame(index,0,4,&actual));
    tasks(2);
}

/* Rotation can run out of a complete preallocation extent before f_write
 * reaches a physical short write. Keep the segment fence while reclaiming. */
static void rotation_full_preserves_fence(bool reclaim_old)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    port_sd_prealloc(1,1,128,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    for(unsigned i=0;i<1200;i++){can_frame_t value=frame(i);CHECK(port_sd_record_frame(&value));if(i%32u==31u)tasks(4);}
    port_sd_close();CHECK(last_result[EV_SD_CLOSE_DONE]==SD_OK);
    int old=find_name("S0000001/00000001.LOG");CHECK(old>=0);
    file_info_t info;CHECK(port_sd_file_info(old,&info)&&info.size_kb>=64);
    port_sd_prealloc(2,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    can_frame_t first=frame(0);CHECK(port_sd_record_frame(&first));
    tick+=1001;tasks(4);
    FIL filler;CHECK(f_open(&filler,"0:/FILLER.BIN",FA_CREATE_NEW|FA_WRITE)==FR_OK);
    CHECK(f_lseek(&filler,UINT32_MAX)==FR_OK&&f_tell(&filler)<UINT32_MAX);
    CHECK(f_close(&filler)==FR_OK);
    unsigned full=events[EV_SD_FULL],errors=events[EV_SD_WRITE_ERR];
    port_sd_next_segment(2);
    for(unsigned i=1;i<=6;i++){can_frame_t value=frame(i);CHECK(port_sd_record_frame(&value));}
    tasks(8);
    CHECK(events[EV_SD_FULL]==full+1&&events[EV_SD_WRITE_ERR]==errors);
    CHECK(find_name("S0000002/00000002.LOG")==-1);
    if(!reclaim_old){
        unsigned deleted=events[EV_SD_DELETE_DONE];
        port_sd_close();CHECK(last_result[EV_SD_CLOSE_DONE]==SD_ERR_IO);
        CHECK(!port_sd_busy()&&events[EV_SD_DELETE_DONE]==deleted);
        int preserved=find_name("S0000002/00000001.LOG");
        CHECK(preserved>=0&&port_sd_file_info(preserved,&info)&&info.frames==1);
        CHECK(find_name("S0000002/00000002.LOG")==-1);
        CHECK(f_unlink("0:/FILLER.BIN")==FR_OK);
        small_record(3); /* Cancelled rotation cannot poison the next session. */
        return;
    }
    port_sd_delete_oldest();CHECK(last_result[EV_SD_DELETE_DONE]==1);
    tasks(8);port_sd_close();CHECK(last_result[EV_SD_CLOSE_DONE]==SD_OK);
    int first_segment=find_name("S0000002/00000001.LOG");
    int second_segment=find_name("S0000002/00000002.LOG");
    CHECK(first_segment>=0&&second_segment>=0);
    CHECK(port_sd_file_info(first_segment,&info)&&info.frames==1);
    CHECK(port_sd_file_info(second_segment,&info)&&info.frames==6);
    can_frame_t actual;CHECK(port_sd_read_frame(second_segment,1,0,&actual)&&actual.data[0]==1);
    CHECK(events[EV_SD_WRITE_ERR]==errors);
    tasks(2);
}

static void queued_protection_follows_file_identity(void)
{
    port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    small_record(1);
    port_sd_prealloc(2,1,64,500000,2000000,true);
    CHECK(last_result[EV_SD_PREALLOC_DONE]==SD_OK);
    can_frame_t value=frame(0);CHECK(port_sd_record_frame(&value));
    port_sd_next_segment(2);tasks(8);
    value=frame(1);CHECK(port_sd_record_frame(&value));
    port_sd_close();CHECK(last_result[EV_SD_CLOSE_DONE]==SD_OK);
    small_record(3);
    int first=find_name("S0000002/00000001.LOG");
    int target=find_name("S0000002/00000002.LOG");
    CHECK(first>=0&&target>=0&&file_port_token(first)==file_port_token(target));
    unsigned deleted=events[EV_SD_DELETE_DONE];
    request_delete();request_protect(target);tasks(100);
    CHECK(events[EV_SD_DELETE_DONE]==deleted+1&&last_result[EV_SD_DELETE_DONE]==1);
    target=find_name("S0000002/00000002.LOG");
    first=find_name("S0000002/00000001.LOG");
    int other=find_name("S0000003/00000001.LOG");file_info_t info;
    CHECK(port_sd_file_info(target,&info)&&info.protected_);
    CHECK(port_sd_file_info(first,&info)&&!info.protected_);
    CHECK(port_sd_file_info(other,&info)&&!info.protected_);
    /* Deletion moves the protected target again; the queued toggle must
     * remove protection from that same segment, not from its neighbor. */
    request_delete();request_protect(target);tasks(100);
    target=find_name("S0000002/00000002.LOG");other=find_name("S0000003/00000001.LOG");
    CHECK(port_sd_file_info(target,&info)&&!info.protected_);
    CHECK(port_sd_file_info(other,&info)&&!info.protected_);
    request_protect(target);request_protect(target);tasks(32);
    CHECK(port_sd_file_info(target,&info)&&!info.protected_);
    /* If the requested file itself was deleted, never reuse its old index. */
    request_delete();request_protect(target);tasks(100);
    CHECK(find_name("S0000002/00000002.LOG")==-1);
    other=find_name("S0000003/00000001.LOG");
    CHECK(port_sd_file_info(other,&info)&&!info.protected_);
}

int main(int argc,char **argv)
{
    cfg_defaults();
    storage_sd_init();
    if(argc==2){
        if(!strcmp(argv[1],"failed-close-index"))failed_close_reindexes_persisted_frames();
        else if(!strcmp(argv[1],"rotation-full"))rotation_full_preserves_fence(true);
        else if(!strcmp(argv[1],"rotation-full-stop"))rotation_full_preserves_fence(false);
        else if(!strcmp(argv[1],"queued-protection"))queued_protection_follows_file_identity();
        else return 2;
        printf("storage_sd %s: %u checks passed\n",argv[1],checks);return 0;
    }
    CHECK(port_sd_present());
    port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE] == SD_ERR_UNSUPPORTED);
    CHECK(writes == 0); /* Mounting blank media never formats automatically. */
    port_sd_format(); /* Explicit test operation; card is a byte array in RAM. */
    CHECK(last_result[EV_SD_FORMAT_DONE] == SD_OK);
    CHECK(port_sd_total_kb() > 15000 && port_sd_free_kb() > 15000);
    CHECK(port_sd_file_count() == 0);
    CHECK(port_sd_next_session() == 1);
    CHECK(!storage_sd_media_give());

    port_sd_prealloc(1, 1, 1024, 500000, 2000000, true);
    CHECK(last_result[EV_SD_PREALLOC_DONE] == SD_OK);
    CHECK(port_sd_file_count() == 1);
    storage_sd_media_reclaim(); /* Ordinary USB detach during local recording. */
    CHECK(port_sd_file_count() == 1 && storage_sd_disk_ready());
    file_info_t info;
    CHECK(port_sd_file_info(0, &info) && info.writing);
    CHECK(!storage_sd_media_give());
    uint32_t count;
    uint64_t duration;
    CHECK(!port_sd_replay_info(0, 0, &count, &duration));
    unsigned before_enqueue_dma = dma_starts;
    for (unsigned i = 0; i < 512; i++) {
        can_frame_t f = frame(i);
        port_sd_record_frame(&f);
    }
    CHECK(dma_starts == before_enqueue_dma); /* Ingestion is pure queue work. */
    storage_sd_stats_t initial_stats;
    storage_sd_stats(&initial_stats);
    CHECK(initial_stats.queued_frames == 512 && initial_stats.queue_high_water == 512);
    tasks(64);
    tick += 1001;
    storage_sd_task();
    FILINFO fatinfo;
    CHECK(f_stat("0:/CANLOG/S0000001/00000001.LOG", &fatinfo) == FR_OK && fatinfo.fsize > 30000);
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);
    CHECK(port_sd_file_info(0, &info) && !info.writing && info.frames == 512 && info.fd);
    CHECK(info.nbr == 500000 && info.dbr == 2000000 && info.channels == 2);
    CHECK(info.sample_available && !info.imported);
    CHECK(port_sd_replay_info(0, 0, &count, &duration) && count == 256 && duration == 255000);
    CHECK(port_sd_replay_info(0, 1, &count, &duration) && count == 256 && duration == 255500);
    for (uint8_t ch = 0; ch < 2; ch++) {
        for (uint32_t i = 0; i < 256; i++) {
            can_frame_t actual, again, expected = frame(i * 2u + ch);
            expected.ts_us -= 1000000u;
            CHECK(port_sd_read_frame(0, ch, i, &actual));
            CHECK(actual.ts_us == expected.ts_us && actual.id == expected.id &&
                  actual.flags == expected.flags && actual.len == expected.len && actual.ch == ch);
            CHECK(!memcmp(actual.data, expected.data, expected.len));
            CHECK(port_sd_read_frame(0, ch, i, &again) && !memcmp(&actual, &again, sizeof actual));
        }
    }
    can_frame_t actual;
    CHECK(port_sd_read_frame(0, 0, 3, &actual) && actual.ts_us == 3000);
    CHECK(!port_sd_read_frame(0, 0, 256, &actual));
    CHECK(!port_sd_read_frame(0, 2, 0, &actual));
    storage_sd_task(); /* Close inactive replay handles. */

    port_sd_protect_toggle(0);
    CHECK(port_sd_file_info(0, &info) && info.protected_);
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 0 && port_sd_file_count() == 1);
    port_sd_unmount();
    CHECK(storage_sd_media_give() && storage_sd_raw_ready());
    CHECK(storage_sd_raw_block_count() == CARD_BLOCKS);
    uint8_t unaligned[514], compare[512] __attribute__((aligned(4)));
    CHECK(storage_sd_raw_read(0, unaligned + 1, 1));
    CHECK(!memcmp(unaligned + 1, ram_card, 512));
    CHECK(storage_sd_raw_write(0, unaligned + 1, 1));
    CHECK(storage_sd_raw_read(0, compare, 1) && !memcmp(compare, unaligned + 1, 512));
    CHECK(!storage_sd_raw_read(CARD_BLOCKS, compare, 1));
    CHECK(!storage_sd_raw_write(1, compare, UINT32_MAX));
    CHECK(disk_read(0, compare, 0, 1) == RES_NOTRDY);
    CHECK(storage_sd_raw_sync());
    uint32_t abort_before = dma_aborts;
    dma_hang = true;
    CHECK(!storage_sd_raw_read(0, compare, 1));
    CHECK(dma_aborts == abort_before + 1 && !dma_pending);
    CHECK(resets > 0 && !irq_enabled && !old_irq_pending && !old_hw_flag);
    CHECK(!storage_sd_raw_ready());
    dma_hang = false;
    uint32_t init_before = card_inits;
    storage_sd_media_reclaim();
    CHECK(storage_sd_media_give()); /* Reinitialize after controller quiesce. */
    CHECK(card_inits == init_before + 1 && irq_enabled);
    CHECK(storage_sd_raw_read(0, compare, 1));
    CHECK(late_callbacks == 0); /* Old completion never finishes this new read. */
    storage_sd_stats_t dma_stats;
    storage_sd_stats(&dma_stats);
    CHECK(dma_stats.dma_timeouts == 1 && dma_stats.dma_errors >= 1);
    CHECK(dma_stats.dma_transfers == dma_starts && pumps >= dma_starts * 3u);
    port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE] == SD_ERR_IO);
    storage_sd_media_reclaim();
    CHECK(!storage_sd_raw_ready() && !storage_sd_raw_write(0, compare, 1));
    port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE] == SD_OK);
    CHECK(port_sd_file_info(0, &info) && info.protected_);
    CHECK(port_sd_next_session() == 2);

    small_record(2);
    int second = find_name("S0000002");
    CHECK(second >= 0);
    players[0].state = PL_PLAYING;
    players[0].file = second;
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 0);
    port_sd_unmount();
    CHECK(!storage_sd_media_give());
    players[0].state = PL_IDLE;
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 1 && removed_index == second);
    CHECK(port_sd_file_count() == 1);

    /* Foreign logs in device-named directories must survive ring deletion. */
    FIL foreign;
    UINT written;
    const char *foreign_log = "(4.000000) can0 123#AABB\n(4.000500) can1 124#CC\n";
    CHECK(f_open(&foreign, "0:/CANLOG/S0000001/USER.LOG", FA_CREATE_NEW | FA_WRITE) == FR_OK);
    CHECK(f_write(&foreign, foreign_log, (UINT)strlen(foreign_log), &written) == FR_OK && written == strlen(foreign_log));
    CHECK(f_close(&foreign) == FR_OK);
    port_sd_unmount();
    port_sd_mount();
    int imported = find_name("USER.LOG");
    CHECK(imported >= 0 && port_sd_file_info(imported, &info) && info.imported);
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 0 && port_sd_file_count() == 2);

    small_record(3);
    /* Fill only the RAM filesystem, then reclaim a real old segment. This
     * exercises FatFs short-write handling rather than a mocked FULL event. */
    port_sd_prealloc(4, 1, 64, 500000, 2000000, true);
    CHECK(last_result[EV_SD_PREALLOC_DONE] == SD_OK);
    FIL filler;
    CHECK(f_open(&filler, "0:/FILLER.BIN", FA_CREATE_NEW | FA_WRITE) == FR_OK);
    CHECK(f_lseek(&filler, UINT32_MAX) == FR_OK);
    CHECK(f_tell(&filler) < UINT32_MAX && f_tell(&filler) > 15000000u);
    CHECK(f_close(&filler) == FR_OK);
    unsigned full_before = events[EV_SD_FULL];
    for (unsigned i = 0; i < 200; i++) {
        can_frame_t f = frame(i);
        port_sd_record_frame(&f);
    }
    tasks(64);
    CHECK(events[EV_SD_FULL] > full_before);
    port_sd_delete_oldest();
    CHECK(last_result[EV_SD_DELETE_DONE] == 1);
    /* The tiny old segment alone may not cover the pending 4 KiB write. */
    CHECK(f_unlink("0:/FILLER.BIN") == FR_OK);
    port_sd_reinit();
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);

    unsigned errors_before = events[EV_SD_WRITE_ERR];
    port_sd_prealloc(5, 1, 64, 500000, 2000000, true);
    CHECK(last_result[EV_SD_PREALLOC_DONE] == SD_OK);
    fail_write = true;
    for (unsigned i = 0; i < 100; i++) {
        can_frame_t f = frame(i);
        port_sd_record_frame(&f);
    }
    tasks(64);
    CHECK(events[EV_SD_WRITE_ERR] > errors_before);
    unsigned first_errors = events[EV_SD_WRITE_ERR];
    tick += 201;
    storage_sd_task();
    CHECK(events[EV_SD_WRITE_ERR] > first_errors);
    port_sd_reinit();
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_ERR_IO);
    fail_write = false;

    /* Commands are deferred, preserve segment fences, and close acknowledges
     * only after every accepted preceding frame has reached the file. */
    port_sd_prealloc(6, 1, 64, 500000, 2000000, true);
    for (unsigned i = 0; i < 20; i++) { can_frame_t f = frame(i); CHECK(port_sd_record_frame(&f)); }
    uint32_t before_rotate_dma = dma_starts;
    port_sd_next_segment(2);
    CHECK(dma_starts == before_rotate_dma);
    for (unsigned i = 20; i < 40; i++) { can_frame_t f = frame(i); CHECK(port_sd_record_frame(&f)); }
    unsigned close_before = events[EV_SD_CLOSE_DONE];
    request_close();
    CHECK(events[EV_SD_CLOSE_DONE] == close_before && dma_starts == before_rotate_dma);
    tasks(64);
    CHECK(events[EV_SD_CLOSE_DONE] == close_before + 1 && last_result[EV_SD_CLOSE_DONE] == SD_OK);
    int segment_a = find_name("S0000006/00000001.LOG");
    int segment_b = find_name("S0000006/00000002.LOG");
    CHECK(segment_a >= 0 && segment_b >= 0);
    CHECK(port_sd_file_info(segment_a, &info) && info.frames == 20);
    CHECK(port_sd_file_info(segment_b, &info) && info.frames == 20);
    CHECK(port_sd_read_frame(segment_b, 0, 0, &actual));
    CHECK(actual.ts_us == 0 && actual.data[0] == 20);
    tasks(2);

    /* Overflow counts every rejected frame, never writes or posts from the
     * capture path; the foreground task publishes one aggregate fault. */
    port_sd_prealloc(7, 1, 64, 500000, 2000000, true);
    storage_sd_stats_t before_overflow, after_overflow;
    storage_sd_stats(&before_overflow);
    unsigned overflow_errors = events[EV_SD_WRITE_ERR];
    before_enqueue_dma = dma_starts;
    for (unsigned i = 0; i < REVD_SD_RECORD_QUEUE_FRAMES + 5u; i++) {
        can_frame_t f = frame(i);
        CHECK(port_sd_record_frame(&f) == (i < REVD_SD_RECORD_QUEUE_FRAMES));
    }
    CHECK(dma_starts == before_enqueue_dma && events[EV_SD_WRITE_ERR] == overflow_errors);
    storage_sd_stats(&after_overflow);
    CHECK(after_overflow.queue_overflows == before_overflow.queue_overflows + 5);
    tasks(64);
    CHECK(events[EV_SD_WRITE_ERR] == overflow_errors + 1);
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);

    /* Simulate CAN arriving from root's capture pump while FatFs is waiting
     * for IDMA completion. Nested storage tasks are intentionally ignored. */
    port_sd_prealloc(8, 1, 64, 500000, 2000000, true);
    for (unsigned i = 0; i < 100; i++) { can_frame_t f = frame(i); CHECK(port_sd_record_frame(&f)); }
    injected = 0; injection_limit = 100; injection_ok = true;
    capture_during_wait = inject_capture;
    for (unsigned i = 0; i < 100000 && injected != injection_limit; i++) storage_sd_task();
    capture_during_wait = NULL;
    CHECK(injected == 100 && injection_ok);
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);
    int interleaved = find_name("S0000008/00000001.LOG");
    CHECK(interleaved >= 0 && port_sd_file_info(interleaved, &info) && info.frames == 200);
    CHECK(port_sd_read_frame(interleaved, 1, 99, &actual) && actual.data[0] == 199);
    tasks(2);

    /* Capture keeps accepting during close/f_open/f_expand/header writes in
     * rotation, even while there is temporarily no open recording FIL. */
    port_sd_prealloc(9, 1, 64, 500000, 2000000, true);
    for (unsigned i = 0; i < 20; i++) { can_frame_t f = frame(i); CHECK(port_sd_record_frame(&f)); }
    port_sd_next_segment(2);
    for (unsigned i = 20; i < 40; i++) { can_frame_t f = frame(i); CHECK(port_sd_record_frame(&f)); }
    injected = 0; injection_limit = 100; injection_ok = true;
    capture_during_wait = inject_capture;
    tasks(1); /* Executes the rotate command and all synchronous FatFs waits. */
    capture_during_wait = NULL;
    CHECK(injected > 0 && injection_ok);
    port_sd_close();
    CHECK(last_result[EV_SD_CLOSE_DONE] == SD_OK);
    int rotate_old = find_name("S0000009/00000001.LOG");
    int rotate_new = find_name("S0000009/00000002.LOG");
    CHECK(rotate_old >= 0 && rotate_new >= 0);
    CHECK(port_sd_file_info(rotate_old, &info) && info.frames == 20);
    CHECK(port_sd_file_info(rotate_new, &info) && info.frames == 20 + injected);
    CHECK(port_sd_read_frame(rotate_new, 0, 0, &actual) && actual.data[0] == 20);
    tasks(2);

    inserted = false;
    storage_sd_task();
    tick += 31;
    storage_sd_task();
    CHECK(events[EV_SD_REMOVE] == 1 && !storage_sd_raw_ready() && port_sd_file_count() == 0);
    inserted = true;
    storage_sd_task();
    tick += 31;
    storage_sd_task();
    CHECK(events[EV_SD_INSERT] == 1);
    port_sd_mount();
    CHECK(last_result[EV_SD_MOUNT_DONE] == SD_OK);
    CHECK(find_name("S0000001/00000001.LOG") >= 0);
    CHECK(port_sd_next_session() == 10);
    fail_write = true;
    port_sd_format();
    CHECK(last_result[EV_SD_FORMAT_DONE] != SD_OK);
    CHECK(!storage_sd_raw_ready() && port_sd_file_count() == 0);
    fail_write=false;port_sd_format();CHECK(last_result[EV_SD_FORMAT_DONE]==SD_OK);
    device_file_operations();
#ifdef CANFD_WINSIM_HW
    keysim_files();
#endif
    full_catalog_record_name();
    reclaim_uncached_segments();
    reclaim_latency_fixture();
    removal_cancels_pending_close();
    failed_close_reindexes_persisted_frames();
    rotation_full_preserves_fence(true);
    rotation_full_preserves_fence(false);
    queued_protection_follows_file_identity();
    printf("storage_sd%s: %u checks passed (production FatFs on 16 MiB RAM card)\n",
#ifdef CANFD_WINSIM_HW
           " + APM key tables",
#else
           "",
#endif
           checks);
    return 0;
}
