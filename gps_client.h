/*
 * gps_client.h - define GpsClient class.
 *
 * GpsClient class can connect to the gps framework thread,
 * and receive the gps location information and gps satellites
 * information from gps framework thread. It also provides
 * this information to user.
 *
 * Author: shanjiejing
 */
#ifndef _GPS_CLIENT_H_
#define _GPS_CLIENT_H_

#include <pthread.h>
#include "gps.h"
#include "gps_info.h"
#include <map>
#include <string>

using namespace std;

class GpsClient {
    public :
        GpsClient();
        ~GpsClient();
        int start();
        int stop();
        GpsInfo getGpsInfo();
        //void* workThread(void*);
        GpsLocation parseLocation(char* buf, int size);
        GpsSvStatus parseSatellites(char* buf, int size);
        void updateGpsInfo(GpsLocation loc);
        void updateGpsInfoSv();
        void mergeSvStatus(GpsSvStatus sv);
        void setSvStatus(GpsSvStatus sv);
        map<string, string> parse(const char* buf, int size) const;
        bool shouldStop() const;

        static const char* const SERVICE_PATH;
        static const char* const CLIENT_PATH;
    private:
        static GpsInfo mGpsInfo;
        GpsSvStatus mSv;
        static pthread_mutex_t mMutex;
        pthread_t mHandle;
        volatile bool mStopFlag;
};

#endif //_GPS_CLIENT_H_
