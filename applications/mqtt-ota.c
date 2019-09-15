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

#include "infra_compat.h"
#include "mqtt_api.h"
#include "ota_api.h"

#include "rtthread.h"
#include "cJSON.h"
#include "fal.h"
#include "easyflash.h"
#include "mqtt-def.h"

#define LOG_TAG              "ali-ota"    
#define LOG_LVL              LOG_LVL_DBG
#include <ulog.h>

#define MQTT_OTA_RECV_BUFF_LEN					(8193)

static void *ota_hd = RT_NULL;
static char ota_recv_buff[MQTT_OTA_RECV_BUFF_LEN];

static char firmware_verison[MQTT_OTA_VERSION_MAXLEN];

extern int HAL_GetProductKey(char product_key[IOTX_PRODUCT_KEY_LEN + 1]);
extern int HAL_GetDeviceName(char device_name[IOTX_DEVICE_NAME_LEN + 1]);
extern int HAL_GetFirmwareVersion(char *version);


void mqtt_start_period_timer(void)
{
	rt_timer_t period_timer;
	
	period_timer = (rt_timer_t)rt_object_find("ali_timer", RT_Object_Class_Timer);
	if (period_timer)
	{
		rt_timer_start(period_timer);
	}
}

void mqtt_stop_period_timer(void)
{
	rt_timer_t period_timer;
	
	period_timer = (rt_timer_t)rt_object_find("ali_timer", RT_Object_Class_Timer);
	if (period_timer)
	{
		rt_timer_stop(period_timer);
	}
}


rt_err_t mqtt_ota_init(void *mqtt_ota_hd)
{
	if (mqtt_ota_hd == RT_NULL)
		return RT_ERROR;

	if (ota_hd)
		return RT_EOK;

	char product_key[IOTX_PRODUCT_KEY_LEN + 1];
	char device_name[IOTX_DEVICE_NAME_LEN + 1];

	rt_memset(product_key, 0, sizeof(product_key));
	if (HAL_GetProductKey(product_key) <= 0)
	{
		LOG_D("get product key failed");
		return RT_ERROR;
	}

	rt_memset(device_name, 0, sizeof(device_name));
	if (HAL_GetDeviceName(device_name) <= 0)
	{
		LOG_D("get device name failed");
		return RT_ERROR;
	}

	ota_hd = IOT_OTA_Init(product_key, device_name, mqtt_ota_hd);
    if (RT_NULL == ota_hd)
	{
        LOG_D("initialize OTA failed");
		return RT_ERROR;
    }
	
	rt_memset(firmware_verison, 0, sizeof(firmware_verison));
	if (HAL_GetFirmwareVersion(firmware_verison) <= 0)
	{
		LOG_D("get device firmware version failed");
        if (ota_hd)
		{
			IOT_OTA_Deinit(ota_hd);
			ota_hd = RT_NULL;
		}
		return RT_ERROR;
	}   
    
	if (0 != IOT_OTA_ReportVersion(ota_hd, firmware_verison)) 
	{
        LOG_D("report OTA version failed");
		if (ota_hd)
		{
			IOT_OTA_Deinit(ota_hd);
			ota_hd = RT_NULL;
		}
		return RT_ERROR;
    }
	
	return RT_EOK;
}

void mqtt_ota_deinit(void)
{
	if (ota_hd)
	{
		IOT_OTA_Deinit(ota_hd);
		ota_hd = RT_NULL;
	}
}

rt_err_t mqtt_ota(void *mqtt_ota_hd)
{
	rt_err_t result = RT_ERROR;

	if (mqtt_ota_init(mqtt_ota_hd) != RT_EOK)
        goto __mqtt_ota_exit;  
	
	if (IOT_OTA_IsFetching(ota_hd))
	{
		char fm_ver[MQTT_OTA_VERSION_MAXLEN], md5sum[33];		
        IOT_OTA_Ioctl(ota_hd, IOT_OTAG_VERSION, fm_ver, MQTT_OTA_VERSION_MAXLEN);
		IOT_OTA_Ioctl(ota_hd, IOT_OTAG_MD5SUM, md5sum, 33);

		if (!rt_strcasecmp(firmware_verison, fm_ver))
		{
			IOT_OTA_ReportVersion(ota_hd, fm_ver);
			result = RT_EOK;
			goto __mqtt_ota_exit;
		}
		
		const struct fal_partition *dl_partition;
		dl_partition = fal_partition_find(MQTT_OTA_DOWNLOAD_PARTITION_NAME);
		if (dl_partition == RT_NULL)
		{
			LOG_I("can not find %s partition", MQTT_OTA_DOWNLOAD_PARTITION_NAME);
			goto __mqtt_ota_exit;
		}

		if (fal_partition_erase_all(dl_partition) < 0)
		{
			LOG_I("can not erase %s partition", dl_partition->name);
			goto __mqtt_ota_exit;
		}

		mqtt_stop_period_timer();

		int fetch_len;
		rt_uint32_t last_percent = 0, percent = 0;
		rt_uint32_t size_of_download = 0, size_of_file;
		rt_uint32_t content_pos = 0, content_write_sz;
		rt_uint32_t update_grade = 0;;

		/* 循环条件:未下载完成or设备在线 */
		while (!IOT_OTA_IsFetchFinish(ota_hd))
		{			
			fetch_len = IOT_OTA_FetchYield(ota_hd, ota_recv_buff, MQTT_OTA_RECV_BUFF_LEN, 1);						
			if (fetch_len > 0)
			{			
				content_write_sz = fal_partition_write(dl_partition, content_pos, (uint8_t *)ota_recv_buff, fetch_len);
				if (content_write_sz !=  fetch_len)
				{
					LOG_I("Write OTA data to file failed");
					
					IOT_OTA_ReportProgress(ota_hd, IOT_OTAP_BURN_FAILED, RT_NULL);	
					mqtt_ota_deinit();

					mqtt_start_period_timer();
					goto __mqtt_ota_exit;
				}
				else
				{				
					content_pos = content_pos + fetch_len;
					LOG_I("receive %d bytes, total recieve: %d bytes", content_pos, size_of_file);
				}
			}
			else 
			{
				LOG_I("ota fetch failed.");				
				IOT_OTA_ReportProgress(ota_hd, IOT_OTAP_FETCH_FAILED, NULL);	

				if (fetch_len < 0)
				{                		                
	                mqtt_ota_deinit();
					mqtt_start_period_timer();
					goto __mqtt_ota_exit;
				}				
            }			

			/* get OTA information */
            IOT_OTA_Ioctl(ota_hd, IOT_OTAG_FETCHED_SIZE, &size_of_download, 4);
            IOT_OTA_Ioctl(ota_hd, IOT_OTAG_FILE_SIZE, &size_of_file, 4);

			last_percent = percent;
            percent = (size_of_download * 100) / size_of_file;
            if (percent - last_percent > 0) 
			{
				/* 每下载400K上报一次进度 */
				update_grade = (update_grade + 1) % (((MQTT_OTA_RECV_BUFF_LEN - 1) / 1024) * 50);
				if (update_grade == 0)
                	IOT_OTA_ReportProgress(ota_hd, (IOT_OTA_Progress_t)percent, RT_NULL);
            }
			
            IOT_MQTT_Yield(mqtt_ota_hd, 100);
		}
		
		IOT_OTA_Ioctl(ota_hd, IOT_OTAG_MD5SUM, md5sum, 33);
        IOT_OTA_Ioctl(ota_hd, IOT_OTAG_VERSION, fm_ver, MQTT_OTA_VERSION_MAXLEN);

		uint32_t firmware_valid;
		IOT_OTA_Ioctl(ota_hd, IOT_OTAG_CHECK_FIRMWARE, &firmware_valid, 4);
        if ((firmware_valid) && (size_of_download == size_of_file) && (size_of_file > 0))
		{
            LOG_D("The firmware is valid!  Download firmware successfully.");

            LOG_D("OTA FW version: %s", fm_ver);
			LOG_D("OTA FW MD5 Sum: %s", md5sum);

			ef_set_env_blob(MQTT_OTA_FIRMWARE_VERSION, fm_ver, rt_strlen(fm_ver));
			IOT_OTA_ReportVersion(ota_hd, fm_ver);
			result = RT_EOK;			
        }

		mqtt_ota_deinit();
		mqtt_start_period_timer();
	}
	
__mqtt_ota_exit:
	return result;
}


