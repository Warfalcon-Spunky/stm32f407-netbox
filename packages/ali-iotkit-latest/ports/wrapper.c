/*
 * Copyright (C) 2015-2018 Alibaba Group Holding Limited
 * 
 * Again edit by rt-thread group
 * Change Logs:
 * Date          Author          Notes
 * 2019-07-21    MurphyZhao      first edit
 * 2019-09-03    Spunky          modify information
 */

#include <rtthread.h>
#include <string.h>

#include "wrappers_defs.h"
#include "board.h"
#include "easyflash.h"

#define DBG_TAG                        "ali.wrap"
#define DBG_LVL                        DBG_INFO
#include <rtdbg.h>
#include "app_log.h"

#if defined(PKG_USING_ALI_IOTKIT_PRODUCT_KEY)
	#define  ALI_IOTKIT_PRODUCT_KEY			PKG_USING_ALI_IOTKIT_PRODUCT_KEY
#else
	#define  ALI_IOTKIT_PRODUCT_KEY			""
#endif

#if defined(PKG_USING_ALI_IOTKIT_PRODUCT_SECRET)
	#define ALI_IOTKIT_PRODUCT_SECRET		PKG_USING_ALI_IOTKIT_PRODUCT_SECRET
#else
	#define ALI_IOTKIT_PRODUCT_SECRET		""
#endif

#if defined(PKG_USING_ALI_IOTKIT_DEVICE_SECRET)
	#define ALI_IOTKIT_DEVICE_SECRET		PKG_USING_ALI_IOTKIT_DEVICE_SECRET
#else
	#define ALI_IOTKIT_DEVICE_SECRET		""
#endif

int HAL_GetFirmwareVersion(char *version)
{
    if (version == RT_NULL)
    	return -1;

	rt_memset(version, 0x0, IOTX_FIRMWARE_VER_LEN);
	rt_snprintf(version, IOTX_FIRMWARE_VER_LEN, "os-%d:app-%s", RTTHREAD_VERSION, APP_VERSION);

    return rt_strlen(version);
}

int HAL_SetProductKey(char* product_key)
{
	product_key = product_key;
	
    LOG_I("ProductKey is fixed [Read-only]");

    return 0;
}

int HAL_SetDeviceName(char* device_name)
{
	device_name = device_name;
	
    LOG_I("DeviceName is fixed [Read-only]");

    return 0;

}

int HAL_SetDeviceSecret(char* device_secret)
{
    int len = strlen(device_secret);
	if (len > IOTX_DEVICE_SECRET_LEN)
	{
		LOG_D("DeviceSecret length exceed limit.");
        return -1;
	}

	if (ef_set_env_blob("DeviceSecret", device_secret, len) != EF_NO_ERR)
	{
		LOG_D("DeviceSecret write error.");
		return -2;
	}

    return len;
}

int HAL_SetProductSecret(char* product_secret)
{
	product_secret = product_secret;
	
    LOG_I("ProductSecret is fixed [Read-only]");

    return 0;

}

int HAL_GetProductKey(char product_key[IOTX_PRODUCT_KEY_LEN + 1])
{   
	int len = rt_strlen(ALI_IOTKIT_PRODUCT_KEY);
	if (len > IOTX_PRODUCT_KEY_LEN)
	{
		len = IOTX_PRODUCT_KEY_LEN;
	}
	
	rt_memset(product_key, 0x0, IOTX_PRODUCT_KEY_LEN + 1);
    rt_strncpy(product_key, ALI_IOTKIT_PRODUCT_KEY, len);
    return len;
}

int HAL_GetProductSecret(char product_secret[IOTX_PRODUCT_SECRET_LEN + 1])
{
	int len = rt_strlen(ALI_IOTKIT_PRODUCT_SECRET);
	if (len > IOTX_PRODUCT_SECRET_LEN)
	{
		len = IOTX_PRODUCT_SECRET_LEN;
	}
	
	rt_memset(product_secret, 0x0, IOTX_PRODUCT_SECRET_LEN + 1);
    rt_strncpy(product_secret, ALI_IOTKIT_PRODUCT_SECRET, len);
    return len;
}

int HAL_GetDeviceName(char device_name[IOTX_DEVICE_NAME_LEN + 1])
{
	/* DeviceName使用MCU的UID的前8个字节 */
    rt_memset(device_name, 0x0, IOTX_DEVICE_NAME_LEN + 1);
    rt_snprintf(device_name, IOTX_DEVICE_NAME_LEN, "%08x%08x", HAL_GetUIDw1(), HAL_GetUIDw0());

    return rt_strlen(device_name);
}

int HAL_GetDeviceSecret(char device_secret[IOTX_DEVICE_SECRET_LEN + 1])
{
#if 0
	int len = rt_strlen(ALI_IOTKIT_DEVICE_SECRET);
	if (len > IOTX_DEVICE_SECRET_LEN)
	{
		len = IOTX_DEVICE_SECRET_LEN;
	}
	
    rt_memset(device_secret, 0x0, IOTX_DEVICE_SECRET_LEN + 1);
	rt_strncpy(device_secret, ALI_IOTKIT_DEVICE_SECRET, len);
    return len;
#endif

#if 1
    int len = ef_get_env_blob("DeviceSecret", device_secret, IOTX_DEVICE_SECRET_LEN, RT_NULL);

	if (len < 0)
	{
		LOG_D("Read DeviceSecret Error");
	}
	else if (len == 0)
	{
		LOG_D("DeviceSecret None Configuration");
	}

	return len;
#endif
}

RT_WEAK int HAL_Kv_Set(const char *key, const void *val, int len, int sync)
{
	LOG_I("Kv Set... [Not implemented]");
    return -1;
}
	
RT_WEAK int HAL_Kv_Get(const char *key, void *val, int *buffer_len)
{
	LOG_I("Kv Get... [Not implemented]");
    return -1;
}

RT_WEAK void HAL_Firmware_Persistence_Start(void)
{
    LOG_I("OTA start... [Not implemented]");
    return;
}

RT_WEAK int HAL_Firmware_Persistence_Write(char *buffer, uint32_t length)
{
    LOG_I("OTA write... [Not implemented]");
    return 0;
}

RT_WEAK int HAL_Firmware_Persistence_Stop(void)
{
    /* check file md5, and burning it to flash ... finally reboot system */

    LOG_I("OTA finish... [Not implemented]");
    return 0;
}
