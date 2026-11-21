/* SPDX-License-Identifier: MIT
 * Included by storage_sd.c: the same storage owner owns every FatFs call.
 * UI APIs below only validate cached state and copy bounded command data.
 */
#define PLAN_DIR "0:/SEND"
#define PREFIX_FILE SD_DIR "/RECNAME.CFG"
#define PREFIX_BACKUP SD_DIR "/RECNAME.BAK"
#ifdef CANFD_REVD_TARGET
#define FILE_CPU_WORK __attribute__((section(".revd_dtcm_bss"),aligned(8)))
#else
#define FILE_CPU_WORK
#endif
static union {sp_file_t catalog[SP_MAX_FILES];char json[SP_JSON_OUTPUT_BYTES];} file_work FILE_CPU_WORK;
static sp_io_request_t file_request FILE_CPU_WORK;
static char file_diagnostic[192];
static char identity_directory[SD_PATH_SIZE];
static uint32_t identity_generation,identity_token;

static bool metadata_idle(void)
{
    return mounted&&!msc_owner&&initialized&&detect_stable&&port_sd_present()&&
        !recording_open&&!close_pending&&frame_in==frame_out&&!player_any_active();
}
static bool complete_name(const char *name)
{return suffix_is(name,".LOG")||suffix_is(name,".TXT");}
uint32_t file_port_token(int index)
{return mounted&&!msc_owner&&index>=0&&(unsigned)index<nfiles?files[index].token:0;}
uint32_t file_port_record_token(uint32_t session)
{
    if(!mounted||msc_owner||!session||session!=recording_session||recording_media_generation!=media_generation)return 0;
    for(unsigned i=0;i<nfiles;i++)if(files[i].session==session&&files[i].token==recording_token)return recording_token;
    return 0;
}
const char *file_port_rec_prefix_get(void){return record_prefix;}
int file_port_rec_prefix_set(const char *name)
{
    int result=sp_json_safe_name(name,FILE_PORT_REC_PREFIX_MAX_BYTES,1);
    if(result)return result;
    if(!producer_context()||task_active||io_busy||!metadata_idle()||command_in!=command_out)return FILE_PORT_ERR_BUSY;
    task_active=true;result=prefix_now(name);task_active=false;return result;
}
int file_port_rename(int index,uint32_t expected_token,const char *name)
{
    if(!expected_token||file_port_token(index)!=expected_token)return FILE_PORT_ERR_CHANGED;
    int result=sp_json_safe_name(name,FILE_PORT_NAME_MAX_BYTES,0);if(result)return result;
    if(!complete_name(name))return FILE_PORT_ERR_EXTENSION;
    if(!metadata_idle()||command_in!=command_out)return FILE_PORT_ERR_BUSY;
    if(files[index].info.protected_)return FILE_PORT_ERR_READONLY;
    if(!producer_context()||task_active||io_busy)return FILE_PORT_ERR_BUSY;
    task_active=true;result=rename_now(expected_token,name);task_active=false;return result;
}
static FRESULT read_prefix(const char *path,char value[19])
{
    memset(value,0,19);
    FIL file;FRESULT result=f_open(&file,path,FA_READ);if(result!=FR_OK)return result;
    if(f_size(&file)>18){(void)f_close(&file);return FR_INVALID_NAME;}
    UINT count=0;result=f_read(&file,value,18,&count);value[count]=0;
    FRESULT closed=f_close(&file);if(result==FR_OK)result=closed;
    if(result==FR_OK&&(strlen(value)!=count||sp_json_safe_name(value,18,1)))result=FR_INVALID_NAME;
    return result;
}
static FRESULT write_new(const char *path,const void *bytes,UINT length)
{
    FIL file;FRESULT result=f_open(&file,path,FA_CREATE_NEW|FA_WRITE);if(result!=FR_OK)return result;
    UINT written=0;result=f_write(&file,bytes,length,&written);
    if(result==FR_OK&&written!=length)result=FR_DISK_ERR;
    if(result==FR_OK)result=f_sync(&file);
    FRESULT closed=f_close(&file);if(result==FR_OK)result=closed;
    if(result!=FR_OK)(void)f_unlink(path); /* Only this just-created temporary. */
    return result;
}
static bool temporary_path(char out[SD_PATH_SIZE],const char *directory)
{
    for(unsigned attempt=0;attempt<32;attempt++){
        unsigned long serial=(unsigned long)(HAL_GetTick()+attempt+file_serial);
        snprintf(out,SD_PATH_SIZE,"%s/T%07lX.TMP",directory,serial&0xFFFFFFFul);
        FILINFO information;FRESULT exists=f_stat(out,&information);
        if(exists==FR_NO_FILE)return true;
        if(exists!=FR_OK)return false;
    }
    return false;
}
static int prefix_now(const char *name)
{
    if(!metadata_idle())return FILE_PORT_ERR_BUSY;
    FILINFO information;FRESULT exists=f_stat(PREFIX_FILE,&information);
    if(exists==FR_OK&&(information.fattrib&AM_RDO))return FILE_PORT_ERR_READONLY;
    if(exists!=FR_OK&&exists!=FR_NO_FILE)return FILE_PORT_ERR_IO;
    char temporary[SD_PATH_SIZE];if(!temporary_path(temporary,SD_DIR))return FILE_PORT_ERR_IO;
    FRESULT result=write_new(temporary,name,(UINT)strlen(name));
    bool backup=false;
    if(result==FR_OK&&exists==FR_OK){result=f_rename(PREFIX_FILE,PREFIX_BACKUP);backup=result==FR_OK;}
    if(result==FR_OK)result=f_rename(temporary,PREFIX_FILE);
    if(result!=FR_OK){
        if(backup)(void)f_rename(PREFIX_BACKUP,PREFIX_FILE);
        (void)f_unlink(temporary);
        return FILE_PORT_ERR_IO;
    }
    if(backup)(void)f_unlink(PREFIX_BACKUP);
    memcpy(record_prefix,name,strlen(name)+1u);return FILE_PORT_OK;
}
static uint32_t session_identity(const char *directory,uint32_t session)
{
    if(recording_token&&recording_session==session&&recording_media_generation==media_generation&&!strcmp(directory,recording_directory))return recording_token;
    if(identity_generation==media_generation&&!strcmp(identity_directory,directory))return identity_token;
    snprintf(identity_directory,sizeof identity_directory,"%s",directory);identity_generation=media_generation;identity_token=0;
    char path[SD_PATH_SIZE];if(!join_path(path,sizeof path,directory,"SESSION.ID"))return 0;
    FIL marker;FRESULT result=f_open(&marker,path,FA_READ);if(result!=FR_OK)return 0;
    char text[48];UINT size=0;result=f_read(&marker,text,sizeof text-1u,&size);text[size]=0;
    (void)f_close(&marker);unsigned long number=0;
    if(result==FR_OK&&sscanf(text,"REVDSESSION1 %lu",&number)==1&&number==session){
        identity_token=++file_serial;if(!identity_token)identity_token=++file_serial;
    }
    return identity_token;
}
static FRESULT ensure_session(const char *directory,uint32_t session)
{
    FRESULT result=f_mkdir(directory);if(result!=FR_OK&&result!=FR_EXIST)return result;
    char path[SD_PATH_SIZE];if(!join_path(path,sizeof path,directory,"SESSION.ID"))return FR_INVALID_NAME;
    FILINFO info;result=f_stat(path,&info);
    if(result==FR_OK)return FR_OK;
    if(result!=FR_NO_FILE)return result;
    char marker[48];int size=snprintf(marker,sizeof marker,"REVDSESSION1 %lu\n",(unsigned long)session);
    if(size<0||(unsigned)size>=sizeof marker)return FR_INVALID_NAME;
    return write_new(path,marker,(UINT)size);
}
static void bind_legacy_session(sd_file_t *entry)
{
    if(entry->session||strncmp(entry->path,SD_DIR "/",sizeof(SD_DIR)))return;
    const char *component=entry->path+sizeof(SD_DIR),*slash=strchr(component,'/');
    if(!slash||(size_t)(slash-component)>=64)return;
    char directory[64];size_t bytes=(size_t)(slash-component);memcpy(directory,component,bytes);directory[bytes]=0;
    uint32_t session;if(!session_number(directory,&session))return;
    size_t prefix=(size_t)(slash-entry->path);
    for(unsigned i=0;i<nfiles;i++)if(files[i].session==session&&!strncmp(files[i].path,entry->path,prefix)&&files[i].path[prefix]=='/'){entry->token=files[i].token;break;}
    entry->session=session;entry->info.is_session=true;
    if(directory[8]=='_'&&bytes-9u<sizeof entry->info.name)memcpy(entry->info.name,directory+9,bytes-9u+1u);
}
static int rename_now(uint32_t token,const char *name)
{
    if(!metadata_idle())return FILE_PORT_ERR_BUSY;
    unsigned index=0;while(index<nfiles&&files[index].token!=token)index++;
    if(index==nfiles)return FILE_PORT_ERR_CHANGED;
    sd_file_t *entry=&files[index];char source[SD_PATH_SIZE],destination[SD_PATH_SIZE];
    if(!entry->session&&entry->info.imported){
        text_reader_t reader={0};char header[LOGFMT_MAX_LINE+1];size_t length;
        if(reader_open(&reader,entry->path)==FR_OK&&reader_line(&reader,header,&length)==1&&!strncmp(header,"# REVD1 ",8))bind_legacy_session(entry);
        reader_close(&reader);
    }
    token=entry->token;
    uint32_t session=entry->session;
    snprintf(source,sizeof source,"%s",entry->path);
    if(session){
        char *slash=strrchr(source,'/');if(!slash)return FILE_PORT_ERR_NAME;*slash=0;
        int bytes=snprintf(destination,sizeof destination,SD_DIR "/S%07lu_%s",(unsigned long)session,name);
        if(bytes<0||(unsigned)bytes>=sizeof destination)return FILE_PORT_ERR_LIMIT;
        DIR dir;FILINFO info;FRESULT result=f_opendir(&dir,source);
        if(result!=FR_OK)return FILE_PORT_ERR_IO;
        for(;;){result=f_readdir(&dir,&info);if(result!=FR_OK||!info.fname[0])break;if(info.fattrib&AM_RDO){(void)f_closedir(&dir);return FILE_PORT_ERR_READONLY;}}
        FRESULT closed=f_closedir(&dir);if(result==FR_OK)result=closed;
        if(result!=FR_OK)return FILE_PORT_ERR_IO;
    }else{
        const char *last=strrchr(source,'/');size_t prefix=last?(size_t)(last-source):2u;
        if(prefix+1u+strlen(name)>=sizeof destination)return FILE_PORT_ERR_LIMIT;
        memcpy(destination,source,prefix);destination[prefix]='/';strcpy(destination+prefix+1u,name);
    }
    if(!strcmp(source,destination))return FILE_PORT_OK;
    size_t prefix=strlen(source);
    if(session)for(unsigned i=0;i<nfiles;i++){
        if(!strncmp(files[i].path,source,prefix)&&files[i].path[prefix]=='/'&&
           strlen(destination)+strlen(files[i].path+prefix)>=SD_PATH_SIZE)return FILE_PORT_ERR_LIMIT;
    }
    FILINFO info;FRESULT exists=f_stat(destination,&info);
    if(exists!=FR_NO_FILE)return exists==FR_OK?FILE_PORT_ERR_EXISTS:FILE_PORT_ERR_IO;
    replay_close_all();FRESULT result=f_rename(source,destination);
    if(result!=FR_OK)return FILE_PORT_ERR_IO;
    for(unsigned i=0;i<nfiles;i++){
        if((session&&!strncmp(files[i].path,source,prefix)&&files[i].path[prefix]=='/')||(!session&&i==index)){
            char path[SD_PATH_SIZE];
            if(session){size_t head=strlen(destination),tail=strlen(files[i].path+prefix);memcpy(path,destination,head);memcpy(path+head,files[i].path+prefix,tail+1u);}
            else snprintf(path,sizeof path,"%s",destination);
            memcpy(files[i].path,path,strlen(path)+1u);snprintf(files[i].info.name,sizeof files[i].info.name,"%s",name);
            if(session){files[i].token=token;files[i].session=session;files[i].info.is_session=true;}
        }
    }
    if(recording_token==token)snprintf(recording_directory,sizeof recording_directory,"%s",destination);
    identity_directory[0]=0;return FILE_PORT_OK;
}
static int fat_json_read(void *context,uint8_t *buffer,size_t capacity)
{
    UINT count=0;FRESULT result=f_read((FIL *)context,buffer,(UINT)capacity,&count);
    return result==FR_OK?(int)count:-1;
}
static int catalog_from_sd(void)
{
    FRESULT result=f_mkdir(PLAN_DIR);if(result!=FR_OK&&result!=FR_EXIST)return SP_ERR_IO;
    DIR directory;FILINFO info;result=f_opendir(&directory,PLAN_DIR);if(result!=FR_OK)return SP_ERR_IO;
    unsigned count=0;int status=SP_OK;
    for(;;){
        result=f_readdir(&directory,&info);if(result!=FR_OK){status=SP_ERR_IO;break;}if(!info.fname[0])break;
        if(info.fattrib&(AM_DIR|AM_HID|AM_SYS)||sp_json_filename(info.fname)!=SP_OK)continue;
        if(count==SP_MAX_FILES){snprintf(file_diagnostic,sizeof file_diagnostic,"/SEND: more than %u plans; old catalog retained",SP_MAX_FILES);status=SP_ERR_FILE;break;}
        char path[SD_PATH_SIZE];if(!join_path(path,sizeof path,PLAN_DIR,info.fname)){status=SP_ERR_NAME;break;}
        FIL input;result=f_open(&input,path,FA_READ);if(result!=FR_OK){status=SP_ERR_IO;break;}
        sp_file_t *file=&file_work.catalog[count];memset(file,0,sizeof *file);
        char error[96];status=sp_json_decode(fat_json_read,&input,&file->plan,file->project,error);
        if(status)snprintf(file_diagnostic,sizeof file_diagnostic,"/SEND/%.47s: %.95s",info.fname,error);
        FRESULT closed=f_close(&input);if(!status&&closed!=FR_OK)status=SP_ERR_IO;
        if(status)break;
        memcpy(file->name,info.fname,strlen(info.fname)+1u);count++;
    }
    FRESULT closed=f_closedir(&directory);if(!status&&closed!=FR_OK)status=SP_ERR_IO;
    if(!status)status=sp_catalog_replace(file_work.catalog,count);
    return status;
}
static int save_plan_to_sd(const sp_io_request_t *request)
{
    if(sp_json_filename(request->name)!=SP_OK||sp_validate(&request->plan)!=SP_OK)return SP_ERR_FORMAT;
    char path[SD_PATH_SIZE];if(!join_path(path,sizeof path,PLAN_DIR,request->name))return SP_ERR_NAME;
    FILINFO info;FRESULT result=f_stat(path,&info);if(result==FR_OK){snprintf(file_diagnostic,sizeof file_diagnostic,"/SEND/%s already exists; choose a new name",request->name);return SP_ERR_NAME;}if(result!=FR_NO_FILE&&result!=FR_NO_PATH)return SP_ERR_IO;
    result=f_mkdir(PLAN_DIR);if(result!=FR_OK&&result!=FR_EXIST)return SP_ERR_IO;
    char project[48]={0};sp_file_t previous;
    for(unsigned i=0;i<sp_catalog_count();i++)if(sp_catalog_get(i,&previous)&&!strcmp(previous.name,sp_state()->loaded_name)){snprintf(project,sizeof project,"%s",previous.project);break;}
    size_t length=0;int status=sp_json_encode(&request->plan,project,file_work.json,sizeof file_work.json,&length);if(status)return status;
    char temporary[SD_PATH_SIZE];if(!temporary_path(temporary,PLAN_DIR))return SP_ERR_IO;
    result=write_new(temporary,file_work.json,(UINT)length);if(result!=FR_OK){snprintf(file_diagnostic,sizeof file_diagnostic,"/SEND/%.47s: FatFs write/sync/close error %u",request->name,(unsigned)result);return SP_ERR_IO;}
    result=f_rename(temporary,path);if(result!=FR_OK){(void)f_unlink(temporary);snprintf(file_diagnostic,sizeof file_diagnostic,"/SEND/%.47s: FatFs rename error %u",request->name,(unsigned)result);return result==FR_EXIST?SP_ERR_NAME:SP_ERR_IO;}
    return SP_OK;
}
#ifdef CANFD_WINSIM_HW
/* APM 按键模拟的键值表：/APMKEY/K1.TXT … K4.TXT（格式见 services/keysim/keysim_file.h）。
 * 只在有第 12 页硬件的目标上编译；当前 Rev D/E 目标不包含这段代码。 */
#include "services/keysim/keysim_file.h"
#define KEYS_DIR "0:/" KS_FILE_DIR
static ks_io_request_t keys_request FILE_CPU_WORK;
static ks_table_t keys_table FILE_CPU_WORK;
static bool keys_paths(uint8_t ch,char path[SD_PATH_SIZE],char backup[SD_PATH_SIZE])
{
    char name[8];
    if(ks_file_name(ch,name,sizeof name)!=KS_FILE_OK||!join_path(path,SD_PATH_SIZE,KEYS_DIR,name))return false;
    snprintf(backup,SD_PATH_SIZE,"%s/K%u.BAK",KEYS_DIR,(unsigned)ch+1u);return true;
}
static int keys_save(const ks_io_request_t *request)
{
    size_t length=0;char path[SD_PATH_SIZE],backup[SD_PATH_SIZE],temporary[SD_PATH_SIZE];
    int status=ks_table_encode(&request->table,request->ch,file_work.json,sizeof file_work.json,&length);if(status)return status;
    if(!keys_paths(request->ch,path,backup))return KS_IO_ERR_IO;
    FRESULT result=f_mkdir(KEYS_DIR);if(result!=FR_OK&&result!=FR_EXIST)return KS_IO_ERR_IO;
    FILINFO info;FRESULT exists=f_stat(path,&info);
    if(exists==FR_OK&&(info.fattrib&AM_RDO))return KS_IO_ERR_IO;
    if(exists!=FR_OK&&exists!=FR_NO_FILE)return KS_IO_ERR_IO;
    if(!temporary_path(temporary,KEYS_DIR))return KS_IO_ERR_IO;
    result=write_new(temporary,file_work.json,(UINT)length);if(result!=FR_OK)return KS_IO_ERR_IO;
    bool backed=false;(void)f_unlink(backup);
    if(exists==FR_OK){result=f_rename(path,backup);backed=result==FR_OK;}
    if(result==FR_OK)result=f_rename(temporary,path);
    if(result!=FR_OK){if(backed)(void)f_rename(backup,path);(void)f_unlink(temporary);return KS_IO_ERR_IO;}
    if(backed)(void)f_unlink(backup);
    return 0;
}
static int keys_load(void)
{
    for(uint8_t ch=0;ch<WINSIM_NUM_CH;ch++){
        char path[SD_PATH_SIZE],backup[SD_PATH_SIZE];if(!keys_paths(ch,path,backup))return KS_IO_ERR_IO;
        FILINFO info;
        /* Power lost between the two renames: only the backup is left. */
        if(f_stat(path,&info)==FR_NO_FILE&&f_stat(backup,&info)==FR_OK)(void)f_rename(backup,path);
        FIL file;FRESULT result=f_open(&file,path,FA_READ);
        if(result==FR_NO_FILE||result==FR_NO_PATH)continue;
        if(result!=FR_OK)return KS_IO_ERR_IO;
        bool fits=f_size(&file)<=KS_FILE_MAX_BYTES;UINT count=0;
        if(fits)result=f_read(&file,file_work.json,KS_FILE_MAX_BYTES,&count);
        FRESULT closed=f_close(&file);if(result!=FR_OK||closed!=FR_OK)return KS_IO_ERR_IO;
        if(fits&&ks_table_decode(file_work.json,count,&keys_table)==KS_FILE_OK)keysim_table_loaded(ch,&keys_table);
        /* An unreadable or oversized file is ignored; the channel keeps its RAM table. */
    }
    return 0;
}
#endif
static void files_on_mount(void)
{
    record_prefix[0]=0;
    char backup[19],current[19];FRESULT old=read_prefix(PREFIX_BACKUP,backup),live=read_prefix(PREFIX_FILE,current);
    if(live==FR_NO_FILE&&old==FR_OK){if(f_rename(PREFIX_BACKUP,PREFIX_FILE)==FR_OK){memcpy(current,backup,19);live=FR_OK;}}
    else if(live==FR_OK&&old==FR_OK)(void)f_unlink(PREFIX_BACKUP);
    if(live==FR_OK)memcpy(record_prefix,current,19);
    plans_refresh_pending=true;
}
static void files_on_unmount(void)
{
#ifndef REVD_FREERTOS
    sp_io_request_t request;
    if(sp_io_peek(&request))(void)sp_io_complete(request.id,SP_ERR_CHANGED,"SD media changed; request cancelled");
    (void)sp_catalog_replace(NULL,0);
#endif
    plans_refresh_pending=false;
    record_prefix[0]=0;
    identity_directory[0]=0;
}
static void files_task(void)
{
#ifndef REVD_FREERTOS
    bool requested=sp_io_peek(&file_request);
#ifdef CANFD_WINSIM_HW
    bool keys=keysim_io_peek(&keys_request);
#else
    bool keys=false;
#endif
    if(!requested&&!plans_refresh_pending&&!keys)return;
    if(!metadata_idle()||command_in!=command_out){
        if(!recording_open&&!close_pending&&!mounted){
            if(requested)(void)sp_io_complete(file_request.id,SP_ERR_BUSY,"SD unavailable or shared");
#ifdef CANFD_WINSIM_HW
            if(keys)keysim_io_complete(keys_request.id,KS_IO_ERR_UNAVAILABLE);
#endif
        }
        return;
    }
    if(requested||plans_refresh_pending){
        int result=0;
        file_diagnostic[0]=0;
        if(requested&&file_request.kind==SP_IO_SAVE)result=save_plan_to_sd(&file_request);
        if(!result)result=catalog_from_sd();
        if(!result)plans_refresh_pending=false;
        if(requested)(void)sp_io_complete(file_request.id,result,result?(file_diagnostic[0]?file_diagnostic:"/SEND read/write failed; old catalog retained"):"SD send plans updated in /SEND");
        else if(result){plans_refresh_pending=false;post_message(MSG_SD_ERROR,result);}
    }
#ifdef CANFD_WINSIM_HW
    if(keys)keysim_io_complete(keys_request.id,keys_request.kind==KS_IO_SAVE?keys_save(&keys_request):keys_load());
#endif
#endif
}
