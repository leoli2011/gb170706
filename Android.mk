LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)


LOCAL_SHARED_LIBRARIES += libDataReportInterface
LOCAL_SHARED_LIBRARIES += libstlport libstdc++

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../libs/data_report \
					vendor/letv/proprietary/cloudclient/libs/mqtt3.1.1 \
                    frameworks/base/libs/carinfo_client \
                    vendor/letv/proprietary/cloudclient/libs/mxml-2.9 \
                    external/stlport/stlport/ \
		            bionic/libstdc++/include/ \
                    bionic/ \
                    frameworks/base/libs/carinfo_client

LOCAL_SRC_FILES :=  gb_selink.cpp \
					gps_client.cpp


########################
LOCAL_MODULE := gb
LOCAL_MODULE_TAGS := optional

LOCAL_SHARED_LIBRARIES += liblog \
						  libbinder \
						  libcutils \
						  libutils \
						  libmxml \
                          libInfoCtrlClient
#						  libmqtt \
#						  liblog \
#						  libstlport \
#						  libstdc++ \


#LOCAL_CFLAGS := -DMQTT_DATA_DEBUG #-DOPENSSL
include $(BUILD_EXECUTABLE)
#include $(BUILD_SHARED_LIBRARY)

########################
