/* drivers/video/msm/src/drv/fb/msm_fb.c
 *
 * Core MSM framebuffer driver.
 *
 * Copyright (c) 2008 QUALCOMM USA, INC.
 * Copyright (C) 2007 Google Incorporated
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/mm.h>
#include <linux/fb.h>
#include <linux/msm_mdp.h>
#include <linux/init.h>
#include <linux/ioport.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <mach/board.h>
#include <asm/uaccess.h>

#include <linux/workqueue.h>
#include <linux/string.h>
#include <linux/version.h>
#include <linux/proc_fs.h>
#include <linux/vmalloc.h>
#include <linux/debugfs.h>
#include <linux/console.h>
#include <linux/android_pmem.h>
#include <linux/leds.h>

#define MSM_FB_C
#include "msm_fb.h"
#include "mddihosti.h"
#include "tvenc.h"
#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
#include "msm_fb_progress.h"
#endif

#ifdef CONFIG_FB_MSM_LOGO
#define INIT_IMAGE_FILE "/logo.rle"
extern int load_565rle_image(char *filename);
#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
extern int load_565rle_image_onfb(struct fb_info *info, char *filename, int start_x, int start_y);
#endif
#endif

///////////////////////////////////////////
static unsigned char *fbram;
static unsigned char *fbram_phys;
static int fbram_size;

static struct platform_device *pdev_list[MSM_FB_MAX_DEV_LIST];
static int pdev_list_cnt;

int vsync_mode = 1;

#define MAX_FBI_LIST 32
static struct fb_info *fbi_list[MAX_FBI_LIST];
static int fbi_list_index;

static struct msm_fb_data_type *mfd_list[MAX_FBI_LIST];
static int mfd_list_index;

static u32 msm_fb_pseudo_palette[16] = {
	0x00000000, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff
};

u32 msm_fb_debug_enabled;
u32 msm_fb_msg_level = 7;	// Setting msm_fb_msg_level to 8 prints out ALL messages
u32 mddi_msg_level = 5;		// Setting mddi_msg_level to 8 prints out ALL messages

extern int32 mdp_block_power_cnt[MDP_MAX_BLOCK];
extern unsigned long mdp_timer_duration;

static int msm_fb_register(struct msm_fb_data_type *mfd);
static int msm_fb_open(struct fb_info *info, int user);
static int msm_fb_release(struct fb_info *info, int user);
static int msm_fb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info);
static int msm_fb_stop_sw_refresher(struct msm_fb_data_type *mfd);
int msm_fb_resume_sw_refresher(struct msm_fb_data_type *mfd);
static int msm_fb_check_var(struct fb_var_screeninfo *var,
			    struct fb_info *info);
static int msm_fb_set_par(struct fb_info *info);
static int msm_fb_blank_sub(int blank_mode, struct fb_info *info,
			    boolean op_enable);
static int msm_fb_suspend_sub(struct msm_fb_data_type *mfd);
static int msm_fb_resume_sub(struct msm_fb_data_type *mfd);
static int msm_fb_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg);


#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
static int inactive_con=0;

void msm_fb_set_inactive_con(int val)
{
	inactive_con = val;
}

int msm_fb_get_inactive_con(void)
{
	return inactive_con;
}

EXPORT_SYMBOL(msm_fb_get_inactive_con);
#endif

#ifdef MSM_FB_ENABLE_DBGFS

#define MSM_FB_MAX_DBGFS 1024
#define MAX_BACKLIGHT_BRIGHTNESS 255

int msm_fb_debugfs_file_index;
struct dentry *msm_fb_debugfs_root;
struct dentry *msm_fb_debugfs_file[MSM_FB_MAX_DBGFS];

struct dentry *msm_fb_get_debugfs_root(void)
{
	if (msm_fb_debugfs_root == NULL) {
		msm_fb_debugfs_root = debugfs_create_dir("msm_fb", NULL);
	}

	return msm_fb_debugfs_root;
}

void msm_fb_debugfs_file_create(struct dentry *root, const char *name,
				u32 *var)
{
	if (msm_fb_debugfs_file_index >= MSM_FB_MAX_DBGFS)
		return;

	msm_fb_debugfs_file[msm_fb_debugfs_file_index++] =
	    debugfs_create_u32(name, S_IRUGO | S_IWUSR, root, var);
}
#endif

int msm_fb_cursor(struct fb_info *info, struct fb_cursor *cursor)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	if (!mfd->cursor_update)
		return -ENODEV;

	return mfd->cursor_update(info, cursor);
}

static int msm_fb_resource_initialized;

#ifndef CONFIG_FB_BACKLIGHT
static int lcd_backlight_registered;

static void msm_fb_set_bl_brightness(struct led_classdev *led_cdev,
					enum led_brightness value)
{
	struct msm_fb_data_type *mfd = dev_get_drvdata(led_cdev->dev->parent);
	int bl_lvl;

	if (value > MAX_BACKLIGHT_BRIGHTNESS)
		value = MAX_BACKLIGHT_BRIGHTNESS;

	/* This maps android backlight level 0 to 255 into
	   driver backlight level 0 to bl_max with rounding */
	bl_lvl = (2 * value * mfd->panel_info.bl_max + MAX_BACKLIGHT_BRIGHTNESS)
		/(2 * MAX_BACKLIGHT_BRIGHTNESS);

	if (!bl_lvl && value)
		bl_lvl = 1;

	msm_fb_set_backlight(mfd, bl_lvl, 1);
}

static struct led_classdev backlight_led = {
	.name		= "lcd-backlight",
	.brightness	= MAX_BACKLIGHT_BRIGHTNESS,
	.brightness_set	= msm_fb_set_bl_brightness,
};
#endif

static struct msm_fb_platform_data *msm_fb_pdata;

int msm_fb_detect_client(const char *name)
{
	int ret = -EPERM;

	if (msm_fb_pdata && msm_fb_pdata->detect_client)
		ret = msm_fb_pdata->detect_client(name);

	return ret;
}

static int msm_fb_probe(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;
	int rc;

	MSM_FB_DEBUG("msm_fb_probe\n");

	if ((pdev->id == 0) && (pdev->num_resources > 0)) {
		msm_fb_pdata = pdev->dev.platform_data;
		fbram_size =
			pdev->resource[0].end - pdev->resource[0].start + 1;
		fbram_phys = (char *)pdev->resource[0].start;
		fbram = ioremap((unsigned long)fbram_phys, fbram_size);

		if (!fbram) {
			printk(KERN_ERR "fbram ioremap failed!\n");
			return -ENOMEM;
		}

		printk(KERN_INFO
			"msm_fb_probe: resource fbram = 0x%x phys=0x%x\n",
			     (int)fbram, (int)fbram_phys);
		msm_fb_resource_initialized = 1;
		return 0;
	}

	if (!msm_fb_resource_initialized)
		return -EPERM;

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);
	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	if (pdev_list_cnt >= MSM_FB_MAX_DEV_LIST)
		return -ENOMEM;

	mfd->bl_level = mfd->panel_info.bl_max;

	rc = msm_fb_register(mfd);
	if (rc)
		return rc;

#ifdef CONFIG_FB_BACKLIGHT
	msm_fb_config_backlight(mfd);
#else
	/* android supports only one lcd-backlight/lcd for now */
	if (!lcd_backlight_registered) {
		if (led_classdev_register(&pdev->dev, &backlight_led))
			printk(KERN_ERR "led_classdev_register failed\n");
		else
			lcd_backlight_registered = 1;
	}
#endif

	pdev_list[pdev_list_cnt++] = pdev;
	return 0;
}

static int msm_fb_remove(struct platform_device *pdev)
{
	struct msm_fb_data_type *mfd;

	MSM_FB_DEBUG("msm_fb_remove\n");

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	if (!mfd)
		return -ENODEV;

	if (mfd->key != MFD_KEY)
		return -EINVAL;

	if (msm_fb_suspend_sub(mfd))
		printk(KERN_ERR "msm_fb_remove: can't stop the device %d\n", mfd->index);

	if (mfd->channel_irq != 0) {
		free_irq(mfd->channel_irq, (void *)mfd);
	}

	if (mfd->vsync_width_boundary)
		vfree(mfd->vsync_width_boundary);

	if (mfd->vsync_resync_timer.function)
		del_timer(&mfd->vsync_resync_timer);

	if (mfd->refresh_timer.function)
		del_timer(&mfd->refresh_timer);

	if (mfd->dma_hrtimer.function)
		hrtimer_cancel(&mfd->dma_hrtimer);

	// remove /dev/fb*
	unregister_framebuffer(mfd->fbi);

#ifdef CONFIG_FB_BACKLIGHT
	// remove /sys/class/backlight
	backlight_device_unregister(mfd->fbi->bl_dev);
#else
	if (lcd_backlight_registered) {
		lcd_backlight_registered = 0;
		led_classdev_unregister(&backlight_led);
	}
#endif

#ifdef MSM_FB_ENABLE_DBGFS
	if (mfd->sub_dir)
		debugfs_remove(mfd->sub_dir);
#endif

	return 0;
}

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static int msm_fb_suspend(struct platform_device *pdev, pm_message_t state)
{
	struct msm_fb_data_type *mfd;
	int ret = 0;

	MSM_FB_DEBUG("msm_fb_suspend\n");

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	if ((!mfd) || (mfd->key != MFD_KEY))
		return 0;

	acquire_console_sem();
	fb_set_suspend(mfd->fbi, 1);

	ret = msm_fb_suspend_sub(mfd);
	if (ret != 0) {
		printk(KERN_ERR "msm_fb: failed to suspend! %d\n", ret);
		fb_set_suspend(mfd->fbi, 0);
	} else {
		pdev->dev.power.power_state = state;
	}

	release_console_sem();
	return ret;
}
#else
#define msm_fb_suspend NULL
#endif

static int msm_fb_suspend_sub(struct msm_fb_data_type *mfd)
{
	int ret = 0;

	if ((!mfd) || (mfd->key != MFD_KEY))
		return 0;

	////////////////////////////////////////////////////
	// suspend this channel
	////////////////////////////////////////////////////
	mfd->suspend.sw_refreshing_enable = mfd->sw_refreshing_enable;
	mfd->suspend.op_enable = mfd->op_enable;
	mfd->suspend.panel_power_on = mfd->panel_power_on;

	if (mfd->op_enable) {
		ret =
		     msm_fb_blank_sub(FB_BLANK_POWERDOWN, mfd->fbi,
				      mfd->suspend.op_enable);
		if (ret) {
			MSM_FB_INFO
			    ("msm_fb_suspend: can't turn off display!\n");
			return ret;
		}
		mfd->op_enable = FALSE;
	}
	////////////////////////////////////////////////////
	// try to power down
	////////////////////////////////////////////////////
	mdp_pipe_ctrl(MDP_MASTER_BLOCK, MDP_BLOCK_POWER_OFF, FALSE);

	////////////////////////////////////////////////////
	// detach display channel irq if there's any
	// or wait until vsync-resync completes
	////////////////////////////////////////////////////
	if ((mfd->dest == DISPLAY_LCD)) {
		if (mfd->panel_info.lcd.vsync_enable) {
			if (mfd->panel_info.lcd.hw_vsync_mode) {
				if (mfd->channel_irq != 0)
					disable_irq(mfd->channel_irq);
			} else {
				volatile boolean vh_pending;
				do {
					vh_pending = mfd->vsync_handler_pending;
				} while (vh_pending);
			}
		}
	}

	return 0;
}

#if defined(CONFIG_PM) && !defined(CONFIG_HAS_EARLYSUSPEND)
static int msm_fb_resume(struct platform_device *pdev)
{
	/* This resume function is called when interrupt is enabled.
	 */
	int ret = 0;
	struct msm_fb_data_type *mfd;

	MSM_FB_DEBUG("msm_fb_resume\n");

	mfd = (struct msm_fb_data_type *)platform_get_drvdata(pdev);

	if ((!mfd) || (mfd->key != MFD_KEY))
		return 0;

	acquire_console_sem();
	ret = msm_fb_resume_sub(mfd);
	pdev->dev.power.power_state = PMSG_ON;
	fb_set_suspend(mfd->fbi, 1);
	release_console_sem();

	return ret;
}
#else
#define msm_fb_resume NULL
#endif

static int msm_fb_resume_sub(struct msm_fb_data_type *mfd)
{
	int ret = 0;

	if ((!mfd) || (mfd->key != MFD_KEY))
		return 0;

	/* attach display channel irq if there's any */
	if (mfd->channel_irq != 0) {
		enable_irq(mfd->channel_irq);
	}

	/* resume state var recover */
	mfd->sw_refreshing_enable = mfd->suspend.sw_refreshing_enable;
	mfd->op_enable = mfd->suspend.op_enable;

	if (mfd->suspend.panel_power_on) {
		ret =
		     msm_fb_blank_sub(FB_BLANK_UNBLANK, mfd->fbi,
				      mfd->op_enable);
		if (ret)
			MSM_FB_INFO("msm_fb_resume: can't turn on display!\n");
	}

	return ret;
}

static int msm_fb_shutdown(void)
{
#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
{
       extern void msm_fb_stop_progress();
       extern int msm_fb_get_progress_state(void);
	   
       if( msm_fb_get_progress_state() == PROGRESS_START  )
       {
	    printk(KERN_ERR "msm_fb_shutdown: Progress is runnig!\n");
		
           msm_fb_stop_progress();
           if(msm_fb_get_inactive_con())
           {
               msm_fb_set_inactive_con(0);
           }
       }
}
#endif

}

static struct platform_driver msm_fb_driver = {
	.probe = msm_fb_probe,
	.remove = msm_fb_remove,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = msm_fb_suspend,
	.suspend_late = NULL,
	.resume_early = NULL,
	.resume = msm_fb_resume,
#endif
	.shutdown = msm_fb_shutdown,
	.driver = {
		   /* Driver name must match the device name added in platform.c. */
		   .name = "msm_fb",
		   },
};

#ifdef CONFIG_HAS_EARLYSUSPEND
static void msmfb_early_suspend(struct early_suspend *h)
{
	struct msm_fb_data_type *mfd = container_of(h, struct msm_fb_data_type,
						    early_suspend);
	msm_fb_suspend_sub(mfd);
}

static void msmfb_early_resume(struct early_suspend *h)
{
	struct msm_fb_data_type *mfd = container_of(h, struct msm_fb_data_type,
						    early_suspend);
	msm_fb_resume_sub(mfd);
}
#endif

void msm_fb_set_backlight(struct msm_fb_data_type *mfd, __u32 bkl_lvl, u32 save)
{
	struct msm_fb_panel_data *pdata;

	pdata = (struct msm_fb_panel_data *)mfd->pdev->dev.platform_data;

	if ((pdata) && (pdata->set_backlight)) {
		down(&mfd->sem);
		if ((bkl_lvl != mfd->bl_level) || (!save)) {
			u32 old_lvl;

			old_lvl = mfd->bl_level;
			mfd->bl_level = bkl_lvl;
			pdata->set_backlight(mfd);

			if (!save)
				mfd->bl_level = old_lvl;
		}
		up(&mfd->sem);
	}
}

static int msm_fb_blank_sub(int blank_mode, struct fb_info *info,
			    boolean op_enable)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct msm_fb_panel_data *pdata = NULL;
	int ret = 0;
#if !defined(CONFIG_SAMSUNG_CAPELA)
	if (!op_enable)
		return -EPERM;
#endif
	pdata = (struct msm_fb_panel_data *)mfd->pdev->dev.platform_data;
	if ((!pdata) || (!pdata->on) || (!pdata->off)) {
		printk(KERN_ERR "msm_fb_blank_sub: no panel operation detected!\n");
		return -ENODEV;
	}

	switch (blank_mode) {
	case FB_BLANK_UNBLANK:
		if (!mfd->panel_power_on) {
			mdelay(100);
			ret = pdata->on(mfd->pdev);
			if (ret == 0) {
				mfd->panel_power_on = TRUE;

				msm_fb_set_backlight(mfd,
						     mfd->bl_level, 0);

/* ToDo: possible conflict with android which doesn't expect sw refresher */
/*
	  if (!mfd->hw_refresh)
	  {
	    if ((ret = msm_fb_resume_sw_refresher(mfd)) != 0)
	    {
	      MSM_FB_INFO("msm_fb_blank_sub: msm_fb_resume_sw_refresher failed = %d!\n",ret);
	    }
	  }
*/
			}
		}
		break;

	case FB_BLANK_VSYNC_SUSPEND:
	case FB_BLANK_HSYNC_SUSPEND:
	case FB_BLANK_NORMAL:
	case FB_BLANK_POWERDOWN:
	default:
		if (mfd->panel_power_on) {
			int curr_pwr_state;

			mfd->op_enable = FALSE;
			curr_pwr_state = mfd->panel_power_on;
			mfd->panel_power_on = FALSE;

			mdelay(100);
			ret = pdata->off(mfd->pdev);
			if (ret)
				mfd->panel_power_on = curr_pwr_state;

			msm_fb_set_backlight(mfd, 0, 0);
			mfd->op_enable = TRUE;
		}
		break;
	}

	return ret;
}

static void msm_fb_fillrect(struct fb_info *info,
			    const struct fb_fillrect *rect)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	cfb_fillrect(info, rect);
	if (!mfd->hw_refresh && (info->var.yoffset == 0) &&
		!mfd->sw_currently_refreshing) {
		struct fb_var_screeninfo var;

		var = info->var;
		var.reserved[0] = 0x54445055;
		var.reserved[1] = (rect->dy << 16) | (rect->dx);
		var.reserved[2] = ((rect->dy + rect->height) << 16) |
		    (rect->dx + rect->width);

		msm_fb_pan_display(&var, info);
	}
}

static void msm_fb_copyarea(struct fb_info *info,
			    const struct fb_copyarea *area)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	cfb_copyarea(info, area);
	if (!mfd->hw_refresh && (info->var.yoffset == 0) &&
		!mfd->sw_currently_refreshing) {
		struct fb_var_screeninfo var;

		var = info->var;
		var.reserved[0] = 0x54445055;
		var.reserved[1] = (area->dy << 16) | (area->dx);
		var.reserved[2] = ((area->dy + area->height) << 16) |
		    (area->dx + area->width);

		msm_fb_pan_display(&var, info);
	}
}

// for LCD
extern int lcd_suspend;

static void msm_fb_imageblit(struct fb_info *info, const struct fb_image *image)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	cfb_imageblit(info, image);

	// for LCD 
//	if (!mfd->hw_refresh && (info->var.yoffset == 0) &&			//original
//		!mfd->sw_currently_refreshing) {
	if (!mfd->hw_refresh && (info->var.yoffset == 0) &&
		!mfd->sw_currently_refreshing && !lcd_suspend) {
		struct fb_var_screeninfo var;

		var = info->var;
		var.reserved[0] = 0x54445055;
		var.reserved[1] = (image->dy << 16) | (image->dx);
		var.reserved[2] = ((image->dy + image->height) << 16) |
		    (image->dx + image->width);

		msm_fb_pan_display(&var, info);
	}
}

static int msm_fb_blank(int blank_mode, struct fb_info *info)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	return msm_fb_blank_sub(blank_mode, info, mfd->op_enable);
}

static struct fb_ops msm_fb_ops = {
	.owner = THIS_MODULE,
	.fb_open = msm_fb_open,
	.fb_release = msm_fb_release,
	.fb_read = NULL,
	.fb_write = NULL,
	.fb_cursor = NULL,
	.fb_check_var = msm_fb_check_var,	/* vinfo check */
	.fb_set_par = msm_fb_set_par,	/* set the video mode according to info->var */
	.fb_setcolreg = NULL,	/* set color register */
	.fb_blank = NULL,	/* blank display */
	.fb_pan_display = msm_fb_pan_display,	/* pan display */
	.fb_fillrect = msm_fb_fillrect,	/* Draws a rectangle */
	.fb_copyarea = msm_fb_copyarea,	/* Copy data from area to another */
	.fb_imageblit = msm_fb_imageblit,	/* Draws a image to the display */
	.fb_rotate = NULL,
	.fb_sync = NULL,	/* wait for blit idle, optional */
	.fb_ioctl = msm_fb_ioctl,	/* perform fb specific ioctl (optional) */
	.fb_mmap = NULL,
};

static int msm_fb_register(struct msm_fb_data_type *mfd)
{
	int ret = -ENODEV;
	int bpp;
	struct msm_panel_info *panel_info = &mfd->panel_info;
	struct fb_info *fbi = mfd->fbi;
	struct fb_fix_screeninfo *fix;
	struct fb_var_screeninfo *var;
	int *id;
	int fbram_offset;

	///////////////////////////////////////////////////////
	// fb info initialization
	///////////////////////////////////////////////////////
	fix = &fbi->fix;
	var = &fbi->var;

	fix->type_aux = 0;	// if type == FB_TYPE_INTERLEAVED_PLANES
	fix->visual = FB_VISUAL_TRUECOLOR;	// True Color
	fix->ywrapstep = 0;	// No support
	fix->mmio_start = 0;	// No MMIO Address
	fix->mmio_len = 0;	// No MMIO Address
	fix->accel = FB_ACCEL_NONE;	// FB_ACCEL_MSM needes to be added in fb.h

	var->xoffset = 0,	// Offset from virtual to visible
	    var->yoffset = 0,	// resolution
	    var->grayscale = 0,	// No graylevels
	    var->nonstd = 0,	// standard pixel format
	    var->activate = FB_ACTIVATE_VBL,	// activate it at vsync
	    var->height = -1,	// height of picture in mm
	    var->width = -1,	// width of picture in mm
	    var->accel_flags = 0,	// acceleration flags
	    var->sync = 0,	// see FB_SYNC_*
	    var->rotate = 0,	// angle we rotate counter clockwise
	    mfd->op_enable = FALSE;

	switch (mfd->fb_imgType) {
	case MDP_RGB_565:
		fix->type = FB_TYPE_PACKED_PIXELS;
		fix->xpanstep = 1;
		fix->ypanstep = 1;
		var->vmode = FB_VMODE_NONINTERLACED;
		var->blue.offset = 0;
		var->green.offset = 5;
		var->red.offset = 11;
		var->blue.length = 5;
		var->green.length = 6;
		var->red.length = 5;
		var->blue.msb_right = 0;
		var->green.msb_right = 0;
		var->red.msb_right = 0;
		var->transp.offset = 0;
		var->transp.length = 0;
		bpp = 2;
		break;

	case MDP_RGB_888:
		fix->type = FB_TYPE_PACKED_PIXELS;
		fix->xpanstep = 1;
		fix->ypanstep = 1;
		var->vmode = FB_VMODE_NONINTERLACED;
		var->blue.offset = 0;
		var->green.offset = 8;
		var->red.offset = 16;
		var->blue.length = 8;
		var->green.length = 8;
		var->red.length = 8;
		var->blue.msb_right = 0;
		var->green.msb_right = 0;
		var->red.msb_right = 0;
		var->transp.offset = 0;
		var->transp.length = 0;
		bpp = 3;
		break;

	case MDP_ARGB_8888:
		fix->type = FB_TYPE_PACKED_PIXELS;
		fix->xpanstep = 1;
		fix->ypanstep = 1;
		var->vmode = FB_VMODE_NONINTERLACED;
		var->blue.offset = 0;
		var->green.offset = 8;
		var->red.offset = 16;
		var->blue.length = 8;
		var->green.length = 8;
		var->red.length = 8;
		var->blue.msb_right = 0;
		var->green.msb_right = 0;
		var->red.msb_right = 0;
		var->transp.offset = 24;
		var->transp.length = 8;
		bpp = 3;
		break;

	case MDP_YCRYCB_H2V1:
		/* ToDo: need to check TV-Out YUV422i framebuffer format */
		/*       we might need to create new type define */
		fix->type = FB_TYPE_INTERLEAVED_PLANES;
		fix->xpanstep = 2;
		fix->ypanstep = 1;
		var->vmode = FB_VMODE_NONINTERLACED;

		/* how about R/G/B offset? */
		var->blue.offset = 0;
		var->green.offset = 5;
		var->red.offset = 11;
		var->blue.length = 5;
		var->green.length = 6;
		var->red.length = 5;
		var->blue.msb_right = 0;
		var->green.msb_right = 0;
		var->red.msb_right = 0;
		var->transp.offset = 0;
		var->transp.length = 0;
		bpp = 2;
		break;

	default:
		MSM_FB_ERR("msm_fb_init: fb %d unkown image type!\n",
			   mfd->index);
		return ret;
	}

	fix->smem_len =
	    panel_info->xres * panel_info->yres * bpp * mfd->fb_page;
	fix->line_length = panel_info->xres * bpp;

	mfd->var_xres = panel_info->xres;
	mfd->var_yres = panel_info->yres;

	var->pixclock = mfd->panel_info.clk_rate;
	mfd->var_pixclock = var->pixclock;

	var->xres = panel_info->xres;
	var->yres = panel_info->yres;
	var->xres_virtual = panel_info->xres;
	var->yres_virtual = panel_info->yres * mfd->fb_page;
	var->bits_per_pixel = bpp * 8,	// FrameBuffer color depth
	    //////////////////////////////////////////
	    // id field for fb app
	    //////////////////////////////////////////
	    id = (int *)&mfd->panel;

#if defined(CONFIG_FB_MSM_MDP22)
	snprintf(fix->id, sizeof(fix->id), "msmfb22_%x", (__u32) *id);
#elif defined(CONFIG_FB_MSM_MDP30)
	snprintf(fix->id, sizeof(fix->id), "msmfb30_%x", (__u32) *id);
#elif defined(CONFIG_FB_MSM_MDP31)
	snprintf(fix->id, sizeof(fix->id), "msmfb31_%x", (__u32) *id);
#else
	error CONFIG_FB_MSM_MDP undefined !
#endif
	 fbi->fbops = &msm_fb_ops;
	fbi->flags = FBINFO_FLAG_DEFAULT;
	fbi->pseudo_palette = msm_fb_pseudo_palette;

	mfd->ref_cnt = 0;
	mfd->sw_currently_refreshing = FALSE;
	mfd->sw_refreshing_enable = TRUE;
	mfd->panel_power_on = FALSE;

	mfd->pan_waiting = FALSE;
	init_completion(&mfd->pan_comp);
	init_completion(&mfd->refresher_comp);
	init_MUTEX(&mfd->sem);

	fbram_offset = PAGE_ALIGN((int)fbram)-(int)fbram;
	fbram += fbram_offset;
	fbram_phys += fbram_offset;
	fbram_size -= fbram_offset;

	if (fbram_size < fix->smem_len) {
		printk(KERN_ERR "error: no more framebuffer memory!\n");
		return -ENOMEM;
	}

	fbi->screen_base = fbram;
	fbi->fix.smem_start = (unsigned long)fbram_phys;

	memset(fbi->screen_base, 0x0, fix->smem_len);
#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
        msm_fb_set_inactive_con(1);
	if (!load_565rle_image_onfb(fbi, "GALAXY.rle",0,163)) ;	/* Flip buffer */
#endif
	mfd->op_enable = TRUE;
	mfd->panel_power_on = FALSE;

	/* cursor memory allocation */
	if (mfd->cursor_update) {
		mfd->cursor_buf = dma_alloc_coherent(NULL,
					MDP_CURSOR_SIZE,
					(dma_addr_t *) &mfd->cursor_buf_phys,
					GFP_KERNEL);
		if (!mfd->cursor_buf)
			mfd->cursor_update = 0;
	}

	if (register_framebuffer(fbi) < 0) {
		if (mfd->cursor_buf)
			dma_free_coherent(NULL,
				MDP_CURSOR_SIZE,
				mfd->cursor_buf,
				(dma_addr_t) mfd->cursor_buf_phys);

		mfd->op_enable = FALSE;
		return -EPERM;
	}

	fbram += fix->smem_len;
	fbram_phys += fix->smem_len;
	fbram_size -= fix->smem_len;

	MSM_FB_INFO
	    ("FrameBuffer[%d] %dx%d size=%d bytes is registered successfully!\n",
	     mfd->index, fbi->var.xres, fbi->var.yres, fbi->fix.smem_len);

#ifdef CONFIG_FB_MSM_LOGO
	if (!load_565rle_image(INIT_IMAGE_FILE)) ;	/* Flip buffer */
#endif
	ret = 0;

#ifdef CONFIG_HAS_EARLYSUSPEND
	mfd->early_suspend.suspend = msmfb_early_suspend;
	mfd->early_suspend.resume = msmfb_early_resume;
	mfd->early_suspend.level = EARLY_SUSPEND_LEVEL_DISABLE_FB - 2;
	register_early_suspend(&mfd->early_suspend);
#endif
#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
{
extern void msm_fb_start_progress();
msm_fb_start_progress();
}
#endif

#ifdef MSM_FB_ENABLE_DBGFS
	{
		struct dentry *root;
		struct dentry *sub_dir;
		char sub_name[2];

		root = msm_fb_get_debugfs_root();
		if (root != NULL) {
			sub_name[0] = (char)(mfd->index + 0x30);
			sub_name[1] = '\0';
			sub_dir = debugfs_create_dir(sub_name, root);
		} else {
			sub_dir = NULL;
		}

		mfd->sub_dir = sub_dir;

		if (sub_dir) {
			msm_fb_debugfs_file_create(sub_dir, "op_enable",
						   (u32 *) &mfd->op_enable);
			msm_fb_debugfs_file_create(sub_dir, "panel_power_on",
						   (u32 *) &mfd->
						   panel_power_on);
			msm_fb_debugfs_file_create(sub_dir, "ref_cnt",
						   (u32 *) &mfd->ref_cnt);
			msm_fb_debugfs_file_create(sub_dir, "fb_imgType",
						   (u32 *) &mfd->fb_imgType);
			msm_fb_debugfs_file_create(sub_dir,
						   "sw_currently_refreshing",
						   (u32 *) &mfd->
						   sw_currently_refreshing);
			msm_fb_debugfs_file_create(sub_dir,
						   "sw_refreshing_enable",
						   (u32 *) &mfd->
						   sw_refreshing_enable);

			msm_fb_debugfs_file_create(sub_dir, "xres",
						   (u32 *) &mfd->panel_info.
						   xres);
			msm_fb_debugfs_file_create(sub_dir, "yres",
						   (u32 *) &mfd->panel_info.
						   yres);
			msm_fb_debugfs_file_create(sub_dir, "bpp",
						   (u32 *) &mfd->panel_info.
						   bpp);
			msm_fb_debugfs_file_create(sub_dir, "type",
						   (u32 *) &mfd->panel_info.
						   type);
			msm_fb_debugfs_file_create(sub_dir, "wait_cycle",
						   (u32 *) &mfd->panel_info.
						   wait_cycle);
			msm_fb_debugfs_file_create(sub_dir, "pdest",
						   (u32 *) &mfd->panel_info.
						   pdest);
			msm_fb_debugfs_file_create(sub_dir, "backbuff",
						   (u32 *) &mfd->panel_info.
						   fb_num);
			msm_fb_debugfs_file_create(sub_dir, "clk_rate",
						   (u32 *) &mfd->panel_info.
						   clk_rate);

			switch (mfd->dest) {
			case DISPLAY_LCD:
				msm_fb_debugfs_file_create(sub_dir,
							   "vsync_enable",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   vsync_enable);
				msm_fb_debugfs_file_create(sub_dir, "refx100",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   refx100);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_back_porch",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   v_back_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_front_porch",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   v_front_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_pulse_width",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   v_pulse_width);
				msm_fb_debugfs_file_create(sub_dir,
							   "hw_vsync_mode",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   hw_vsync_mode);
				msm_fb_debugfs_file_create(sub_dir,
							   "vsync_notifier_period",
							   (u32 *) &mfd->
							   panel_info.lcd.
							   vsync_notifier_period);
				break;

			case DISPLAY_LCDC:
				msm_fb_debugfs_file_create(sub_dir,
							   "h_back_porch",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   h_back_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "h_front_porch",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   h_front_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "h_pulse_width",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   h_pulse_width);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_back_porch",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   v_back_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_front_porch",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   v_front_porch);
				msm_fb_debugfs_file_create(sub_dir,
							   "v_pulse_width",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   v_pulse_width);
				msm_fb_debugfs_file_create(sub_dir,
							   "border_clr",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   border_clr);
				msm_fb_debugfs_file_create(sub_dir,
							   "underflow_clr",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   underflow_clr);
				msm_fb_debugfs_file_create(sub_dir,
							   "hsync_skew",
							   (u32 *) &mfd->
							   panel_info.lcdc.
							   hsync_skew);
				break;

			default:
				break;
			}
		}
	}
#endif //MSM_FB_ENABLE_DBGFS

	return ret;
}

static int msm_fb_open(struct fb_info *info, int user)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
 
	if (!mfd->ref_cnt) {
		mdp_set_dma_pan_info(info, NULL, TRUE);

		if (msm_fb_blank_sub(FB_BLANK_UNBLANK, info, mfd->op_enable)) {
			printk(KERN_ERR "msm_fb_open: can't turn on display!\n");
			return -1;
		}
	}

#ifdef CONFIG_FB_MSM_SEC_BOOTLOGO
{
       extern void msm_fb_stop_progress();
       extern int msm_fb_get_progress_state(void);
       
       if( msm_fb_get_progress_state() == PROGRESS_START  )
       {
           msm_fb_stop_progress();
           if(msm_fb_get_inactive_con())
           {
               msm_fb_set_inactive_con(0);
           }
       }
}
	mfd->sw_currently_refreshing = 0;   //SR 123450 - OpenGL 3D
#endif
	mfd->ref_cnt++;
	return 0;
}

static int msm_fb_release(struct fb_info *info, int user)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	int ret = 0;

	if (!mfd->ref_cnt) {
		MSM_FB_INFO("msm_fb_release: try to close unopened fb %d!\n",
			    mfd->index);
		return -EINVAL;
	}

	mfd->ref_cnt--;

	if (!mfd->ref_cnt) {
		if ((ret =
		     msm_fb_blank_sub(FB_BLANK_POWERDOWN, info,
				      mfd->op_enable)) != 0) {
			printk(KERN_ERR "msm_fb_release: can't turn off display!\n");
			return ret;
		}
	}

	return ret;
}

DECLARE_MUTEX(msm_fb_pan_sem);

static int msm_fb_pan_display(struct fb_var_screeninfo *var,
			      struct fb_info *info)
{
	struct mdp_dirty_region dirty;
	struct mdp_dirty_region *dirtyPtr = NULL;
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
#if !defined(CONFIG_SAMSUNG_CAPELA)
	if ((!mfd->op_enable) || (!mfd->panel_power_on))
		return -EPERM;
#endif
	if (var->xoffset > (info->var.xres_virtual - info->var.xres))
		return -EINVAL;

	if (var->yoffset > (info->var.yres_virtual - info->var.yres))
		return -EINVAL;

	if (info->fix.xpanstep)
		info->var.xoffset =
		    (var->xoffset / info->fix.xpanstep) * info->fix.xpanstep;

	if (info->fix.ypanstep)
		info->var.yoffset =
		    (var->yoffset / info->fix.ypanstep) * info->fix.ypanstep;

	/* "UPDT" */
	if (var->reserved[0] == 0x54445055) {
		dirty.xoffset = var->reserved[1] & 0xffff;
		dirty.yoffset = (var->reserved[1] >> 16) & 0xffff;

		if ((var->reserved[2] & 0xffff) <= dirty.xoffset)
			return -EINVAL;
		if (((var->reserved[2] >> 16) & 0xffff) <= dirty.yoffset)
			return -EINVAL;

		dirty.width = (var->reserved[2] & 0xffff) - dirty.xoffset;
		dirty.height =
		    ((var->reserved[2] >> 16) & 0xffff) - dirty.yoffset;
		info->var.yoffset = var->yoffset;

		if (dirty.xoffset < 0)
			return -EINVAL;

		if (dirty.yoffset < 0)
			return -EINVAL;

		if ((dirty.xoffset + dirty.width) > info->var.xres)
			return -EINVAL;

		if ((dirty.yoffset + dirty.height) > info->var.yres)
			return -EINVAL;

		if ((dirty.width <= 0) || (dirty.height <= 0))
			return -EINVAL;

		dirtyPtr = &dirty;
	}

	down(&msm_fb_pan_sem);
	mdp_set_dma_pan_info(info, dirtyPtr,
			     (var->activate == FB_ACTIVATE_VBL));
	mdp_dma_pan_update(info);
	up(&msm_fb_pan_sem);

	return 0;
}

static int msm_fb_check_var(struct fb_var_screeninfo *var, struct fb_info *info)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;

	if (var->rotate != FB_ROTATE_UR)
		return -EINVAL;
	if (var->grayscale != info->var.grayscale)
		return -EINVAL;

	switch (var->bits_per_pixel) {
	case 16:
		if ((var->green.offset != 5) ||
			!((var->blue.offset == 11)
				|| (var->blue.offset == 0)) ||
			!((var->red.offset == 11)
				|| (var->red.offset == 0)) ||
			(var->blue.length != 5) ||
			(var->green.length != 6) ||
			(var->red.length != 5) ||
			(var->blue.msb_right != 0) ||
			(var->green.msb_right != 0) ||
			(var->red.msb_right != 0) ||
			(var->transp.offset != 0) ||
			(var->transp.length != 0))
				return -EINVAL;
		break;

	case 24:
		if ((var->blue.offset != 0) ||
			(var->green.offset != 8) ||
			(var->red.offset != 16) ||
			(var->blue.length != 8) ||
			(var->green.length != 8) ||
			(var->red.length != 8) ||
			(var->blue.msb_right != 0) ||
			(var->green.msb_right != 0) ||
			(var->red.msb_right != 0) ||
			!(((var->transp.offset == 0) &&
				(var->transp.length == 0)) ||
			  ((var->transp.offset == 24) &&
				(var->transp.length == 8))))
				return -EINVAL;
		break;

	default:
		return -EINVAL;
	}

	if ((var->xres_virtual <= 0) || (var->yres_virtual <= 0))
		return -EINVAL;

	if (info->fix.smem_len <
		(var->xres_virtual*var->yres_virtual*(var->bits_per_pixel/8)))
		return -EINVAL;

	if ((var->xres == 0) || (var->yres == 0))
		return -EINVAL;

	if ((var->xres > mfd->panel_info.xres) ||
		(var->yres > mfd->panel_info.yres))
		return -EINVAL;

	if (var->xoffset > (var->xres_virtual - var->xres))
		return -EINVAL;

	if (var->yoffset > (var->yres_virtual - var->yres))
		return -EINVAL;

	return 0;
}

static int msm_fb_set_par(struct fb_info *info)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	struct fb_var_screeninfo *var = &info->var;
	int old_imgType;
	int blank = 0;

	old_imgType = mfd->fb_imgType;
	switch (var->bits_per_pixel) {
	case 16:
		if (var->red.offset == 0)
			mfd->fb_imgType = MDP_BGR_565;
		else
			mfd->fb_imgType = MDP_RGB_565;
		break;

	case 24:
		if ((var->transp.offset == 0) && (var->transp.length == 0))
			mfd->fb_imgType = MDP_RGB_888;
		else if ((var->transp.offset == 24) &&
				(var->transp.length == 8)) {
			mfd->fb_imgType = MDP_ARGB_8888;
			info->var.bits_per_pixel = 32;
		}
		break;

	default:
		return -EINVAL;
	}

	if ((mfd->var_pixclock != var->pixclock) ||
		(mfd->hw_refresh && ((mfd->fb_imgType != old_imgType) ||
				(mfd->var_pixclock != var->pixclock) ||
				(mfd->var_xres != var->xres) ||
				(mfd->var_yres != var->yres)))) {
		mfd->var_xres = var->xres;
		mfd->var_yres = var->yres;
		mfd->var_pixclock = var->pixclock;
		blank = 1;
	}

	if (blank) {
		msm_fb_blank_sub(FB_BLANK_POWERDOWN, info, mfd->op_enable);
		msm_fb_blank_sub(FB_BLANK_UNBLANK, info, mfd->op_enable);
	}

	return 0;
}

static int msm_fb_stop_sw_refresher(struct msm_fb_data_type *mfd)
{
	if (mfd->hw_refresh)
		return -EPERM;

	if (mfd->sw_currently_refreshing) {
		down(&mfd->sem);
		mfd->sw_currently_refreshing = FALSE;
		up(&mfd->sem);

		// wait until the refresher finishes the last job
		wait_for_completion_interruptible(&mfd->refresher_comp);
	}

	return 0;
}

int msm_fb_resume_sw_refresher(struct msm_fb_data_type *mfd)
{
	boolean do_refresh;

	if (mfd->hw_refresh)
		return -EPERM;

	down(&mfd->sem);
	if ((!mfd->sw_currently_refreshing) && (mfd->sw_refreshing_enable)) {
		do_refresh = TRUE;
		mfd->sw_currently_refreshing = TRUE;
	} else {
		do_refresh = FALSE;
	}
	up(&mfd->sem);

	if (do_refresh)
		mdp_refresh_screen((unsigned long)mfd);

	return 0;
}

void mdp_ppp_put_img(struct mdp_blit_req *req, struct file *p_src_file,
		struct file *p_dst_file)
{
#ifdef CONFIG_ANDROID_PMEM
	if (p_src_file)
		put_pmem_file(p_src_file);
	if (p_dst_file)
		put_pmem_file(p_dst_file);
#endif
}

int mdp_blit(struct fb_info *info, struct mdp_blit_req *req)
{
	int ret;
	struct file *p_src_file = 0, *p_dst_file = 0;
	
	if (unlikely(req->src_rect.h == 0 || req->src_rect.w == 0)) {
		printk(KERN_ERR "mpd_ppp: src img of zero size!\n");
		return -EINVAL;
	}
	if (unlikely(req->dst_rect.h == 0 || req->dst_rect.w == 0))
		return 0;

	ret = mdp_ppp_blit(info, req, &p_src_file, &p_dst_file);
	mdp_ppp_put_img(req, p_src_file, p_dst_file);
	return ret;
}

static int msmfb_blit(struct fb_info *info, void __user *p)
{
	struct mdp_blit_req req;
	struct mdp_blit_req_list req_list;
	int i;
	int ret;

	if (copy_from_user(&req_list, p, sizeof(req_list)))
		return -EFAULT;

	for (i = 0; i < req_list.count; i++) {
		struct mdp_blit_req_list *list = (struct mdp_blit_req_list *)p;
		if (copy_from_user(&req, &list->req[i], sizeof(req)))
			return -EFAULT;
		ret = mdp_blit(info, &req);
		if (ret)
			return ret;
	}
	return 0;
}

DECLARE_MUTEX(msm_fb_ioctl_ppp_sem);

static int msm_fb_ioctl(struct fb_info *info, unsigned int cmd,
			unsigned long arg)
{
	struct msm_fb_data_type *mfd = (struct msm_fb_data_type *)info->par;
	void __user *argp = (void __user *)arg;
	struct fb_cursor cursor;

	int ret = 0;

#if !defined(CONFIG_SAMSUNG_CAPELA)
	if (!mfd->op_enable)
		return -EPERM;
#endif
	switch (cmd) {
	case MSMFB_BLIT:
		down(&msm_fb_ioctl_ppp_sem);
		ret = msmfb_blit(info, argp);
		up(&msm_fb_ioctl_ppp_sem);

		break;

	case MSMFB_GRP_DISP:
#ifdef CONFIG_FB_MSM_MDP22
		{
			unsigned long grp_id;

			ret = copy_from_user(&grp_id, argp, sizeof(grp_id));
			if (ret)
				return ret;

			mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_ON, FALSE);
			writel(grp_id, MDP_FULL_BYPASS_WORD43);
			mdp_pipe_ctrl(MDP_CMD_BLOCK, MDP_BLOCK_POWER_OFF,
				      FALSE);
			break;
		}
#else
		return -EFAULT;
#endif
	case MSMFB_SUSPEND_SW_REFRESHER:
		if (!mfd->panel_power_on)
			return -EPERM;

		mfd->sw_refreshing_enable = FALSE;
		ret = msm_fb_stop_sw_refresher(mfd);
		break;

	case MSMFB_RESUME_SW_REFRESHER:
		if (!mfd->panel_power_on)
			return -EPERM;

		mfd->sw_refreshing_enable = TRUE;
		ret = msm_fb_resume_sw_refresher(mfd);
		break;

	case MSMFB_CURSOR:
		ret = copy_from_user(&cursor, argp, sizeof(cursor));
		if (ret)
			return ret;

		ret = msm_fb_cursor(info, &cursor);
		break;

	default:
		MSM_FB_INFO("MDP: unknown ioctl received!\n");
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int msm_fb_register_driver(void)
{
	return platform_driver_register(&msm_fb_driver);
}

void msm_fb_add_device(struct platform_device *pdev)
{
	struct msm_fb_panel_data * pdata;
	struct platform_device *this_dev = NULL;
	struct fb_info *fbi;
	struct msm_fb_data_type *mfd = NULL;
	u32 type, id, fb_num;

	if (!pdev)
		return;
	id = pdev->id;

	pdata = pdev->dev.platform_data;
	if (!pdata)
		return;
	type = pdata->panel_info.type;
	fb_num = pdata->panel_info.fb_num;

	if (fb_num <= 0)
		return;

	if (fbi_list_index >= MAX_FBI_LIST) {
		printk(KERN_ERR "msm_fb: no more framebuffer info list!\n");
		return;
	}
	/////////////////////////////////////////
	// alloc panel device data
	/////////////////////////////////////////
	this_dev = msm_fb_device_alloc(pdata, type, id);

	if (!this_dev) {
		printk(KERN_ERR
		"%s: msm_fb_device_alloc failed!\n",__func__);
		return;
	}

	/////////////////////////////////////////
	// alloc framebuffer info + par data
	/////////////////////////////////////////
	fbi = framebuffer_alloc(sizeof(struct msm_fb_data_type),NULL);
	if (fbi == NULL) {
		platform_device_put(this_dev);
		printk(KERN_ERR "msm_fb: can't alloca framebuffer info data!\n");
		return;
	}

	mfd = (struct msm_fb_data_type *)fbi->par;
	mfd->key = MFD_KEY;
	mfd->fbi = fbi;
	mfd->panel.type = type;
	mfd->panel.id = id;
	mfd->fb_page = fb_num;
	mfd->index = fbi_list_index;

	// link to the latest pdev
	mfd->pdev = this_dev;

	mfd_list[mfd_list_index++] = mfd;
	fbi_list[fbi_list_index++] = fbi;

	/////////////////////////////////////////
	// set driver data
	/////////////////////////////////////////
	platform_set_drvdata(this_dev, mfd);

	if (platform_device_add(this_dev)) {
		printk(KERN_ERR "msm_fb: platform_device_add failed!\n");
		platform_device_put(this_dev);
		framebuffer_release(fbi);
		fbi_list_index--;
		return;
	}
}

EXPORT_SYMBOL(msm_fb_add_device);

int __init msm_fb_init(void)
{
	int rc = -ENODEV;

	if (msm_fb_register_driver())
		return rc;

#ifdef MSM_FB_ENABLE_DBGFS
	{
		struct dentry *root;

		if ((root = msm_fb_get_debugfs_root()) != NULL) {
			msm_fb_debugfs_file_create(root,
						   "msm_fb_msg_printing_level",
						   (u32 *) & msm_fb_msg_level);
			msm_fb_debugfs_file_create(root,
						   "mddi_msg_printing_level",
						   (u32 *) & mddi_msg_level);
			msm_fb_debugfs_file_create(root, "msm_fb_debug_enabled",
						   (u32 *) &
						   msm_fb_debug_enabled);
		}
	}
#endif

	return 0;
}

module_init(msm_fb_init);
