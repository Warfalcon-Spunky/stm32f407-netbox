/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-05-19     Warfalcon    first implementation
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "easyflash.h"
#include "modbus.h"
#include "string.h"
#include "mqtt-def.h"

// #define DEV_MODBUS_DEBUG			

#define THE_QUEUE_NAME					"rtu-msg"
#define THE_QUEUE_TIMEOUT				200
#define THE_MAX_QUEUE_MSG_SIZE			256
#define THE_MAX_QUEUE_SIZE				16

#define DOOR_CTRL_CMD					"door_idx"
#define DEV_CTRL_CMD					"ctrl_cmd"
#define DEV_CTRL_PARA					"ctrl_para"
#define DEV_CTRL_CMD_OPENTIME			"max_door_open_time"
#define DEV_CTRL_CMD_POWERTIME			"max_door_power_time"

#define MODBUS_SERIAL_DEV				"/dev/uart2"
#define MODBUS_SERIAL_BANDRATE			9600

#define MODBUS_REGS_ADDR				21
#define MODBUS_ALARM_NUMS				3			/* 每个设备总共有3种报警:非法撬门报警,超时报警和过流报警 */

#define MODBUS_MAX_OPEN_TIME_ADDR		24
#define MODBUS_MAX_OPEN_TIME_NUM		1

#define MODBUS_MAX_POWER_TIME_ADDR		25
#define MODBUS_MAX_POWER_TIME_NUM		1

#define THE_DOOR_CTRL_OPEN				1
#define THE_DOOR_CTRL_CLOSE				0
#define THE_DOOR_IS_OPEN				1
#define THE_DOOR_IS_CLOSE				0

#define THE_DEVICE_COMM_ERRCNT			10
#define THE_DEVICE_COMM_ERROR(err_cnt)			\
{												\
	if (err_cnt < 0xff)							\
	{											\
		err_cnt++;								\
	}											\
}

#define THE_DEVICE_COMM_CLEAN(err_cnt)			\
{												\
	if (err_cnt >= THE_DEVICE_COMM_ERRCNT)		\
	{											\
		err_cnt = THE_DEVICE_COMM_ERRCNT - 1;	\
	}											\
	else										\
	{											\
		if (err_cnt)							\
			err_cnt--;							\
	}											\
}

#define LOG_TAG              "mod-rtu"    
#define LOG_LVL              LOG_LVL_ASSERT
#include <ulog.h>

static rt_mq_t msg_queue = RT_NULL;
static char *msg_buff = RT_NULL;
/* 每1个控制门号以1个字节来表示,              非0代表控制 */
static rt_uint8_t *door_coil_buff = RT_NULL;
/* 每1个状态门号以1个字节来表示,              非0代表门开 */
static rt_uint8_t *door_status_buff = RT_NULL;
/* 每1个报警门号以2个字节来表示 */
static rt_uint16_t *door_alarm_buff = RT_NULL;
/* 每1个控制失败门号以1个字节来表示 */
static rt_uint8_t *door_fail_buff = RT_NULL;
/* 每1个设备号以1个字节来表示 */
static rt_uint8_t *dev_error_buff = RT_NULL;
/* RS485总线挂载的最大设备数 */
static rt_uint8_t device_num;		
/* 每个设备的最大通道数 */
static rt_uint8_t device_chn_num;

rt_err_t dev_modbus_get_period_data(rt_uint8_t **door_status, rt_uint16_t **door_alarm, rt_uint8_t **dev_error, rt_uint8_t *p_device_num, rt_uint8_t *p_device_chn_num)
{	
	if (door_status_buff == RT_NULL)
		return RT_ERROR;
	if (door_alarm_buff == RT_NULL)
		return RT_ERROR;
	if (dev_error_buff == RT_NULL)
		return RT_ERROR;
	if (p_device_num == RT_NULL)
		return RT_ERROR;
	if (p_device_chn_num == RT_NULL)
		return RT_ERROR;

	*door_status = door_status_buff;
	*door_alarm = door_alarm_buff;
	*dev_error = dev_error_buff;

	*p_device_num = device_num;
	*p_device_chn_num = device_chn_num;
	return RT_EOK;
}

rt_err_t dev_modbus_send_queue_msg(char *queue_msg)
{
	RT_ASSERT(queue_msg != RT_NULL);

	return rt_mq_urgent(msg_queue, queue_msg, rt_strlen(queue_msg) + 1);
}

static void dev_modbus_thread(void *param)
{
	rt_err_t err;
    modbus_t *ctx;
    ctx = modbus_new_rtu(MODBUS_SERIAL_DEV, MODBUS_SERIAL_BANDRATE, 'N', 8, 1);
	RT_ASSERT(ctx != RT_NULL);

	/* 初始化RS485方向控制引脚 */
#ifdef BSP_USING_SUB_CTRL
    rt_pin_mode(BSP_RS485_DIR_PIN, PIN_MODE_OUTPUT);
    modbus_rtu_set_serial_mode(ctx, BSP_RS485_DIR_PIN);
    modbus_rtu_set_rts(ctx, BSP_RS485_DIR_PIN, MODBUS_RTU_RTS_UP);
#endif

	/* 初始化先设置为广播地址, 从机地址最大不能超过247 */
    modbus_set_slave(ctx, 0);
	/* 打开串口操作,包括波特率设置和串口初始化等 */
    modbus_connect(ctx);
	/* 初始化设置超时时间为1s */
    modbus_set_response_timeout(ctx, 1, 0);

#ifdef DEV_MODBUS_DEBUG
    modbus_set_debug(ctx, RT_TRUE);
#else
	modbus_set_debug(ctx, RT_FALSE);
#endif

	msg_queue = rt_mq_create(THE_QUEUE_NAME, THE_MAX_QUEUE_MSG_SIZE, THE_MAX_QUEUE_SIZE, RT_IPC_FLAG_FIFO);
	RT_ASSERT(msg_queue != RT_NULL);

	msg_buff = rt_malloc(THE_MAX_QUEUE_MSG_SIZE);
	RT_ASSERT(msg_buff != RT_NULL);
	
	door_coil_buff = rt_malloc(device_chn_num * device_num);
	RT_ASSERT(door_coil_buff != RT_NULL);
	rt_memset(door_coil_buff, 0, device_chn_num * device_num);

	door_status_buff = rt_malloc(device_chn_num * device_num);
	RT_ASSERT(door_status_buff != RT_NULL);
	rt_memset(door_status_buff, 0, device_chn_num * device_num);
	
	door_alarm_buff = rt_malloc(device_num * 2 * MODBUS_ALARM_NUMS);
	RT_ASSERT(door_alarm_buff != RT_NULL);
	rt_memset(door_alarm_buff, 0, device_num * 2 * MODBUS_ALARM_NUMS);

	door_fail_buff = rt_malloc(device_chn_num * device_num);
	RT_ASSERT(door_fail_buff != RT_NULL);
	rt_memset(door_fail_buff, 0, device_chn_num * device_num);

	dev_error_buff = rt_malloc(device_num);
	RT_ASSERT(dev_error_buff != RT_NULL);
	rt_memset(dev_error_buff, 0, device_num);
	
    while (1)
    {		
		err = rt_mq_recv(msg_queue, msg_buff, THE_MAX_QUEUE_MSG_SIZE, THE_QUEUE_TIMEOUT);
		if (err == RT_EOK)		
		{	
			/* 指令消息格式:id=123;door_idx=1,2,3,4,5,6,....,MAXDOOR */
			char *str_id, *str_cmd, *str_val;
			
			/* 任何消息都应该带有id=XXX,解析id */
			str_id = rt_strstr(msg_buff, "=");
			if (str_id == RT_NULL)
				continue;				
			str_val = rt_strstr(msg_buff, ";");
			if (str_val == RT_NULL)
				continue;
			str_id = str_id + 1;
			*str_val = '\0';
			if (str_id == str_val)
				continue;
			
			/* 解析具体命令 */
			str_cmd = str_val + 1;
			str_val = rt_strstr(str_cmd, "=");
			if (str_val)
			{
				extern void mqtt_service_reply_pub(const char *topic_idx, const char *id, const char *code, const char *data);
				
				*str_val = '\0'; str_val++;
				if (!rt_strcasecmp(str_cmd, DOOR_CTRL_CMD))
				{
					rt_uint8_t door_idx, last_door_idx = 0;
					/* 1个字节代表1个门,首先全部初始化为不控制开门:THE_DOOR_CTRL_CLOSE */
					rt_memset(door_coil_buff, THE_DOOR_CTRL_CLOSE, device_chn_num * device_num);
					/* 1个字节代表1个门,首先全部初始化为不控制开门:THE_DOOR_IS_CLOSE */
					rt_memset(door_status_buff, THE_DOOR_IS_CLOSE, device_chn_num * device_num);
					/* 解析消息数据 */
					while (1)
					{
						str_cmd = rt_strstr(str_val, ",");
						if (str_cmd)
						{
							*str_cmd = '\0'; 
							
							door_idx = atoi(str_val);
							if ((door_idx > 0) && (door_idx > last_door_idx) && (door_idx <= (device_chn_num * device_num)))
								door_coil_buff[door_idx - 1] = THE_DOOR_CTRL_OPEN;
							else
								LOG_D("door control message have some error to igiore.");

							str_val = str_cmd + 1;
							last_door_idx = door_idx;
						}
						else
						{
							door_idx = atoi(str_val);
							if ((door_idx > 0) && (door_idx > last_door_idx) && (door_idx <= (device_chn_num * device_num)))
								door_coil_buff[door_idx - 1] = THE_DOOR_CTRL_OPEN;
							else
								LOG_D("door control message have some error to igiore.");
								
							break;
						}
					}

					/* 消息解析完毕进行数据发送 */
					for (int i = 0; i < device_num; i++)
					{
                        /* 设置从机地址,从机地址从1开始 */
						modbus_set_slave(ctx, i + 1);
                        
                        uint32_t timeout_cnt = 0;                                                                                   
                        for (int j = 0; j < device_chn_num; j++)
                        {
                            if (door_coil_buff[i * device_chn_num + j])
                                timeout_cnt++;
                        }
                        if (timeout_cnt)
                            /* 设置超时时间,按照1个门1s超时时间计算 */
                            modbus_set_response_timeout(ctx, timeout_cnt, 0);
                        else
                            /* 当前设备没有需要打开的门,直接跳过 */
                            continue;
                        
						/* 通过MODBUS发送数据,等待回应 */
						if (modbus_write_bits(ctx, 0, device_chn_num, &door_coil_buff[i * device_chn_num]) > 0)
						{
							LOG_D("modbus device[%d] write success.", i + 1);
							THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
						}
						else
						{
							LOG_D("modbus device[%d] write failed.", i + 1);
							THE_DEVICE_COMM_ERROR(dev_error_buff[i]);
						}
					}

					/* 等待分板执行 */
					rt_thread_mdelay(RT_TICK_PER_SECOND);

					rt_memset(door_fail_buff, 0, device_num * device_chn_num);
					/* 读取门状态值,查看是否打开成功 */
					for (int i = 0; i < device_num; i++)
					{
						/* 设置从机地址,从机地址从1开始 */
						modbus_set_slave(ctx, i + 1);                                                
						/* 设置单个设备的超时时间: 1s */
    					modbus_set_response_timeout(ctx, 1, 0);
						/* 通过MODBUS发送数据,等待回应 */
						if (modbus_read_input_bits(ctx, 0, device_chn_num, &door_status_buff[i * device_chn_num]) > 0)
						{
							LOG_D("modbus device[%d] read success.", i + 1);
							THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
							
							for (int j = 0; j < device_chn_num; j++)
							{
								if ((door_coil_buff[i * device_chn_num + j] == THE_DOOR_IS_OPEN) && (door_coil_buff[i * device_chn_num + j] != door_status_buff[i * device_chn_num + j]))
								{
									LOG_D("door[%d] open failed.", (i * device_chn_num) + j + 1);
									door_fail_buff[i * device_chn_num + j] = 1;
								}
							}
						}
						else
						{
							LOG_D("modbus device[%d] read failed.", i + 1);                            
							THE_DEVICE_COMM_ERROR(dev_error_buff[i]);      
                            
                            for (int j = 0; j < device_chn_num; j++)
							{
								if ((door_coil_buff[i * device_chn_num + j] == THE_DOOR_IS_OPEN) && (door_coil_buff[i * device_chn_num + j] != door_status_buff[i * device_chn_num + j]))
									door_fail_buff[i * device_chn_num + j] = 1;
							}
						}
					}

					/* 构造门打开失败的topic_data */					
					char topic_data[256];
					rt_memset(topic_data, 0, sizeof(topic_data));

					int err_cnt = 0;
					int pos = 0;
					pos += rt_snprintf(&topic_data[pos], sizeof(topic_data) - pos - 1, "%s", "\"open_fail\":\"");
					for (int i = 0; (i < (device_num * device_chn_num)) && (pos < sizeof(topic_data)); i++)
					{
						if (door_fail_buff[i])
						{
							pos += rt_snprintf(&topic_data[pos], sizeof(topic_data) - pos - 1, "%d,", i + 1);
							err_cnt++;
						}
					}

					if (err_cnt > 0)
                    {
                        topic_data[rt_strlen(topic_data) - 1] = '\"';
						topic_data[rt_strlen(topic_data)] = '\0';
						mqtt_service_reply_pub(ALI_SERVICE_DOOR_CTRL_REPLY_PUB, str_id, ALI_CODE_DOOR_CTRL_FAIL, topic_data);
                    }
					else
						mqtt_service_reply_pub(ALI_SERVICE_DOOR_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
				}
				/* 指令消息格式:id=123;ctrl_cmd=xxx;ctrl_para=xxx */
				else if (!rt_strstr(str_cmd, DEV_CTRL_CMD) && !rt_strstr(str_cmd, DEV_CTRL_PARA))
				{											
					str_cmd = str_val + 1;
					str_val = rt_strstr(str_cmd, ";");
					if (str_val == RT_NULL)
						continue;
					str_val = '\0';
					if (!rt_strcasecmp(str_cmd, DEV_CTRL_CMD_OPENTIME))
					{
						str_cmd = str_val + 1;
						str_val = rt_strstr(str_cmd, "=");
						if (str_val == RT_NULL)
							continue;
						str_cmd = str_val + 1;
						rt_uint16_t open_time = atoi(str_cmd);
						LOG_I("set door max open time: %dsec", open_time);

						rt_bool_t is_set_ok = RT_TRUE;
						/* 设置单个设备超时时间: 1s */
						modbus_set_response_timeout(ctx, 1, 0);
						for (int i = 0; i < device_num; i++)
						{
							/* 设置从机地址,从机地址从1开始 */
							modbus_set_slave(ctx, i + 1);									
							/* 写最大开门报警时间参数 */
							if (modbus_write_registers(ctx, MODBUS_MAX_OPEN_TIME_ADDR, MODBUS_MAX_OPEN_TIME_NUM, &open_time) > 0)
							{
								THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);								
							}
							else
							{
								LOG_D("modbus device[%d] door max open time write failed.", i + 1);
								THE_DEVICE_COMM_ERROR(dev_error_buff[i]);

								is_set_ok = RT_FALSE;								
							}
						}

						if (is_set_ok == RT_TRUE)
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
						else
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEVICE_CTRL_ERROR, RT_NULL);
					}
					else if (!rt_strcasecmp(str_cmd, DEV_CTRL_CMD_POWERTIME))
					{
						str_cmd = str_val + 1;
						str_val = rt_strstr(str_cmd, "=");
						if (str_val == RT_NULL)
							continue;
						str_cmd = str_val + 1;
						rt_uint16_t power_time = atoi(str_cmd);
						LOG_I("set door max power time: %dsec", power_time);

						rt_bool_t is_set_ok = RT_TRUE;
						/* 设置单个设备超时时间: 1s */
						modbus_set_response_timeout(ctx, 1, 0);
						for (int i = 0; i < device_num; i++)
						{
							/* 设置从机地址,从机地址从1开始 */
							modbus_set_slave(ctx, i + 1);									
							/* 写最大开门报警时间参数 */
							if (modbus_write_registers(ctx, MODBUS_MAX_POWER_TIME_ADDR, MODBUS_MAX_POWER_TIME_NUM, &power_time) > 0)
							{
								THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
							}
							else
							{
								LOG_D("modbus device[%d] door max power time write failed.", i + 1);
								THE_DEVICE_COMM_ERROR(dev_error_buff[i]);

								is_set_ok = RT_FALSE;
							}
						}

						if (is_set_ok == RT_TRUE)
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
						else
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEVICE_CTRL_ERROR, RT_NULL);
					}
					else
					{
						mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEVICE_CTRL_ERROR, RT_NULL);
					}
				}
			}
		}
		else if (err == -RT_ETIMEOUT)	/* 超时代表定时扫描时间到 */
		{
			/* 设置单个设备超时时间: 1s */
			modbus_set_response_timeout(ctx, 1, 0);
			for (int i = 0; i < device_num; i++)
			{
				/* 设置从机地址,从机地址从1开始 */
				modbus_set_slave(ctx, i + 1);				
				/* 读门状态 */
				if (modbus_read_input_bits(ctx, 0, device_chn_num, &door_status_buff[i * device_chn_num]) > 0)
				{
					THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
				}
				else
				{
					LOG_D("modbus device[%d] door status read failed.", i + 1);
					THE_DEVICE_COMM_ERROR(dev_error_buff[i]);
				}
				/* 读报警状态 */
				if (modbus_read_registers(ctx, MODBUS_REGS_ADDR, MODBUS_ALARM_NUMS, &door_alarm_buff[i * MODBUS_ALARM_NUMS]) > 0)
				{
					THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
				}
				else
				{
					LOG_D("modbus device[%d] door alarm read failed.", i + 1);
					THE_DEVICE_COMM_ERROR(dev_error_buff[i]);
				}
			}
		}
		else
		{
			LOG_D("queue recv error.");
			break;
		}
    }
	
    /* 关闭MODBUS组件和资源 */
    modbus_close(ctx);
    modbus_free(ctx);

	if (msg_queue)
	{
		rt_mq_delete(msg_queue);
		msg_queue = RT_NULL;
	}
	if (msg_buff)
	{
		rt_free(msg_buff);
		msg_buff = RT_NULL;
	}
	if (door_coil_buff)
	{
		rt_free(door_coil_buff);
		door_coil_buff = RT_NULL;
	}
	if (door_status_buff)
	{
		rt_free(door_status_buff);
		door_status_buff = RT_NULL;
	}
	if (door_alarm_buff)
	{
		rt_free(door_alarm_buff);
		door_alarm_buff = RT_NULL;
	}
	if (dev_error_buff)
	{
		rt_free(dev_error_buff);
		dev_error_buff = RT_NULL;
	}
}

static int dev_modbus_rtu_init(void)
{
	char str_dev_num[8];
	rt_memset(str_dev_num, 0, sizeof(str_dev_num));
	if (ef_get_env_blob(ALI_DEVICE_NUM, str_dev_num, sizeof(str_dev_num), RT_NULL) <= 0)
	{
		rt_snprintf(str_dev_num, sizeof(str_dev_num), "%d", ALI_DEVICE_NUM_DEFAULT);
		ef_set_env_blob(ALI_DEVICE_NUM, str_dev_num, rt_strlen(str_dev_num));
		device_num = ALI_DEVICE_NUM_DEFAULT;
	}
	else
		device_num = atoi(str_dev_num);

	char str_dev_chn_num[8];
	rt_memset(str_dev_chn_num, 0, sizeof(str_dev_chn_num));
	if (ef_get_env_blob(ALI_DEVICE_CHN_NUM, str_dev_chn_num, sizeof(str_dev_chn_num), RT_NULL) <= 0)
	{
		rt_snprintf(str_dev_chn_num, sizeof(str_dev_chn_num), "%d", ALI_DEVICE_CHN_NUM_DEFAULT);
		ef_set_env_blob(ALI_DEVICE_CHN_NUM, str_dev_chn_num, rt_strlen(str_dev_chn_num));
		device_chn_num = ALI_DEVICE_CHN_NUM_DEFAULT;
	}
	else
		device_chn_num = atoi(str_dev_chn_num);
	
    rt_thread_t tid;
	tid = rt_thread_create("modbus", dev_modbus_thread, RT_NULL, 4096, 11, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
	
    return RT_EOK;
}
INIT_APP_EXPORT(dev_modbus_rtu_init);

#ifdef FINSH_USING_MSH
#include <finsh.h>
static void dev_modbus(int argc, char *argv[])
{
	int i, j;
	char coil_char, door_char, online_char;
	char tmp_chars[10];

	if (rt_thread_find("modbus") == RT_NULL)
	{
		rt_kprintf("the modbus thread is not created!");
		return;
	}
	
	if (argc == 1)
	{
		rt_kprintf("device   channel   coil status   door status   alarm status\r\n");
		rt_kprintf("------   -------   -----------   -----------   ------------\r\n");
		for (i = 0; i < device_num; i++)
		{
			if (dev_error_buff[i] < THE_DEVICE_COMM_ERRCNT)
				online_char = 'O';
			else
				online_char = 'F';
			
			for (j = 0; j < device_chn_num; j++)
			{	
				if (door_coil_buff[i * device_chn_num + j])
					coil_char = 'O';
				else
					coil_char = 'C';

				if (door_status_buff[i * device_chn_num + j])
					door_char = 'O';
				else
					door_char = 'C';

				if (door_alarm_buff[i * device_num + 0] & (1 << j))
					tmp_chars[0] = 'C';
				else
					tmp_chars[0] = '*';
				tmp_chars[1] = '|';

				if (door_alarm_buff[i * device_num + 1] & (1 << j))
					tmp_chars[2] = 'T';
				else
					tmp_chars[2] = '*';
				tmp_chars[3] = '|';
				
				if (door_alarm_buff[i * device_num + 2] & (1 << j))
					tmp_chars[4] = 'L';
				else
					tmp_chars[4] = '*';
				tmp_chars[5] = '\0';
				
				rt_kprintf(" %02d-%c        %02d         %c             %c            %s     \r\n", i + 1, online_char, j + 1, coil_char, door_char, tmp_chars);				

				rt_thread_mdelay(100);
			}
		}
	}
}
MSH_CMD_EXPORT(dev_modbus, check modbus device status);
#endif

