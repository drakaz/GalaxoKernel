/*
 * Copyright (c) 2008 QUALCOMM USA, INC.
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
 * Video encode driver for QDSP6.
 */

#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/android_pmem.h>
#include <linux/msm_q6venc.h>

#include "msm_q6venc_remote.h"

#define DALDEVICEID_VENC_DEVICE       0x0200002D
#define DALDEVICEID_VENC_PORTNAME     "DAL_AQ_VID"

#define VENC_NAME   "q6venc"

#define DEBUG 0

#if DEBUG
#define DBG(x...) printk(KERN_DEBUG x)
#else
#define DBG(x...) do {} while (0)
#endif

#define LOG_VENC_ENCODE_PARAM(encode_param)  \
		DBG("encode_param->y_addr.fd  = 0x%08x" \
		"encode_param->y_addr.offset = 0x%08x \n" \
		 "encode_param->uv_addr.fd = 0x%08x " \
		 "encode_param->uv_addr.offset = 0x%08x \n", \
		(unsigned int)encode_param->y_addr.fd, \
		(unsigned int)encode_param->y_addr.offset, \
		(unsigned int)encode_param->uv_addr.fd, \
		(unsigned int)encode_param->uv_addr.offset)

#define LOG_VENC_Y_PHY_ADDR(q6_encode_param,len)   \
		DBG("from pmem  y_addr_phy = 0x%08x, len = 0x%08x" ,\
		(unsigned int)q6_encode_param->y_addr_phy,\
		(unsigned int)len)

#define LOG_VENC_UV_PHY_ADDR(q6_encode_param,len)  \
		DBG("from pmem y_addr_phy = 0x%08x, len = 0x%08x" ,\
		(unsigned int)q6_encode_param->uv_addr_phy, \
		(unsigned int)len)

#define LOG_VENC_Y_UV_PHY_ADDR_WITH_OFFSET(q6_encode_param) \
	DBG("after adding offset y_addr_phy  = 0x%08x, uv_addr_phy = 0x%08x" ,\
	(unsigned int)q6_encode_param->y_addr_phy, \
	(unsigned int)q6_encode_param->uv_addr_phy);

struct callback_event_data {
	void *data_notify_event;
	void *enc_cb_handle;
	void *empty_input_buffer_event;
};

struct phy_to_venc_buf_map {
	unsigned int phy_address;
	struct file *file;
	struct venc_buf venc_buf;
};

#define VENC_MAX_BUF_NUM            15

struct q6venc_dev {
	struct cdev cdev;
	struct device *class_devp;
	void *q6venc_handle;
	struct callback_event_data cb_ev_data;
	int encode_done;
	int stop_encode;
	unsigned int buf_num;
	struct phy_to_venc_buf_map phy_to_vbuf_map[VENC_MAX_BUF_NUM];
	struct frame_type frame_type;
	wait_queue_head_t encode_wq;
};

static struct q6venc_dev *q6venc_device_p;
static dev_t q6venc_dev_num;
static struct class *q6venc_class;

static void q6venc_callback(void *context, uint32_t param, void *data,
			    uint32_t len)
{
	unsigned int id;
	struct q6venc_dev *q6venc_devp = context;

	struct q6_frame_type *q6_frame_type = data;

	DBG("%s \n", __func__);

	q6venc_devp->encode_done = 1;

	for (id = 0; id < q6venc_devp->buf_num; id++) {
		if (q6venc_devp->phy_to_vbuf_map[id].phy_address ==
		    q6_frame_type->frame_addr)
			break;
	}

	if ((q6venc_devp->buf_num == 0) || (id == q6venc_devp->buf_num)) {
		printk(KERN_ERR
		       "%s got Incorrect phy address 0x%08x from q6 \n",
		       __func__, q6_frame_type->frame_addr);
		q6venc_devp->frame_type.q6_frame_type.frame_len = 0;
		wake_up_interruptible(&q6venc_devp->encode_wq);
		return;
	}

	q6venc_devp->frame_type.frame_addr =
	    q6venc_devp->phy_to_vbuf_map[id].venc_buf;

	q6venc_devp->frame_type.q6_frame_type = *q6_frame_type;

	wake_up_interruptible(&q6venc_devp->encode_wq);
}

static int q6venc_open(struct inode *inode, struct file *file)
{
	int err;
	struct q6venc_dev *q6venc_devp;
	void *cb_handle;

	DBG("%s \n", __func__);

	q6venc_devp = container_of(inode->i_cdev, struct q6venc_dev, cdev);

	memset(&(q6venc_devp->phy_to_vbuf_map[0]), 0, VENC_MAX_BUF_NUM *
	       sizeof(struct phy_to_venc_buf_map));

	file->private_data = q6venc_devp;

	err = daldevice_attach(DALDEVICEID_VENC_DEVICE,
			       DALDEVICEID_VENC_PORTNAME,
			       DALRPC_DEST_QDSP, &q6venc_devp->q6venc_handle);
	if (err) {
		printk(KERN_ERR "%s: daldevice_attach failed\n", __func__);
		return -EIO;
	}

	DBG("%s: daldevice_attach done  q6venc_handle = 0x%08x  \n", __func__,
	    (unsigned int)q6venc_devp->q6venc_handle);

	cb_handle = dalrpc_alloc_cb(q6venc_devp->q6venc_handle,
				    q6venc_callback, q6venc_devp);

	if (!cb_handle) {
		err = -ENOMEM;
		printk(KERN_ERR "%s: dalrpc_alloc_cb failed \n", __func__);
		goto err_q6venc_dalrpc_alloc_cb;
	}

	q6venc_devp->cb_ev_data.enc_cb_handle = cb_handle;

	DBG("cb_handle = 0x%08x\n",
	    (unsigned int)q6venc_devp->cb_ev_data.enc_cb_handle);

	err = q6venc_set_cb_channel(q6venc_devp->q6venc_handle,
				    &q6venc_devp->cb_ev_data,
				    sizeof(q6venc_devp->cb_ev_data));

	if (err) {
		printk(KERN_ERR "%s: q6venc_set_cb_channel failed \n",
		       __func__);
		err = -EIO;

		goto err_q6venc_set_cb_channel;

	}
	DBG("%s  done \n", __func__);

	return 0;

err_q6venc_set_cb_channel:
	dalrpc_dealloc_cb(q6venc_devp->q6venc_handle,
			  q6venc_devp->cb_ev_data.enc_cb_handle);

err_q6venc_dalrpc_alloc_cb:
	daldevice_detach(q6venc_devp->q6venc_handle);

	return err;
}

static int q6venc_release(struct inode *inode, struct file *file)
{
	struct q6venc_dev *q6venc_devp;

	DBG("%s\n", __func__);
	q6venc_devp = file->private_data;

	dalrpc_dealloc_cb(q6venc_devp->q6venc_handle,
			  (q6venc_devp->cb_ev_data).enc_cb_handle);

	daldevice_detach(q6venc_devp->q6venc_handle);

	return 0;
}

static int convert_to_q6_init_config(struct q6venc_dev *q6venc_devp,
				     struct init_config *init_config)
{
	unsigned long len;
	unsigned long vaddr;
	struct file *file;
	int ret;

	struct q6_init_config *q6_init_config = &init_config->q6_init_config;

	ret = get_pmem_file(init_config->ref_frame_buf1.fd,
			    (unsigned long *)&q6_init_config->
			    ref_frame_buf1_phy, &vaddr, (unsigned long *)&len,
			    &file);

	if (ret) {
		printk(KERN_ERR "%s: get_pmem_file failed"
		       " ref_frame_buf1.fd = 0x%08x \n",
		       __func__, init_config->ref_frame_buf1.fd);
		return ret;
	}

	q6_init_config->ref_frame_buf1_phy +=
	    init_config->ref_frame_buf1.offset;

	q6venc_devp->phy_to_vbuf_map[0].phy_address =
	    q6_init_config->ref_frame_buf1_phy;

	q6venc_devp->phy_to_vbuf_map[0].file = file;

	q6venc_devp->phy_to_vbuf_map[0].venc_buf = init_config->ref_frame_buf1;

	ret = get_pmem_file(init_config->ref_frame_buf2.fd,
			    (unsigned long *)&q6_init_config->
			    ref_frame_buf2_phy, &vaddr, (unsigned long *)&len,
			    &file);

	if (ret) {
		printk(KERN_ERR "%s: get_pmem_file failed"
		       "ref_frame_buf2.fd = 0x%08x \n",
		       __func__, init_config->ref_frame_buf2.fd);
		return ret;
	}

	q6_init_config->ref_frame_buf2_phy +=
	    init_config->ref_frame_buf2.offset;

	q6venc_devp->phy_to_vbuf_map[1].phy_address =
	    q6_init_config->ref_frame_buf2_phy;

	q6venc_devp->phy_to_vbuf_map[1].file = file;

	q6venc_devp->phy_to_vbuf_map[1].venc_buf = init_config->ref_frame_buf2;

	ret = get_pmem_file(init_config->rlc_buf1.fd,
			    (unsigned long *)&q6_init_config->rlc_buf1_phy,
			    &vaddr, (unsigned long *)&len, &file);

	if (ret) {
		printk(KERN_ERR "%s: get_pmem_file failed"
		       "rlc_buf1.fd = 0x%08x \n",
		       __func__, init_config->rlc_buf1.fd);
		return ret;
	}

	q6_init_config->rlc_buf1_phy += init_config->rlc_buf1.offset;

	q6venc_devp->phy_to_vbuf_map[2].phy_address =
	    q6_init_config->rlc_buf1_phy;

	q6venc_devp->phy_to_vbuf_map[2].file = file;

	q6venc_devp->phy_to_vbuf_map[2].venc_buf = init_config->rlc_buf1;

	ret = get_pmem_file(init_config->rlc_buf2.fd,
			    (unsigned long *)&q6_init_config->rlc_buf2_phy,
			    &vaddr, (unsigned long *)&len, &file);

	if (ret) {
		printk(KERN_ERR "%s: get_pmem_file failed "
		       " rlc_buf2.fd = 0x%08x\n", __func__,
		       init_config->rlc_buf2.fd);
		return ret;
	}

	q6_init_config->rlc_buf2_phy += init_config->rlc_buf2.offset;

	q6venc_devp->phy_to_vbuf_map[3].phy_address =
	    q6_init_config->rlc_buf2_phy;

	q6venc_devp->phy_to_vbuf_map[3].file = file;

	q6venc_devp->phy_to_vbuf_map[3].venc_buf = init_config->rlc_buf2;

	q6venc_devp->buf_num = 4;

	return 0;

}

static int convert_to_q6_encode_param(struct q6venc_dev *q6venc_devp,
				      struct encode_param *encode_param)
{
	int ret;
	unsigned long len;
	unsigned long vaddr;
	struct file *file;
	unsigned int id, buf_num = q6venc_devp->buf_num;
	struct q6_encode_param *q6_encode_param =
	    &encode_param->q6_encode_param;

	LOG_VENC_ENCODE_PARAM(encode_param);

	for (id = 0; id < buf_num; id++) {
		if ((q6venc_devp->phy_to_vbuf_map[id].venc_buf.fd ==
		     encode_param->y_addr.fd) &&
		    (q6venc_devp->phy_to_vbuf_map[id].venc_buf.offset ==
		     encode_param->y_addr.offset)) {
			q6_encode_param->y_addr_phy =
			    q6venc_devp->phy_to_vbuf_map[id].phy_address;
			break;
		}
	}

	if (buf_num == VENC_MAX_BUF_NUM) {
		printk(KERN_ERR
		       "%s: input buffer number exceeds maximum buffer number %d ",
		       __func__, VENC_MAX_BUF_NUM - 4);

		return -ENOMEM;
	}

	if ((buf_num == 0) || (id == buf_num)) {
		ret = get_pmem_file(encode_param->y_addr.fd,
				    (unsigned long *)&q6_encode_param->
				    y_addr_phy, &vaddr, (unsigned long *)&len,
				    &file);

		LOG_VENC_Y_PHY_ADDR(q6_encode_param, len);

		if (ret) {
			printk(KERN_ERR "%s: get_pmem_file failed "
			       " y_addr.fd = 0x%08x\n",
			       __func__, encode_param->y_addr.fd);
			return -ENOMEM;
		}

		q6_encode_param->y_addr_phy += encode_param->y_addr.offset;

		q6venc_devp->phy_to_vbuf_map[buf_num].phy_address =
		    q6_encode_param->y_addr_phy;

		q6venc_devp->phy_to_vbuf_map[buf_num].file = file;

		q6venc_devp->phy_to_vbuf_map[buf_num].venc_buf =
		    encode_param->y_addr;
		q6venc_devp->buf_num++;
	}

	if (encode_param->y_addr.fd != encode_param->uv_addr.fd) {
		printk(KERN_ERR
		       "Get different fd for input frame y_addr and uv_addr ");
		return -EIO;
	}

	q6_encode_param->uv_addr_phy = q6_encode_param->y_addr_phy -
	    encode_param->y_addr.offset;
	q6_encode_param->uv_addr_phy += encode_param->uv_addr.offset;

	LOG_VENC_Y_UV_PHY_ADDR_WITH_OFFSET(q6_encode_param);

	return 0;
}

static int q6venc_ioctl(struct inode *inode, struct file *file,
			unsigned cmd, unsigned long arg)
{
	int err;
	unsigned int id;
	struct init_config config;
	struct encode_param encode_param;
	struct intra_refresh intra_refresh;
	struct rc_config rc_config;

	struct q6venc_dev *q6venc_devp = file->private_data;

	DBG("%s\n", __func__);

	if (_IOC_TYPE(cmd) != VENC_IOCTL_MAGIC)
		return -ENOTTY;

	switch (cmd) {
	case VENC_IOCTL_INITIALIZE:

		DBG("%s VENC_IOCTL_INITIALIZE \n", __func__);

		if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
			return -EFAULT;

		err = convert_to_q6_init_config(q6venc_devp, &config);

		if (err) {
			printk(KERN_ERR "%s(): convert_to_q6_init_config "
			       " failed \n", __func__);
			return err;
		}

		err = q6venc_initialize(q6venc_devp->q6venc_handle,
					(void *)&config.q6_init_config,
					sizeof(struct q6_init_config));

		if (err) {
			printk(KERN_ERR "%s: q6venc_initialize failed \n",
			       __func__);
			return -EIO;
		}
		break;

	case VENC_IOCTL_ENCODE:

		DBG("%s VENC_IOCTL_ENCODE  \n", __func__);

		if (copy_from_user(&encode_param, (void __user *)arg,
				   sizeof(encode_param)))
			return -EFAULT;

		err = convert_to_q6_encode_param(q6venc_devp, &encode_param);
		if (err) {
			printk(KERN_ERR "%s(): convert_to_q6_encode_param "
			       " failed \n", __func__);
			return err;
		}

		err = q6venc_encode(q6venc_devp->q6venc_handle,
				    (void *)&encode_param.q6_encode_param,
				    sizeof(struct q6_encode_param));

		if (err) {
			printk(KERN_ERR "%s: q6venc_encode failed \n",
			       __func__);
			return -EIO;
		}

		break;

	case VENC_IOCTL_INTRA_REFRESH:

		DBG("%s VENC_IOCTL_INTRA_REFRESH  \n", __func__);

		if (copy_from_user(&intra_refresh, (void __user *)arg,
				   sizeof(intra_refresh)))
			return -EFAULT;

		err = q6venc_intra_refresh(q6venc_devp->q6venc_handle,
					   (void *)&intra_refresh,
					   sizeof(struct intra_refresh));

		if (err) {
			printk(KERN_ERR "%s: q6venc_intra_refresh failed \n",
			       __func__);
			return -EIO;
		}

		break;

	case VENC_IOCTL_RC_CONFIG:

		DBG("%s VENC_IOCTL_RC_CONFIG  \n", __func__);

		if (copy_from_user(&rc_config, (void __user *)arg,
				   sizeof(rc_config)))
			return -EFAULT;

		err = q6venc_rc_config(q6venc_devp->q6venc_handle,
				       (void *)&rc_config, sizeof(rc_config));

		if (err) {

			printk(KERN_ERR "%s: q6venc_rc_config  failed \n",
			       __func__);
			return -EIO;
		}

		break;

	case VENC_IOCTL_ENCODE_CONFIG:

		DBG("%s   VENC_IOCTL_ENCODE_CONFIG \n", __func__);

		if (copy_from_user(&config, (void __user *)arg, sizeof(config)))
			return -EFAULT;

		err = convert_to_q6_init_config(q6venc_devp, &config);
		if (err) {
			printk(KERN_ERR "%s(): convert_to_q6_init_config "
			       " failed \n", __func__);
			return err;
		}

		err = q6venc_encode_config(q6venc_devp->q6venc_handle,
					   (void *)&config.q6_init_config,
					   sizeof(struct q6_init_config));
		if (err) {
			printk(KERN_ERR "%s : q6venc_encode_config failed \n",
			       __func__);
			return -EIO;
		}

		break;

	case VENC_IOCTL_STOP:

		DBG("%s VENC_IOCTL_STOP  \n", __func__);

		for (id = 0; id < q6venc_devp->buf_num; id++) {
			if (q6venc_devp->phy_to_vbuf_map[id].file) {
				put_pmem_file(q6venc_devp->phy_to_vbuf_map[id].
					      file);
			}
		}

		err = q6venc_stop(q6venc_devp->q6venc_handle);

		if (err) {
			printk(KERN_ERR "%s: q6venc_stop faild \n", __func__);
			return -EIO;
		}
		break;

	case VENC_IOCTL_WAIT_FOR_ENCODE:

		DBG("%s: waiting for encode done event \n", __func__);

		if ((wait_event_interruptible(q6venc_devp->encode_wq,
					      (q6venc_devp->encode_done ||
					       q6venc_devp->stop_encode)) < 0))
			return -ERESTARTSYS;

		if (q6venc_devp->stop_encode) {

			DBG("%s: Received Stop encode event \n", __func__);
			q6venc_devp->stop_encode = 0;
			return -EINTR;

		} else if (q6venc_devp->encode_done) {

			q6venc_devp->encode_done = 0;

			if (q6venc_devp->frame_type.q6_frame_type.frame_len ==
			    0) {

				DBG("%s: got incorrect phy address from q6 \n",
				    __func__);
				return -ENOMEM;

			} else {
				DBG("%s: done encoding \n", __func__);
				q6venc_devp->encode_done = 0;

				if (copy_to_user((void __user *)arg,
						 &q6venc_devp->frame_type,
						 sizeof(q6venc_devp->
							frame_type)))
					return -EFAULT;
			}
		} else {
			DBG("%s: ERRROR Woke up for unknown reason \n",
			    __func__);
			return -EIO;
		}

		break;

	case VENC_IOCTL_STOP_ENCODE:

		DBG("%s: Stop  encode event   \n", __func__);

		q6venc_devp->stop_encode = 1;

		wake_up_interruptible(&q6venc_devp->encode_wq);

		break;

	default:
		return -ENOTTY;
	}

	return 0;
}

static struct file_operations q6venc_fops = {
	.owner = THIS_MODULE,
	.open = q6venc_open,
	.release = q6venc_release,
	.ioctl = q6venc_ioctl,
};

static int __init q6venc_init(void)
{
	int rc = 0;

	q6venc_device_p = kzalloc(sizeof(struct q6venc_dev), GFP_KERNEL);
	if (!q6venc_device_p) {
		printk(KERN_ERR "%s Unable to allocate memory for q6venc_dev\n",
		       __func__);
		return -ENOMEM;
	}

	rc = alloc_chrdev_region(&q6venc_dev_num, 0, 1, VENC_NAME);
	if (rc < 0) {
		printk(KERN_ERR "%s: alloc_chrdev_region Failed rc = %d\n",
		       __func__, rc);
		goto error_q6venc_alloc_chrdev_region;
	}

	q6venc_class = class_create(THIS_MODULE, VENC_NAME);
	if (IS_ERR(q6venc_class)) {
		rc = PTR_ERR(q6venc_class);
		printk(KERN_ERR "%s: couldn't create q6venc_class rc = %d\n",
		       __func__, rc);

		goto error_q6venc_class_create;
	}
	q6venc_device_p->class_devp = device_create(q6venc_class, NULL,
						    q6venc_dev_num, NULL,
						    VENC_NAME);

	if (IS_ERR(q6venc_device_p->class_devp)) {
		rc = PTR_ERR(q6venc_device_p->class_devp);
		printk(KERN_ERR "%s: class_device_create failed %d\n",
		       __func__, rc);
		goto error_q6venc_class_device_create;
	}

	cdev_init(&q6venc_device_p->cdev, &q6venc_fops);
	q6venc_device_p->cdev.owner = THIS_MODULE;
	rc = cdev_add(&(q6venc_device_p->cdev), q6venc_dev_num, 1);

	if (rc < 0) {
		printk(KERN_ERR "%s: cdev_add failed %d\n", __func__, rc);
		goto error_q6venc_cdev_add;
	}

	init_waitqueue_head(&q6venc_device_p->encode_wq);

	q6venc_device_p->encode_done = 0;
	q6venc_device_p->stop_encode = 0;

	return 0;

error_q6venc_cdev_add:
	device_destroy(q6venc_class, q6venc_dev_num);
error_q6venc_class_device_create:
	class_destroy(q6venc_class);
error_q6venc_class_create:
	unregister_chrdev_region(q6venc_dev_num, 1);
error_q6venc_alloc_chrdev_region:
	kfree(q6venc_device_p);

	return rc;
}

static void __exit q6venc_exit(void)
{
	cdev_del(&(q6venc_device_p->cdev));
	device_destroy(q6venc_class, q6venc_dev_num);
	class_destroy(q6venc_class);
	unregister_chrdev_region(q6venc_dev_num, 1);
	kfree(q6venc_device_p);
}

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Video encode driver for QDSP6");
MODULE_VERSION("1.0");

module_init(q6venc_init);
module_exit(q6venc_exit);
