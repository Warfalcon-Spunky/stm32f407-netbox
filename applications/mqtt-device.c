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

#include "rtthread.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "infra_types.h"
#include "infra_defs.h"
#include "dynreg_api.h"
#include "dev_sign_api.h"
#include "mqtt_api.h"
#include "mqtt-def.h"
#include "cJSON.h"
#include "easyflash.h"


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
#define MQTT_KEEPALIVE_INTERNAL                 (60)
#define MQTT_TOPIC_MAX_SIZE						(128)

#define MQTT_MAN_INFO_STRING					"Netbox,Spunky,Radiation.Corp,2019"

static char *topic_buff = RT_NULL;

static void    *mqtt_client_hd  = RT_NULL;
static uint8_t  is_mqtt_exit    = 0;
static uint8_t  is_mqtt_disconnect = 1;
static uint32_t mqtt_packet_id  = 1;

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
    {ALI_EVENT_DEVICE_ALARM_REPLY_SUB,     IOTX_MQTT_QOS1, ali_mqtt_alarm_msg_arrive,      		   RT_NULL},

    {ALI_EVENT_DEVICE_ERROR_PUB,           IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_EVENT_DEVICE_ERROR_REPLY_SUB,     IOTX_MQTT_QOS1, ali_mqtt_device_error_msg_arrive,       RT_NULL},

	{ALI_PROPERTY_SET_SUB,                 IOTX_MQTT_QOS1, ali_mqtt_property_set_msg_arrive,       RT_NULL},    
    {ALI_PROPERTY_SET_REPLY_PUB,           IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},

    {ALI_PROPERTY_POST_PUB,                IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_PROPERTY_POST_REPLY_SUB,          IOTX_MQTT_QOS1, ali_mqtt_property_post_msg_arrive,      RT_NULL},

    {ALI_DEVICEINFO_UPDATE_PUB,            IOTX_MQTT_QOS1, RT_NULL,                                RT_NULL},
    {ALI_DEVICEINFO_UPDATE_REPLY_SUB,      IOTX_MQTT_QOS1, ali_mqtt_device_info_update_msg_arrive, RT_NULL}
};

void *ali_mqtt_get_handle(void)
{
	return mqtt_client_hd;
}

static char *mqtt_topic_find(const char *topic_fliter)
{
	if (topic_buff == RT_NULL || topic_fliter == RT_NULL)
		return RT_NULL;

	char *topic;
	int topic_idx;
	int sub_items = sizeof(mqtt_sub_item) / sizeof(mqtt_subscribe_item);

	for (topic = RT_NULL, topic_idx = 0; (topic_idx < sub_items) && (topic == RT_NULL); topic_idx++)
	{	
		if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, topic_fliter))
			topic = &topic_buff[MQTT_TOPIC_MAX_SIZE * topic_idx];
	}

	return topic;
}

static rt_err_t mqtt_check_load_topic(char *product_key, char *device_name, char *topic)
{
	RT_ASSERT(product_key != RT_NULL);
	RT_ASSERT(device_name != RT_NULL);

	int topic_idx;
	int sub_items = sizeof(mqtt_sub_item) / sizeof(mqtt_subscribe_item);
	
	topic = rt_calloc(sub_items, MQTT_TOPIC_MAX_SIZE);
	if (topic == RT_NULL)
	{
		LOG_D("not enough memory for topic name!");
        return -RT_ENOMEM;
	}
	rt_memset(topic, 0x0, MQTT_TOPIC_MAX_SIZE * sub_items);

	for (topic_idx = 0; topic_idx < sub_items; topic_idx++)
	{	
		if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DOOR_CTRL_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/door_ctrl", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DOOR_CTRL_REPLY_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/door_ctrl_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DEVICE_CTRL_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/device_ctrl", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_SERVICE_DEVICE_CTRL_REPLY_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/device_ctrl_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ALARM_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/alarm/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ALARM_REPLY_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/alarm/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ERROR_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/device_error/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_EVENT_DEVICE_ERROR_REPLY_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/device_error/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_POST_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/property/post", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_POST_REPLY_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/event/property/post_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_SET_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/property/set", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_PROPERTY_SET_REPLY_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/service/property/set_reply", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_DEVICEINFO_UPDATE_PUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/deviceinfo/update", product_key, device_name);
		else if (!rt_strcmp(mqtt_sub_item[topic_idx].topic_filter, ALI_DEVICEINFO_UPDATE_REPLY_SUB))
			rt_snprintf(&topic[topic_idx * 128], MQTT_TOPIC_MAX_SIZE, "/sys/%s/%s/thing/deviceinfo/update_reply", product_key, device_name);
		else
		{
			LOG_D("can not find the topic: %s", mqtt_sub_item[topic_idx].topic_filter);
			continue;
		}	
	}
	
	return RT_EOK;
}

static void ali_mqtt_event_handle(void *pcontext, void *pclient, iotx_mqtt_event_msg_pt msg)
{
    iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
    if (topic_info == RT_NULL)
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
			is_mqtt_disconnect = 1;
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
        case IOTX_MQTT_EVENT_PUBLISH_RECEIVED:
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

void mqtt_service_reply_pub(const char *topic_fliter, const char *id, const char *code, const char *data)
{    
	RT_ASSERT(id != RT_NULL);
	RT_ASSERT(code != RT_NULL);
	RT_ASSERT(topic_fliter != RT_NULL);
	
	char msg_pub[256];
	rt_memset(msg_pub, 0, sizeof(msg_pub));

	if (data == RT_NULL)
		rt_snprintf(msg_pub, 128, "{\"id\": \"%s\",\"code\": \"%s\",\"data\": {}}", id, code);
	else
		rt_snprintf(msg_pub, 128, "{\"id\": \"%s\",\"code\": \"%s\",\"data\": {%s}}", id, code, data);

	LOG_I("Service reply: %.*s", strlen(msg_pub), msg_pub);

	iotx_mqtt_topic_info_t topic_msg;
	topic_msg.packet_id = 0;
	topic_msg.dup       = 0;
    topic_msg.qos       = IOTX_MQTT_QOS1;
    topic_msg.retain    = 0;
	topic_msg.topic_len = 0;
	topic_msg.ptopic    = RT_NULL;
	topic_msg.payload_len = rt_strlen(msg_pub);
    topic_msg.payload     = (void *)msg_pub;

	char *topic = mqtt_topic_find(topic_fliter);
	if (topic != RT_NULL)
    	IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);
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

	LOG_D("id=%.*s;door_idx=%.*s", rt_strlen(id->valuestring), id->valuestring, rt_strlen(door_idx->valuestring), door_idx->valuestring);

	char msg_dat[256];
	rt_memset(msg_dat, 0, sizeof(msg_dat));
	rt_snprintf(msg_dat, sizeof(msg_dat), "id=%.*s;door_idx=%.*s", rt_strlen(id->valuestring), id->valuestring, rt_strlen(door_idx->valuestring), door_idx->valuestring);

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
	
	if (!rt_strncmp("reboot", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		if (!rt_strncmp("yes", ctrl_para->valuestring, rt_strlen(ctrl_para->valuestring)))
		{
			LOG_I("remote command: reboot");
			extern void rt_hw_cpu_reset(void);
			rt_hw_cpu_reset();
			while (1);
		}
	}
	else if (!rt_strncmp("beep", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_I("remote command: beep");
		mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
		goto __device_ctrl_exit;
	}
	else if (!rt_strncmp("max_door_open_time", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_D("remote command: set sub device max open time");
		rt_snprintf(msg_dat, sizeof(msg_dat), "id=%.*s;ctrl_cmd=%.*s;ctrl_para=%.*s", rt_strlen(id->valuestring), id->valuestring, rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring, rt_strlen(ctrl_para->valuestring), ctrl_para->valuestring);		
	}
	else if (!rt_strncmp("max_door_power_time", ctrl_cmd->valuestring, rt_strlen(ctrl_cmd->valuestring)))
	{
		LOG_D("remote command: set sub device max power time");
		rt_snprintf(msg_dat, sizeof(msg_dat), "id=%.*s;ctrl_cmd=%.*s;ctrl_para=%.*s", rt_strlen(id->valuestring), id->valuestring, rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring, rt_strlen(ctrl_para->valuestring), ctrl_para->valuestring);		
	}
	else
	{
		LOG_D("No such remote command defined: %.*s.", rt_strlen(ctrl_cmd->valuestring), ctrl_cmd->valuestring);
		mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEV_CTRL_FAIL, RT_NULL);
		goto __device_ctrl_exit;
	}
       
    extern rt_err_t dev_modbus_send_queue_msg(char *queue_msg);
	if (dev_modbus_send_queue_msg(msg_dat) != RT_EOK)
		LOG_D("send queue message failed");
    
__device_ctrl_exit:
    if (root)
        cJSON_Delete(root);
}


static void ali_mqtt_property_set_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
    cJSON *root, *id, *params, *device_info_cj, *device_num_cj, *device_chn_num_cj;
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
	
    params = cJSON_GetObjectItem(root, "params");
    if (params == RT_NULL)
    {
        LOG_D("cJSON get object[params] failed.");
        goto __property_set_exit;
    }

	device_info_cj = cJSON_GetObjectItem(params, ALI_DEVICE_INFO_NAME);
    if (device_info_cj)
    {
        if (ef_set_env_blob(ALI_DEVICE_INFO_NAME, device_info_cj->valuestring, rt_strlen(device_info_cj->valuestring)) == EF_NO_ERR)
        {
        	LOG_D("write device_info success: %.*s", rt_strlen(device_info_cj->valuestring), device_info_cj->valuestring);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_OK, RT_NULL);
        }
		else
		{
			LOG_D("write device_info failed: %.*s", rt_strlen(device_info_cj->valuestring), device_info_cj->valuestring);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_PROPERTY_ERROR, RT_NULL);			
		}
		goto __property_set_exit;
    }
	
	device_num_cj = cJSON_GetObjectItem(params, ALI_DEVICE_NUM);
    if (device_num_cj)
    {
		if (ef_set_env_blob(ALI_DEVICE_NUM, &device_num_cj->valueint, sizeof(device_num_cj->valueint)) == EF_NO_ERR)
        {
        	LOG_D("write device_num success: %d", device_num_cj->valueint);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_OK, RT_NULL);
        }
		else
		{
			LOG_D("write device_num failed: %d", device_num_cj->valueint);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_PROPERTY_ERROR, RT_NULL);
		}
		goto __property_set_exit;
	}
	
	device_chn_num_cj = cJSON_GetObjectItem(params, ALI_DEVICE_CHN_NUM);
    if (device_chn_num_cj)
    {
        if (ef_set_env_blob(ALI_DEVICE_CHN_NUM, &device_chn_num_cj->valueint, sizeof(device_chn_num_cj->valueint)) == EF_NO_ERR)
        {
        	LOG_D("write device_chn_num success: %d", device_chn_num_cj->valueint);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_OK, RT_NULL);
        }
		else
		{
			LOG_D("write device_chn_num failed: %d", device_chn_num_cj->valueint);
			mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_PROPERTY_ERROR, RT_NULL);			
		}
		goto __property_set_exit;
    }

	/* 违规参数设置,直接反馈负相应 */
	mqtt_service_reply_pub(ALI_PROPERTY_SET_REPLY_PUB, id->valuestring, ALI_CODE_PROPERTY_ERROR, RT_NULL);		
__property_set_exit:
	if (root)
        cJSON_Delete(root);
}


static void ali_mqtt_alarm_msg_arrive(void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
#if 0
		iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
		if (topic_info == NULL)
		{
			LOG_I("Topic info is null! Exit.");
			return;
		}
		
		LOG_I("-------------------");
		LOG_I("feedback topic: %.*s.", rt_strlen(topic_info->ptopic), topic_info->ptopic);
		LOG_I("feedback payload: %.*s.", topic_info->payload_len, topic_info->payload);
		LOG_I("-------------------");
#endif
}

static void ali_mqtt_device_error_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
#if 0
		iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
		if (topic_info == NULL)
		{
			LOG_I("Topic info is null! Exit.");
			return;
		}
		
		LOG_I("-------------------");
		LOG_I("feedback topic: %.*s.", rt_strlen(topic_info->ptopic), topic_info->ptopic);
		LOG_I("feedback payload: %.*s.", topic_info->payload_len, topic_info->payload);
		LOG_I("-------------------");
#endif
}

static void ali_mqtt_property_post_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
#if 0
		iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
		if (topic_info == NULL)
		{
			LOG_I("Topic info is null! Exit.");
			return;
		}
		
		LOG_I("-------------------");
		LOG_I("feedback topic: %.*s.", rt_strlen(topic_info->ptopic), topic_info->ptopic);
		LOG_I("feedback payload: %.*s.", topic_info->payload_len, topic_info->payload);
		LOG_I("-------------------");
#endif
}

static void ali_mqtt_device_info_update_msg_arrive (void *pcontext, void *handle, iotx_mqtt_event_msg_pt msg)
{
#if 0
		iotx_mqtt_topic_info_pt topic_info = (iotx_mqtt_topic_info_pt)msg->msg;
		if (topic_info == NULL)
		{
			LOG_I("Topic info is null! Exit.");
			return;
		}
		
		LOG_I("-------------------");
		LOG_I("feedback topic: %.*s.", rt_strlen(topic_info->ptopic), topic_info->ptopic);
		LOG_I("feedback payload: %.*s.", topic_info->payload_len, topic_info->payload);
		LOG_I("-------------------");
#endif	
}

void mqtt_period_task(void)
{
	int i;
	int pos;
	
	rt_uint8_t *door_status, *dev_error;
	rt_uint16_t *door_alarm;
    rt_uint8_t device_num, device_chn_num;

	/* 获得modbus读取的周期型数据 */
	extern rt_err_t dev_modbus_get_period_data(rt_uint8_t **door_status, rt_uint16_t **door_alarm, rt_uint8_t **dev_error, rt_uint8_t *p_device_num, rt_uint8_t *p_device_chn_num);
	if (dev_modbus_get_period_data(&door_status, &door_alarm, &dev_error, &device_num, &device_chn_num) != RT_EOK)
		return;

	/* get current time */
	rt_uint64_t now;
    now = (rt_uint64_t)(time(RT_NULL) - (3600 * 8));
    now = now * 1000L;
                
	/* 设备错误主题监测 */
	{
		char error_buff[32];
		rt_memset(error_buff, 0, sizeof(error_buff));
		for (pos = 0, i = 0; (i < device_num) && (pos < sizeof(error_buff)); i++)
		{
			if (dev_error[i])
			{
				if (i == (device_num - 1))
					pos += rt_snprintf(&error_buff[pos], sizeof(error_buff) - pos - 1, "%d", i + 1);
				else
					pos += rt_snprintf(&error_buff[pos], sizeof(error_buff) - pos - 1, "%d,", i + 1);
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
					char str_id[16];
					rt_memset(str_id, 0, sizeof(str_id));
					rt_snprintf(str_id, sizeof(str_id), "%d", mqtt_packet_id++);
					cJSON_AddStringToObject(root, "id", str_id);
                    
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

						char *topic = mqtt_topic_find(ALI_EVENT_DEVICE_ERROR_PUB);
						if (topic != RT_NULL)
							IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);  

						rt_free(msg_pub);
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
			alarm_buff[rt_strlen(alarm_buff) - 1] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					char str_id[16];
					rt_memset(str_id, 0, sizeof(str_id));
					rt_snprintf(str_id, sizeof(str_id), "%d", mqtt_packet_id++);
					cJSON_AddStringToObject(root, "id", str_id);
					
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

						char *topic = mqtt_topic_find(ALI_EVENT_DEVICE_ALARM_PUB);
						if (topic != RT_NULL)
							IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);

						rt_free(msg_pub);
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
			alarm_buff[rt_strlen(alarm_buff) - 1] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					char str_id[16];
					rt_memset(str_id, 0, sizeof(str_id));
					rt_snprintf(str_id, sizeof(str_id), "%d", mqtt_packet_id++);
					cJSON_AddStringToObject(root, "id", str_id);
					
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

						char *topic = mqtt_topic_find(ALI_EVENT_DEVICE_ALARM_PUB);
						if (topic != RT_NULL)
							IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);

						rt_free(msg_pub);
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
			alarm_buff[rt_strlen(alarm_buff) - 1] = '\0';
			
			char  *msg_pub;
			cJSON *root = cJSON_CreateObject();
			if (root)
			{
				cJSON *js_params = cJSON_CreateObject();
				cJSON *js_value  = cJSON_CreateObject();
				if (js_params && js_value)
				{
					char str_id[16];
					rt_memset(str_id, 0, sizeof(str_id));
					rt_snprintf(str_id, sizeof(str_id), "%d", mqtt_packet_id++);
					cJSON_AddStringToObject(root, "id", str_id);
					
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

						char *topic = mqtt_topic_find(ALI_EVENT_DEVICE_ALARM_PUB);
						if (topic != RT_NULL)
							IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);

						rt_free(msg_pub);
					}
				}
				cJSON_Delete(root);
			}
		}
	}

	/* 周期上报设备参数 */
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
				//extern rt_uint8_t sim800c_rssi;
				rt_uint8_t sim800c_rssi = 20;
				
				char str_id[16];
				rt_memset(str_id, 0, sizeof(str_id));
				rt_snprintf(str_id, sizeof(str_id), "%d", mqtt_packet_id++);
				cJSON_AddStringToObject(root, "id", str_id);
				
				cJSON_AddStringToObject(root, "version", "1.0");
				cJSON_AddItemToObject(root, "params", js_params);
				cJSON_AddItemToObject(js_params, "signal_quality", js_quality);
				cJSON_AddNumberToObject(js_quality, "value", sim800c_rssi);
				cJSON_AddNumberToObject(js_quality, "time", now);
				cJSON_AddItemToObject(js_params, "door_status", js_status);
				cJSON_AddStringToObject(js_status, "value", status_buff);
				cJSON_AddNumberToObject(js_status, "time", now);

				/* 获取当前使用的网卡名字 */
				cJSON_AddItemToObject(js_params, "net_device", js_device);
				struct netdev *default_netdev = netdev_get_first_by_flags(NETDEV_FLAG_LINK_UP);
				if (default_netdev)
					cJSON_AddStringToObject(js_device, "value", default_netdev->name);
				else
					cJSON_AddStringToObject(js_device, "value", "unknown");					
				cJSON_AddNumberToObject(js_device, "time", now);
				
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

					char *topic = mqtt_topic_find(ALI_PROPERTY_POST_PUB);
					if (topic != RT_NULL)
						IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);

					rt_free(msg_pub);
				}
			}
			cJSON_Delete(root);
		}
	}
}


static void mqtt_devtag_task(void)
{
	char dev_info[64];
	rt_memset(dev_info, 0x0, sizeof(dev_info));
	if (ef_get_env_blob(ALI_DEVICE_INFO_NAME, dev_info, sizeof(dev_info), RT_NULL) <= 0)
	{
		rt_strncpy(dev_info, ALI_DEVICE_INFO_DEFAULT, rt_strlen(ALI_DEVICE_INFO_DEFAULT));
		ef_set_env_blob(ALI_DEVICE_INFO_NAME, dev_info, rt_strlen(dev_info));
	}

	char msg_pub[128];
	rt_memset(msg_pub, 0x0, sizeof(msg_pub));
	rt_snprintf(msg_pub, sizeof(msg_pub), "{\"id\": \"123\",\"version\": \"1.0\",\"params\": [{\"attrKey\": \"%s\",\"attrValue\": \"%s\"}],\"method\": \"thing.deviceinfo.update\"}", ALI_DEVICE_INFO_NAME, dev_info);
	
	iotx_mqtt_topic_info_t topic_msg;
	rt_memset(&topic_msg, 0, sizeof(iotx_mqtt_topic_info_t));
	topic_msg.qos    = IOTX_MQTT_QOS1;
    topic_msg.retain = 0;
    topic_msg.dup    = 0;
    topic_msg.payload     = (void *)msg_pub;
    topic_msg.payload_len = rt_strlen(msg_pub);    

	char *topic = mqtt_topic_find(ALI_DEVICEINFO_UPDATE_PUB);
	if (topic != RT_NULL)
		IOT_MQTT_Publish(mqtt_client_hd, topic, &topic_msg);  
}

static void mqtt_connect_check_thread(void *arg)
{
	struct netdev *netdev_link;

	while (1)
	{
		if ((is_mqtt_disconnect) && (mqtt_client_hd != RT_NULL))
		{
			if (IOT_MQTT_CheckStateNormal(mqtt_client_hd) <= 0)
			{
				if (is_mqtt_disconnect >= 60)
				{
					netdev_link = netdev_get_first_by_flags(NETDEV_FLAG_LINK_UP);
					if (!rt_strcmp(netdev_link->name, "sim0"))
					{
						netdev_link->ops->set_down(netdev_link);
						rt_thread_mdelay(rt_tick_from_millisecond(RT_TICK_PER_SECOND));
						netdev_link->ops->set_up(netdev_link);
					}
					else
					{
						netdev_low_level_set_status(netdev_link, RT_FALSE);
						rt_thread_mdelay(rt_tick_from_millisecond(RT_TICK_PER_SECOND));
						netdev_low_level_set_status(netdev_link, RT_TRUE);
					}

					/* next period to check */
					is_mqtt_disconnect = 1;
				}
				else
					is_mqtt_disconnect++;
			}
			else
				is_mqtt_disconnect = 0;
		}

		rt_thread_mdelay(rt_tick_from_millisecond(RT_TICK_PER_SECOND));
	}
}

extern int HAL_GetProductKey(char product_key[IOTX_PRODUCT_KEY_LEN + 1]);
extern int HAL_GetProductSecret(char product_secret[IOTX_PRODUCT_SECRET_LEN + 1]);
extern int HAL_GetDeviceName(char device_name[IOTX_DEVICE_NAME_LEN + 1]);
extern int HAL_GetDeviceSecret(char device_secret[IOTX_DEVICE_SECRET_LEN + 1]);

static void mqtt_thread_main_thread(void *arg)
{
#if 0
	/* 确定其中至少有网卡上线才执行线程 */
	struct netdev *netdev_link = RT_NULL;
	while (netdev_link == RT_NULL)
	{
		struct netdev *netdev_link = netdev_get_first_by_flags(NETDEV_FLAG_LINK_UP);
		if (netdev_link)
			netdev_set_default(netdev_link);
		
		rt_thread_mdelay(rt_tick_from_millisecond(RT_TICK_PER_SECOND));
	}
#endif
	
	/* 如果读存储的设备密码错误或者为空,那么就进行动态注册 */
	iotx_dev_meta_info_t meta;
	memset(&meta, 0x0, sizeof(iotx_dev_meta_info_t));
	
	iotx_sign_mqtt_t sign_mqtt;
	iotx_http_region_types_t region = IOTX_HTTP_REGION_SHANGHAI;

	if (topic_buff != RT_NULL)
	{
		rt_free(topic_buff);
		topic_buff = RT_NULL;
	}

	char *topic = RT_NULL;
	while (topic == RT_NULL)
	{
		mqtt_check_load_topic(meta.product_key, meta.device_name, topic);
	}
	topic_buff = topic;

	HAL_GetProductKey(meta.product_key);
	HAL_GetProductSecret(meta.product_secret);
	HAL_GetDeviceName(meta.device_name);
	
	if (HAL_GetDeviceSecret(meta.product_secret) <= 0)
	{
		while (1)
		{
			if (IOT_Dynamic_Register(region, &meta) < 0)
			{
				LOG_D("IOT_Dynamic_Register failed.");
			}
			else
			{
				LOG_D("Device Secret: %s.", meta.device_secret);
				break;
			}
		}
	}

	while (is_mqtt_exit == 0)
	{
		int i;
		int mqtt_period_cnt = 0;
		int sub_items = sizeof(mqtt_sub_item) / sizeof(mqtt_subscribe_item);
		
		for (int32_t res = -1; res < 0;) 
		{
			res = IOT_Sign_MQTT(region, &meta, &sign_mqtt);
			LOG_D("Device Sign failed: 0x%x", res);
	    }

		LOG_D("sign_mqtt.hostname: %s", sign_mqtt.hostname);
	    LOG_D("sign_mqtt.port    : %d", sign_mqtt.port);
	    LOG_D("sign_mqtt.username: %s", sign_mqtt.username);
	    LOG_D("sign_mqtt.password: %s", sign_mqtt.password);
	    LOG_D("sign_mqtt.clientid: %s", sign_mqtt.clientid);

		/* Initialize MQTT parameter */
		iotx_mqtt_param_t mqtt_params;
	    rt_memset(&mqtt_params, 0x0, sizeof(mqtt_params));
		
	    /* feedback parameter of platform when use IOT_SetupConnInfo() connect */
		mqtt_params.customize_info = MQTT_MAN_INFO_STRING;
		
	    mqtt_params.port      = sign_mqtt.port;
	    mqtt_params.host      = sign_mqtt.hostname;
	    mqtt_params.client_id = sign_mqtt.clientid;
	    mqtt_params.username  = sign_mqtt.username;
	    mqtt_params.password  = sign_mqtt.password;
		/* not use TLS or SSL, only TCP channel */
	    mqtt_params.pub_key = RT_NULL;
	    /* timeout of request. uint: ms */
	    mqtt_params.request_timeout_ms = 2000;
	    mqtt_params.clean_session      = 0;
	    /* internal of keepalive checking: 60s~300s */
	    mqtt_params.keepalive_interval_ms = MQTT_KEEPALIVE_INTERNAL * 1000; 
		/* MQTT read/write buffer size */
	    mqtt_params.read_buf_size  = MQTT_MSGLEN;
	    mqtt_params.write_buf_size = MQTT_MSGLEN;
	    /* configure handle of event */
	    mqtt_params.handle_event.h_fp     = ali_mqtt_event_handle;
	    mqtt_params.handle_event.pcontext = RT_NULL;
	
		/* construct a MQTT device with specify parameter */
	    mqtt_client_hd = IOT_MQTT_Construct(&mqtt_params);
	    if (RT_NULL == mqtt_client_hd) 
	    {
	        LOG_D("construct MQTT failed!");
	        rt_thread_mdelay(rt_tick_from_millisecond(RT_TICK_PER_SECOND));
			continue;
	    }
        
        /* sbuscribe all topic */
        for (i = 0; i < sub_items; i++)
        {	
        	if (mqtt_sub_item[i].topic_handle_func == RT_NULL)
        		continue;
					
            if (IOT_MQTT_Subscribe(mqtt_client_hd, &topic[i * 128], mqtt_sub_item[i].qos, mqtt_sub_item[i].topic_handle_func, mqtt_sub_item[i].pcontext) < 0)
            {
                LOG_D("IOT_MQTT_Subscribe() failed, topic = %s", &topic[i * 128]);
                goto __do_main_release;
            }         
        }

		IOT_MQTT_Yield(mqtt_client_hd, 200);
		is_mqtt_disconnect = 0;

		/* 每次连接成功后发送一次设备标签信息 */
		mqtt_devtag_task();
	
        while (is_mqtt_exit == 0)
        {
            /* handle the MQTT packet received from TCP or SSL connection */
            IOT_MQTT_Yield(mqtt_client_hd, 200);

			/* 每10s执行一次周期任务 */
			if ((mqtt_period_cnt % 50) == 0)
				mqtt_period_task();

			/* OTA周期执行 */
			extern rt_err_t mqtt_ota(void *mqtt_ota_hd, char *product_key, char *device_name);
			mqtt_ota(mqtt_client_hd, meta.product_key, meta.device_name);
			
            mqtt_period_cnt++;        
        }
		
        IOT_MQTT_Yield(mqtt_client_hd, 200);
		is_mqtt_disconnect = 1;

		/* OTA模块释放 */
		extern void mqtt_ota_deinit(void);
		mqtt_ota_deinit();

__do_main_release:
        /* ubsbuscribe all topic */
        for (i = 0; i < sub_items; i++)			
            IOT_MQTT_Unsubscribe(mqtt_client_hd, &topic[i * 128]);

		if (RT_NULL != mqtt_client_hd)
		{
        	IOT_MQTT_Destroy(&mqtt_client_hd);
			mqtt_client_hd = RT_NULL;
		}
	}

	if (topic_buff != RT_NULL)
	{
		rt_free(topic_buff);
		topic_buff = RT_NULL;
	}
}

static int ali_mqtt_init(void)
{
	rt_thread_t tid;
	
    tid = rt_thread_create("mqtt.main", mqtt_thread_main_thread, RT_NULL, 6 * 1024, RT_THREAD_PRIORITY_MAX / 2 - 1, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);

	tid = rt_thread_create("mqtt.chk", mqtt_connect_check_thread, RT_NULL, 2 * 1024, RT_THREAD_PRIORITY_MAX / 2, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);

    return 0;
}
INIT_APP_EXPORT(ali_mqtt_init);


