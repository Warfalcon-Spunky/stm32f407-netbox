/*
 * Copyright (c) 2006-2018 RT-Thread Development Team. All rights reserved.
 * License-Identifier: Apache-2.0
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may
 * not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include "iot_import.h"
#include "iot_export.h"

#include "rtthread.h"
#include "cJSON.h"
#include "easyflash.h"
#include "mqtt-def.h"

#if !defined(RT_USING_NETDEV)
#error "This RT-Thread version is older, please check and updata laster RT-Thread!"
#else
#include <arpa/inet.h>
#include <netdev.h>
#endif /* RT_USING_NETDEV */



#define LOG_TAG              "ali-sdk"    
#define LOG_LVL              LOG_LVL_DBG
#include <ulog.h>

#define MQTT_MSGLEN                             (1024)
#define MQTT_KEEPALIVE_INTERNAL                 (120)
#define MQTT_TOPIC_MAX_SIZE						(128)

static void *mqtt_device_hd = RT_NULL;
static uint8_t mqtt_is_running;
static rt_uint8_t mqtt_timer_cnt;


static void ali_mqtt_property_set_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_door_ctrl_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_device_ctrl_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_alarm_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_device_error_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_property_post_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);
static void ali_mqtt_device_info_update_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg);

typedef struct
{
    const char *topic_filter;
    iotx_mqtt_qos_t qos;
    iotx_mqtt_event_handle_func_fpt topic_handle_func;
    void *pcontext;
} mqtt_subscribe_item, *mqtt_subscribe_item_t;

static const mqtt_subscribe_item mqtt_sub_item[] = 
{    
    {ALI_SERVICE_DOOR_CTRL_SUB,            IOTX_MQTT_QOS1, ali_mqtt_door_ctrl_msg_arrive,          RT_NULL},
    {ALI_SERVICE_DOOR_CTRL_REPLY_PUB,      IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    
    {ALI_SERVICE_DEVICE_CTRL_SUB,          IOTX_MQTT_QOS1, ali_mqtt_device_ctrl_msg_arrive,        RT_NULL},
    {ALI_SERVICE_DEVICE_CTRL_REPLY_PUB,    IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},

	{ALI_EVENT_DEVICE_ALARM_PUB,           IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_EVENT_DEVICE_ALARM_REPLY_SUB,     IOTX_MQTT_QOS1, ali_mqtt_alarm_msg_arrive,      RT_NULL},

    {ALI_EVENT_DEVICE_ERROR_PUB,           IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_EVENT_DEVICE_ERROR_REPLY_SUB,     IOTX_MQTT_QOS1, ali_mqtt_device_error_msg_arrive,       RT_NULL},

	{ALI_PROPERTY_SET_SUB,                 IOTX_MQTT_QOS1, ali_mqtt_property_set_msg_arrive,       RT_NULL},    
    {ALI_PROPERTY_SET_REPLY_PUB,           IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},

    {ALI_PROPERTY_POST_PUB,                IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_PROPERTY_POST_REPLY_SUB,          IOTX_MQTT_QOS1, ali_mqtt_property_post_msg_arrive,      RT_NULL},

    {ALI_DEVICEINFO_UPDATE_PUB,            IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_DEVICEINFO_UPDATE_REPLY_SUB,      IOTX_MQTT_QOS1, ali_mqtt_device_info_update_msg_arrive, RT_NULL}
};

static rt_uint8_t device_num;
static rt_uint8_t device_chn_num;

static rt_sem_t device_info_up_sem;


static void ali_mqtt_event_handle(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
    if (topic_info == NULL)
    {
        rt_kprintf("Topic info is null! Exit.");
        return;
    }
    uintptr_t packet_id = (uintptr_t)topic_info;

    switch (msg->event_type) 
    {
        case IOTX_MQTT_EVENT_UNDEF:
            LOG_D("undefined event occur.");
            break;

        case IOTX_MQTT_EVENT_DISCONNECT:
            LOG_D("MQTT disconnect.");
            break;
        case IOTX_MQTT_EVENT_RECONNECT:
            LOG_D("MQTT reconnect.");
            break;

        case IOTX_MQTT_EVENT_SUBCRIBE_SUCCESS:
            LOG_D("subscribe success, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_SUBCRIBE_TIMEOUT:
            LOG_D("subscribe wait ack timeout, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_SUBCRIBE_NACK:
            LOG_D("subscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;

        case IOTX_MQTT_EVENT_UNSUBCRIBE_SUCCESS:
            LOG_D("unsubscribe success, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_UNSUBCRIBE_TIMEOUT:
            LOG_D("unsubscribe timeout, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_UNSUBCRIBE_NACK:
            LOG_D("unsubscribe nack, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_PUBLISH_SUCCESS:
            LOG_D("publish success, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_PUBLISH_TIMEOUT:
            LOG_D("publish timeout, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_PUBLISH_NACK:
            LOG_D("publish nack, packet-id=%u", (unsigned int)packet_id);
            break;
        case IOTX_MQTT_EVENT_PUBLISH_RECVEIVED:
            LOG_D("topic message arrived but without any related handle: topic=%.*s, topic_msg=%.*s",
                          topic_info->topic_len,
                          topic_info->ptopic,
                          topic_info->payload_len,
                          topic_info->payload);
            break;

        case IOTX_MQTT_EVENT_BUFFER_OVERFLOW:
            LOG_D("buffer overflow, %s", msg->msg);
            break;

        default:
            LOG_D("Should NOT arrive here.");
            break;
    }
}

void mqtt_service_reply_pub(const char *topic_idx, const char *id, const char *code, const char *data)
{    
	RT_ASSERT(id != RT_NULL);
	RT_ASSERT(code != RT_NULL);
	RT_ASSERT(topic_idx != RT_NULL);
	
	char msg_pub[128];
	rt_memset(msg_pub, 0, sizeof(msg_pub));

	if (!rt_strcasecmp(code, ALI_CODE_OK))
		rt_snprintf(msg_pub, 128, "{\"id\": \"%s\",\"code\": \"%s\",\"data\": {}}", id, code);
	else
		rt_snprintf(msg_pub, 128, "{\"id\": \"%s\",\"code\": \"%s\",\"data\": {%s}}", id, code, data);

	iotx_mqtt_topic_info_t topic_msg;
	topic_msg.packet_id = 0;
	topic_msg.dup       = 0;
    topic_msg.qos       = IOTX_MQTT_QOS1;
    topic_msg.retain    = 0;
	topic_msg.topic_len = 0;
	topic_msg.ptopic    = RT_NULL;
	topic_msg.payload_len = rt_strlen(msg_pub);
    topic_msg.payload     = (void *)msg_pub;

	char topic_name[128];
	rt_memset(topic_name, 0, sizeof(topic_name));
	if (ef_get_env_blob(topic_idx, topic_name, 127, RT_NULL) > 0)
    	IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg);
	else
		LOG_D("can not read env variable of topic");
}

static void ali_mqtt_door_ctrl_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
    cJSON *root, *id, *params, *door_idx;
    iotx_mqtt_topic_info_pt ptopic_info = (iotx_mqtt_topic_info_pt) msg->msg;

    LOG_D("subcrible message arrive: %.*s.", ptopic_info->topic_len, ptopic_info->ptopic);

    root = cJSON_Parse(ptopic_info->payload);
    if (root == RT_NULL)
    {
        LOG_D("cJSON parse failed."); 
        goto __door_ctrl_exit;
    }

	id = cJSON_GetObjectItem(root, "id");
    if (id == RT_NULL)
    {
        LOG_D("cJSON get object[id] failed.");
        goto __door_ctrl_exit;
    }

    params = cJSON_GetObjectItem(root, "params");
    if (params == RT_NULL)
    {
        LOG_D("cJSON get object[params] failed.");
        goto __door_ctrl_exit;
    }

	door_idx = cJSON_GetObjectItem(params, "door_idx");
    if (door_idx == RT_NULL)
    {
        LOG_D("cJSON get object[door_idx] failed.");
        goto __door_ctrl_exit;
    }

	LOG_D("id=%d;door_idx=%.*s", id->valueint, rt_strlen(door_idx->valuestring), door_idx->valuestring);

	char msg_dat[256];
	rt_memset(msg_dat, 0, sizeof(msg_dat));
	rt_snprintf(msg_dat, sizeof(msg_dat), "id=%d;door_idx=%.*s", id->valueint, rt_strlen(door_idx->valuestring), door_idx->valuestring);

	extern rt_err_t dev_modbus_send_queue_msg(char *queue_msg);
	if (dev_modbus_send_queue_msg(msg_dat) != RT_EOK)
		LOG_D("send queue message failed");
	
__door_ctrl_exit:
    if (root)
        cJSON_Delete(root);
}

static void ali_mqtt_device_ctrl_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
	char str_id[10];
    cJSON *root, *id, *params, *ctrl_cmd, *ctrl_para;
    iotx_mqtt_topic_info_pt ptopic_info = (iotx_mqtt_topic_info_pt) msg->msg;

    LOG_D("subcrible message arrive: %.*s.", ptopic_info->topic_len, ptopic_info->ptopic);

    root = cJSON_Parse(ptopic_info->payload);
    if (root == RT_NULL)
    {
        LOG_D("cJSON parse failed.");
        goto __device_ctrl_exit;
    }

	id = cJSON_GetObjectItem(root, "id");
    if (id == RT_NULL)
    {
        LOG_D("cJSON get object[id] failed.");
        goto __device_ctrl_exit;
    }
	rt_snprintf(str_id, sizeof(str_id), "%d", id->valueint);

    params = cJSON_GetObjectItem(root, "params");
    if (params == RT_NULL)
    {
        LOG_D("cJSON get object[params] failed.");
        goto __device_ctrl_exit;
    }

	ctrl_cmd = cJSON_GetObjectItem(params, "ctrl_cmd");
    if (ctrl_cmd == RT_NULL)
    {
        LOG_D("cJSON get object[ctrl_cmd] failed.");
        goto __device_ctrl_exit;
    }

	ctrl_para = cJSON_GetObjectItem(params, "ctrl_para");
    if (ctrl_para == RT_NULL)
    {
        LOG_D("cJSON get object[ctrl_para] failed.");
        goto __device_ctrl_exit;
    }
	
	char msg_dat[256];
	rt_memset(msg_dat, 0, sizeof(msg_dat));
	
	extern rt_err_t dev_modbus_send_queue_msg(char *queue_msg);
	
	if (!rt_strncmp("reboot", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_D("remote command: reboot");
		rt_snprintf(msg_dat, sizeof(msg_dat), "id=%d;ctrl_cmd=%.*s", id->valueint, rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring);
	}
	else if (!rt_strncmp("beep", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_D("remote command: beep");
		rt_snprintf(msg_dat, sizeof(msg_dat), "id=%d;ctrl_cmd=%.*s", id->valueint, rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring);
	}
	else if (!rt_strncmp("beep", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_D("remote command: reset");
		rt_snprintf(msg_dat, sizeof(msg_dat), "id=%d;ctrl_cmd=%.*s", id->valueint, rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring);
	}
	else
	{
		LOG_D("No such remote command defined: %.*s.", rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring);
		mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEV_CTRL_FAIL, RT_NULL);
		goto __device_ctrl_exit;
	}

	if (dev_modbus_send_queue_msg(msg_dat) != RT_EOK)
		LOG_D("send queue message failed");
    
__device_ctrl_exit:
    if (root)
        cJSON_Delete(root);
}


static void ali_mqtt_property_set_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
	char str_id[10];
    uint8_t op_mode = 0;
	size_t para_len;
    cJSON *root, *id, *params, *mode, *para_name, *para_value;
	iotx_mqtt_topic_info_pt ptopic_info = (iotx_mqtt_topic_info_pt) msg->msg;

    LOG_D("subcrible message arrive: %.*s.", ptopic_info->topic_len, ptopic_info->ptopic);

	root = cJSON_Parse(ptopic_info->payload);
    if (root == RT_NULL)
    {
        LOG_D("cJSON parse failed.");
        goto __property_set_exit;
    }

	id = cJSON_GetObjectItem(root, "id");
    if (id == RT_NULL)
    {
        LOG_D("cJSON get object[id] failed.");
        goto __property_set_exit;
    }
	rt_snprintf(str_id, sizeof(str_id), "%d", id->valueint);
	
    params = cJSON_GetObjectItem(root, "params");
    if (params == RT_NULL)
    {
        LOG_D("cJSON get object[params] failed.");
        goto __property_set_exit;
    }

	mode = cJSON_GetObjectItem(params, "mode");
    if (mode == RT_NULL)
    {
        LOG_D("cJSON get object[mode] failed.");
        goto __property_set_exit;
    }
	para_name = cJSON_GetObjectItem(params, "para_name");
    if (para_name == RT_NULL)
    {
        LOG_D("cJSON get object[para_name] failed.");
        goto __property_set_exit;
    }
	para_value = cJSON_GetObjectItem(params, "para_value");
    if (para_value == RT_NULL)
    {
        LOG_D("cJSON get object[para_value] failed.");
        goto __property_set_exit;
    }
    
	para_len = rt_strlen(mode->valuestring);
	
	if (!rt_strncmp("write", mode->valuestring, para_len))
		op_mode = op_mode | 0x01;
	else if (!rt_strncmp("read", mode->valuestring, para_len))
		op_mode = op_mode | 0x02;
	else
	{
		LOG_D("KV operation mode error.");
		mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_PROPERTY_ERROR, RT_NULL);
        goto __property_set_exit;
	}

	if (ef_get_env_blob(para_name->valuestring, RT_NULL, 0, &para_len) > 0)
	{
		if (op_mode & 0x01)
		{
			if (ef_set_env_blob(para_name->valuestring, para_value->valuestring, rt_strlen(para_value->valuestring)) == EF_NO_ERR)
			{
				LOG_D("write parament success: para_name=%.*s, para_value=%.*s", rt_strlen(para_name->valuestring), para_name->valuestring, rt_strlen(para_value->valuestring), para_value->valuestring);
				mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
			}
			else
			{
				LOG_D("write parament failed: para_name=%.*s, para_value=%.*s", rt_strlen(para_name->valuestring), para_name->valuestring, rt_strlen(para_value->valuestring), para_value->valuestring);
				mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_PROPERTY_ERROR, RT_NULL);
			}
		}
		else
		{
			char para_val_buff[128];
			rt_memset(para_val_buff, 0, 128);
			char msg_dat[256];
			rt_memset(msg_dat, 0, sizeof(msg_dat));
			
			if (ef_get_env_blob(para_name->valuestring, para_val_buff, sizeof(para_val_buff), RT_NULL) > 0)
			{
				LOG_D("read parament success: para_name=%.*s, para_value=%s", rt_strlen(para_name->valuestring), para_name->valuestring, para_val_buff);
				rt_snprintf(msg_dat, sizeof(msg_dat), "\"para_name\":\"%.*s\",\"para_value\":\"%s\"", rt_strlen(para_name->valuestring), para_name->valuestring, para_val_buff);
				mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_OK, msg_dat);
			}
			else
			{
				LOG_D("read parament failed: para_name=%.*s, para_value=%.*s", rt_strlen(para_name->valuestring), para_name->valuestring, rt_strlen(para_value->valuestring), para_value->valuestring);
				mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_PROPERTY_ERROR, RT_NULL);
			}
		}
	}
	else
	{
		LOG_D("access parament failed: para_name=%.*s", rt_strlen(para_name->valuestring), para_name->valuestring);
		mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, str_id, ALI_CODE_PROPERTY_ERROR, RT_NULL);
	}

__property_set_exit:
	if (root)
        cJSON_Delete(root);
}

static void ali_mqtt_alarm_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
	LOG_I("-------------------");
    LOG_I("timeout alarm feedback.");
    LOG_I("-------------------");
}

static void ali_mqtt_device_error_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
    LOG_I("-------------------");
    LOG_I("device error feedback.");
    LOG_I("-------------------");
}

static void ali_mqtt_property_post_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
    LOG_I("-------------------");
    LOG_I("property post feedback.");
    LOG_I("-------------------");
}

static void ali_mqtt_device_info_update_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
    LOG_I("-------------------");
    LOG_I("device info update feedback.");
    LOG_I("-------------------");
	/* 该信号量表明已经连接到MQTT并且成功上传的设备标签 */
	rt_sem_release(device_info_up_sem);
}

static rt_err_t mqtt_check_load_topic(char *product_key, char *device_name)
{
	RT_ASSERT(product_key != RT_NULL);
	RT_ASSERT(device_name != RT_NULL);
	
	char topic[MQTT_TOPIC_MAX_SIZE];
	char topic_old[MQTT_TOPIC_MAX_SIZE];
	int topic_idx;
	int sub_items = sizeof(mqtt_sub_item) / sizeof(mqtt_subscribe_item);
	rt_bool_t is_write;
	rt_err_t err = RT_EOK;

	for (topic_idx = 0; topic_idx < sub_items; topic_idx++)
	{
		rt_memset(topic, 0, MQTT_TOPIC_MAX_SIZE);
		rt_memset(topic_old, 0, MQTT_TOPIC_MAX_SIZE);
		
		if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DOOR_CTRL_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/door_ctrl", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DOOR_CTRL_REPLY_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/door_ctrl_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DEVICE_CTRL_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/device_ctrl", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DEVICE_CTRL_REPLY_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/device_ctrl_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ALARM_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/alarm/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ALARM_REPLY_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/alarm/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ERROR_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/device_error/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ERROR_REPLY_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/device_error/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_POST_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/property/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_POST_REPLY_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/property/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_SET_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/property/set", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_SET_REPLY_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/property/set_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_DEVICEINFO_UPDATE_PUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/deviceinfo/update", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_DEVICEINFO_UPDATE_REPLY_SUB))
			rt_snprintf(topic, MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/deviceinfo/update_reply", product_key, device_name);
		else
		{
			LOG_D("can not find the topic: %s", mqtt_sub_item[topic_idx].topic_filter);
			continue;
		}

		if (ef_get_env_blob(mqtt_sub_item[topic_idx].topic_filter, topic_old, MQTT_TOPIC_MAX_SIZE, RT_NULL) > 0)
		{
			if (rt_strcmp(topic, topic_old))
				is_write = RT_TRUE;
			else
				is_write = RT_FALSE;
		}
		else
			is_write = RT_TRUE;

		if (is_write == RT_TRUE)
		{
			if (ef_set_env_blob(mqtt_sub_item[topic_idx].topic_filter, topic, MQTT_TOPIC_MAX_SIZE) != EF_NO_ERR)
			{
				LOG_D("write topic into flash error");
				err = RT_ERROR;
				break;
			}
		}	
	}
	
	return err;
}

static void mqtt_device(void *arg)
{
	int i;
    int sub_items = sizeof(mqtt_sub_item) / sizeof(mqtt_subscribe_item);
	char *topic_name = RT_NULL, *msg_writebuf = RT_NULL, *msg_readbuf = RT_NULL;
    char product_key[PRODUCT_KEY_LEN + 1], device_name[DEVICE_NAME_LEN + 1], device_secret[DEVICE_SECRET_LEN + 1];

    iotx_mqtt_param_t device_params;
    iotx_conn_info_pt mqtt_conn_info = RT_NULL; 

	/* get the secret of ali-iot platform */
    HAL_GetProductKey(product_key);
    HAL_GetDeviceName(device_name);
    HAL_GetDeviceSecret(device_secret);

	if (mqtt_check_load_topic(product_key, device_name) != RT_EOK)
	{
		LOG_D("topic check and load error");
		return;
	}

    while (1)
    {        
        /* connect ali-iot platform */
        if (0 != IOT_SetupConnInfo(product_key, device_name, device_secret, (void **)&mqtt_conn_info)) 
        {
            LOG_I("platform authentication request failed!");
            goto do_device_exit;
        }

        /* malloc write and read buffer of MQTT */
        if ((RT_NULL == (msg_writebuf = (char *)rt_malloc(MQTT_MSGLEN))) 
        || (RT_NULL == (msg_readbuf = (char *)rt_malloc(MQTT_MSGLEN))))
        {
            LOG_W("not enough memory.");
            goto do_device_exit;
        }

        /* Initialize MQTT parameter */
        rt_memset(&device_params, 0x0, sizeof(device_params));
        /* feedback parameter of platform when use IOT_SetupConnInfo() connect */
        device_params.port      = mqtt_conn_info->port;
        device_params.host      = mqtt_conn_info->host_name;
        device_params.client_id = mqtt_conn_info->client_id;
        device_params.username  = mqtt_conn_info->username;
        device_params.password  = mqtt_conn_info->password;
        device_params.pub_key   = mqtt_conn_info->pub_key;
        /* timeout of request. uint: ms */
        device_params.request_timeout_ms    = 2000;
        device_params.clean_session         = 0;
        /* internal of keepalive checking: 60s~300s */
        device_params.keepalive_interval_ms = MQTT_KEEPALIVE_INTERNAL * 1000; 
        device_params.pread_buf             = msg_readbuf;
        device_params.read_buf_size         = MQTT_MSGLEN;
        device_params.pwrite_buf            = msg_writebuf;
        device_params.write_buf_size        = MQTT_MSGLEN;
        /* configure handle of event */
        device_params.handle_event.h_fp     = ali_mqtt_event_handle;
        device_params.handle_event.pcontext = NULL;

        /* construct a MQTT device with specify parameter */
        mqtt_device_hd = IOT_MQTT_Construct(&device_params);
        if (RT_NULL == mqtt_device_hd) 
        {
            LOG_D("construct MQTT failed!");
            goto do_device_exit;
        }

		topic_name = rt_calloc(sub_items, 128);
		if (topic_name == RT_NULL)
		{
			LOG_D("not enough memory for topic name!");
            goto do_device_exit;
		}
        rt_memset(topic_name, 0, 128 * sub_items);
        
        /* sbuscribe all topic */
        for (i = 0; i < sub_items; i++)
        {	
        	if (mqtt_sub_item[i].topic_handle_func == RT_NULL)
        		continue;
			
        	if (ef_get_env_blob(mqtt_sub_item[i].topic_filter, &topic_name[i * 128], 127, RT_NULL) <= 0)
				continue;
			
            if (IOT_MQTT_Subscribe(mqtt_device_hd, &topic_name[i * 128], mqtt_sub_item[i].qos, mqtt_sub_item[i].topic_handle_func, mqtt_sub_item[i].pcontext) < 0)
            {
                LOG_D("IOT_MQTT_Subscribe() failed, topic = %s", &topic_name[i * 128]);
                IOT_MQTT_Destroy(&mqtt_device_hd);
                goto do_device_exit;
            }         
        }

		/* handle the MQTT packet received from TCP or SSL connection */
        IOT_MQTT_Yield(mqtt_device_hd, 200);
		mqtt_is_running = 1;
	
        while (IOT_MQTT_CheckStateNormal(mqtt_device_hd))
        {
            /* handle the MQTT packet received from TCP or SSL connection */
            IOT_MQTT_Yield(mqtt_device_hd, 200);
            rt_thread_delay(rt_tick_from_millisecond(200));          
        }
		mqtt_is_running = 0;
        IOT_MQTT_Yield(mqtt_device_hd, 200);
        
        rt_memset(topic_name, 0, 128 * sub_items);

        /* ubsbuscribe all topic */
        for (i = 0; i < sub_items; i++)
        {		
        	if (ef_get_env_blob(mqtt_sub_item[i].topic_filter, &topic_name[i * 128], 127, RT_NULL) <= 0)
				continue;
			
            IOT_MQTT_Unsubscribe(mqtt_device_hd, &topic_name[i * 128]);
        }

        IOT_MQTT_Destroy(&mqtt_device_hd);

do_device_exit:
        if (RT_NULL != msg_writebuf)
            rt_free(msg_writebuf);

        if (RT_NULL != msg_readbuf)
            rt_free(msg_readbuf);		

		if (RT_NULL != topic_name)
			rt_free(topic_name);

		topic_name = RT_NULL;
        msg_writebuf = RT_NULL;
        msg_readbuf = RT_NULL;       
    }
}

void mqtt_period_thread(void *arg)
{
	int i;
	int pos;

	RT_ASSERT(arg != RT_NULL);
	
	if (mqtt_is_running == 0)
		return;
	
	rt_uint8_t *door_status, *dev_error;
	rt_uint16_t *door_alarm;

	/* 获得modbus读取的周期型数据 */
	extern rt_err_t dev_modbus_get_period_data(rt_uint8_t **door_status, rt_uint16_t **door_alarm, rt_uint8_t **dev_error);
	if (dev_modbus_get_period_data(&door_status, &door_alarm, &dev_error) != RT_EOK)
		return;

	/* get current time */
	time_t now = time(RT_NULL);
	/* 设备错误主题监测 */
	{
		char error_buff[32];
		rt_memset(error_buff, 0, sizeof(error_buff));
		for (pos = 0, i = 0; (i < device_num) && (pos < sizeof(error_buff)); i++)
		{
			if (dev_error[i])
			{
				if (i == (device_num - 1))
					pos += rt_snprintf(&error_buff[pos], sizeof(error_buff) - pos - 1, "%d,", i + 1);
				else
					pos += rt_snprintf(&error_buff[pos], sizeof(error_buff) - pos - 1, "%d", i + 1);
			}
		}
		if (pos)
		{	
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					cJSON_AddStringToObject(root, "id", "123");
					cJSON_AddStringToObject(root, "version", "1.0");
					cJSON_AddItemToObject(root, "params", js_params);
					cJSON_AddItemToObject(js_params, "value", js_value);
					cJSON_AddStringToObject(js_value, "error_name", "Communication");
					cJSON_AddStringToObject(js_value, "error_info", error_buff);
					cJSON_AddNumberToObject(js_params, "time", now);
					cJSON_AddStringToObject(root, "method", "thing.event.device_error.post");
					msg_pub = cJSON_PrintUnformatted(root);
					if (msg_pub)
					{
						iotx_mqtt_topic_info_t topic_msg;
					    rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
					    topic_msg.qos    = IOTX_MQTT_QOS1;
					    topic_msg.retain = 0;
					    topic_msg.dup    = 0;
					    topic_msg.payload     = (void *)msg_pub;
					    topic_msg.payload_len = rt_strlen(msg_pub); 

						char topic_name[128];
						rt_memset(topic_name, 0, sizeof(topic_name));
						if (ef_get_env_blob(ALI_EVENT_DEVICE_ERROR_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
							IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg); 
					}
				}
				cJSON_Delete(root);
			}
		}
	}
	/* 设备报警主题监测 */
	{
		char alarm_buff[128];
		rt_memset(alarm_buff, 0, sizeof(alarm_buff));
		/* 开门超时报警 */
		for (pos = 0, i = 0; (i < device_num) && (pos < sizeof(alarm_buff)); i++)
		{
			if (door_alarm[i * ALI_ALARM_TYPES + 1])
			{
				for (int j = 0; j < device_chn_num; j++)
				{
					if (door_alarm[i * ALI_ALARM_TYPES + 1] & (1 << j))
						pos += rt_snprintf(&alarm_buff[pos], sizeof(alarm_buff) - pos - 1, "%d,", i * device_chn_num + j + 1);
				}
			}
		}
		if (pos)
		{
			alarm_buff[pos] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					cJSON_AddStringToObject(root, "id", "123");
					cJSON_AddStringToObject(root, "version", "1.0");
					cJSON_AddItemToObject(root, "params", js_params);
					cJSON_AddItemToObject(js_params, "value", js_value);
					cJSON_AddStringToObject(js_value, "alarm_name", "Timeout");
					cJSON_AddStringToObject(js_value, "alarm_info", alarm_buff);
					cJSON_AddNumberToObject(js_params, "time", now);
					cJSON_AddStringToObject(root, "method", "thing.event.alarm.post");
					msg_pub = cJSON_PrintUnformatted(root);
					if (msg_pub)
					{
						iotx_mqtt_topic_info_t topic_msg;
					    rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
					    topic_msg.qos    = IOTX_MQTT_QOS1;
					    topic_msg.retain = 0;
					    topic_msg.dup    = 0;
					    topic_msg.payload     = (void *)msg_pub;
					    topic_msg.payload_len = rt_strlen(msg_pub); 

						char topic_name[128];
						rt_memset(topic_name, 0, sizeof(topic_name));
						if (ef_get_env_blob(ALI_EVENT_DEVICE_ALARM_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
							IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg); 
					}
				}
				cJSON_Delete(root);
			}
		}
		/* 非法撬门报警 */
		rt_memset(alarm_buff, 0, sizeof(alarm_buff));
		for (pos = 0, i = 0; (i < device_num) && (pos < sizeof(alarm_buff)); i++)
		{
			if (door_alarm[i * ALI_ALARM_TYPES + 0])
			{
				for (int j = 0; j < device_chn_num; j++)
				{
					if (door_alarm[i * ALI_ALARM_TYPES + 0] & (1 << j))
						pos += rt_snprintf(&alarm_buff[pos], sizeof(alarm_buff) - pos - 1, "%d,", i * device_chn_num + j + 1);
				}
			}
		}
		if (pos)
		{
			alarm_buff[pos] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					cJSON_AddStringToObject(root, "id", "123");
					cJSON_AddStringToObject(root, "version", "1.0");
					cJSON_AddItemToObject(root, "params", js_params);
					cJSON_AddItemToObject(js_params, "value", js_value);
					cJSON_AddStringToObject(js_value, "alarm_name", "Illegal");
					cJSON_AddStringToObject(js_value, "alarm_info", alarm_buff);
					cJSON_AddNumberToObject(js_params, "time", now);
					cJSON_AddStringToObject(root, "method", "thing.event.alarm.post");
					msg_pub = cJSON_PrintUnformatted(root);
					if (msg_pub)
					{
						iotx_mqtt_topic_info_t topic_msg;
					    rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
					    topic_msg.qos    = IOTX_MQTT_QOS1;
					    topic_msg.retain = 0;
					    topic_msg.dup    = 0;
					    topic_msg.payload     = (void *)msg_pub;
					    topic_msg.payload_len = rt_strlen(msg_pub); 

						char topic_name[128];
						rt_memset(topic_name, 0, sizeof(topic_name));
						if (ef_get_env_blob(ALI_EVENT_DEVICE_ALARM_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
							IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg); 
					}
				}
				cJSON_Delete(root);
			}
		}

		/* 过流报警 */
		rt_memset(alarm_buff, 0, sizeof(alarm_buff));
		for (pos = 0, i = 0; (i < device_num) && (pos < sizeof(alarm_buff)); i++)
		{
			if (door_alarm[i * ALI_ALARM_TYPES + 2])
			{
				for (int j = 0; j < device_chn_num; j++)
				{
					if (door_alarm[i * ALI_ALARM_TYPES + 2] & (1 << j))
						pos += rt_snprintf(&alarm_buff[pos], sizeof(alarm_buff) - pos - 1, "%d,", i * device_chn_num + j + 1);
				}
			}
		}
		if (pos)
		{
			alarm_buff[pos] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					cJSON_AddStringToObject(root, "id", "123");
					cJSON_AddStringToObject(root, "version", "1.0");
					cJSON_AddItemToObject(root, "params", js_params);
					cJSON_AddItemToObject(js_params, "value", js_value);
					cJSON_AddStringToObject(js_value, "alarm_name", "Current");
					cJSON_AddStringToObject(js_value, "alarm_info", alarm_buff);
					cJSON_AddNumberToObject(js_params, "time", now);
					cJSON_AddStringToObject(root, "method", "thing.event.alarm.post");
					msg_pub = cJSON_PrintUnformatted(root);
					if (msg_pub)
					{
						iotx_mqtt_topic_info_t topic_msg;
					    rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
					    topic_msg.qos    = IOTX_MQTT_QOS1;
					    topic_msg.retain = 0;
					    topic_msg.dup    = 0;
					    topic_msg.payload     = (void *)msg_pub;
					    topic_msg.payload_len = rt_strlen(msg_pub); 

						char topic_name[128];
						rt_memset(topic_name, 0, sizeof(topic_name));
						if (ef_get_env_blob(ALI_EVENT_DEVICE_ALARM_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
							IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg); 
					}
				}
				cJSON_Delete(root);
			}
		}
	}

	{
		rt_uint8_t timer_cnt = *(rt_uint8_t *)(arg);
		timer_cnt = (timer_cnt + 1) % 2;
		*(rt_uint8_t *)(arg) = timer_cnt;
		if (timer_cnt == 0)
		{
			char status_buff[128];
			rt_memset(status_buff, 0, sizeof(status_buff));
			for (pos = 0, i = 0; (i < (device_chn_num * device_num)) && (pos < sizeof(status_buff)); i++)
			{
				if (i == ((device_chn_num * device_num) - 1))
					pos += rt_snprintf(&status_buff[pos], sizeof(status_buff) - pos - 1, "%d", door_status[i]);
				else
					pos += rt_snprintf(&status_buff[pos], sizeof(status_buff) - pos - 1, "%d,", door_status[i]);
			}

			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params  = cJSON_CreateObject();
				cJSON *js_quality = cJSON_CreateObject();
				cJSON *js_status  = cJSON_CreateObject();
				cJSON *js_device  = cJSON_CreateObject();
				if (js_params && js_quality && js_status && js_device)
				{
					extern rt_uint8_t sim800c_rssi;
					
					cJSON_AddStringToObject(root, "id", "123");
					cJSON_AddStringToObject(root, "version", "1.0");
					cJSON_AddItemToObject(root, "params", js_params);
					cJSON_AddItemToObject(js_params, "sigal_quality", js_quality);
					cJSON_AddNumberToObject(js_quality, "value", sim800c_rssi);
					cJSON_AddItemToObject(js_params, "door_status", js_status);
					cJSON_AddStringToObject(js_status, "value", status_buff);

					cJSON_AddItemToObject(js_params, "net_device", js_device);
					struct netdev *default_netdev = netdev_get_first_by_flags(NETDEV_FLAG_LINK_UP);
					if (default_netdev)
						cJSON_AddStringToObject(js_device, "value", default_netdev->name);
					else
						cJSON_AddStringToObject(js_device, "value", "unknown");
					
					cJSON_AddNumberToObject(js_params, "time", now);
					cJSON_AddStringToObject(root, "method", "thing.event.property.post");
					char *msg_pub = cJSON_PrintUnformatted(root);
					if (msg_pub)
					{
						iotx_mqtt_topic_info_t topic_msg;
						rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
						topic_msg.qos	 = IOTX_MQTT_QOS1;
						topic_msg.retain = 0;
						topic_msg.dup	 = 0;
						topic_msg.payload	  = (void *)msg_pub;
						topic_msg.payload_len = rt_strlen(msg_pub); 
			
						char topic_name[128];
						rt_memset(topic_name, 0, sizeof(topic_name));
						if (ef_get_env_blob(ALI_PROPERTY_POST_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
							IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg); 
					}
				}
				cJSON_Delete(root);
			}
		}
	}
}

static void ali_mqtt_dev_info(void *arg)
{
	/* 等待MQTT上线 */
	while (mqtt_is_running == 0)
	{
		rt_thread_mdelay(1000);
	}

	char dev_info[64];
	if (ef_get_env_blob(ALI_DEVICE_INFO_NAME, dev_info, sizeof(dev_info), RT_NULL) <= 0)
	{
		LOG_D("can not find env of %s", ALI_DEVICE_INFO_NAME);
		return;
	}
	
	while (1)
	{		
		char msg_pub[128];
		rt_memset(msg_pub, 0, sizeof(msg_pub));
		rt_snprintf(msg_pub, sizeof(msg_pub), "{\"id\": \"123\",\"version\": \"1.0\",\"params\": [{\"attrKey\": \"%s\",\"attrValue\": \"%s\"}],\"method\": \"thing.deviceinfo.update\"}",
	                                        ALI_DEVICE_INFO_NAME, dev_info);

		iotx_mqtt_topic_info_t topic_msg;
	    rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
	    topic_msg.qos    = IOTX_MQTT_QOS1;
	    topic_msg.retain = 0;
	    topic_msg.dup    = 0;
	    topic_msg.payload     = (void *)msg_pub;
	    topic_msg.payload_len = rt_strlen(msg_pub);       

		char topic_name[128];
		rt_memset(topic_name, 0, sizeof(topic_name));
		if (ef_get_env_blob(ALI_DEVICEINFO_UPDATE_PUB, topic_name, sizeof(topic_name), RT_NULL) > 0)
			IOT_MQTT_Publish(mqtt_device_hd, topic_name, &topic_msg);  

		if (rt_sem_take(device_info_up_sem, rt_tick_from_millisecond(10 * RT_TICK_PER_SECOND)) == RT_EOK)
		{
			rt_timer_t  tim;
			mqtt_timer_cnt = 0;
			tim = rt_timer_create("ali_timer", mqtt_period_thread, &mqtt_timer_cnt, rt_tick_from_millisecond(10 * 1000), RT_TIMER_FLAG_SOFT_TIMER | RT_TIMER_FLAG_PERIODIC);
			RT_ASSERT(tim != RT_NULL);
			
			rt_timer_start(tim);
			break;
		}
	}
}

static int ali_mqtt_init(void)
{
	size_t env_len;
    rt_thread_t tid;

	env_len = HAL_GetProductKey(RT_NULL);
	if (env_len <= 0)
	{
		LOG_D("ProductKey read failed.");
		goto __mqtt_init_exit;	
	}

	env_len = HAL_GetDeviceName(RT_NULL);
	if (env_len <= 0)
	{
		LOG_D("DeviceName read failed.");
		goto __mqtt_init_exit;	
	}

	env_len = HAL_GetDeviceSecret(RT_NULL);
	if (env_len <= 0)
	{
		LOG_D("DeviceSecret read failed.");
		goto __mqtt_init_exit;	
	}	

	char str_dev_num[8];
	char str_dev_chn_num[8];
	rt_memset(str_dev_num, 0, sizeof(str_dev_num));
	rt_memset(str_dev_chn_num, 0, sizeof(str_dev_chn_num));
	
	if (ef_get_env_blob(ALI_DEVICE_NUM, str_dev_num, sizeof(str_dev_num), RT_NULL) <= 0)
	{
		LOG_D("%s read failed.", ALI_DEVICE_NUM);
		goto __mqtt_init_exit;
	}

	if (ef_get_env_blob(ALI_DEVICE_CHN_NUM, str_dev_chn_num, sizeof(str_dev_chn_num), RT_NULL) <= 0)
	{
		LOG_D("%s read failed.", ALI_DEVICE_CHN_NUM);
		goto __mqtt_init_exit;
	}

	device_info_up_sem = rt_sem_create("info_sem", 0, RT_IPC_FLAG_PRIO);
	RT_ASSERT(device_info_up_sem != RT_NULL);
	
	device_num = atoi(str_dev_num);
	device_chn_num = atoi(str_dev_chn_num);
	LOG_D("device_num=%d, device_chn_num=%d", device_num, device_chn_num);

	mqtt_is_running = 0;
    tid = rt_thread_create("mqtt", mqtt_device, RT_NULL, 6 * 1024, RT_THREAD_PRIORITY_MAX / 2 - 1, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);

	tid = rt_thread_create("dev_info", ali_mqtt_dev_info, RT_NULL, 2 * 1024, RT_THREAD_PRIORITY_MAX / 2, 10);
	if (tid != RT_NULL)
		rt_thread_startup(tid);

__mqtt_init_exit:
    return 0;
}
INIT_APP_EXPORT(ali_mqtt_init);


