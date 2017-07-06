#ifndef __GB_P_H__
#define __GB_P_H__
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#define VEHICLE_LOGIN       (0x01)
#define REALTIME_REPORT     (0x02)
#define RETRANS_REPORT      (0x03)
#define VEHICLE_LOGOUT      (0x04)
#define CLOUD_HEARBEAT      (0x07)

#define PKG_COMMON_LEN      (25)

#define NUM_ARRAY(a)  (sizeof(a) / sizeof(a)[0])
#define MAX_SIG_NAME_LEN    (32)
#define MAX_HASH_BUCKETS    (512)

#define SIG_CONFIG_FILE "/system/etc/DK01_cfg_RT_sig_change.xml"
#define MQTT_DATA_DEBUG
typedef enum
{
    SIG_INT,
    SIG_DOUBLE
} SIG_TYPE;


typedef struct gb_time_s {
    char year;
    char month;
    char day;
    char hour;
    char minute;
    char second;
}__attribute__((packed, aligned(1))) gb_time_t;

typedef struct gb_server_info_s {
    char server_ip_s[17];
    char server_port_s[6];
    uint32_t server_ip;
    uint16_t port;
} gb_server_info_t;

typedef struct gb_rinfo_s {
    int sock_fd;
    char vin[17];
    char iccid[20];
} gb_rinfo_t;

typedef struct sig_info_s {
    char sig_name[MAX_SIG_NAME_LEN];
    double factor;
    int offset;
    double value;
    struct sig_info_s *next;
} sig_info_t;

struct sig_hash {
    sig_info_t *head;
    int cnt;
};

int insert_signode(unsigned long key, sig_info_t *node);
int sig_hash_init();
sig_info_t * search_signode(unsigned long key, const char *sig_name);
int show_sigtable();

#endif
