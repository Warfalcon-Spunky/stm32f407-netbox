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

#define BEEP_PWM_DEVICE  	"pwm1"
#define BEEP_PWM_CH      	1


#define SEMIBREVE_LEN 			1600
/****    播放效果设置        ****/
#define SOUND_SIGNATURE       	0    /* 调号：升0个半音 */
#define SOUND_OCTACHORD       	1    /* 升降八度：升一个八度 */
#define SOUND_SPACE           	4/5  /* 定义普通音符演奏的长度分率, 每4分音符间隔 */

#define SONG_NAME_LENGTH_MAX  	30
#define SONG_DATA_LENGTH_MAX  	500

#define LOG_TAG              	"app.beep"    
#include <app_log.h>

struct beep_song
{
    const rt_uint8_t name[SONG_NAME_LENGTH_MAX];
    const rt_uint8_t data[SONG_DATA_LENGTH_MAX];
};

struct beep_song_data
{
    rt_uint16_t freq;
    rt_uint16_t sound_len;
    rt_uint16_t nosound_len;
};

const struct beep_song song_alarm1 =
{
    .name = "两只老虎",
    .data = {
        0x15, 0x02, 0x16, 0x02, 0x17, 0x02, 0x15, 0x02, 0x15, 0x02,
        0x16, 0x02, 0x17, 0x02, 0x15, 0x02, 0x17, 0x02, 0x18, 0x02,
        0x19, 0x01, 0x17, 0x02, 0x18, 0x02, 0x19, 0x01, 0x19, 0x03,
        0x1A, 0x03, 0x19, 0x03, 0x18, 0x03, 0x17, 0x02, 0x15, 0x16,
        0x19, 0x03, 0x1A, 0x03, 0x19, 0x03, 0x18, 0x03, 0x17, 0x02,
        0x15, 0x16, 0x15, 0x02, 0x0F, 0x02, 0x15, 0x01, 0x15, 0x02,
        0x0F, 0x02, 0x15, 0x01, 0x00, 0x00
    }
};

const struct beep_song song_alarm2 =
{
	.name = "找朋友",
	.data = {
				0x15,0x02, 0x15,0x02, 0x19,0x02, 0x19,0x02, 0x1A,0x02,
				0x1A,0x02, 0x19,0x01, 0x18,0x02, 0x18,0x02, 0x17,0x02,
				0x17,0x02, 0x16,0x02, 0x16,0x02, 0x15,0x01, 0x19,0x02,
				0x19,0x02, 0x18,0x02, 0x18,0x02, 0x17,0x02, 0x17,0x02,
				0x16,0x01, 0x19,0x02, 0x19,0x02, 0x18,0x02, 0x18,0x02,
				0x17,0x02, 0x17,0x02, 0x16,0x01, 0x15,0x02, 0x15,0x02,
				0x19,0x02, 0x19,0x02, 0x1A,0x02, 0x1A,0x02, 0x19,0x01,
				0x18,0x02, 0x18,0x02, 0x17,0x02, 0x17,0x02, 0x16,0x02,
				0x16,0x02, 0x15,0x01, 0x00,0x00 
            }
};

static rt_mailbox_t beep_mbox;

static struct rt_device_pwm *pwm_device = RT_NULL;

/* 原始频率表 CDEFGAB */
static const uint16_t freq_tab[12]  = {262, 277, 294, 311, 330, 349, 369, 392, 415, 440, 466, 494}; 
/* 1~7在频率表中的位置 */
static const uint8_t sign_tab[7]    = {0, 2, 4, 5, 7, 9, 11};
/* 几分音符 2^0,2^1,2^2,2^3... */
static const uint8_t length_tab[7]  = {1, 2, 4, 8, 16, 32, 64};  
/* 新的频率表 */
static rt_uint16_t freq_tab_new[12];

int beep_on(void)
{
	/* 使能蜂鸣器对应的 PWM 通道 */
    rt_pwm_enable(pwm_device, BEEP_PWM_CH);
    return RT_EOK;
}

int beep_off(void)
{
	/* 失能蜂鸣器对应的 PWM 通道 */
    rt_pwm_disable(pwm_device, BEEP_PWM_CH); 
    return RT_EOK;
}

int beep_set(rt_uint16_t freq, rt_uint8_t volume)
{
    rt_uint32_t period, pulse;

    /* 将频率转化为周期 周期单位:ns 频率单位:HZ */
    period = 1000000000 / freq;  //unit:ns 1/HZ*10^9 = ns

    /* 根据声音大小计算占空比 蜂鸣器低电平触发 */
    pulse = period - period / 100 * volume;

    /* 利用 PWM API 设定 周期和占空比 */
    rt_pwm_set(pwm_device, BEEP_PWM_CH, period, pulse);

    return RT_EOK;
}

int beep_init(void)
{
    pwm_device = (struct rt_device_pwm *)rt_device_find(BEEP_PWM_DEVICE);
    if (pwm_device == RT_NULL)
    {
        LOG_D("pwm device %s not found!\n", BEEP_PWM_DEVICE);
        return -RT_ERROR;
    }
	
    return RT_EOK;
}


//signature|调号(0-11)       :  是指乐曲升多少个半音演奏;
//octachord|升降八度(-2到+2) :  < 0 降几个八度; > 0 升几个八度
static int beep_song_decode_new_freq(rt_uint8_t signature, rt_int8_t octachord)
{
    uint8_t i, j;
    for (i = 0; i < 12; i++)        // 根据调号及升降八度来生成新的频率表
    {
        j = i + signature;

        if (j > 11) //升完之后超出本组基本音级，就跳到下一组音级
        {
            j = j - 12;
            freq_tab_new[i] = freq_tab[j] * 2;
        }
        else
        {
            freq_tab_new[i] = freq_tab[j];
        }

        /* 升降八度 */
        if (octachord < 0)
        {
            freq_tab_new[i] >>= (- octachord);
        }
        else if (octachord > 0)
        {
            freq_tab_new[i] <<= octachord; //每升一个八度 频率就翻一倍
        }
    }
    return 0;
}

static int beep_song_decode(rt_uint16_t tone, rt_uint16_t length, rt_uint16_t *freq, rt_uint16_t *sound_len, rt_uint16_t *nosound_len)
{
    static const rt_uint16_t div0_len = SEMIBREVE_LEN;        // 全音符的长度(ms)
    rt_uint16_t note_len, note_sound_len, current_freq;
    rt_uint8_t note, sharp, range, note_div, effect, dotted;

    note = tone % 10;                             //计算出音符
    range = tone / 10 % 10;                       //计算出高低音
    sharp = tone / 100;                           //计算出是否升半

    current_freq = freq_tab_new[sign_tab[note - 1] + sharp]; //查出对应音符的频率

    if (note != 0)
    {
        if (range == 1) current_freq >>= 1;       //低音 降八度
        if (range == 3) current_freq <<= 1;       //高音 升八度
        *freq = current_freq;
    }
    else
    {
        *freq = 0;
    }
    note_div = length_tab[length % 10];           //算出是几分音符

    effect = length / 10 % 10;                    //算出音符类型(0普通1连音2顿音)
    dotted = length / 100;                        //算出是否附点

    note_len = div0_len / note_div;               //算出音符的时长

    if (dotted == 1)
        note_len = note_len + note_len / 2;

    if (effect != 1)
    {
        if (effect == 0)                          //算出普通音符的演奏长度
        {
            note_sound_len = note_len * SOUND_SPACE;
        }
        else                                      //算出顿音的演奏长度
        {
            note_sound_len = note_len / 2;
        }
    }
    else                                          //算出连音的演奏长度
    {
        note_sound_len = note_len;
    }
    if (note == 0)
    {
        note_sound_len = 0;
    }
    *sound_len = note_sound_len;

    *nosound_len = note_len - note_sound_len;     //算出不发音的长度

    return 0;
}

uint16_t beep_song_get_len(const struct beep_song *song)
{
    uint16_t cnt = 0;

    /* 歌曲以0x00 0x00结尾 检测结束标志*/
    while (song->data[cnt])
    {
        cnt += 2;
    }
    return cnt / 2;
}

int beep_song_get_name(const struct beep_song *song, char *name)
{
    int i = 0;
    while (song->name[i])
    {
        name[i] = song->name[i];
        i++;
    }
    name[i] = '\0';
    return 0;
}

uint16_t beep_song_get_data(const struct beep_song *song, uint16_t index, struct beep_song_data *data)
{
    beep_song_decode(song->data[index * 2], song->data[index * 2 + 1], &data->freq, &data->sound_len, &data->nosound_len);

    return 2;
}

void *beep_select_song(rt_uint32_t song_idx)
{
	struct beep_song *song_handle;
	
	if (song_idx == 1)
	{
		beep_song_decode_new_freq(SOUND_SIGNATURE, SOUND_OCTACHORD);
		song_handle = (struct beep_song *)&song_alarm1;		
	}
	else if (song_idx == 2)
	{
		beep_song_decode_new_freq(SOUND_SIGNATURE, SOUND_OCTACHORD);
		song_handle = (struct beep_song *)&song_alarm2;
	}
	else
	{
		song_handle = RT_NULL;
	}
	
	return song_handle;
}

void beep_song_thread(void *arg)
{
	int song_len, index;	
	rt_uint32_t mb_val;
	struct beep_song *song_handle, *song_handle_old;
	struct beep_song_data song_data;

	if (beep_init() != RT_EOK)
	{
		LOG_D("beep driver initial failed.");
		return;
	}

	beep_mbox = rt_mb_create("beep_mb", 4, RT_IPC_FLAG_FIFO);
	if (beep_mbox == RT_NULL)
	{
		LOG_D("beep mailbox create failed.");
		return;
	}

	while (1)
	{	
		if (rt_mb_recv(beep_mbox, (rt_ubase_t *)&mb_val, RT_WAITING_FOREVER) == RT_EOK)
		{
			song_handle = beep_select_song(mb_val);
			if (song_handle == RT_NULL)
			{
				continue;
			}

			mb_val = 0;
			
			song_len = beep_song_get_len(song_handle);
			for (index = 0; index < song_len; index++)
			{				
				beep_song_get_data(song_handle, index, &song_data);
		        beep_set(song_data.freq, 3);
				
		        beep_on();
				rt_thread_mdelay(song_data.sound_len);
				
				beep_off();
		        rt_thread_mdelay(song_data.nosound_len);

				if (rt_mb_recv(beep_mbox, (rt_ubase_t *)&mb_val, RT_WAITING_NO) == RT_EOK)
				{
					song_handle_old = beep_select_song(mb_val);
					if (song_handle_old != RT_NULL)
					{
						if (song_handle != song_handle_old)
						{
							song_handle = song_handle_old;
							song_len = beep_song_get_len(song_handle);
							index = 0;
						}
					}
					else
					{	
						rt_mb_control(beep_mbox, RT_IPC_CMD_RESET, RT_NULL);
						break;
					}
				}
			}
		}
	}		
}

rt_err_t beep_ctrl_song(rt_uint32_t cmd_val)
{
	return rt_mb_send(beep_mbox, cmd_val);
}

int beep_song_init(void)
{
	rt_thread_t tid;
	
	tid = rt_thread_create("song", beep_song_thread, RT_NULL, 512, 2, 10);
	if (tid != RT_NULL)
	{
		rt_thread_startup(tid);
	}

	return 0;
}
INIT_APP_EXPORT(beep_song_init);

#ifdef FINSH_USING_MSH
#include <finsh.h>
static void beep_test(int argc, char *argv[])
{
	rt_uint32_t cmd_val;
	
	if (argc == 2)
	{
		cmd_val = atoi(argv[1]);
		rt_mb_send(beep_mbox, (rt_ubase_t)cmd_val);
	}
}
MSH_CMD_EXPORT(beep_test, test beep song);
#endif


