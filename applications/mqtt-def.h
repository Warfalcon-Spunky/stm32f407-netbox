
/* 设备服务调用主题索引值 */
#define ALI_SERVICE_DOOR_CTRL_SUB             	"Topic-ServiceDoorCtrlSub"
#define ALI_SERVICE_DOOR_CTRL_REPLY_PUB       	"Topic-ServiceDoorCtrlReplyPub"
#define ALI_SERVICE_DEVICE_CTRL_SUB           	"Topic-ServiceDeviceCtrlSub"
#define ALI_SERVICE_DEVICE_CTRL_REPLY_PUB     	"Topic-ServiceDeviceCtrlReplyPub"
/* 设备时间上报主题索引值 */
#define ALI_EVENT_DEVICE_ALARM_PUB        		"Topic-EventDeviceAlarmPub"
#define ALI_EVENT_DEVICE_ALARM_REPLY_SUB    	"Topic-EventDeviceAlarmReplySub"
#define ALI_EVENT_DEVICE_ERROR_PUB        		"Topic-EventDeviceErrorPub"
#define ALI_EVENT_DEVICE_ERROR_REPLY_SUB    	"Topic-EventDeviceErrorReplySub"

/* 设备属性上报主题索引值 */
#define ALI_PROPERTY_POST_PUB                	"Topic-PropertyPostPub"
#define ALI_PROPERTY_POST_REPLY_SUB           	"Topic-PropertyPostReplySub"
/* 设备属性设置主题索引值 */
#define ALI_PROPERTY_SET_SUB                  	"Topic-PropertySetSub"
#define ALI_PROPERTY_SET_REPLY_PUB            	"Topic-PropertySetReplyPub"
/* 设备标签上报主题索引值 */
#define ALI_DEVICEINFO_UPDATE_PUB             	"Topic-DeviceInfoUpdatePub"
#define ALI_DEVICEINFO_UPDATE_REPLY_SUB       	"Topic-DeviceInfoUpdateReplySub"

#define ALI_DEVICE_INFO_NAME					"Device-info"
#define ALI_DEVICE_INFO_DEFAULT					"Radiation.Corp"

#define ALI_DEVICE_NUM							"Device-num"
#define ALI_DEVICE_NUM_DEFAULT					2
#define ALI_DEVICE_CHN_NUM						"Device-chn_num"
#define ALI_DEVICE_CHN_NUM_DEFAULT				16

#define ALI_CODE_OK								"200"
#define ALI_CODE_DOOR_CTRL_FAIL					"100000"
#define ALI_CODE_DEV_CTRL_FAIL					"100001"
#define ALI_CODE_PROPERTY_ERROR					"100002"
#define ALI_CODE_DEVICE_CTRL_ERROR				"100003"

#define ALI_ALARM_TYPES							3
#define ALI_ALARM_TIMEOUT						0x0001
#define ALI_ALARM_ILLEGAL						0x0002

#define MQTT_OTA_DOWNLOAD_PARTITION_NAME		"dl-area"
#define MQTT_OTA_VERSION_MAXLEN					33
#define MQTT_OTA_FIRMWARE_VERSION				"Firmware-Version"


