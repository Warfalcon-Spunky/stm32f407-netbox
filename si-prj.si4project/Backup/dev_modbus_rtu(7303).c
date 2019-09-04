/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2019-05-19     Warfalcon    first implementation
 */
 
#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>
#include "easyflash.h"
#include "modbus.h"
#include "string.h"
#include "mqtt-def.h"

#define DEV_MODBUS_DEBUG			

#define THE_QUEUE_NAME					"rtu-msg"
#define THE_QUEUE_TIMEOUT				200
#define THE_MAX_QUEUE_MSG_SIZE			256
#define THE_MAX_QUEUE_SIZE				16

#define DOOR_CTRL_CMD					"door_idx"
#define DEV_CTRL_CMD					"ctrl_cmd"
#define DEV_CTRL_PARA					"ctrl_para"
#define DEV_CTRL_CMD_REBOOT				"reboot"
#define DEV_CTRL_CMD_BEEP				"beep"
#define DEV_CTRL_CMD_RESET				"reset"

#define MODBUS_SERIAL_DEV				"/dev/uart2"
#define MODBUS_SERIAL_BANDRATE			19200
#define MODBUS_RS485_RE            		93

#define MODBUS_REGS_ADDR				21
#define MODBUS_ALARM_NUMS				3			/* ÿ���豸�ܹ���3�ֱ���:�Ƿ����ű���,��ʱ�����͹������� */

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
#include <ulog.h>

static rt_mq_t msg_queue = RT_NULL;
static char *msg_buff = RT_NULL;
/* ÿ1�������ź���1���ֽ�����ʾ,              ��0������� */
static rt_uint8_t *door_coil_buff = RT_NULL;
/* ÿ1��״̬�ź���1���ֽ�����ʾ,              ��0�����ſ� */
static rt_uint8_t *door_status_buff = RT_NULL;
/* ÿ1�������ź���2���ֽ�����ʾ */
static rt_uint16_t *door_alarm_buff = RT_NULL;
/* ÿ1������ʧ���ź���1���ֽ�����ʾ */
static rt_uint8_t *door_fail_buff = RT_NULL;
/* ÿ1���豸����1���ֽ�����ʾ */
static rt_uint8_t *dev_error_buff = RT_NULL;
/* RS485���߹��ص�����豸�� */
static rt_uint8_t device_num;		
/* ÿ���豸�����ͨ���� */
static rt_uint8_t device_chn_num;

rt_err_t dev_modbus_get_period_data(rt_uint8_t **door_status, rt_uint16_t **door_alarm, rt_uint8_t **dev_error)
{	
	if (door_status_buff == RT_NULL)
		return RT_ERROR;
	if (door_alarm_buff == RT_NULL)
		return RT_ERROR;
	if (dev_error_buff == RT_NULL)
		return RT_ERROR;

	*door_status = door_status_buff;
	*door_alarm = door_alarm_buff;
	*dev_error = dev_error_buff;
	return RT_EOK;
}

rt_err_t dev_modbus_send_queue_msg(char *queue_msg)
{
	RT_ASSERT(queue_msg != RT_NULL);

	return rt_mq_urgent(msg_queue, queue_msg, rt_strlen(queue_msg));
}

static void dev_modbus_thread(void *param)
{
	rt_err_t err;
    modbus_t *ctx;
    ctx = modbus_new_rtu(MODBUS_SERIAL_DEV, MODBUS_SERIAL_BANDRATE, 'N', 8, 1);
	RT_ASSERT(ctx != RT_NULL);

	/* ��ʼ��RS485����������� */
    rt_pin_mode(MODBUS_RS485_RE, PIN_MODE_OUTPUT);
    modbus_rtu_set_serial_mode(ctx, MODBUS_RTU_RS485);
    modbus_rtu_set_rts(ctx, MODBUS_RS485_RE, MODBUS_RTU_RTS_UP);

	/* ��ʼ��������Ϊ�㲥��ַ, �ӻ���ַ����ܳ���247 */
    modbus_set_slave(ctx, 0);
	/* �򿪴��ڲ���,�������������úʹ��ڳ�ʼ���� */
    modbus_connect(ctx);
	/* ��ʼ�����ó�ʱʱ��Ϊ1s */
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

	door_status_buff = rt_malloc(device_chn_num * device_num);
	RT_ASSERT(door_status_buff != RT_NULL);
	
	door_alarm_buff = rt_malloc(device_num * 2 * MODBUS_ALARM_NUMS);
	RT_ASSERT(door_alarm_buff != RT_NULL);

	door_fail_buff = rt_malloc(device_chn_num * device_num);
	RT_ASSERT(door_fail_buff != RT_NULL);

	dev_error_buff = rt_malloc(device_num);
	RT_ASSERT(dev_error_buff != RT_NULL);
	
    while (1)
    {
		/* ����ָ����Ϣ��ʽ:id=123;door_idx=1,2,3,4,5,6,....,MAXDOOR */
		err = rt_mq_recv(msg_queue, msg_buff, THE_MAX_QUEUE_MSG_SIZE, THE_QUEUE_TIMEOUT);
		if (err == RT_EOK)		
		{
			char *str_id, *str_cmd, *str_val;
			
			/* �κ���Ϣ��Ӧ�ô���id=XXX,����id */
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
			
			/* ������������ */
			str_cmd = str_val + 1;
			str_val = rt_strstr(str_cmd, "=");
			if (str_val)
			{
				extern void mqtt_service_reply_pub(const char *topic_idx, const char *id, const char *code, const char *data);
				
				*str_val = '\0'; str_val++;
				if (!rt_strcasecmp(str_cmd, DOOR_CTRL_CMD))
				{
					rt_uint8_t door_idx, last_door_idx = 0;
					/* 1���ֽڴ���1����,����ȫ����ʼ��Ϊ�����ƿ���:THE_DOOR_CTRL_CLOSE */
					rt_memset(door_coil_buff, THE_DOOR_CTRL_CLOSE, device_chn_num * device_num);
					/* 1���ֽڴ���1����,����ȫ����ʼ��Ϊ�����ƿ���:THE_DOOR_IS_CLOSE */
					rt_memset(door_status_buff, THE_DOOR_IS_CLOSE, device_chn_num * device_num);
					/* ������Ϣ���� */
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
					/* ��Ϣ������Ͻ������ݷ��� */
					for (int i = 0; i < device_num; i++)
					{
						/* ���ôӻ���ַ,�ӻ���ַ��1��ʼ */
						modbus_set_slave(ctx, i + 1);
						/* ���ó�ʱʱ��,����1����1s��ʱʱ����� */
    					modbus_set_response_timeout(ctx, 16, 0);
						/* ͨ��MODBUS��������,�ȴ���Ӧ */
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

					rt_memset(door_fail_buff, 0, device_num * device_chn_num);
					/* ��ȡ��״ֵ̬,�鿴�Ƿ�򿪳ɹ� */
					for (int i = 0; i < device_num; i++)
					{
						/* ���ôӻ���ַ,�ӻ���ַ��1��ʼ */
						modbus_set_slave(ctx, i + 1);
						/* ���ó�ʱʱ�� */
    					modbus_set_response_timeout(ctx, 3, 0);
						/* ͨ��MODBUS��������,�ȴ���Ӧ */
						if (modbus_read_input_bits(ctx, 0, device_chn_num, &door_status_buff[i * device_chn_num]) > 0)
						{
							LOG_D("modbus device[%d] read success.", i + 1);
							THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
							
							for (int j = 0; j < device_chn_num; j++)
							{
								if (door_coil_buff[j] != door_status_buff[j])
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
						}
					}

					/* �����Ŵ�ʧ�ܵ�topic_data */
					int pos = 0;
					char topic_data[128];
					rt_memset(topic_data, 0, sizeof(topic_data));
					for (int i = 0; (i < (device_num * device_chn_num)) && (pos < sizeof(topic_data)); i++)
					{
						if (door_fail_buff[i])
						{
							if (i == ((device_num * device_chn_num) - 1))
								pos += rt_snprintf(&topic_data[pos], sizeof(topic_data) - pos - 1, "%d", i + 1);
							else
								pos += rt_snprintf(&topic_data[pos], sizeof(topic_data) - pos - 1, "%d,", i + 1);
						}
					}

					if (pos > 0)
						mqtt_service_reply_pub(ALI_SERVICE_DOOR_CTRL_REPLY_PUB, str_id, ALI_CODE_DOOR_CTRL_FAIL, topic_data);
					else
						mqtt_service_reply_pub(ALI_SERVICE_DOOR_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
				}
				else if (!rt_strstr(str_cmd, DEV_CTRL_CMD))
				{
					str_cmd = str_val + 1;
					str_val = rt_strstr(str_cmd, DEV_CTRL_PARA);
					if (str_val)
					{
						if (!rt_strcasecmp(str_val, DEV_CTRL_CMD_REBOOT))
						{
							LOG_D("-----------------------");
							LOG_D("ctrl cmd:  ---reboot---");
							LOG_D("-----------------------");
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
							rt_thread_mdelay(10 * 1000);
							LOG_D("device reboot after 10s");
						}
						else if (!rt_strcasecmp(str_val, DEV_CTRL_CMD_BEEP))
						{
							LOG_D("---------------------");
							LOG_D("ctrl cmd:  ---beep---");
							LOG_D("---------------------");
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
						}
						else if (!rt_strcasecmp(str_val, DEV_CTRL_CMD_RESET))
						{
							LOG_D("----------------------");
							LOG_D("ctrl cmd:  ---reset---");
							LOG_D("----------------------");
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_OK, RT_NULL);
						}
						else
						{
							LOG_D("-----------------------");
							LOG_D("ctrl cmd:  ---unknow---");
							LOG_D("-----------------------");
							mqtt_service_reply_pub(ALI_SERVICE_DEVICE_CTRL_REPLY_PUB, str_id, ALI_CODE_DEV_CTRL_FAIL, RT_NULL);
						}
					}
				}
			}
		}
		else if (err == -RT_ETIMEOUT)	/* ��ʱ����ʱɨ��ʱ�䵽 */
		{
			for (int i = 0; i < device_num; i++)
			{
				/* ���ôӻ���ַ,�ӻ���ַ��1��ʼ */
				modbus_set_slave(ctx, i + 1);
				/* ���ó�ʱʱ�� */
				modbus_set_response_timeout(ctx, 3, 0);
				
				/* ����״̬ */
				if (modbus_read_input_bits(ctx, 0, device_chn_num, &door_status_buff[i * device_chn_num]) > 0)
				{
					THE_DEVICE_COMM_CLEAN(dev_error_buff[i]);
				}
				else
				{
					LOG_D("modbus device[%d] door status read failed.", i + 1);
					THE_DEVICE_COMM_ERROR(dev_error_buff[i]);
				}
				/* ������״̬ */
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
	
    /* �ر�MODBUS�������Դ */
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
	char str_dev_chn_num[8];
	rt_memset(str_dev_num, 0, sizeof(str_dev_num));
	rt_memset(str_dev_chn_num, 0, sizeof(str_dev_chn_num));
	
	if (ef_get_env_blob(ALI_DEVICE_NUM, str_dev_num, sizeof(str_dev_num), RT_NULL) <= 0)
	{
		LOG_D("%s read failed.", ALI_DEVICE_NUM);
		return RT_ERROR;
	}

	if (ef_get_env_blob(ALI_DEVICE_CHN_NUM, str_dev_chn_num, sizeof(str_dev_chn_num), RT_NULL) <= 0)
	{
		LOG_D("%s read failed.", ALI_DEVICE_CHN_NUM);
		return RT_ERROR;
	}

	device_num = atoi(str_dev_num);
	device_chn_num = atoi(str_dev_chn_num);
	LOG_D("device_num=%d, device_chn_num=%d", device_num, device_chn_num);
	
    rt_thread_t tid;
	tid = rt_thread_create("modbus", dev_modbus_thread, RT_NULL, 4096, 12, 10);
    if (tid != RT_NULL)
        rt_thread_startup(tid);
	
    return RT_EOK;
}
INIT_APP_EXPORT(dev_modbus_rtu_init);
