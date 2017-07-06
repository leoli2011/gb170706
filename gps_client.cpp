/*
 * gps_client.cpp - impletement GpsClient class.
 *
 * Author: shanjiejing
 */

#include "gps_client.h"
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/select.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "veh_log.h"

GpsInfo GpsClient::mGpsInfo;
pthread_mutex_t GpsClient::mMutex = PTHREAD_MUTEX_INITIALIZER;
const char* const GpsClient::SERVICE_PATH = "/data/gps_server";
const char* const GpsClient::CLIENT_PATH = "/data/gps_client";

static void* workThread(void*);

GpsClient::GpsClient() {
    mHandle = 0;
    mStopFlag = false;
    memset(&mSv, 0, sizeof(mSv));
}

GpsClient::~GpsClient() {
    if (mHandle != 0) stop();
}

int GpsClient::start() {
    return pthread_create(&mHandle, NULL, workThread, (void*) this);
}

int GpsClient::stop() {
    mStopFlag = true;
    sleep(1);
    pthread_join(mHandle, NULL);
    return 0;
}

map<string, string> GpsClient::parse(const char* buf, int size) const{
    map<string, string> ret;
    char* pLeft = NULL;
    char* pRight = NULL;
    char* pEqu = NULL;
    bool inBracket = false;
    //printf("shjj: in parse\n");

    if(NULL == buf || 0 >= size) return ret;

    pLeft = (char*)buf;
    pRight = (char*)buf;

    while((size > pLeft - buf) && (*pRight != '\n')) {
        //printf("shjj: right is %c,%p\n", *pRight, pRight);
        if (*pRight != ';' || inBracket) {
            if (*pRight == '[') inBracket = true;
            if (*pRight == ']') inBracket = false;
            pRight ++;
            continue;
        }

        //printf("shjj: left is %c,%p\n", *pLeft, pLeft);
        for (pEqu = pLeft; pEqu != pRight; pEqu ++) {
            if (*pEqu == '=') break;
        }
        //printf("find '=' on %p, right is %p\n", pEqu, pRight);
        if (pEqu != pRight) {
            string key(pLeft, pEqu - pLeft);
            string value(pEqu + 1, pRight - pEqu - 1);
            //printf("shjj: set %s -> %s", key.c_str(), value.c_str());
            ret[key] = value;
        }

        pLeft = pRight + 1;
        pRight = pLeft;
    }

    /*for (map<string, string>::iterator it = ret.begin();
            it != ret.end();
            it ++) {
        printf("shjj: in parse: [%s] = %s\n", it->first.c_str(), it->second.c_str());
    }

    printf("shjj:---------------------\n");*/
    return ret;
}

GpsLocation GpsClient::parseLocation(char* buf, int size) {
    map<string, string> kv;
    GpsLocation loc;

    if (NULL == buf || 0 >= size) return loc;
    kv = parse(buf, size);
    if (0 >= kv.size()) return loc;
    loc.size = sizeof(loc);

    if (!kv["flags"].empty()) loc.flags = atoi(kv["flags"].c_str());
    if (!kv["latitude"].empty()) loc.latitude = atof(kv["latitude"].c_str());
    if (!kv["longitude"].empty())
        loc.longitude = atof(kv["longitude"].c_str());
    if (!kv["altitude"].empty())
        loc.altitude = atof(kv["altitude"].c_str());
    if (!kv["speed"].empty())
        loc.speed = atof(kv["speed"].c_str());
    if (!kv["bearing"].empty())
        loc.bearing = atof(kv["bearing"].c_str());
    if (!kv["timestamp"].empty())
        loc.timestamp = atoll(kv["timestamp"].c_str());

    return loc;
 }

GpsSvStatus GpsClient::parseSatellites(char* buf, int size) {
    map<string, string> kv;
    GpsSvStatus status;

    if (NULL == buf || 0 >= size) return status;

    kv = parse(buf, size);
    if (0 >= kv.size()) return status;
    status.size = sizeof(status);

    if (!kv["ephemeris_mask"].empty())
        status.ephemeris_mask = atoi(kv["ephemeris_mask"].c_str());
    if (!kv["almanac_mask"].empty())
        status.almanac_mask = atoi(kv["almanac_mask"].c_str());
    if (!kv["used_in_fix_mask"].empty())
        status.used_in_fix_mask = atoi(kv["used_in_fix_mask"].c_str());
    if (!kv["num_svs"].empty())
        status.num_svs = atoi(kv["num_svs"].c_str());
    char key[10] = "\0";
    for (int i = 0; i < status.num_svs; i++) {
        sprintf(key, "sv%d", i);
        string svstr = kv[string(key)];
        if (svstr.empty()) continue;
        svstr = svstr.substr(1, svstr.size() - 2);
        map<string, string> svkv;
        svkv = parse(svstr.c_str(), svstr.size());
        if (svkv.empty()) continue;
        status.sv_list[i].size = sizeof(status.sv_list[i]);
        status.sv_list[i].prn = atoi(svkv["prn"].c_str());
        status.sv_list[i].snr = atof(svkv["snr"].c_str());
        status.sv_list[i].elevation = atof(svkv["elevation"].c_str());
        status.sv_list[i].azimuth = atof(svkv["azimuth"].c_str());
    }

    return status;
}

bool GpsClient::shouldStop() const {
    return mStopFlag;
}

static void* workThread(void* arg) {
    GpsClient *cli = (GpsClient*)arg;
    int clientSock = 0;
    struct sockaddr_un cliSockAddr;
    struct sockaddr_un srvSockAddr;
    int len = 0;
    char buffer[2000] = "\0";
    fd_set rdst;

    clientSock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (0 >= clientSock) {
        LOGD("shjj: create socket fail. %d, %s\n", clientSock, strerror(errno));
        return NULL;
    }

    unlink(GpsClient::CLIENT_PATH);
    cliSockAddr.sun_family = AF_UNIX;
    strcpy(cliSockAddr.sun_path, GpsClient::CLIENT_PATH);
    len = sizeof(cliSockAddr);
    int ret = ::bind(clientSock, (struct sockaddr*)&cliSockAddr, len);
    if (0 != ret) {
        LOGD("shjj: bind fail. %d\n", ret);
        close(clientSock);
        unlink(GpsClient::CLIENT_PATH);
        return NULL;
    }

    srvSockAddr.sun_family = AF_UNIX;
    strcpy(srvSockAddr.sun_path, GpsClient::SERVICE_PATH);

    ret = connect(clientSock, (struct sockaddr*)&srvSockAddr, len);
    if (0 != ret) {
        LOGD("shjj: connect failed! %d: %s\n", errno, strerror(errno));
        close(clientSock);
        unlink(GpsClient::CLIENT_PATH);
        return NULL;
    }
    LOGD("shjj: connect success %d\n", ret);

    while (!cli->shouldStop()) {
        LOGE("in while %d\n", cli->shouldStop());
        FD_ZERO(&rdst);
        FD_SET(clientSock, &rdst);
        struct timeval tv;
        tv.tv_sec = 10;
        tv.tv_usec = 0;

        ret = select(clientSock + 1, &rdst, NULL, NULL, &tv);
        if (ret > 0) {
            if (FD_ISSET(clientSock, &rdst)) {
                ret = recv(clientSock, buffer, 2000, 0);
                //printf("shjj: recv return %d\n", ret);
                if (0 >= ret) continue;
                buffer[ret] = '\0';
                //printf("shjj: recv num %d, info:%s\n", ret, buffer);
                LOGD("shjj: buffer[5] is %c, len %d\n", buffer[5], ret);

                if (buffer[5] == 'L') {
                    GpsLocation loc = cli->parseLocation(buffer, ret);
                    cli->updateGpsInfo(loc);
                }
                if (buffer[5] == 'S') {
                    printf("shjj: buffer[6] is %c\n", buffer[6]);
                    GpsSvStatus status = cli->parseSatellites(buffer, ret);
                    //cli->updateGpsInfo(status);
                    /*if (buffer[6] == 'C')
                        cli->updateGpsInfo(status, true);
                    else if (buffer[6] == ';')
                        cli->updateGpsInfo(status, false);*/
                    if (buffer[6] == 'S')
                        cli->setSvStatus(status);
                    else if (buffer[6] == 'C')
                        cli->mergeSvStatus(status);
                    else if (buffer[6] == 'E') {
                        cli->mergeSvStatus(status);
                        cli->updateGpsInfoSv();
                    } else if (buffer[6] == 'A') {
                        cli->setSvStatus(status);
                        cli->updateGpsInfoSv();
                    }
                }
                //sleep(5);
            } else {
                LOGD("shjj: client socket cann't read\n");
            }
        }
        LOGE("exit this while.\n");
    }
    LOGE("exit\n");

    close(clientSock);
    unlink(GpsClient::CLIENT_PATH);
    return NULL;
}

void GpsClient::updateGpsInfo(GpsLocation loc) {
    if (loc.size != sizeof(loc)) return ;

    pthread_mutex_lock(&mMutex);
    if (0 != (loc.flags & GPS_LOCATION_HAS_LAT_LONG)) {
        mGpsInfo.valid_flag = true;
        //mGpsInfo.longitude = loc.longitude * 10000 * 3600;
        mGpsInfo.longitude = loc.longitude * 1e6;
        //mGpsInfo.latitude = loc.latitude * 10000 * 3600;
        mGpsInfo.latitude = loc.latitude * 1e6;
    }

    if (0 != (loc.flags & GPS_LOCATION_HAS_ALTITUDE)) {
        mGpsInfo.altitude = loc.altitude * 100;
        mGpsInfo.flags |= GpsInfo::HAS_ALTITUDE_FLAG;
    }

    if (0 != (loc.flags & GPS_LOCATION_HAS_SPEED)) {
        mGpsInfo.speed = loc.speed * 100;
        mGpsInfo.flags |= GpsInfo::HAS_SPEED_FLAG;
    }

    mGpsInfo.utc = loc.timestamp;
    mGpsInfo.flags |= GpsInfo::HAS_UTC_FLAG;
    pthread_mutex_unlock(&mMutex);
}

void GpsClient::updateGpsInfoSv() {
    printf("in updateGpsInfo, sv num %d\n", mSv.num_svs);
    pthread_mutex_lock(&mMutex);
    mGpsInfo.satellites_num = mSv.num_svs;
    mGpsInfo.mode = GpsInfo::GNSS_HYBRID;
    pthread_mutex_unlock(&mMutex);
    memset(&mSv, 0, sizeof(mSv));
}

void GpsClient::mergeSvStatus(GpsSvStatus status) {
    printf("shjj: in mergeSvStatus\n");
    if (mSv.num_svs + status.num_svs > GPS_MAX_SVS) return;
    pthread_mutex_lock(&mMutex);
    for (int i = 0; i < status.num_svs; i++) {
        mSv.sv_list[mSv.num_svs + i] = status.sv_list[i];
    }
    mSv.num_svs += status.num_svs;

    pthread_mutex_unlock(&mMutex);
}

void GpsClient::setSvStatus(GpsSvStatus sv) {
    printf("shjj: in setSvStatus\n");
    pthread_mutex_lock(&mMutex);
    mSv = sv;
    pthread_mutex_unlock(&mMutex);
}

GpsInfo GpsClient::getGpsInfo() {
    GpsInfo tmp;
//    LOGE("in getGpsInfo\n");
    pthread_mutex_lock(&mMutex);
    tmp = mGpsInfo;
    pthread_mutex_unlock(&mMutex);

//    LOGE("exit getGpsInfo\n");
    return tmp;
}
