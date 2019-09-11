/*
 * Copyright (c) 2006-2018, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-06     SummerGift   first version
 */

#include <rtthread.h>
#include <rtdevice.h>
#include <board.h>

#if defined(RT_USING_POSIX)
#include <dfs_posix.h>
#include <dfs_poll.h>
#include <libc.h>
static int dev_old_flag;
#endif

#include <fal.h>
#define FS_PARTITION_NAME   			"elmfs"

#include <easyflash.h>

#define LOG_TAG                        	"app.main"
#include <app_log.h>

int main(void)
{		
    return RT_EOK;
}

int fs_init(void)
{
	/* partition initialized */
	fal_init();
    /* easyflash initialized */
    easyflash_init();

	/* Create a block device on the file system partition of spi flash */
    struct rt_device *flash_dev = fal_blk_device_create(FS_PARTITION_NAME);
    if (flash_dev == RT_NULL)
	{
        LOG_D("Can't create a block device on '%s' partition.", FS_PARTITION_NAME);
	}
    else
	{
        LOG_D("Create a block device on the %s partition of flash successful.", FS_PARTITION_NAME);
	}
	
    /* mount the file system from "filesystem" partition of spi flash. */
    if (dfs_mount(flash_dev->parent.name, "/", "elm", 0, 0) == 0)
	{
        LOG_D("Filesystem initialized!");
	}
    else
    {
        LOG_D("Failed to initialize filesystem!\n");
        LOG_D("You should create a filesystem on the block device first!");
		LOG_D("msh> mkfs -t elm %s", flash_dev->parent.name);
    }    
    
    return 0;
}
INIT_ENV_EXPORT(fs_init);

int vcom_init(void)
{
    /* set console */
    rt_console_set_device("vcom");
    
#if defined(RT_USING_POSIX)    
    /* backup flag */
    dev_old_flag = ioctl(libc_stdio_get_console(), F_GETFL, (void *) RT_NULL);
    /* add non-block flag */
    ioctl(libc_stdio_get_console(), F_SETFL, (void *) (dev_old_flag | O_NONBLOCK));
    /* set tcp shell device for console */
    libc_stdio_set_console("vcom", O_RDWR);
   
    /* resume finsh thread, make sure it will unblock from last device receive */
    rt_thread_t tid = rt_thread_find(FINSH_THREAD_NAME);
    if (tid)
    {
        rt_thread_resume(tid);
        rt_schedule();
    }
#else
    /* set finsh device */
    finsh_set_device("vcom");
#endif /* RT_USING_POSIX */
    
    return 0;
}
INIT_ENV_EXPORT(vcom_init);



