/*
 *
 * Copyright (c) 2009 QUALCOMM USA, INC.
 * 
 * All source code in this file is licensed under the following license
 * 
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/miscdevice.h>
#include <linux/platform_device.h>
#include <linux/uaccess.h>
#include <linux/msm_audio.h>

#include <asm/ioctls.h>
#include <mach/qdsp6/msm8k_cad.h>
#include <mach/qdsp6/msm8k_cad_ioctl.h>
#include <mach/qdsp6/msm8k_ard.h>
#include <mach/qdsp6/msm8k_cad_write_amr_format.h>
#include <mach/qdsp6/msm8k_cad_devices.h>

#if 0
#define D(fmt, args...) printk(KERN_INFO "msm8k_amr_in: " fmt, ##args)
#else
#define D(fmt, args...) do {} while (0)
#endif

#define MSM8K_AMR_IN_PROC_NAME "msm8k_amr_in"

#define AUDIO_MAGIC 'a'

struct amr {
	u32 cad_w_handle;
	struct msm_audio_config cfg;
};

struct amr g_amr_in;

static int msm8k_amr_in_open(struct inode *inode, struct file *f)
{
	struct amr *amr = &g_amr_in;
	struct cad_open_struct_type  cos;
	D("%s\n", __func__);

	cos.format = CAD_FORMAT_AMRNB;

	f->private_data = amr;

	cos.op_code = CAD_OPEN_OP_READ;
	amr->cad_w_handle = cad_open(&cos);

	if (amr->cad_w_handle == 0)
		return CAD_RES_FAILURE;
	else
		return CAD_RES_SUCCESS;
}

static int msm8k_amr_in_release(struct inode *inode, struct file *f)
{
	int rc = CAD_RES_SUCCESS;
	struct amr *amr = f->private_data;
	D("%s\n", __func__);

	cad_close(amr->cad_w_handle);

	return rc;
}

static ssize_t msm8k_amr_in_read(struct file *f, char __user *buf, size_t cnt,
		loff_t *pos)
{
	struct amr			*amr = f->private_data;
	struct cad_buf_struct_type	cbs;

	D("%s\n", __func__);

	memset(&cbs, 0, sizeof(struct cad_buf_struct_type));
	cbs.buffer = (void *)buf;
	cbs.max_size = cnt;
	cbs.actual_size = cnt;


	cnt = cad_read(amr->cad_w_handle, &cbs);
	return cnt;
}

static ssize_t msm8k_amr_in_write(struct file *f, const char __user *buf,
		size_t cnt, loff_t *pos)
{
	D("%s\n", __func__);

	return cnt;
}

static int msm8k_amr_in_ioctl(struct inode *inode, struct file *f,
		unsigned int cmd, unsigned long arg)
{
	int rc = CAD_RES_SUCCESS;
	struct amr *p = f->private_data;
	void *cad_arg = (void *)arg;
	u32 stream_device[1];
	struct cad_device_struct_type cad_dev;
	struct cad_stream_device_struct_type cad_stream_dev;
	struct cad_stream_info_struct_type cad_stream_info;
	struct cad_write_amr_format_struct_type cad_write_amr_fmt;
	D("%s\n", __func__);

	memset(&cad_dev, 0, sizeof(struct cad_device_struct_type));
	memset(&cad_stream_dev, 0,
			sizeof(struct cad_stream_device_struct_type));
	memset(&cad_stream_info, 0, sizeof(struct cad_stream_info_struct_type));
	memset(&cad_write_amr_fmt, 0,
			sizeof(struct cad_write_amr_format_struct_type));

	switch (cmd) {
	case AUDIO_START:

		cad_stream_info.app_type = CAD_STREAM_APP_RECORD;
		cad_stream_info.priority = 0;
		cad_stream_info.buf_mem_type = CAD_STREAM_BUF_MEM_HEAP;
		cad_stream_info.ses_buf_max_size = 1024 * 10;
		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_SET_STREAM_INFO,
			&cad_stream_info,
			sizeof(struct cad_stream_info_struct_type));
		if (rc) {
			pr_err("cad_ioctl() SET_STREAM_INFO failed\n");
			break;
		}

		stream_device[0] = CAD_HW_DEVICE_ID_DEFAULT_TX;
		cad_stream_dev.device = (u32 *)&stream_device[0];
		cad_stream_dev.device_len = 1;
		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_SET_STREAM_DEVICE,
			&cad_stream_dev,
			sizeof(struct cad_stream_device_struct_type));
		if (rc) {
			pr_err("cad_ioctl() SET_STREAM_DEVICE failed\n");
			break;
		}

		cad_write_amr_fmt.ver_id = CAD_WRITE_AMR_VERSION_10;
		cad_write_amr_fmt.amr.stereo_config = 0;
		cad_write_amr_fmt.amr.sample_rate = 8000;

		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_SET_STREAM_CONFIG,
			&cad_write_amr_fmt,
			sizeof(struct cad_write_amr_format_struct_type));
		if (rc) {
			pr_err("cad_ioctl() SET_STREAM_CONFIG failed\n");
			break;
		}

		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_STREAM_START,
			NULL, 0);
		if (rc) {
			pr_err("cad_ioctl() STREAM_START failed\n");
			break;
		}
		break;
	case AUDIO_STOP:
		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_STREAM_PAUSE,
			cad_arg, sizeof(u32));
		break;
	case AUDIO_FLUSH:
		rc = cad_ioctl(p->cad_w_handle, CAD_IOCTL_CMD_STREAM_FLUSH,
			cad_arg, sizeof(u32));
		break;
	case AUDIO_GET_CONFIG:
		if (copy_to_user((void *)arg, &p->cfg,
				sizeof(struct msm_audio_config)))
			return -EFAULT;
	case AUDIO_SET_CONFIG:
		rc = copy_from_user(&p->cfg, (void *)arg,
				sizeof(struct msm_audio_config));
		break;
	default:
		rc = -EINVAL;
	}

	return rc;
}

#ifdef CONFIG_PROC_FS
int msm8k_amr_in_read_proc(char *pbuf, char **start, off_t offset,
			int count, int *eof, void *data)
{
	int len = 0;
	len += snprintf(pbuf, 16, "amr_in\n");

	*eof = 1;
	return len;
}
#endif

static const struct file_operations msm8k_amr_in_fops = {
	.owner = THIS_MODULE,
	.open = msm8k_amr_in_open,
	.release = msm8k_amr_in_release,
	.read = msm8k_amr_in_read,
	.write = msm8k_amr_in_write,
	.ioctl = msm8k_amr_in_ioctl,
	.llseek = no_llseek,
};


struct miscdevice msm8k_amr_in_misc = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "msm_amr_in",
	.fops	= &msm8k_amr_in_fops,
};

static int __init msm8k_amr_in_init(void)
{
	int rc;
	D("%s\n", __func__);

	rc = misc_register(&msm8k_amr_in_misc);

#ifdef CONFIG_PROC_FS
	create_proc_read_entry(MSM8K_AMR_IN_PROC_NAME,
			0, NULL, msm8k_amr_in_read_proc, NULL);
#endif

	return rc;
}

static void __exit msm8k_amr_in_exit(void)
{
	D("%s\n", __func__);
#ifdef CONFIG_PROC_FS
	remove_proc_entry(MSM8K_AMR_IN_PROC_NAME, NULL);
#endif
}


module_init(msm8k_amr_in_init);
module_exit(msm8k_amr_in_exit);

MODULE_DESCRIPTION("MSM AMR IN driver");
MODULE_LICENSE("GPL v2");

