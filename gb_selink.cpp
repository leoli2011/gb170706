#include "InfoCtrlClient.h"
#include "mxml.h"
#include "gb_p.h"
#include "gps_client.h"
#include <cutils/properties.h>

#define RECONN_TIME 3
#define CAN_MSG_LEN 2048
#define WRITE_FILE_PERIOD  (30 * 1000)  //unit: 30 * 1000 ms

#define SEVEN_DAYS_FILE                 "/data/gb_7day.file"
#define THREE_LEVEL_FAULT_FILE          "/data/gb_three_level_fault.file"
#define COMM_FAILED_FILE                "/data/gb_comm_failed.file"

using namespace android;
#define LOGD(fmt, args...) __android_log_print(ANDROID_LOG_DEBUG, "gb_selink", fmt, ##args)
//#define LOGD(fmt, args...) printf(fmt, ##args)
pthread_mutex_t can_msg_mutex = PTHREAD_MUTEX_INITIALIZER;
const int can_msg_type_num = 4;
unsigned char period_can_msg[can_msg_type_num][CAN_MSG_LEN] = {{0}, {0}};
volatile int can_msg_switch = 0;
int doExit = 0;

typedef struct {
    int head;
    int tail;
    const int max_unit;
    char **data;
} ringbuf_t;

typedef struct battery_info_s {
    char len;
    char buf[20];
} battery_info;
battery_info  g_batt_info;

typedef struct gps_info_s {
    char status;
    char longtitude[4];
    char altitude[4];
} gps_info_t;
gps_info_t  g_gps_info;

ringbuf_t pub_ring = {0, 0, 513, NULL};
ringbuf_t save_ring = {0, 0, 129, NULL};
ringbuf_t commf_ring = {0, 0, 129, NULL};
ringbuf_t three_level_ring = {0, 0, 31, NULL};
pthread_mutex_t publish_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t ringbuf_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t commf_ring_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t three_level_fault_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t comm_failed_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_mutex_t gps_info_mutex = PTHREAD_MUTEX_INITIALIZER;

uint16_t login_seq_num;
gb_server_info_t ser_info;
gb_rinfo_t rinfo;

static int g_three_level_fault_issued = 0;
static int g_comm_failed_issued = 0;
static int g_reconnect_status = 0;
static struct sig_hash *sig_htable;

FILE* open_write_file(char *fname, const char *mode);
int parse_battery_info(char *buf, battery_info* bi);
int gb_login();

class Listener: public ClientListener
{
public :
   Listener(sp<InfoCtrlClient> &client):ClientListener(client) {};
   ~Listener(){};
   virtual void postData(int msgType, String8 &content);
   virtual void postInt(int msgType, int value);
};


void Listener::postData(int msgType, String8 &content)
{
    pthread_mutex_lock(&can_msg_mutex);
    const char *can_msg_buf = content.string();
    if(can_msg_buf == NULL)
    {
        pthread_mutex_unlock(&can_msg_mutex);
        LOGD("[%d]can_msg_buf is NULL", gettid());
        return;
    }

    if(can_msg_buf[0] == 0x42 || can_msg_buf[0] == 0x44)
    {
        int len=0;
        int i=0;
        char buf[2048];
        LOGD("[%d]can_msg_id = 0x%x, index = %d, size=%d, %s", gettid(), can_msg_buf[0], can_msg_switch, content.size(), content.string());
        //for (i = 0; i < content.size(); i++) {
        //   len += snprintf(buf+len, sizeof(buf), "%02X", can_msg_buf[i]);
        //}
        //LOGD("[%d]content:%s", gettid(), buf);

        can_msg_switch = can_msg_switch % can_msg_type_num;
        memset(period_can_msg[can_msg_switch], 0, CAN_MSG_LEN);
        memcpy(period_can_msg[can_msg_switch], can_msg_buf, content.size());
        can_msg_switch++;
    }

    pthread_mutex_unlock(&can_msg_mutex);
    return;
}

void Listener::postInt(int msgType, int value)
{
    value=value;
    return;
}

int connect_to_mcu_service(sp<InfoCtrlClient>& mcu_client)
{
    LOGD("[%d]connect mcu service begin...", gettid());
    InfoCtrlClient::connect(1, mcu_client);
    if (mcu_client != NULL)
    {
        mcu_client->setListener(new Listener(mcu_client));
        LOGD("[%d]connect mcu service end!", gettid());
        return 0;
    }

    LOGD("[%d]connect mcu service error !", gettid());
    return -1;
}

int android_property_get(const char *key, char *value, const char *default_value )
{
    int len = 0;
    if (!key || !value) {
        LOGD("[%d] the key or the value invalid", gettid(), key, value);
        return NULL;
    }

    len = property_get(key, value, default_value);
    LOGD("[%d]get property %s:%s, len=%d", gettid(), key, value, len);
    return len;
}

int ring_init()
{
    save_ring.data = (char **)malloc(sizeof(char *) * save_ring.max_unit);
    if (!save_ring.data) {
        LOGD("Failed to init the save_ring \n");
        return -1;
    }

    commf_ring.data = (char **)malloc(sizeof(char *) * commf_ring.max_unit);
    if (!commf_ring.data) {
        LOGD("Failed to init the commf_ring \n");
        return -1;
    }

    three_level_ring.data = (char **)malloc(sizeof(char *) * three_level_ring.max_unit);
    if (!three_level_ring.data) {
        LOGD("Failed to init the three_level_ring \n");
        return -1;
    }

    pub_ring.data = (char **)malloc(sizeof(char *) * pub_ring.max_unit);
    if (!pub_ring.data) {
        LOGD("Failed to init the pub_ring \n");
        return -1;
    }

    LOGD("Init the ring buffer successfully\n");
    return 0;
}

int ring_free()
{
    if (pub_ring.data) {
        free(pub_ring.data);
    }

    if (three_level_ring.data) {
        free(three_level_ring.data);
    }

    if (commf_ring.data) {
        free(commf_ring.data);
    }

    if (save_ring.data) {
        free(save_ring.data);
    }

    return 0;
}

int ring_push(ringbuf_t *ring, void *data) {
    if (!ring->data) {
        LOGD("[%d]The ring init did not done!", gettid());
        return 0;
    }

    int next = ring->head + 1;
    if (next >= ring->max_unit) {
        next = 0;
    }

    if (next == ring->tail) {
        LOGD("[%d]Ring buff is Full", gettid());
        return -1;
    }

    ring->data[ring->head] = (char *)data;
    ring->head = next;
    return 0;
}

int ring_pop(ringbuf_t *ring, void **data) {
    if (ring->tail == ring->head) {
        LOGD("[%d]Ring buff is empty", gettid());
        return -1;
    }

    *data = ring->data[ring->tail];

    int next = ring->tail +1;
    if (next >= ring->max_unit) {
        next = 0;
    }

    ring->tail = next;
    return 0;
}

struct timeval start_clock(void)
{
	static struct timeval start;
	gettimeofday(&start, NULL);
	return start;
}

long elapsed_clock(struct timeval start)
{
	struct timeval now, res;

	gettimeofday(&now, NULL);
	timersub(&now, &start, &res);
	return (res.tv_sec)*1000 + (res.tv_usec)/1000;
}

int gb_getdate(gb_time_t *date)
{
    time_t raw_time;
    struct tm *tmp;
    char outstr[200];

    time(&raw_time);
    tmp = localtime(&raw_time);
    if (tmp == NULL) {
        LOGD("[%d] Failed to get the localtime %s", gettid(), strerror(errno));
        return -1;
    }

    if (strftime(outstr, sizeof(outstr), "%Y:%m:%d:%H:%M:%S", tmp) == 0) {
        LOGD("[%d] Failed to format localtime", gettid());
        return -1;
    }

    LOGD("[%d] get time=%s ", gettid(), outstr);

    date->year = (char)tmp->tm_year;
    date->month = (char)tmp->tm_mon;
    date->day = (char)tmp->tm_mday;
    date->hour = (char)tmp->tm_hour;
    date->minute = (char)tmp->tm_min;
    date->second = (char)tmp->tm_sec;

    return 0;
}

int gb_get_input()
{
    int rc = -1;
    sp<InfoCtrlClient> mcu_client;
    do {
        rc = connect_to_mcu_service(mcu_client);
        LOGD("[%d]rc=%d connected to mcu service!", gettid(), rc);
        sleep(RECONN_TIME);
    } while(rc != 0);

    return 0;
}

/*
int  carinfo_release()
{
	mcu_client->release();
    mcu_client.clear();
    return NO_ERROR;
}
*/

int gb_calc_checkcode(char *pkg, uint16_t offset)
{
    int i;
    char tmp;
    if (pkg == NULL) {
        LOGD("[%d] the pkg to be cacl is NULL", gettid());
        return -1;
    }

    if (offset < 24) {
        LOGD("[%d] The calc len is wrong", gettid());
        return -1;
    }

    tmp = pkg[2] ^ pkg[3]; /* start from command unit to calc the check code */
    for (i = 4; i < offset; i++) {
        tmp = tmp ^ pkg[i];
    }

    return tmp;
}

char* gb_construct_package(char cmd, char *buf, uint16_t len)
{
    int rc;
    int offset = 24;
    char encrypt = 0x01;
    char check_code;
    uint16_t tmp;

    char *pkg = (char *)malloc(len + PKG_COMMON_LEN); /* add 1 for check code */
    if (pkg == NULL) {
        LOGD("[%d] Failed to alloc the memory for construct the package, buf len=%d", gettid(), len+PKG_COMMON_LEN);
        return NULL;
    }

    memset(pkg, 0x0, len+PKG_COMMON_LEN);
    pkg[0] = '#';
    pkg[1] = '#';
    pkg[2] = cmd;
    pkg[3] = 0xFE;
    memcpy(pkg+4, rinfo.vin, sizeof(rinfo.vin));
    pkg[21] = encrypt;
    tmp = htons(len);
    memcpy(pkg+22, &tmp, 2);

    if (buf && len > 0) {
        memcpy(pkg+offset, buf, len);
    }

    offset += len;
    /*  TODO: need to compare with JAVA code for calc portion */
    check_code = gb_calc_checkcode(pkg, offset);
    LOGD("[%d] cmd:%x, data_len=%d, package len=%d check_code=%x, len=%d",
         gettid(), cmd, len, offset, check_code, len+PKG_COMMON_LEN);
    pkg[offset] = check_code;

    return pkg;
}

unsigned long sig_hash(unsigned char *str)
{
    if (!str) {
        return 0;
    }

    unsigned long hash = 5381;
    int c;

    while ((c = *str++))
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

typedef struct signal_parse{
    char sig_name[MAX_SIG_NAME_LEN];
    char sig_len;
    char sig_type;
} sig_parse_t;

sig_parse_t period_sig_table[] {
    #include "period_sig_bit_map.h"
};

char g_tmp_buf[can_msg_type_num][2048];
int get_value(char *buf, char len)
{
    switch (len) {
    case 1:
        return buf[0];
        break;
    case 2:
        return buf[1] << 8 | buf[0];
        break;
    case 3:
        return buf[2] << 16 | buf[1] << 8 | buf[0];
        break;
    case 4:
        return buf[3] << 24 | buf[2] << 16 | buf[1] << 8 | buf[0];
        break;
    default :
        LOGD("The len of the buf exceed than expected");
    }

    return -1;
}

int parse_peroid_msg(char *buf)
{
    unsigned int i;
    int rc = -1;
    int index;
    static char battery_flags = 0;
    sig_parse_t signal_parse;
    sig_info_t *pn;

    if (buf == NULL) {
        LOGD("[%d] the parse buf is empty!", gettid());
        return 0;
    }


    //LOGD("[%d] id=%x num=%d ", gettid(), buf[0], NUM_ARRAY(period_sig_table));
    for (i = 0, index = 1; i < NUM_ARRAY(period_sig_table); i++) {
        int orig_value = get_value(buf+index, period_sig_table[i].sig_len);
        //LOGD("[%d] parse_peroid_msg: name=%s, len=%d, orig_value=%d ", gettid(), period_sig_table[i].sig_name, period_sig_table[i].sig_len, orig_value);

        unsigned long key = sig_hash((unsigned char *)period_sig_table[i].sig_name);
        pn = search_signode(key, period_sig_table[i].sig_name);
        if (pn == NULL) {
            //LOGD("[%d] parse_peroid_msg: should not reach there!", gettid());
            index += period_sig_table[i].sig_len;
            continue;
        }

        pn->value = orig_value * pn->factor + pn->offset;
        index += period_sig_table[i].sig_len;
        //LOGD("[%d] parse_peroid_msg: name=%s, len=%d, real_value=%f ", gettid(), period_sig_table[i].sig_name, period_sig_table[i].sig_len, pn->value);
    }

    if (buf[0] == 0x42) {
        if (!battery_flags) {
            rc = parse_battery_info(buf, &g_batt_info);
        }

        if (rc == 0) {
            gb_login();
            battery_flags = 1;
        }
    }


    //show_sigtable();
    return 0;
}

int parse_battery_info(char *buf, battery_info* bi)
{
    int i;
    if (buf == NULL) {
        LOGD("[%d] the parse battery info buf is empty!", gettid());
        return -1;
    }

    char battcodelenth = buf[320];
    LOGD("[%d] type=%x battcodelenth=%d", gettid(), buf[0], battcodelenth);
    char *battstartcode = (char *)malloc(battcodelenth);
    if (!battstartcode) {
        LOGD("[%d] Failed to alloc the memory!", gettid());
        return -1;
    }

    bi->len = battcodelenth;
    memcpy(bi->buf, buf+321, battcodelenth);

    return 0;
}

char convert_veh_charge_status()
{
    char rc = -1;
    unsigned long key = 0;
    sig_info_t *chr_f;
    sig_info_t *chr_s;

    key = sig_hash((unsigned char *)"BMS_ChrFastState");
    chr_f = search_signode(key, "BMS_ChrFastState");
    key = sig_hash((unsigned char *)"BMS_ChrSlowState");
    chr_s = search_signode(key, "BMS_ChrSlowState");
    if (!chr_f || !chr_s) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }
    LOGD("[%d] BMS_ChrFastState=%f, BMS_ChrSlowState=%f", gettid(), chr_f->value, chr_s->value);
    if (chr_f->value == 0x1 || chr_s->value == 0x1) {
        return 0x01;
    } else if (chr_f->value == 0x0 && chr_s->value == 0x0) {
        return 0x03;
    } else if (chr_f->value == 0x02 || chr_s->value == 0x02) {
        return 0x04;
    } else if (chr_f->value == 0x03 || chr_s->value == 0x03) {
        return 0xfE;
    } else {
        return 0xFF;
    }

    return 0xFF;
}

char convert_veh_status()
{
    char rc = -1;
    unsigned long key = 0;
    sig_info_t *veh_bk;
    sig_info_t *veh_br;

    key = sig_hash((unsigned char *)"VCU_BPCAN_KeyPosition");
    veh_bk = search_signode(key, "VCU_BPCAN_KeyPosition");
    key = sig_hash((unsigned char *)"VCU_BPCAN_ReadySts");
    veh_br = search_signode(key, "VCU_BPCAN_ReadySts");
    if (!veh_bk || !veh_br) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }
    LOGD("[%d]VCU_BPCAN_KeyPosition=%f, VCU_BPCAN_ReadySts=%f", gettid(), veh_bk->value, veh_br->value);
    if (veh_bk->value == 0x2 && veh_br->value == 0x1) {
        return 0x01;
    } else if (veh_bk->value == 0x0) {
        return 0x02;
    } else if (veh_bk->value == 0x01 || (veh_bk->value == 0x02 && veh_br->value == 0x0)) {
        return 0x03;
    } else {
        return 0xFF;
    }

    return 0xFF;
}

char convert_gear_level()
{
    char rc = -1;
    unsigned long key = 0;
    char gear_level = 0;
    sig_info_t *accsw_sts;
    sig_info_t *brksw_sts;
    sig_info_t *gearlevel_sts;

    key = sig_hash((unsigned char *)"VCU_ALLCAN_AccSW_Sts");
    accsw_sts = search_signode(key, "VCU_ALLCAN_AccSW_Sts");
    if (!accsw_sts) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    LOGD("[%d]ACCSW_sts value = %f", gettid(), accsw_sts->value);
    if (accsw_sts->value == 0x01) {
        gear_level |= 0x1 << 5;
    } else if (accsw_sts->value == 0x0) {
        gear_level &= ~(0x1 << 5);
    }

    key = sig_hash((unsigned char *)"VCU_ALLCAN_BrkSw_Sts");
    brksw_sts = search_signode(key, "VCU_ALLCAN_BrkSw_Sts");
    if (!brksw_sts) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    if (brksw_sts->value == 0x01) {
        gear_level |= 0x1 << 4;
    } else if (brksw_sts->value == 0x0) {
        gear_level &= ~(0x1 << 4);
    }

    key = sig_hash((unsigned char *)"VCU_BPCAN_GearLeverPos_Sts");
    gearlevel_sts = search_signode(key, "VCU_BPCAN_GearLeverPos_Sts");
    if (!gearlevel_sts) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    if (gearlevel_sts->value == 0x02) {
        gear_level &= ~(0x0F);
    } else if (gearlevel_sts->value == 0x01) {
        gear_level &= ~(0x0F);
        gear_level |= 0x0D;
    } else if (gearlevel_sts->value == 0x03) {
        gear_level &= ~(0x0F);
        gear_level |= 0x0E;
    } else if (gearlevel_sts->value == 0x00) {
        gear_level &= ~(0x0F);
        gear_level |= 0x0F;
    } else {
        return 0xFF;
    }

    return gear_level;
}

char convert_dcdc_mode()
{
    char rc = -1;
    unsigned long key = 0;
    sig_info_t *dcdc;

    key = sig_hash((unsigned char *)"DCDC_OperatingMode");
    dcdc = search_signode(key, "DCDC_OperatingMode");
    if (!dcdc) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    LOGD("[%d]DCDC value = %f", gettid(), dcdc->value);
    if (dcdc->value == 0x02) {
        return 0x01;
    } else if (dcdc->value == 0x04) {
        return 0x02;
    } else if (dcdc->value == 0x03) {
        return 0xFE;
    } else if (dcdc->value == 0x00 || dcdc->value == 0x01) {
        return 0xFF;
    } else {
        return 0xFF;
    }

    return 0xFF;
}

char convert_elecmotor_status()
{
    unsigned long key = 0;
    sig_info_t *mode;
    sig_info_t *worksts;

    key = sig_hash((unsigned char *)"MCU_Mode");
    mode = search_signode(key, "MCU_Mode");
    if (!mode) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    LOGD("[%d] MCU mode value = %f", gettid(), mode->value);
    if (mode->value == 0x01) {
        return 0x01;
    } else if (mode->value == 0x02) {
        return 0x02;
    }

    key = sig_hash((unsigned char *)"MCUWorkSts");
    worksts = search_signode(key, "MCUWorkSts");
    if (!worksts) {
        LOGD("Can't get the signal node in hash map");
        return 0xFF;
    }

    LOGD("[%d]MCUworksts value = %f", gettid(), worksts->value);
    if (worksts->value == 0x00) {
        return 0x03;
    } else if (worksts->value == 0x01 || worksts->value == 0x02 || worksts->value == 0x03) {
        return 0x04;
    }

    return 0xFF;
}

int gb_get_veh_data(char *buf)
{
    char veh[20];
    sig_info_t *speed;
    sig_info_t *ODOtotal;
    sig_info_t *PackU;
    sig_info_t *PackI;
    sig_info_t *PackSoc;
    sig_info_t *InsR;
    sig_info_t *AccPed;
    sig_info_t *BrkSw;
    unsigned long key = 0;

    veh[0] = convert_veh_status();
    LOGD("[%d] vehicle status=%x", gettid(), veh[0]);
    veh[1] = convert_veh_charge_status();
    LOGD("[%d] charge status=%x", gettid(), veh[1]);
    veh[2] = 0x1; /* running mode, always 1*/

    key = sig_hash((unsigned char *)"VCU_BPCAN_VehicleSpeed");
    speed = search_signode(key, "VCU_BPCAN_VehicleSpeed");
    if (speed) {
        memcpy(veh+3,&speed->value, 2);
    } else {
        veh[3] = 0xFF;
        veh[4] = 0xFF;
    }

    key = sig_hash((unsigned char *)"ICM_ODOTotal");
    ODOtotal = search_signode(key, "ICM_ODOTotal");
    if (ODOtotal) {
        memcpy(veh+5, &ODOtotal->value, 4);
    } else {
        veh[5] = 0xFF;
        veh[6] = 0xFF;
        veh[7] = 0xFF;
        veh[8] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_PackU");
    PackU = search_signode(key, "BMS_PackU");
    if (PackU) {
        memcpy(veh+9, &PackU->value, 2);
    } else {
        veh[9] = 0xFF;
        veh[10] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_PackI");
    PackI = search_signode(key, "BMS_PackI");
    if (PackI) {
        memcpy(veh+11, &PackI->value, 2);
    } else {
        veh[11] = 0xFF;
        veh[12] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_PackSoc");
    PackSoc = search_signode(key, "BMS_PackSoc");
    if (PackSoc) {
        memcpy(veh+13, &PackSoc->value, 1);
    } else {
        veh[13] = 0xFF;
    }

    veh[14] = convert_dcdc_mode();
    veh[15] = convert_gear_level();

    key = sig_hash((unsigned char *)"BMS_Insulation_R");
    InsR = search_signode(key, "BMS_Insulation_R");
    if (InsR) {
        memcpy(veh+16, &InsR->value, 2);
    } else {
        veh[16] = 0xFF;
        veh[17] = 0xFF;
    }

    key = sig_hash((unsigned char *)"VCU_ALLCAN_AccPed_Pos");
    AccPed = search_signode(key, "VCU_ALLCAN_AccPed_Pos");
    if (AccPed) {
        memcpy(veh+18, &AccPed->value, 1);
    } else {
        veh[18] = 0xFF;
    }

    key = sig_hash((unsigned char *)"VCU_ALLCAN_BrkSw_Sts");
    BrkSw = search_signode(key, "VCU_ALLCAN_BrkSw_Sts");
    if (BrkSw) {
        memcpy(veh+19, &BrkSw->value, 1);
    } else {
        veh[19] = 0xFF;
    }

    memcpy(buf, veh, sizeof(veh));

    return sizeof(veh);
}

int gb_get_elec_motor(char *buf)
{
    char elec_motor[13];
    sig_info_t *Invertertemp;
    sig_info_t *MotorSpeed;
    sig_info_t *Contr_Tor;
    sig_info_t *MotorTemp;
    sig_info_t *DC_linkVo;
    sig_info_t *DC_linkCu;
    unsigned long key = 0;

    elec_motor[0] = 0x1; /* the number of electric motor, always 1 */
    elec_motor[1] = 0x1; /* the sequence number of electric motor, always 1 */

    elec_motor[2] = convert_elecmotor_status();
    LOGD("[%d]electric motor status=%x", gettid(), elec_motor[2]);

    key = sig_hash((unsigned char *)"MCU_InverterTemp");
    Invertertemp = search_signode(key, "MCU_InverterTemp");
    if (Invertertemp) {
        memcpy(elec_motor+3, &Invertertemp->value, 1);
    } else {
        elec_motor[3] = 0xFF;
    }

    key = sig_hash((unsigned char *)"MCU_MotorSpeed");
    MotorSpeed = search_signode(key, "MCU_MotorSpeed");
    if (MotorSpeed) {
        memcpy(elec_motor+4, &MotorSpeed->value, 2);
    } else {
        elec_motor[4] = 0xFF;
        elec_motor[5] = 0xFF;
    }

    key = sig_hash((unsigned char *)"MCU_ControllerTorque");
    Contr_Tor = search_signode(key, "MCU_ControllerTorque");
    if (Contr_Tor) {
        memcpy(elec_motor+6, &Contr_Tor->value, 2);
    } else {
        elec_motor[6] = 0xFF;
        elec_motor[7] = 0xFF;
    }

    key = sig_hash((unsigned char *)"MCU_MotorTemp");
    MotorTemp = search_signode(key, "MCU_MotorTemp");
    if (MotorTemp) {
        memcpy(elec_motor+8, &MotorTemp->value, 1);
    } else {
        elec_motor[8] = 0xFF;
    }

    key = sig_hash((unsigned char *)"MCU_DC_linkVoltage");
    DC_linkVo = search_signode(key, "MCU_DC_linkVoltage");
    if (DC_linkVo) {
        memcpy(elec_motor+9, &DC_linkVo->value, 2);
    } else {
        elec_motor[9] = 0xFF;
        elec_motor[10] = 0xFF;
    }

    key = sig_hash((unsigned char *)"MCU_DC_linkCurrent");
    DC_linkCu = search_signode(key, "MCU_DC_linkCurrent");
    if (DC_linkCu) {
        memcpy(elec_motor+11, &DC_linkCu->value, 2);
    } else {
        elec_motor[11] = 0xFF;
        elec_motor[12] = 0xFF;
    }

    memcpy(buf, elec_motor, sizeof(elec_motor));

    return sizeof(elec_motor);
}

int gb_get_extreme_value(char *buf)
{
    char extreme[14];
    sig_info_t *maxcellno;
    sig_info_t *maxcellU;
    sig_info_t *mincellno;
    sig_info_t *mincellU;
    sig_info_t *maxtempno;
    sig_info_t *maxtemp;
    sig_info_t *mintempno;
    sig_info_t *mintemp;
    unsigned long key = 0;

    extreme[0] = 0x1;

    key = sig_hash((unsigned char *)"BMS_MaxCellNo");
    maxcellno = search_signode(key, "BMS_MaxCellNo");
    if (maxcellno) {
        memcpy(extreme+1, &maxcellno->value, 1);
    } else {
        extreme[1] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_MaxCellU");
    maxcellU = search_signode(key, "BMS_MaxCellU");
    if (maxcellU) {
        memcpy(extreme+2, &maxcellU->value, 2);
    } else {
        extreme[2] = 0xFF;
        extreme[3] = 0xFF;
    }

    extreme[4] = 0x1;

    key = sig_hash((unsigned char *)"BMS_MinCellNo");
    mincellno = search_signode(key, "BMS_MinCellNo");
    if (mincellno) {
        memcpy(extreme+5, &mincellno->value, 1);
    } else {
        extreme[5] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_MinCellU");
    mincellU = search_signode(key, "BMS_MinCellU");
    if (mincellU) {
        memcpy(extreme+6, &mincellU->value, 2);
    } else {
        extreme[6] = 0xFF;
        extreme[7] = 0xFF;
    }

    extreme[8] = 0x1;

    key = sig_hash((unsigned char *)"BMS_MaxTempNo");
    maxtempno = search_signode(key, "BMS_MaxTempNo");
    if (maxtempno) {
        memcpy(extreme+9, &maxtempno->value, 1);
    } else {
        extreme[9] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_MaxTemp");
    maxtemp = search_signode(key, "BMS_MaxTemp");
    if (maxtemp) {
        memcpy(extreme+10, &maxtemp->value, 1);
    } else {
        extreme[10] = 0xFF;
    }

    extreme[11] = 0x1;

    key = sig_hash((unsigned char *)"BMS_MinTempNo");
    mintempno = search_signode(key, "BMS_MinTempNo");
    if (mintempno) {
        memcpy(extreme+12, &mintempno->value, 1);
    } else {
        extreme[12] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_MinTemp");
    mintemp = search_signode(key, "BMS_MinTemp");
    if (mintemp) {
        memcpy(extreme+13, &mintemp->value, 1);
    } else {
        extreme[13] = 0xFF;
    }

    memcpy(buf, extreme, sizeof(extreme));

    return sizeof(extreme);
}

int gb_get_reess_vol(char *buf)
{
    char reess_vol[100];
    sig_info_t *packu;
    sig_info_t *packi;
    sig_info_t *celltotalnum;
    sig_info_t *battcellno;
    unsigned long key = 0;
    uint16_t startno = 0;
    int totolnum = 0;
    char cellname[64];
    int len = 0;
    int i;

    reess_vol[0] = 0x1;
    reess_vol[1] = 0x1;

    key = sig_hash((unsigned char *)"BMS_PackU");
    packu = search_signode(key, "BMS_PackU");
    if (packu) {
        memcpy(reess_vol+2, &packu->value, 2);
    } else {
        reess_vol[2] = 0xFF;
        reess_vol[3] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_PackI");
    packi = search_signode(key, "BMS_PackI");
    if (packi) {
        memcpy(reess_vol+4, &packi->value, 2);
    } else {
        reess_vol[4] = 0xFF;
        reess_vol[5] = 0xFF;
    }

    key = sig_hash((unsigned char *)"BMS_CellTotalNum");
    celltotalnum = search_signode(key, "BMS_CellTotalNum");
    if (celltotalnum) {
        memcpy(reess_vol+6, &celltotalnum->value, 2);
        totolnum = celltotalnum->value;
    } else {
        reess_vol[6] = 0xFF;
        reess_vol[7] = 0xFF;
    }

    startno = htons(0x01);
    memcpy(reess_vol+8, &startno, 2);
    reess_vol[10] = totolnum;

    LOGD("[%d] the BMS celltotalnum=%d", gettid(), totolnum);
    memset(cellname, 0x0, sizeof(cellname));
    for (i = 1; i <= totolnum; i++) {
        snprintf(cellname, sizeof(cellname),"BMS_BattCellNum%dVol", i);
        key = sig_hash((unsigned char *)cellname);
        battcellno = search_signode(key, cellname);
        if (battcellno) {
            memcpy(reess_vol+9+i*2, &battcellno->value, 2);
        } else {
            reess_vol[9+i*2] = 0xFF;
            reess_vol[9+i*2+1] = 0xFF;
        }
    }

    len = 11 + totolnum * 2;

    memcpy(buf, reess_vol, len);

    return len;
}

int gb_get_reess_temp(char *buf)
{
    char reess_temp[100];
    sig_info_t *temp_probenum;
    sig_info_t *battcellno;
    unsigned long key = 0;
    uint16_t probenum = 0;
    char temp_sensor[64];
    int len = 0;
    int i;

    reess_temp[0] = 0x1;
    reess_temp[1] = 0x1;

    key = sig_hash((unsigned char *)"BMS_Temp_ProbeNum");
    temp_probenum = search_signode(key, "BMS_Temp_ProbeNum");
    if (temp_probenum) {
        memcpy(reess_temp+2, &temp_probenum->value, 2);
        probenum = temp_probenum->value;
    } else {
        reess_temp[2] = 0xFF;
        reess_temp[3] = 0xFF;
    }

    LOGD("[%d]the temperature probenum=%d", gettid(), probenum);

    memset(temp_sensor, 0x0, sizeof(temp_sensor));
    for (i = 1; i <= probenum; i++) {
        snprintf(temp_sensor, sizeof(temp_sensor),"BMS_BattTempSensorNum%d", i);
        key = sig_hash((unsigned char *)temp_sensor);
        battcellno = search_signode(key, temp_sensor);
        if (battcellno) {
            memcpy(reess_temp+3+i, &battcellno->value, 1);
        } else {
            reess_temp[3+i] = 0xFF;
        }
    }

    len = 4 + probenum;

    memcpy(buf, reess_temp, len);

    return len;
}

int gb_get_alarm_data(char *buf)
{
    char alarm[9];
    sig_info_t *almDT;
    sig_info_t *almOT;
    sig_info_t *almOBV;
    sig_info_t *almUBV;
    sig_info_t *almUC;
    sig_info_t *almOV;
    sig_info_t *almUV;
    sig_info_t *almSOCOverHigh;
    sig_info_t *almSOCSkip;
    sig_info_t *almSOCMismatch;
    sig_info_t *almCellVol_DOH;
    sig_info_t *almIso;
    sig_info_t *almDCDC_OTF;
    sig_info_t *almLowBrake;
    sig_info_t *almDCDC_OM;
    sig_info_t *almInverterOT;
    sig_info_t *almHVLock;
    sig_info_t *almMotorOverTemp;
    sig_info_t *almBattOverCharge;
    unsigned long key = 0;
    uint32_t alarm_flags = 0;
    char three_fault_issued = 0;
    char two_fault_issued = 0;
    char one_fault_issued = 0;

    alarm[0] = 0x0;

    key = sig_hash((unsigned char *)"BMS_AlmDT");
    almDT = search_signode(key, "BMS_AlmDT");
    if (almDT) {
        if (almDT->value == 0x1) {
            alarm_flags |= 1 << 0;
        } else {
            alarm_flags &= ~(1 << 0);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmOT");
    almOT = search_signode(key, "BMS_AlmOT");
    if (almOT) {
        if (almOT->value == 0x1) {
            alarm_flags |= 1 << 1;
            three_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 1);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmOBV");
    almOBV = search_signode(key, "BMS_AlmOBV");
    if (almOBV) {
        if (almOBV->value == 0x1) {
            alarm_flags |= 1 << 2;
            three_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 2);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmUBV");
    almUBV = search_signode(key, "BMS_AlmUBV");
    if (almUBV) {
        if (almUBV->value == 0x1) {
            alarm_flags |= 1 << 3;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 3);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmUC");
    almUC = search_signode(key, "BMS_AlmUC");
    if (almUC) {
        if (almUC->value == 0x1) {
            alarm_flags |= 1 << 4;
            one_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 4);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmOV");
    almOV = search_signode(key, "BMS_AlmOV");
    if (almOV) {
        if (almOV->value == 0x1) {
            alarm_flags |= 1 << 5;
            three_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 5);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmUV");
    almUV = search_signode(key, "BMS_AlmUV");
    if (almUV) {
        if (almUV->value == 0x1) {
            alarm_flags |= 1 << 6;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 6);
        }
    }

    key = sig_hash((unsigned char *)"BMS_SOCOverHigh");
    almSOCOverHigh = search_signode(key, "BMS_SOCOverHigh");
    if (almSOCOverHigh) {
        if (almSOCOverHigh->value == 0x1) {
            alarm_flags |= 1 << 7;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 7);
        }
    }

    key = sig_hash((unsigned char *)"BMS_SOCSkip");
    almSOCSkip = search_signode(key, "BMS_SOCSkip");
    if (almSOCSkip) {
        if (almSOCSkip->value == 0x1) {
            alarm_flags |= 1 << 8;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 8);
        }
    }

    key = sig_hash((unsigned char *)"BMS_Mismatch");
    almSOCMismatch = search_signode(key, "BMS_Mismatch");
    if (almSOCMismatch) {
        if (almSOCMismatch->value == 0x1) {
            alarm_flags |= 1 << 9;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 9);
        }
    }

    key = sig_hash((unsigned char *)"BMS_CellVol_DevOverHigh");
    almCellVol_DOH = search_signode(key, "BMS_CellVol_DevOverHigh");
    if (almCellVol_DOH) {
        if (almCellVol_DOH->value == 0x1) {
            alarm_flags |= 1 << 10;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 10);
        }
    }

    key = sig_hash((unsigned char *)"BMS_AlmIso");
    almIso= search_signode(key, "BMS_AlmIso");
    if (almIso) {
        if (almIso->value == 0x1) {
            alarm_flags |= 1 << 11;
            three_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 11);
        }
    }

    key = sig_hash((unsigned char *)"DCDC_OverTemperature_F");
    almDCDC_OTF = search_signode(key, "DCDC_OverTemperature_F");
    if (almDCDC_OTF) {
        if (almDCDC_OTF->value == 0x1) {
            alarm_flags |= 1 << 12;
            one_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 12);
        }
    }

    key = sig_hash((unsigned char *)"ICM_LowBrakeFluidLevel");
    almLowBrake = search_signode(key, "ICM_LowBrakeFluidLevel");
    if (almLowBrake) {
        if (almLowBrake->value == 0x1) {
            alarm_flags |= 1 << 13;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 13);
        }
    }

    key = sig_hash((unsigned char *)"DCDC_OperatingMode");
    almDCDC_OM = search_signode(key, "DCDC_OperatingMode");
    if (almDCDC_OM) {
        if (almDCDC_OM->value == 0x1) {
            alarm_flags |= 1 << 14;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 14);
        }
    }

    key = sig_hash((unsigned char *)"MCU_InverterOverTemp_F");
    almInverterOT = search_signode(key, "MCU_InverterOverTemp_F");
    if (almInverterOT) {
        if (almInverterOT->value == 0x1) {
            alarm_flags |= 1 << 15;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 15);
        }
    }

    key = sig_hash((unsigned char *)"VCU_BPCAN_HVLock");
    almHVLock = search_signode(key, "VCU_BPCAN_HVLock");
    if (almHVLock) {
        if (almHVLock->value == 0x1) {
            alarm_flags |= 1 << 16;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 16);
        }
    }

    key = sig_hash((unsigned char *)"MCU_MotorOverTemp_F");
    almMotorOverTemp = search_signode(key, "MCU_MotorOverTemp_F");
    if (almMotorOverTemp) {
        if (almMotorOverTemp->value == 0x1) {
            alarm_flags |= 1 << 17;
            two_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 17);
        }
    }

    key = sig_hash((unsigned char *)"BMS_BattOverCharge");
    almBattOverCharge = search_signode(key, "BMS_BattOverCharge");
    if (almBattOverCharge) {
        if (almBattOverCharge->value == 0x1) {
            alarm_flags |= 1 << 18;
            three_fault_issued = 1;
        } else {
            alarm_flags &= ~(1 << 18);
        }
    }

    if (three_fault_issued) {
        alarm[0] = 0x3;
        g_three_level_fault_issued = 1;
    } else if (two_fault_issued) {
        alarm[0] = 0x2;
    } else if (one_fault_issued) {
        alarm[0] = 0x1;
    }

    memcpy(alarm+1, &alarm_flags, 4);
    alarm[5] = 0;
    alarm[6] = 0;
    alarm[7] = 0;
    alarm[8] = 0;

    memcpy(buf, alarm, sizeof(alarm));

    return sizeof(alarm);
}

int gb_get_gps_data(char *buf)
{
    char gps_data[9];

    pthread_mutex_lock(&gps_info_mutex);
    gps_data[0] = g_gps_info.status;
    memcpy(gps_data+1, &g_gps_info.longtitude, 4);
    memcpy(gps_data+5, &g_gps_info.altitude, 4);
    pthread_mutex_unlock(&gps_info_mutex);

    memcpy(buf, gps_data, sizeof(gps_data));
    return sizeof(gps_data);
}

int write_three_level_file(char *tmp_msg)
{
    FILE *fault_fp;
    uint16_t len = 0;
    if (!tmp_msg) {
        LOGD("[%d] tmp_msg = NULL", gettid());
        return -1;
    }

    fault_fp = open_write_file(THREE_LEVEL_FAULT_FILE, "a+b");
    if (!fault_fp) {
        LOGD("[%d] fault_fp=%s ", gettid(), fault_fp ? "NOT NULL": "NULL" );
        return -1;
    }

    len = (uint16_t)ntohs(*(uint16_t *)(tmp_msg+22));
    len += PKG_COMMON_LEN;
    LOGD("[%d]write three_level_fault.file, size=%d, ", gettid(), len);
    fwrite(tmp_msg, len, 1, fault_fp);
    fflush(fault_fp);
    fclose(fault_fp);

    return 0;
}

int send_pre_30s()
{
    int cnt = 30;
    int len;
    int rc;
    char *pub_buf = NULL;

    pthread_mutex_lock(&ringbuf_mutex);
    while (ring_pop(&three_level_ring, (void **)&pub_buf) != -1 && cnt > 0) {
        cnt--;
        if (pub_buf != NULL ) {
            len = (uint16_t)ntohs(*(uint16_t *)(pub_buf+22));
            len += PKG_COMMON_LEN;
            if (pub_buf[24] == 0x02) {
                pub_buf[24] = 0x03;
            }
        }
        pthread_mutex_lock(&publish_mutex);
        rc = ring_push(&pub_ring, (void *)pub_buf);
        if (rc) {
            char *tmp_msg = NULL;
            LOGD("[%d]the pub_ring buffer is full, size=%d", gettid(), pub_ring.max_unit);
            ring_pop(&pub_ring, (void **)&tmp_msg);
            free(tmp_msg);
            ring_push(&pub_ring, (void *)pub_buf);
        }
        pthread_mutex_unlock(&publish_mutex);

        LOGD("[%d] writed and pushed message cnt=%d", gettid(), 30-cnt);
        write_three_level_file(pub_buf);
    }
    pthread_mutex_unlock(&ringbuf_mutex);

    return 0;
}

int send_post_30s()
{
    int rc  = -1;
    int len = 0;
    char *pub_buf;

    pthread_mutex_lock(&ringbuf_mutex);
    rc = ring_pop(&three_level_ring, (void **)&pub_buf);
    if (rc != 0) {
        LOGD("[%d] Failed to pop the message from three level ring", gettid());
        return rc;
    }

    if (pub_buf != NULL) {
        len = (uint16_t)ntohs(*(uint16_t *)(pub_buf+22));
        len += PKG_COMMON_LEN;
        if (pub_buf[24] == 0x02) {
            pub_buf[24] = 0x03;
        }
    }
    pthread_mutex_unlock(&ringbuf_mutex);
    write_three_level_file(pub_buf);

    pthread_mutex_lock(&publish_mutex);
    rc = ring_push(&pub_ring, (void *)pub_buf);
    if (rc) {
        char *tmp_msg = NULL;
        LOGD("[%d]the post 30s pub_ring buffer is full, size=%d", gettid(), pub_ring.max_unit);
        ring_pop(&pub_ring, (void **)&tmp_msg);
        free(tmp_msg);
        ring_push(&pub_ring, (void *)pub_buf);
    }
    pthread_mutex_unlock(&publish_mutex);

    return rc;
}

int do_three_level_trans()
{
    int rc;
    static char  did_pre_30s  = 0;
    static char  did_post_30s = 0;
    static int cnt = 30;
    LOGD("[%d] did_pre_30s = %d, did_post_30=%d, post cnt=%d", gettid(), did_pre_30s, did_post_30s, (30 - cnt));

    if (did_pre_30s == 0) {
        rc = send_pre_30s();
        if (rc == 0) {
            LOGD("[%d] Successful add the three level fault previous 30s to the send queue", gettid());
            did_pre_30s = 1;
        }
        return 0;
    }

    if (did_post_30s == 0 && cnt > 0) {
        rc = send_post_30s();
        if (rc == 0) {
            cnt--;
            LOGD("[%d] Successful add the three level fault post 30s to the send queue", gettid());
        }

        if (cnt == 0) {
            did_post_30s = 1;
        }

        return 0;
    }

    if (did_pre_30s && did_post_30s) {
        //pthread_mutex_lock(&three_level_fault_mutex);
        g_three_level_fault_issued = 0;
        //pthread_mutex_unlock(&three_level_fault_mutex);
        did_pre_30s = 0;
        did_post_30s = 0;
        cnt = 30;
        LOGD("[%d] the three level report action done ", gettid());
    }

    return 0;
}

int construct_data_msg()
{
    char buf[2048];
    int len = 0;
    char veh_ischaring;
    char *pkg;
    static int time_cnt = 0;
    char* three_buf = NULL;
    char* pub_buf = NULL;
    int rc;

    gb_time_t date;
    gb_getdate(&date);
    memcpy(buf, &date, sizeof(gb_time_t));
    len += sizeof(gb_time_t);

    buf[len] = 0x01;
    len += 1;
    len += gb_get_veh_data(buf+len);

    veh_ischaring = convert_veh_charge_status();
    if (veh_ischaring != 0x01) {
        buf[len] = 0x02;
        len += 1;
        len += gb_get_elec_motor(buf+len);
    }

    buf[len] = 0x05;
    len += 1;
    len += gb_get_gps_data(buf+len);

    buf[len] = 0x06;
    len += 1;
    len += gb_get_extreme_value(buf+len);

    buf[len] = 0x07;
    len += 1;
    len += gb_get_alarm_data(buf+len);

    buf[len] = 0x08;
    len += 1;
    len += gb_get_reess_vol(buf+len);

    buf[len] = 0x09;
    len += 1;
    len += gb_get_reess_temp(buf+len);

    pkg = gb_construct_package(REALTIME_REPORT, buf, len);

    pthread_mutex_lock(&ringbuf_mutex);
    rc = ring_push(&save_ring, (void *)pkg);
    if (rc) {
        char *tmp_msg = NULL;
        LOGD("[%d]the ring buffer is full, size=%d", gettid(), save_ring.max_unit);
        ring_pop(&save_ring, (void **)&tmp_msg);
        free(tmp_msg);
        ring_push(&save_ring, (void *)pkg);
    }

    three_buf = (char*)malloc(len + PKG_COMMON_LEN);
    if (three_buf == NULL) {
        LOGD("[%d]Failed to alloc the memory for three buf", gettid());
        pthread_mutex_unlock(&ringbuf_mutex);
        return -1;
    }

    memcpy(three_buf, pkg, len + PKG_COMMON_LEN);
    rc = ring_push(&three_level_ring, (void *)three_buf);
    if (rc) {
        char *tmp_msg = NULL;
        LOGD("[%d]the three_buf buffer is full, size=%d", gettid(), save_ring.max_unit);
        ring_pop(&three_level_ring, (void **)&tmp_msg);
        free(tmp_msg);
        ring_push(&three_level_ring, (void *)three_buf);
    }
    pthread_mutex_unlock(&ringbuf_mutex);

    if (g_three_level_fault_issued) {
        do_three_level_trans();
    }

    if (++time_cnt%3 == 0 && !g_three_level_fault_issued) {
        pthread_mutex_lock(&publish_mutex);
        pub_buf = (char*)malloc(len + PKG_COMMON_LEN);
        if (pub_buf == NULL) {
            LOGD("[%d]Failed to alloc the memory for three buf", gettid());
            pthread_mutex_unlock(&publish_mutex);
            return -1;
        }

        memcpy(pub_buf, pkg, len + PKG_COMMON_LEN);
        rc = ring_push(&pub_ring, (void *)pub_buf);
        if (rc) {
            char *tmp_msg = NULL;
            LOGD("[%d]the pub_ring buffer is full, size=%d", gettid(), pub_ring.max_unit);
            ring_pop(&pub_ring, (void **)&tmp_msg);
            free(tmp_msg);
            ring_push(&pub_ring, (void *)three_buf);
        }

        pthread_mutex_unlock(&publish_mutex);
    }

    return 0;
}

void* parse_canmsg(void *param)
{
    param = param;
    int i;
    while(doExit == 0) {

        pthread_mutex_lock(&can_msg_mutex);
        for (i = 0; i < can_msg_type_num; i++) {
            memcpy(g_tmp_buf[i], period_can_msg[i], sizeof(period_can_msg[i]));
        }
        pthread_mutex_unlock(&can_msg_mutex);

        for (i = 0; i < can_msg_type_num; i++) {
            if (g_tmp_buf[i][0] == 0x42 || g_tmp_buf[i][0] == 0x44){
                parse_peroid_msg(g_tmp_buf[i]);
            }
        }
        usleep(1000 * 1000);
        construct_data_msg();
    }

    LOGD("parse msg process exit");
    return NULL;
}

int recv_timeout(int socket_desc, int timeout, char *total)
{
    int total_num=0;
    int len=0;
    int i=0;
    char reply[1024];
    int offset = 0;
    struct timeval now, old;
    double diff;
    memset(reply, 0x0, sizeof(reply));
    gettimeofday(&old, NULL);
    while (1) {
        gettimeofday(&now, NULL);
        diff  = (now.tv_sec - old.tv_sec) + 1e-6 * (now.tv_usec-old.tv_usec);
        if (total_num > 0 && diff > timeout) {
            break;
        } else if (diff > 2 * timeout) {
            break;
        }

        if ((len = recv(socket_desc, reply , sizeof(reply) , 0)) <= 0) {
               usleep(100*1000);
        } else {
            ++i;
            LOGD("i=%d received len=%d, total_num=%d\n", i, len, total_num);
            //memcpy(total+offset, reply, sizeof(reply));
            memcpy(total+offset, reply, len);
            offset += len;
            total_num += len;
        }
    }

    return total_num;
}

int gb_handle_response(char *total)
{
    char rcode;
    if (!total) {
        return 0;
    }
    rcode = total[3];
    switch (rcode) {
        case 0x01:
            LOGD("[%d] response successful from server", gettid());
            break;
        case 0x02:
            LOGD("[%d] response error from server", gettid());
            break;
        case 0x03:
            LOGD("[%d] response VIN duplicated error from server", gettid());
            break;
        case 0xFE:
            LOGD("[%d] response is command package not a response package", gettid());
            break;
        default:
            LOGD("[%d] response code is %x ", gettid(), rcode);
    }
    return 0;
}

void* send_data_thread(void *param)
{
    int rc = -1;
    int i;
    uint16_t len;
    char *tmp_msg = NULL;
    char total[1024 * 10];
    while (!doExit) {
        int total_len;
        pthread_mutex_lock(&publish_mutex);
        while (ring_pop(&pub_ring, (void **)&tmp_msg) != -1) {
            /* The transmit portion for GB data */
            len = (uint16_t)ntohs(*(uint16_t *)(tmp_msg+22));
            len += PKG_COMMON_LEN;
            rc = send(rinfo.sock_fd, tmp_msg, len, 0);
            if (rc <= 0) {
                LOGD("[%d] Failed to send the package len=%d rc=%d, errno=%s", gettid(), len, rc, strerror(errno));
                if (tmp_msg) {
                    free(tmp_msg);
                }
                continue;
            }

            LOGD("[%d] Publish send the data successfully, send len=%d", gettid(), rc);

            total_len = recv_timeout(rinfo.sock_fd, 1, total);
            if (total_len <= 0) {
                LOGD("[%d] No response from server, check passed", gettid());
            } else if (total_len > 0 && total[3] != 0x0) {
                gb_handle_response(total);
            }

            if (!g_comm_failed_issued) {
                if (tmp_msg) {
                    free(tmp_msg);
                }
            } else {
                pthread_mutex_lock(&commf_ring_mutex);
                rc = ring_push(&commf_ring, (void *)tmp_msg);
                if (rc) {
                    char *msg = NULL;
                    LOGD("[%d]the comm_failed buffer is full, size=%d", gettid(), save_ring.max_unit);
                    ring_pop(&commf_ring, (void **)&msg);
                    free(msg);
                    ring_push(&commf_ring, (void *)tmp_msg);
                }
                pthread_mutex_unlock(&commf_ring_mutex);
            }
        }

        pthread_mutex_unlock(&publish_mutex);
        usleep(200 * 1000);
    }

    LOGD("send thread process exit");
    return NULL;
}

int  gb_create_link()
{
    int rc;
    int sock_fd;
    struct sockaddr_in server_addr;
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        LOGD("[%d] Failed to create the socket %s", gettid(), strerror(errno));
        return -1;
    }

    LOGD("[%d] create the socket=%d ", gettid(), sock_fd);

    int flags = fcntl(sock_fd, F_GETFL, 0);
    if (flags < 0) {
        LOGD("[%d] Failed to get the socket flags", gettid());
        return -1;
    }
    rc = fcntl(sock_fd, F_SETFD, fcntl(sock_fd, F_GETFD) | FD_CLOEXEC);
    if (rc < 0) {
        LOGD("[%d] Failed to set the FD_CLOEXEC flags", gettid());
        return -1;
    }

    memset(&server_addr, 0x0, sizeof(struct sockaddr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = ser_info.port;
    server_addr.sin_addr.s_addr = ser_info.server_ip;
    //server_addr.sin_port = htons(10369);
    //inet_pton(AF_INET, "120.35.20.145", &server_addr.sin_addr);

    LOGD("[%d] port=%x:%d ip=%x:%s", gettid(), server_addr.sin_port, ntohs(server_addr.sin_port),
         server_addr.sin_addr, inet_ntoa((in_addr)server_addr.sin_addr));
    rc = connect(sock_fd, (struct sockaddr *)&server_addr, sizeof(struct sockaddr_in));
    if (rc < 0) {
        LOGD("[%d] Failed to connect the socket %s, server:%s, port:%s", gettid(),
             strerror(errno), ser_info.server_ip_s, ser_info.server_port_s);
        return rc;
    }

    //rc = fcntl(sock_fd, F_SETFL, flags|O_NONBLOCK);
    //if (rc < 0) {
        //LOGD("[%d] Failed to set the O_NONBLOCK flags", gettid());
        //return -1;
    //}
    return sock_fd;
}

void do_reconnect()
{
    int rc = -1;

    while(1) {
        if (rinfo.sock_fd >= 0) {
            close(rinfo.sock_fd);
            LOGD("[%d] close the inactive the socket %d", gettid(), rinfo.sock_fd);
            rinfo.sock_fd = -1;
        }

        rc = gb_create_link();
        if (rc < 0) {
            LOGD("[%d] Failed to build the new socket rc=%d", gettid(), rc);
            sleep(1);
        } else if (rc > 0) {
            LOGD("[%d] success reconnect new fd =%d", gettid(), rc);
            rinfo.sock_fd = rc;
            g_reconnect_status = 1;
            break;
        }
    }

    return;
}

void* heartbeat_thread(void *param)
{
    int rc = -1;
    int i;
    uint16_t len = PKG_COMMON_LEN;
    char *tmp_msg = NULL;
    char total[128];
    char *pkg;
    int try_cnt = 0;
    int total_len;
    while (doExit == 0) {
        tmp_msg = gb_construct_package(CLOUD_HEARBEAT, NULL, 0);
        rc = send(rinfo.sock_fd, tmp_msg, PKG_COMMON_LEN, 0);
        if (rc <= 0) {
            LOGD("[%d] Failed to send the package len=%d rc=%d, errno=%s", gettid(), len, rc, strerror(errno));
            if (tmp_msg) {
                free(tmp_msg);
            }
            usleep(3000 * 1000); /* send the hearbeat every 3s */
            continue;
        }

        LOGD("[%d] Send the data successfully, send len=%d", gettid(), rc);

        total_len = recv_timeout(rinfo.sock_fd, 1, total);
        if (total_len <= 0) {
            LOGD("[%d] No response from server side for heartbeat!", gettid());
            if (++try_cnt%3 == 0) {
                pthread_mutex_lock(&comm_failed_mutex);
                g_comm_failed_issued = 1;
                pthread_mutex_unlock(&comm_failed_mutex);
                do_reconnect();
            }
        } else if (total_len > 0 && total[3] != 0x0) {
            gb_handle_response(total);
        }

        if (tmp_msg) {
            free(tmp_msg);
        }

        usleep(30 * 1000 * 1000); /* send the hearbeat every 30s */
    }

    LOGD("heartbeat thread process exit");
    return NULL;
}

int load_config()
{
    FILE *fp;
    mxml_node_t *xml_config = NULL;
    mxml_node_t *xml_sig_info;
    mxml_node_t *xml_sig_name, *xml_factor, *xml_offset;
    fp = fopen(SIG_CONFIG_FILE, "r");
    if(fp != NULL)
    {
        xml_config = mxmlLoadFile(NULL, fp, MXML_TEXT_CALLBACK);
        fclose(fp);
    }
    else
    {
        LOGD("loading signal from %s failed: %s", SIG_CONFIG_FILE, strerror(errno));
        return -1;
    }

    xml_sig_info = mxmlFindElement(xml_config, xml_config, "array","name", "sig_info", MXML_DESCEND);
    LOGD("load signal from %s:", SIG_CONFIG_FILE);
    for( ; xml_sig_info != NULL; xml_sig_info = mxmlFindElement(xml_sig_info, xml_config, "array","name", "sig_info", MXML_DESCEND))
    {
        xml_sig_name = mxmlFindElement(xml_sig_info, xml_config, "key","name", "sig_name", MXML_DESCEND);
        xml_factor = mxmlFindElement(xml_sig_info, xml_config, "key","name", "factor", MXML_DESCEND);
        xml_offset = mxmlFindElement(xml_sig_info, xml_config, "key","name", "offset", MXML_DESCEND);

        sig_info_t* temp_sig_info = (sig_info_t*)malloc(sizeof(sig_info_t));
        memset(temp_sig_info, 0, sizeof(sig_info_t));

        strcpy(temp_sig_info->sig_name, mxmlGetText(xml_sig_name, NULL));
        temp_sig_info->factor = atof(mxmlGetText(xml_factor, NULL));
        temp_sig_info->offset = atoi(mxmlGetText(xml_offset, NULL));
        temp_sig_info->value = 0;
        temp_sig_info->next = NULL;


#ifdef MQTT_DATA_DEBUG
        LOGD("sig_name = %s, factor = %f, offset = %d, value = %f",
             mxmlGetText(xml_sig_name, NULL), temp_sig_info->factor, temp_sig_info->offset, temp_sig_info->value);
#endif
        unsigned long key = sig_hash((unsigned char *)temp_sig_info->sig_name);

        insert_signode(key, temp_sig_info);
    }

#ifdef MQTT_DATA_DEBUG
    show_sigtable();
#endif

    mxmlDelete(xml_config);
    return 0;
}

int show_sigtable()
{
    int i;
    int cnt = 0;
    sig_info_t *tmp;
    for (i = 0; i < MAX_HASH_BUCKETS; i++) {
        tmp = sig_htable[i].head;
        if (tmp == NULL) {
            continue;
        }
        //LOGD("[%d] index=%d, number=%d", gettid(), i, sig_htable[i].cnt);
        cnt += sig_htable[i].cnt;
        while (tmp != NULL) {
            //LOGD("[%d] hash index=%d, signame:%s, factor=%f, offset=%d, real_value=%f",
             //     gettid(), i, tmp->sig_name, tmp->factor, tmp->offset, tmp->value);
            tmp = tmp->next;
        }
    }

    LOGD("[%d] total sig number=%d", gettid(), cnt);
    return 0;
}

char *invalid_sig[] = {
    "BMS_AlmUC",
    "HU_TimeSyncData",
    "BMS_CellTotalNum",
    "BMS_Temp_ProbeNum",
    "BMS_BattManuYear",
    "BMS_BattManuMonth",
    "BMS_BattManuDay",
    "BMS_BattCodeLenth",
    "BMS_BattCodeStartN",
    "BMS_BattCodeStartNCode",
    "BMS_BattCodeStartN_1Code",
    "BMS_BattCodeStartN_2Code",
    "BMS_BattCodeStartN_3Code",
    "BMS_BattCodeStartN_4Code",
    "BMS_BattCodeStartN_5Code",
    "BMS_BattSerialNumber"
};

int isinvalidsignal(const char *sig_name)
{
    int i;
    for (i = 0; i < NUM_ARRAY(invalid_sig); i++) {
        if (!strcmp(sig_name, invalid_sig[i])) {
            return 1;
        }
    }

    return 0;
}

sig_info_t * search_signode(unsigned long key, const char *sig_name)
{
    int index = key % MAX_HASH_BUCKETS;
    sig_info_t *tmp = sig_htable[index].head;

    if (isinvalidsignal(sig_name)) {
        return NULL;
    }

    if (sig_name == NULL) {
        return NULL;
    }

    if (tmp == NULL) {
        LOGD("[%d]the key of the node(name:%s) is not in the hash table!", gettid(), sig_name);
        return NULL;
    }

    while (tmp != NULL) {
        if (!strcmp(tmp->sig_name, sig_name)) {
            return tmp;
        }
        tmp = tmp->next;
    }

    LOGD("[%d]Did not found the signal:%s in the hash table!", gettid(), sig_name);
    return NULL;
}

int insert_signode(unsigned long key, sig_info_t *node)
{
    int index = key % MAX_HASH_BUCKETS;

    if (node == NULL) {
        LOGD("[%d]the node is empty!", gettid());
        return -1;
    }

    if (sig_htable[index].head == NULL) {
        sig_htable[index].head = node;
        sig_htable[index].cnt = 1;
        return 0;
    }

    node->next = sig_htable[index].head;
    sig_htable[index].head = node;
    sig_htable[index].cnt++;

    return 0;
}

int sig_hash_init()
{
    sig_htable = (struct sig_hash *)malloc(sizeof(struct sig_hash) * MAX_HASH_BUCKETS);
    if (sig_htable == NULL) {
        return -1;
    }

    return 0;
}

void* get_position_msg_thread(void* param)
{
    //UNUSED(param);
    int i;
    GpsClient cli;
    cli.start();

    struct timeval get_posi_start_time = {0, 0};
    long elapsed_time = 0;
    int wrap_cnt = 0;
    int cycle = 1000;

    while(doExit != 1)
    {
        LOGD("[%d]Get position ", gettid());
        get_posi_start_time = start_clock();

        GpsInfo info = cli.getGpsInfo();
        LOGD("[%d]gps position: valid %s, longitude %lld, latitude %lld, utc %lld, satellites %d\n", gettid(),
            info.valid_flag? "true": "false",
            info.longitude,
            info.latitude,
            info.utc,
            info.satellites_num);

        pthread_mutex_lock(&gps_info_mutex);
        memset(&g_gps_info, 0x0, sizeof(g_gps_info));
        if (info.valid_flag == 1) {
            g_gps_info.status = 0;
            memcpy(&g_gps_info.longtitude, &info.longitude, 4);
            memcpy(&g_gps_info.altitude, &info.altitude, 4);
        } else if (info.valid_flag == 0) {
            g_gps_info.status = 1;
        }
        pthread_mutex_unlock(&gps_info_mutex);

        elapsed_time = elapsed_clock(get_posi_start_time);
        if(elapsed_time >= cycle)
        {
            LOGD("[%d]Get position: no need to sleep(%dms-%ldms)", gettid(), cycle, elapsed_time);
            continue;
        }
        else
        {
            LOGD("[%d]Get position: sleep for %ldms(%d-%ld)", gettid(), (cycle - elapsed_time), cycle, elapsed_time);
            usleep((cycle - elapsed_time)*1000);
        }

    }

    cli.stop();
    LOGD("[%d]Get position thread exit", gettid());
    return NULL;
}


void get_position_msg()
{
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    pthread_t get_position_msg_tid;
    pthread_create(&get_position_msg_tid, &attr, get_position_msg_thread, NULL);
    pthread_attr_destroy (&attr);
}

void get_signal_msg()
{
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    pthread_t get_signal_msg_tid;
    pthread_create(&get_signal_msg_tid, &attr, parse_canmsg, NULL);
    pthread_attr_destroy (&attr);
}

void send_data_msg()
{
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    pthread_t get_signal_msg_tid;
    pthread_create(&get_signal_msg_tid, &attr, send_data_thread, NULL);
    pthread_attr_destroy (&attr);
}

void heartbeat_handle()
{
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    pthread_t get_signal_msg_tid;
    pthread_create(&get_signal_msg_tid, &attr, heartbeat_thread, NULL);
    pthread_attr_destroy (&attr);
}

FILE* open_write_file(char *fname, const char *mode) {
    FILE *fp;
    fp = fopen(fname, mode);
    if (!fp) {
        LOGD("[%d]Failed to open the file %s", gettid(), SEVEN_DAYS_FILE);
        return NULL;
    }

    return fp;
}

int do_retrans_action()
{
    int rc;
    FILE *fp = open_write_file(COMM_FAILED_FILE, "rb");
    if (!fp) {
        LOGD("[%d]Failed to open file %s\n", gettid(), COMM_FAILED_FILE);
        return -1;
    }
    uint16_t len;
    char head[24];
    int total_len;
    char total[10 * 1024];

    while (fread(head, sizeof(head), 1, fp) == 1) {
        len = (uint16_t)ntohs(*(uint16_t *)(head+22));
        if (len <= 0) {
            LOGD("[%d]The payloadlen is negative or zero\n", gettid());
            break;
        }
        len += PKG_COMMON_LEN;
        unsigned char *tmp_msg = (unsigned char*)malloc(len);
        fread(tmp_msg, len, 1, fp);

        if (tmp_msg != NULL) {
            if (tmp_msg[24] == 0x02) { /* change realtime report to reissue type */
                tmp_msg[24] = 0x03;
            }
            LOGD("[%d]Finish the update the newest token and the type", gettid());
        }

        LOGD("[%d]publish retrans msg begin ...", gettid());
        rc = send(rinfo.sock_fd, tmp_msg, len, 0);
        if (rc <= 0) {
            LOGD("[%d] Failed to send the package len=%d rc=%d, errno=%s", gettid(), len, rc, strerror(errno));
            if (tmp_msg) {
                free(tmp_msg);
            }
            continue;
        }
        LOGD("[%d]publish retrans msg end ...", gettid());

        total_len = recv_timeout(rinfo.sock_fd, 1, total);
        if (total_len <= 0) {
            LOGD("[%d] No response from server, check passed", gettid());
        } else if (total_len > 0 && total[3] != 0x0) {
            gb_handle_response(total);
        }


        if (tmp_msg) {
            free(tmp_msg);
        }
    }

    fclose(fp);
    return 0;
}

void* write_file_thread(void*)
{
    int rc = 0;
    struct timeval start_time = {0, 0};
    long elapsed_time = 0;
    long sz;
    char *tmp_msg = NULL;
    FILE *seven_fp;
    FILE *comm_fp;
    uint16_t len;

    while (doExit != 1) {
        start_time = start_clock();
        seven_fp = open_write_file(SEVEN_DAYS_FILE, "a+b");
        comm_fp = open_write_file(COMM_FAILED_FILE, "a+b");
        if (!seven_fp || !comm_fp) {
            LOGD("[%d] seven_fp=%s comm_fp=%s", gettid(), seven_fp ? "NOT NULL":"NULL",
                 comm_fp ? "NOT NULL":"NULL");
            break;
        }

        pthread_mutex_lock(&ringbuf_mutex);
        while (ring_pop(&save_ring, (void **)&tmp_msg) != -1) {
            sz = ftell(seven_fp);
            if (sz >= 1 * 1024 * 1024 * 1024) {
                //TODO need to add rotate mode in future
                fclose(seven_fp);
                //do_compress(SEVEN_DAYS_FILE);
                seven_fp = open_write_file(SEVEN_DAYS_FILE, "a+b");
                if (!seven_fp) {
                    break;
                }
            }

            len = (uint16_t)ntohs(*(uint16_t *)(tmp_msg+22));
            len += PKG_COMMON_LEN;
            rc = fwrite(tmp_msg, len, 1, seven_fp);
            fflush(seven_fp);
            LOGD("[%d]write 7day.file, size=%d, sz=%ld ", gettid(), len, sz);

            if (tmp_msg) {
                free(tmp_msg);
            }
        }
        pthread_mutex_unlock(&ringbuf_mutex);

        pthread_mutex_lock(&commf_ring_mutex);
        while (ring_pop(&commf_ring, (void **)&tmp_msg) != -1) {
            sz = ftell(comm_fp);
            if (sz >= 1 * 512 * 1024 * 1024) {
                //TODO need to add rotate mode in future
                fclose(comm_fp);
                //do_compress(COMM_FAILED_FILE);
                comm_fp = open_write_file(COMM_FAILED_FILE, "a+b");
                if (!comm_fp) {
                    break;
                }
            }
            len = (uint16_t)ntohs(*(uint16_t *)(tmp_msg+22));
            len += PKG_COMMON_LEN;
            rc = fwrite(tmp_msg, len, 1, seven_fp);
            fflush(seven_fp);
            LOGD("[%d]write 7day.file, size=%d, sz=%ld ", gettid(), len, sz);

            LOGD("[%d] write data to the comm_failed.file(sz=%ld), writesize=%d ", gettid(), sz,
                 len);
            fflush(comm_fp);
            if (tmp_msg) {
                free(tmp_msg);
            }
        }
        pthread_mutex_unlock(&commf_ring_mutex);

        fclose(seven_fp);
        fclose(comm_fp);

        if ((g_comm_failed_issued == 1) && (g_reconnect_status == 1)) {
            rc = do_retrans_action();
            if (rc) {
                LOGD("[%d]Failed to do_returans_action\n", gettid());
            } else {
                LOGD("[%d]Do_returans_action successfully\n", gettid());
                remove(COMM_FAILED_FILE);
                pthread_mutex_lock(&comm_failed_mutex);
                g_comm_failed_issued = 0;
                g_reconnect_status = 0;
                pthread_mutex_unlock(&comm_failed_mutex);
            }
        }

        elapsed_time = elapsed_clock(start_time);
        LOGD("[%d]write_file_thread: sleep for %ldms(%dms-%ldms)", gettid(), (WRITE_FILE_PERIOD - elapsed_time), WRITE_FILE_PERIOD, elapsed_time);
        usleep((WRITE_FILE_PERIOD - elapsed_time) * 1000);
    }

    return NULL;
}

void write_file()
{
    pthread_attr_t attr;
    pthread_attr_init (&attr);
    pthread_attr_setdetachstate (&attr, PTHREAD_CREATE_DETACHED);

    pthread_t write_file_tid;
    pthread_create(&write_file_tid, NULL, write_file_thread, NULL);
    pthread_attr_destroy (&attr);
}

int gb_rinfo_init()
{
    char value[PROPERTY_VALUE_MAX];
    int len;

    memset(ser_info.server_ip_s, 0x0, sizeof(ser_info.server_ip_s));
    memset(ser_info.server_port_s, 0x0, sizeof(ser_info.server_port_s));
    snprintf(ser_info.server_ip_s, sizeof(ser_info.server_ip_s), "%s", "120.35.20.145");
    snprintf(ser_info.server_port_s, sizeof(ser_info.server_port_s), "%s", "10369");
    ser_info.server_ip = inet_addr(ser_info.server_ip_s);
    ser_info.port = htons(atoi(ser_info.server_port_s));

    rinfo.sock_fd = gb_create_link();
    if (rinfo.sock_fd < 0) {
        LOGD("[%d]Failed to create the socket!", gettid());
        return -1;
    }

    len = android_property_get("persist.sys.le.vin", value, NULL);
    if (len > 0) {
        memset(rinfo.vin, 0x0, 17);
        memcpy(rinfo.vin, value, 17);
    }

    return 0;
}

int gb_logout()
{
    char buf[8];
    char *pkg;
    int len;
    int rc;
    gb_time_t date;
    uint16_t tmp;

    gb_getdate(&date);
    memset(buf, 0x0, sizeof(buf));
    memcpy(buf, &date, 6);
    tmp = htons(login_seq_num);
    memcpy(buf+6, &tmp, 2);
    pkg = gb_construct_package(VEHICLE_LOGOUT, buf, 8);
    len = 8 + PKG_COMMON_LEN;
    rc = send(rinfo.sock_fd, pkg, len, 0);
    if (rc <= 0) {
        LOGD("[%d] Failed to send the package len=%d rc=%d, errno=%s", gettid(), len, rc, strerror(errno));
    } else {
        LOGD("[%d] Send the logout successfully, send len=%d", gettid(), rc);
    }

    if (pkg) {
        free(pkg);
    }

    return 0;
}

int gb_login()
{
    char buf[512];
    char total[128];
    uint16_t len = 0;
    gb_time_t date;
    int recharge_subsystem_number = 1;
    char *pkg;
    int rc;
    int total_len;
    static int login_cnt = 0;
    //char iccid[20];
    //char iccid[21] = {"898602B3011630051910"};
    char *iccid = "898602B3011630051910";
    uint16_t tmp;

    //len = android_property_get("persist.sys.le.sim_iccid", iccid, NULL);
    len = android_property_get("persist.sys.le.sim_iccid", rinfo.iccid, NULL);
    memset(rinfo.iccid, 0x0, sizeof(rinfo.iccid));
    if (len > 0) {
        //memcpy(rinfo.iccid, iccid, sizeof(iccid));
    }
    memcpy(rinfo.iccid, iccid, sizeof(rinfo.iccid));

    login_seq_num++;

tryagain:
    if (login_cnt >= 3) {
    //if (login_cnt >= 300) {
        LOGD("[%d] Failed to do login action!", gettid());
        return -1;
    }
    gb_getdate(&date);
    memset(buf, 0x0, sizeof(buf));
    memcpy(buf, &date, 6);
    tmp = htons(login_seq_num);
    memcpy(buf+6, &tmp, 2);
    memcpy(buf+8, rinfo.iccid, sizeof(rinfo.iccid));
    memcpy(buf+28, &recharge_subsystem_number, 1);
    memcpy(buf+29, &g_batt_info.len, 1);
    memcpy(buf+30, &g_batt_info.buf, g_batt_info.len);

    pkg = gb_construct_package(VEHICLE_LOGIN, buf, 30+g_batt_info.len);
    len = 30 + g_batt_info.len + PKG_COMMON_LEN;

    rc = send(rinfo.sock_fd, pkg, len, 0);
    if (rc <= 0) {
        LOGD("[%d] Failed to send the package len=%d rc=%d, errno=%s", gettid(), len, rc, strerror(errno));
        if (pkg) {
            free(pkg);
        }
        login_cnt++;
        //sleep(60);
        sleep(3);
        goto tryagain;
        //return rc;
    }

    LOGD("[%d] Send the login info successfully, send len=%d", gettid(), rc);

    total_len = recv_timeout(rinfo.sock_fd, 1, total);
    if (total_len <= 0) {
        LOGD("[%d] login action, No response from server.", gettid());
        login_cnt++;
        if (pkg) {
            free(pkg);
        }
        //sleep(60);
        sleep(3);
        goto tryagain;
    } else if (total_len > 0 && total[3] == 0x01) {
        LOGD("[%d] login action successful! ", gettid());
    } else if (total_len > 0 && total[3] != 0x00) {
        LOGD("[%d] login failed, return code=%x! ", gettid(), total[3]);
        login_cnt++;
        if (pkg) {
            free(pkg);
        }
        //sleep(60);
        sleep(3);
        goto tryagain;
    }

    if (pkg) {
        free(pkg);
    }

    return 0;
}

void handle_signal(int signal)
{
    const char *signal_name;
    sigset_t pending;
    switch (signal) {
        case SIGUSR1:
        case SIGHUP:
            LOGD("got the signal %d", signal);
            break;
        case SIGINT:
        case SIGTERM:
            LOGD("got the SIGTERM signal,  do logout action");
            gb_logout();
            ring_free();
            doExit = 1;
            break;
        case SIGPIPE:
            LOGD("got the pipe signal %d", signal);
            do_reconnect();
        default:
            LOGD("Caught wrong signal: %d\n ", signal);
    }

    return;
}

void signal_init()
{
    struct sigaction sa;
    sa.sa_handler = &handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGINT, &sa, NULL) == -1) {
        LOGD("Error: cannot handle SIGHUP");
    }

    if (sigaction(SIGUSR1, &sa, NULL) == -1) {
        LOGD("Error: cannot handle SIGUSR1");
    }

    if (sigaction(SIGHUP, &sa, NULL) == -1) {
        LOGD("Error: cannot handle SIGHUP");
    }

    if (sigaction(SIGTERM, &sa, NULL) == -1) {
        LOGD("Error: cannot handle SIGTERM");
    }

    if (sigaction(SIGPIPE, &sa, NULL) == -1) {
        LOGD("Error: cannot handle SIGTERM");
    }

    return ;
}

int start_selink()
{
    signal_init();
    sig_hash_init();
    load_config();
    gb_rinfo_init();
    ring_init();
    gb_get_input();
    get_signal_msg();
    //gb_login();
    send_data_msg();
    //heartbeat_handle();
    write_file();

    return 0;
}

int main()
{
	sp<ProcessState> proc(ProcessState::self());

    start_selink();

    LOGD("[%d]started second link!", gettid());
	ProcessState::self()->startThreadPool();
	IPCThreadState::self()->joinThreadPool();
    return 0;
}
