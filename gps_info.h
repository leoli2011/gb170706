/*
 * gps_info.h - define gps information class.
 *
 * Author: shanjiejing
 */
#ifndef _GPS_INFO_H_
#define _GPS_INFO_H_

class GpsInfo {
    public:
        GpsInfo() {
            longitude = 0;
            latitude = 0;
            speed = 0;
            direction = 0;
            altitude = 0;
            hdop = 0;
            utc = 0;
            satellites_num = 0;
            satellites_bei_num = 0;
            satellites_gl_num = 0;
            satellites_ga_num = 0;
            mode = GNSS_HYBRID;
            valid_flag = false;
            flags = 0;
        }
        ~GpsInfo() {}
        long long longitude;
        long long latitude;
        int speed;
        int direction;
        int altitude;
        int hdop;
        long long utc;
        int satellites_num;
        int satellites_bei_num;
        int satellites_gl_num;
        int satellites_ga_num;
        enum LOCATION_MODE {
        GNSS_GPS = 1,
        GNSS_BEIDOU,
        GNSS_GLONASS,
        GNSS_GALILEO,
        GNSS_HYBRID,
        WLAN_WIFI,
        CELL_TRACK,
        INERTIAL,
        };
        LOCATION_MODE mode;
        bool valid_flag;
        unsigned int flags;

        // define flags
        const static unsigned int HAS_SPEED_FLAG = 0x01;
        const static unsigned int HAS_DIRECTION_FLAG = (0x01 << 1);
        const static unsigned int HAS_ALTITUDE_FLAG = (0x01 << 2);
        const static unsigned int HAS_HDOP_FLAG = (0x01 << 3);
        const static unsigned int HAS_UTC_FLAG = (0x01 << 4);
        const static unsigned int HAS_SA_NUM_FLAG = (0x01 << 5);
        const static unsigned int HAS_SA_BEI_NUM_FLAG = (0x01 << 6);
        const static unsigned int HAS_SA_GL_NUM_FLAG = (0x01 << 7);
        const static unsigned int HAS_SA_GA_NUM_FLAG = (0x01 << 8);
        const static unsigned int HAS_MODE_FLAG = (0x01 << 9);
};

#endif //_GPS_INFO_H_
