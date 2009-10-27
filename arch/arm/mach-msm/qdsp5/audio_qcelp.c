/* arch/arm/mach-msm/qdsp5/audio_qcelp.c
 *
 * qcelp 13k audio decoder device
 *
 * Copyright (c) 2008-2009 QUALCOMM USA, INC.
 *
 * This code is based in part on audio_mp3.c, which is
 * Copyright (C) 2008 Google, Inc.
 * Copyright (C) 2008 HTC Corporation
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org.
 *
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/dma-mapping.h>
#include <linux/debugfs.h>
#include <linux/earlysuspend.h>
#include <linux/list.h>
#include <asm/ioctls.h>
#include <mach/msm_adsp.h>
#include <linux/msm_audio.h>
#include <mach/qdsp5/qdsp5audppcmdi.h>
#include <mach/qdsp5/qdsp5audppmsg.h>
#include <mach/qdsp5/qdsp5audplaycmdi.h>
#include <mach/qdsp5/qdsp5audplaymsg.h>

#include "audmgr.h"
/* for queue ids - should be relative to module number*/
#include "adsp.h"

#ifdef DEBUG
#define dprintk(format, arg...) \
printk(KERN_DEBUG format, ## arg)
#else
#define dprintk(format, arg...) do {} while (0)
#endif

#define BUFSZ 1080 /* QCELP 13K Hold 600ms packet data = 36 * 30 */
#define BUF_COUNT 2
#define DMASZ (BUFSZ * BUF_COUNT)

#define PCM_BUFSZ_MIN 1600 /* 100ms worth of data */
#define PCM_BUF_MAX_COUNT 5

#define AUDDEC_DEC_QCELP 9

#define	ROUTING_MODE_FTRT	1
#define	ROUTING_MODE_RT		2
/* Decoder status received from AUDPPTASK */
#define	AUDPP_DEC_STATUS_SLEEP	0
#define	AUDPP_DEC_STATUS_INIT	1
#define	AUDPP_DEC_STATUS_CFG	2
#define	AUDPP_DEC_STATUS_PLAY	3

struct buffer {
	void *data;
	unsigned size;
	unsigned used;		/* Input usage actual DSP produced PCM size  */
	unsigned addr;
	int      eos; /* non-tunnel EOS purpose */
};

#ifdef CONFIG_HAS_EARLYSUSPEND
struct audqcelp_suspend_ctl {
	struct early_suspend node;
	struct audio *audio;
};
#endif

struct audqcelp_event{
	struct list_head list;
	int event_type;
	union msm_audio_event_payload payload;
};

struct audio {
	struct buffer out[BUF_COUNT];

	spinlock_t dsp_lock;

	uint8_t out_head;
	uint8_t out_tail;
	uint8_t out_needed;	/* number of buffers the dsp is waiting for */

	struct mutex lock;
	struct mutex write_lock;
	wait_queue_head_t write_wait;

	/* Host PCM section - START */
	struct buffer in[PCM_BUF_MAX_COUNT];
	struct mutex read_lock;
	wait_queue_head_t read_wait;    /* Wait queue for read */
	char *read_data;        /* pointer to reader buffer */
	dma_addr_t read_phys;   /* physical address of reader buffer */
	uint8_t read_next;      /* index to input buffers to be read next */
	uint8_t fill_next;      /* index to buffer that DSP should be filling */
	uint8_t pcm_buf_count;  /* number of pcm buffer allocated */
	/* Host PCM section - END */

	struct msm_adsp_module *audplay;

	struct audmgr audmgr;

	/* data allocated for various buffers */
	char *data;
	dma_addr_t phys;

	int rflush; /* Read  flush */
	int wflush; /* Write flush */
	uint8_t opened:1;
	uint8_t enabled:1;
	uint8_t running:1;
	uint8_t stopped:1;	/* set when stopped, cleared on flush */
	uint8_t pcm_feedback:1; /* set when non-tunnel mode */
	uint8_t buf_refresh:1;
	int teos; /* valid only if tunnel mode & no data left for decoder */

	unsigned volume;

	uint16_t dec_id;

#ifdef CONFIG_HAS_EARLYSUSPEND
	struct audqcelp_suspend_ctl suspend_ctl;
#endif

	struct list_head event_queue;
	wait_queue_head_t event_wait;
	spinlock_t event_queue_lock;
	struct mutex get_event_lock;
	int event_abort;
};

static struct audio the_qcelp_audio;

static int auddec_dsp_config(struct audio *audio, int enable);
static void audpp_cmd_cfg_adec_params(struct audio *audio);
static void audpp_cmd_cfg_routing_mode(struct audio *audio);
static void audqcelp_send_data(struct audio *audio, unsigned needed);
static void audqcelp_config_hostpcm(struct audio *audio);
static void audqcelp_buffer_refresh(struct audio *audio);
static void audqcelp_dsp_event(void *private, unsigned id, uint16_t *msg);
static void audqcelp_post_event(struct audio *audio, int type,
		union msm_audio_event_payload payload);

/* must be called with audio->lock held */
static int audqcelp_enable(struct audio *audio)
{
	struct audmgr_config cfg;
	int rc;

	dprintk("audqcelp_enable()\n");

	if (audio->enabled)
		return 0;

	audio->out_tail = 0;
	audio->out_needed = 0;

	cfg.tx_rate = RPC_AUD_DEF_SAMPLE_RATE_NONE;
	cfg.rx_rate = RPC_AUD_DEF_SAMPLE_RATE_48000;
	cfg.def_method = RPC_AUD_DEF_METHOD_PLAYBACK;
	cfg.codec = RPC_AUD_DEF_CODEC_13K;
	cfg.snd_method = RPC_SND_METHOD_MIDI;

	rc = audmgr_enable(&audio->audmgr, &cfg);
	if (rc < 0)
		return rc;

	if (msm_adsp_enable(audio->audplay)) {
		pr_err("audio: msm_adsp_enable(audplay) failed\n");
		audmgr_disable(&audio->audmgr);
		return -ENODEV;
	}

	if (audpp_enable(audio->dec_id, audqcelp_dsp_event, audio)) {
		pr_err("audio: audpp_enable() failed\n");
		msm_adsp_disable(audio->audplay);
		audmgr_disable(&audio->audmgr);
		return -ENODEV;
	}
	audio->enabled = 1;
	return 0;
}

/* must be called with audio->lock held */
static int audqcelp_disable(struct audio *audio)
{
	dprintk("audqcelp_disable()\n");
	if (audio->enabled) {

		audio->enabled = 0;
		auddec_dsp_config(audio, 0);
		wake_up(&audio->write_wait);
		wake_up(&audio->read_wait);
		msm_adsp_disable(audio->audplay);
		audpp_disable(audio->dec_id, audio);
		audmgr_disable(&audio->audmgr);
		audio->out_needed = 0;
	}
	return 0;
}

/* ------------------- dsp --------------------- */
static void audqcelp_update_pcm_buf_entry(struct audio *audio,
	uint32_t *payload)
{
	uint8_t index;
	unsigned long flags;

	if (audio->rflush)
		return;

	spin_lock_irqsave(&audio->dsp_lock, flags);
	for (index = 0; index < payload[1]; index++) {
		if (audio->in[audio->fill_next].addr ==
			payload[2 + index * 2]) {
			dprintk("audqcelp_update_pcm_buf_entry: in[%d] ready\n",
			audio->fill_next);
			audio->in[audio->fill_next].used =
			payload[3 + index * 2];
			if (audio->in[audio->fill_next].used == 0) {
				dprintk("%s: EOS signaled\n", __func__);
				audio->in[audio->fill_next].eos = 1;
			}
			if ((++audio->fill_next) == audio->pcm_buf_count)
				audio->fill_next = 0;
		} else {
			pr_err(
			"audqcelp_update_pcm_buf_entry: expected=%x ret=%x\n",
			audio->in[audio->fill_next].addr,
			payload[1 + index * 2]);
			break;
		}
	}
	if (audio->in[audio->fill_next].used == 0 &&
		!audio->in[audio->fill_next].eos) {
		audqcelp_buffer_refresh(audio);
	} else {
		dprintk("audqcelp_update_pcm_buf_entry: read cannot keep up\n");
		audio->buf_refresh = 1;
	}
	wake_up(&audio->read_wait);
	spin_unlock_irqrestore(&audio->dsp_lock, flags);
}

static void audplay_dsp_event(void *data, unsigned id, size_t len,
			      void (*getevent) (void *ptr, size_t len))
{
	struct audio *audio = data;
	uint32_t msg[28];
	getevent(msg, sizeof(msg));

	dprintk("audplay_dsp_event: msg_id=%x\n", id);

	switch (id) {
	case AUDPLAY_MSG_DEC_NEEDS_DATA:
		audqcelp_send_data(audio, 1);
		break;

	case AUDPLAY_MSG_BUFFER_UPDATE:
		audqcelp_update_pcm_buf_entry(audio, msg);
		break;

	default:
		pr_err("unexpected message from decoder \n");
	}
}

static void audqcelp_dsp_event(void *private, unsigned id, uint16_t *msg)
{
	struct audio *audio = private;

	switch (id) {
	case AUDPP_MSG_STATUS_MSG:{
			unsigned status = msg[1];

			switch (status) {
			case AUDPP_DEC_STATUS_SLEEP:
				dprintk("decoder status: sleep \n");
				break;

			case AUDPP_DEC_STATUS_INIT:
				dprintk("decoder status: init \n");
				audpp_cmd_cfg_routing_mode(audio);
				break;

			case AUDPP_DEC_STATUS_CFG:
				dprintk("decoder status: cfg \n");
				break;
			case AUDPP_DEC_STATUS_PLAY:
				dprintk("decoder status: play \n");
				if (audio->pcm_feedback) {
					audqcelp_config_hostpcm(audio);
					audqcelp_buffer_refresh(audio);
				}
				break;
			default:
				pr_err("unknown decoder status \n");
			}
			break;
		}
	case AUDPP_MSG_CFG_MSG:
		if (msg[0] == AUDPP_MSG_ENA_ENA) {
			dprintk("audqcelp_dsp_event: CFG_MSG ENABLE\n");
			auddec_dsp_config(audio, 1);
			audio->out_needed = 0;
			audio->running = 1;
			audpp_set_volume_and_pan(audio->dec_id, audio->volume,
						 0);
			audpp_avsync(audio->dec_id, 22050);
		} else if (msg[0] == AUDPP_MSG_ENA_DIS) {
			dprintk("audqcelp_dsp_event: CFG_MSG DISABLE\n");
			audpp_avsync(audio->dec_id, 0);
			audio->running = 0;
		} else {
			pr_err("audqcelp_dsp_event: CFG_MSG %d?\n", msg[0]);
		}
		break;
	case AUDPP_MSG_ROUTING_ACK:
		dprintk("audqcelp_dsp_event: ROUTING_ACK mode=%d\n", msg[1]);
		audpp_cmd_cfg_adec_params(audio);
		break;
	case AUDPP_MSG_FLUSH_ACK:
		dprintk("%s: FLUSH_ACK\n", __func__);
		audio->wflush = 0;
		audio->rflush = 0;
		wake_up(&audio->write_wait);
		if (audio->pcm_feedback)
			audqcelp_buffer_refresh(audio);
		break;
	case AUDPP_MSG_PCMDMAMISSED:
		dprintk("%s: PCMDMAMISSED\n", __func__);
		audio->teos = 1;
		wake_up(&audio->write_wait);
		break;
	default:
		pr_err("audqcelp_dsp_event: UNKNOWN (%d)\n", id);
	}

}

struct msm_adsp_ops audplay_adsp_ops_qcelp = {
	.event = audplay_dsp_event,
};

#define audplay_send_queue0(audio, cmd, len) \
	msm_adsp_write(audio->audplay, QDSP_uPAudPlay0BitStreamCtrlQueue, \
		       cmd, len)

static int auddec_dsp_config(struct audio *audio, int enable)
{
	audpp_cmd_cfg_dec_type cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPP_CMD_CFG_DEC_TYPE;
	if (enable)
		cmd.dec0_cfg = AUDPP_CMD_UPDATDE_CFG_DEC |
		    AUDPP_CMD_ENA_DEC_V | AUDDEC_DEC_QCELP;
	else
		cmd.dec0_cfg = AUDPP_CMD_UPDATDE_CFG_DEC | AUDPP_CMD_DIS_DEC_V;

	return audpp_send_queue1(&cmd, sizeof(cmd));
}

static void audpp_cmd_cfg_adec_params(struct audio *audio)
{
	struct audpp_cmd_cfg_adec_params_v13k cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.common.cmd_id = AUDPP_CMD_CFG_ADEC_PARAMS;
	cmd.common.length = AUDPP_CMD_CFG_ADEC_PARAMS_V13K_LEN;
	cmd.common.dec_id = audio->dec_id;
	cmd.common.input_sampling_frequency = 8000;
	cmd.stereo_cfg = AUDPP_CMD_PCM_INTF_MONO_V;

	audpp_send_queue2(&cmd, sizeof(cmd));
}

static void audpp_cmd_cfg_routing_mode(struct audio *audio)
{
	struct audpp_cmd_routing_mode cmd;
	dprintk("audpp_cmd_cfg_routing_mode()\n");
	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd_id = AUDPP_CMD_ROUTING_MODE;
	cmd.object_number = audio->dec_id;
	if (audio->pcm_feedback)
		cmd.routing_mode = ROUTING_MODE_FTRT;
	else
		cmd.routing_mode = ROUTING_MODE_RT;
	audpp_send_queue1(&cmd, sizeof(cmd));
}

static int audplay_dsp_send_data_avail(struct audio *audio,
				       unsigned idx, unsigned len)
{
	audplay_cmd_bitstream_data_avail cmd;

	cmd.cmd_id = AUDPLAY_CMD_BITSTREAM_DATA_AVAIL;
	cmd.decoder_id = audio->dec_id;
	cmd.buf_ptr = audio->out[idx].addr;
	cmd.buf_size = len / 2;
	cmd.partition_number = 0;
	return audplay_send_queue0(audio, &cmd, sizeof(cmd));
}

static int audplay_signal_eos(struct audio *audio)
{
	audplay_cmd_bitstream_data_avail cmd;

	dprintk("%s()\n", __func__);
	cmd.cmd_id		= AUDPLAY_CMD_BITSTREAM_DATA_AVAIL;
	cmd.decoder_id	= -1; /* Set metafield to -1 */
	cmd.buf_ptr = 0;
	cmd.buf_size	= 0; /* Set Zero Buffer size, to signal EOS to FW */
	cmd.partition_number	= 0;
	return audplay_send_queue0(audio, &cmd, sizeof(cmd));
}

static void audqcelp_buffer_refresh(struct audio *audio)
{
	struct audplay_cmd_buffer_refresh refresh_cmd;

	refresh_cmd.cmd_id = AUDPLAY_CMD_BUFFER_REFRESH;
	refresh_cmd.num_buffers = 1;
	refresh_cmd.buf0_address = audio->in[audio->fill_next].addr;
	refresh_cmd.buf0_length = audio->in[audio->fill_next].size;
	refresh_cmd.buf_read_count = 0;
	dprintk("audplay_buffer_fresh: buf0_addr=%x buf0_len=%d\n",
		refresh_cmd.buf0_address, refresh_cmd.buf0_length);

	(void)audplay_send_queue0(audio, &refresh_cmd, sizeof(refresh_cmd));
}

static void audqcelp_config_hostpcm(struct audio *audio)
{
	struct audplay_cmd_hpcm_buf_cfg cfg_cmd;

	dprintk("audqcelp_config_hostpcm()\n");
	cfg_cmd.cmd_id = AUDPLAY_CMD_HPCM_BUF_CFG;
	cfg_cmd.max_buffers = 1;
	cfg_cmd.byte_swap = 0;
	cfg_cmd.hostpcm_config = (0x8000) | (0x4000);
	cfg_cmd.feedback_frequency = 1;
	cfg_cmd.partition_number = 0;

	(void)audplay_send_queue0(audio, &cfg_cmd, sizeof(cfg_cmd));
}

static void audqcelp_send_data(struct audio *audio, unsigned needed)
{
	struct buffer *frame;
	unsigned long flags;

	spin_lock_irqsave(&audio->dsp_lock, flags);
	if (!audio->running)
		goto done;

	if (needed && !audio->wflush) {
		/* We were called from the callback because the DSP
		 * requested more data.  Note that the DSP does want
		 * more data, and if a buffer was in-flight, mark it
		 * as available (since the DSP must now be done with
		 * it).
		 */
		audio->out_needed = 1;
		frame = audio->out + audio->out_tail;
		if (frame->used == 0xffffffff) {
			dprintk("frame %d free\n", audio->out_tail);
			frame->used = 0;
			audio->out_tail ^= 1;
			wake_up(&audio->write_wait);
		}
	}

	if (audio->out_needed) {
		/* If the DSP currently wants data and we have a
		 * buffer available, we will send it and reset
		 * the needed flag.  We'll mark the buffer as in-flight
		 * so that it won't be recycled until the next buffer
		 * is requested
		 */

		frame = audio->out + audio->out_tail;
		if (frame->used) {
			BUG_ON(frame->used == 0xffffffff);
			dprintk("frame %d busy\n", audio->out_tail);
			audplay_dsp_send_data_avail(audio, audio->out_tail,
						    frame->used);
			frame->used = 0xffffffff;
			audio->out_needed = 0;
		}
	}
 done:
	spin_unlock_irqrestore(&audio->dsp_lock, flags);
}

/* ------------------- device --------------------- */

static void audqcelp_flush(struct audio *audio)
{
	audio->out[0].used = 0;
	audio->out[1].used = 0;
	audio->out_head = 0;
	audio->out_tail = 0;
	audio->out_needed = 0;
}

static void audqcelp_flush_pcm_buf(struct audio *audio)
{
	uint8_t index;

	for (index = 0; index < PCM_BUF_MAX_COUNT; index++)
		audio->in[index].used = 0;

	audio->buf_refresh = 0;
	audio->read_next = 0;
	audio->fill_next = 0;
}

static void audqcelp_ioport_reset(struct audio *audio)
{
	/* Make sure read/write thread are free from
	 * sleep and knowing that system is not able
	 * to process io request at the moment
	 */
	wake_up(&audio->write_wait);
	mutex_lock(&audio->write_lock);
	audqcelp_flush(audio);
	mutex_unlock(&audio->write_lock);
	wake_up(&audio->read_wait);
	mutex_lock(&audio->read_lock);
	audqcelp_flush_pcm_buf(audio);
	mutex_unlock(&audio->read_lock);
}

static int audqcelp_events_pending(struct audio *audio)
{
	unsigned long flags;
	int empty;

	spin_lock_irqsave(&audio->event_queue_lock, flags);
	empty = !list_empty(&audio->event_queue);
	spin_unlock_irqrestore(&audio->event_queue_lock, flags);
	return empty || audio->event_abort;
}

static void audqcelp_reset_event_queue(struct audio *audio)
{
	unsigned long flags;
	struct audqcelp_event *drv_evt;
	struct list_head *ptr, *next;

	spin_lock_irqsave(&audio->event_queue_lock, flags);
	list_for_each_safe(ptr, next, &audio->event_queue) {
		drv_evt = list_first_entry(&audio->event_queue,
				struct audqcelp_event, list);
		list_del(&drv_evt->list);
		kfree(drv_evt);
	}
	spin_unlock_irqrestore(&audio->event_queue_lock, flags);

	return;
}


static long audqcelp_process_event_req(struct audio *audio, void __user *arg)
{
	long rc;
	struct msm_audio_event usr_evt;
	struct audqcelp_event *drv_evt = NULL;
	int timeout;
	unsigned long flags;

	if (copy_from_user(&usr_evt, arg, sizeof(struct msm_audio_event)))
		return -EFAULT;

	timeout = (int) usr_evt.timeout_ms;

	if (timeout > 0) {
		rc = wait_event_interruptible_timeout(
			audio->event_wait, audqcelp_events_pending(audio),
			msecs_to_jiffies(timeout));
		if (rc == 0)
			return -ETIMEDOUT;
	} else {
		rc = wait_event_interruptible(
			audio->event_wait, audqcelp_events_pending(audio));
	}

	if (rc < 0)
		return rc;

	if (audio->event_abort) {
		audio->event_abort = 0;
		return -ENODEV;
	}

	rc = 0;

	spin_lock_irqsave(&audio->event_queue_lock, flags);
	if (!list_empty(&audio->event_queue)) {
		drv_evt = list_first_entry(&audio->event_queue,
				struct audqcelp_event, list);
		list_del(&drv_evt->list);
	}
	spin_unlock_irqrestore(&audio->event_queue_lock, flags);

	if (drv_evt) {
		usr_evt.event_type = drv_evt->event_type;
		usr_evt.event_payload = drv_evt->payload;

		if (copy_to_user(arg, &usr_evt, sizeof(usr_evt)))
			rc = -EFAULT;

		kfree(drv_evt);
	} else
		rc = -1;
	return rc;
}

static long audqcelp_ioctl(struct file *file, unsigned int cmd,
		unsigned long arg)
{
	struct audio *audio = file->private_data;
	int rc = 0;

	dprintk("audqcelp_ioctl() cmd = %d\n", cmd);

	if (cmd == AUDIO_GET_STATS) {
		struct msm_audio_stats stats;
		stats.byte_count = audpp_avsync_byte_count(audio->dec_id);
		stats.sample_count = audpp_avsync_sample_count(audio->dec_id);
		if (copy_to_user((void *)arg, &stats, sizeof(stats)))
			return -EFAULT;
		return 0;
	}
	if (cmd == AUDIO_SET_VOLUME) {
		unsigned long flags;
		spin_lock_irqsave(&audio->dsp_lock, flags);
		audio->volume = arg;
		if (audio->running)
			audpp_set_volume_and_pan(audio->dec_id, arg, 0);
		spin_unlock_irqrestore(&audio->dsp_lock, flags);
		return 0;
	}

	if (cmd == AUDIO_GET_EVENT) {
		dprintk("%s: AUDIO_GET_EVENT\n", __func__);
		if (mutex_trylock(&audio->get_event_lock)) {
			rc = audqcelp_process_event_req(audio,
					(void __user *) arg);
			mutex_unlock(&audio->get_event_lock);
		} else
			rc = -EBUSY;
		return rc;
	}

	if (cmd == AUDIO_ABORT_GET_EVENT) {
		audio->event_abort = 1;
		wake_up(&audio->event_wait);
		return 0;
	}

	mutex_lock(&audio->lock);
	switch (cmd) {
	case AUDIO_START:
		rc = audqcelp_enable(audio);
		break;
	case AUDIO_STOP:
		rc = audqcelp_disable(audio);
		audio->stopped = 1;
		audqcelp_ioport_reset(audio);
		audio->stopped = 0;
		break;
	case AUDIO_FLUSH:
		dprintk("%s: AUDIO_FLUSH\n", __func__);
		audio->rflush = 1;
		audio->wflush = 1;
		audqcelp_ioport_reset(audio);
		if (audio->running) {
			audpp_flush(audio->dec_id);
			rc = wait_event_interruptible(audio->write_wait,
				!audio->wflush);
			if (rc < 0) {
				pr_err("%s: AUDIO_FLUSH interrupted\n",
					__func__);
				rc = -EINTR;
			}
		} else {
			audio->rflush = 0;
			audio->wflush = 0;
		}
		break;
	case AUDIO_SET_CONFIG:
		dprintk("AUDIO_SET_CONFIG not applicable \n");
		break;
	case AUDIO_GET_CONFIG:{
			struct msm_audio_config config;
			config.buffer_size = BUFSZ;
			config.buffer_count = BUF_COUNT;
			config.sample_rate = 8000;
			config.channel_count = 1;
			config.unused[0] = 0;
			config.unused[1] = 0;
			config.unused[2] = 0;
			config.unused[3] = 0;
			if (copy_to_user((void *)arg, &config,
					 sizeof(config)))
				rc = -EFAULT;
			else
				rc = 0;

			break;
		}
	case AUDIO_GET_PCM_CONFIG:{
			struct msm_audio_pcm_config config;

			config.pcm_feedback = 0;
			config.buffer_count = PCM_BUF_MAX_COUNT;
			config.buffer_size = PCM_BUFSZ_MIN;
			if (copy_to_user((void *)arg, &config,
				sizeof(config)))
				rc = -EFAULT;
			else
				rc = 0;
			break;
		}
	case AUDIO_SET_PCM_CONFIG:{
			struct msm_audio_pcm_config config;

			if (copy_from_user(&config, (void *)arg,
				sizeof(config))) {
				rc = -EFAULT;
				break;
			}
			if ((config.buffer_count > PCM_BUF_MAX_COUNT) ||
				(config.buffer_count == 1))
				config.buffer_count = PCM_BUF_MAX_COUNT;

			if (config.buffer_size < PCM_BUFSZ_MIN)
				config.buffer_size = PCM_BUFSZ_MIN;

			/* Check if pcm feedback is required */
			if ((config.pcm_feedback) && (!audio->read_data)) {
				dprintk(
				"audqcelp_ioctl: allocate PCM buf %d\n",
				config.buffer_count * config.buffer_size);
				audio->read_data = dma_alloc_coherent(NULL,
				config.buffer_size * config.buffer_count,
				&audio->read_phys, GFP_KERNEL);
				if (!audio->read_data) {
					pr_err(
					"audqcelp_ioctl: no mem for pcm buf\n"
					);
					rc = -ENOMEM;
				} else {
					uint8_t index;
					uint32_t offset = 0;

					audio->pcm_feedback = 1;
					audio->buf_refresh = 0;
					audio->pcm_buf_count =
						config.buffer_count;
					audio->read_next = 0;
					audio->fill_next = 0;

					for (index = 0;
					index < config.buffer_count; index++) {
						audio->in[index].data =
						audio->read_data + offset;
						audio->in[index].addr =
						audio->read_phys + offset;
						audio->in[index].size =
						config.buffer_size;
						audio->in[index].used = 0;
						offset += config.buffer_size;
					}
					rc = 0;
				}
			} else {
				rc = 0;
			}
			break;
		}
	case AUDIO_PAUSE:
		dprintk("%s: AUDIO_PAUSE %ld\n", __func__, arg);
		rc = audpp_pause(audio->dec_id, (int) arg);
		break;
	default:
		rc = -EINVAL;
	}
	mutex_unlock(&audio->lock);
	return rc;
}

/* Only useful in tunnel-mode */
static int audqcelp_fsync(struct file *file, struct dentry *dentry,
			int datasync)
{
	struct audio *audio = file->private_data;
	int rc = 0;

	dprintk("%s()\n", __func__);

	if (!audio->running || audio->pcm_feedback) {
		rc = -EINVAL;
		goto done_nolock;
	}

	mutex_lock(&audio->write_lock);

	rc = wait_event_interruptible(audio->write_wait,
		(!audio->out[0].used &&
		!audio->out[1].used &&
		audio->out_needed) || audio->wflush);

	if (rc < 0)
		goto done;
	else if (audio->wflush) {
		rc = -EBUSY;
		goto done;
	}

	/* pcm dmamiss message is sent continously
	 * when decoder is starved so no race
	 * condition concern
	 */
	audio->teos = 0;

	rc = wait_event_interruptible(audio->write_wait,
		audio->teos || audio->wflush);

	if (audio->wflush)
		rc = -EBUSY;

done:
	mutex_unlock(&audio->write_lock);
done_nolock:
	return rc;
}

static ssize_t audqcelp_read(struct file *file, char __user *buf, size_t count,
			loff_t *pos)
{
	struct audio *audio = file->private_data;
	const char __user *start = buf;
	int rc = 0;

	if (!audio->pcm_feedback)
		return 0; /* PCM feedback is not enabled. Nothing to read */

	mutex_lock(&audio->read_lock);
	dprintk("audqcelp_read() %d \n", count);
	while (count > 0) {
		rc = wait_event_interruptible(audio->read_wait,
				(audio->in[audio->read_next].used > 0) ||
				(audio->stopped) || (audio->rflush) ||
				(audio->in[audio->read_next].eos));
		if (rc < 0)
			break;

		if (audio->stopped || audio->rflush) {
			rc = -EBUSY;
			break;
		}

		if (count < audio->in[audio->read_next].used) {
			/* Read must happen in frame boundary. Since driver does
			not know frame size, read count must be greater or equal
			to size of PCM samples */
			dprintk("audqcelp_read:read stop - partial frame\n");
			break;
		} else {
			dprintk("audqcelp_read: read from in[%d]\n",
				audio->read_next);
			if (audio->in[audio->read_next].eos) {
				dprintk("%s: EOS set\n", __func__);
				if (buf == start) {
					audio->in[audio->read_next].eos = 0;
					if ((++audio->read_next) ==
						audio->pcm_buf_count)
						audio->read_next = 0;
				}
				/* else client buffer already has data
				 * return the buffer first so next read
				 * returns 0 byte
				 */
				break;
			}

			if (copy_to_user(buf,
				audio->in[audio->read_next].data,
				audio->in[audio->read_next].used)) {
				pr_err("audqcelp_read: invalid addr %x \n",
					(unsigned int)buf);
				rc = -EFAULT;
				break;
			}
			count -= audio->in[audio->read_next].used;
			buf += audio->in[audio->read_next].used;
			audio->in[audio->read_next].used = 0;
			if ((++audio->read_next) == audio->pcm_buf_count)
				audio->read_next = 0;
			if (audio->in[audio->read_next].used == 0)
				break;  /* No data ready at this moment
					 * Exit while loop to prevent
					 * output thread sleep too long
					 */
		}
	}

	/* don't feed output buffer to HW decoder during flushing
	 * buffer refresh command will be sent once flush completes
	 * send buf refresh command here can confuse HW decoder
	 */
	if (audio->buf_refresh && !audio->rflush) {
		audio->buf_refresh = 0;
		dprintk("audqcelp_read: kick start pcm feedback again\n");
		audqcelp_buffer_refresh(audio);
	}

	mutex_unlock(&audio->read_lock);

	if (buf > start)
		rc = buf - start;

	dprintk("audqcelp_read: read %d bytes\n", rc);
	return rc;
}

static int audqcelp_handle_eos(struct audio *audio)
{
	int rc = 0;

	mutex_lock(&audio->write_lock);

	rc = wait_event_interruptible(audio->write_wait,
		(audio->out_needed &&
		audio->out[0].used == 0 &&
		audio->out[1].used == 0)
		|| (audio->stopped)
		|| (audio->wflush));

	if (rc < 0)
		goto done;
	if (audio->stopped || audio->wflush) {
		rc = -EBUSY;
		goto done;
	}
	audplay_signal_eos(audio);
done:
	mutex_unlock(&audio->write_lock);
	return rc;
}

static ssize_t audqcelp_write(struct file *file, const char __user *buf,
			   size_t count, loff_t *pos)
{
	struct audio *audio = file->private_data;
	const char __user *start = buf;
	struct buffer *frame;
	size_t xfer;
	int rc = 0;

	dprintk("%s: cnt=%d\n", __func__, count);

	if (count & 1)
		return -EINVAL;

	if (!count) { /* client signal EOS */
		return audqcelp_handle_eos(audio);
	}

	mutex_lock(&audio->write_lock);
	while (count > 0) {
		frame = audio->out + audio->out_head;
		rc = wait_event_interruptible(audio->write_wait,
					      (frame->used == 0)
						|| (audio->stopped)
						|| (audio->wflush));
		dprintk("audqcelp_write() buffer available\n");
		if (rc < 0)
			break;
		if (audio->stopped || audio->wflush) {
			rc = -EBUSY;
			break;
		}
		xfer = (count > frame->size) ? frame->size : count;
		if (copy_from_user(frame->data, buf, xfer)) {
			rc = -EFAULT;
			break;
		}

		frame->used = xfer;
		audio->out_head ^= 1;
		count -= xfer;
		buf += xfer;

		audqcelp_send_data(audio, 0);

	}
	mutex_unlock(&audio->write_lock);
	if (buf > start)
		return buf - start;
	return rc;
}

static int audqcelp_release(struct inode *inode, struct file *file)
{
	struct audio *audio = file->private_data;

	dprintk("audqcelp_release()\n");

	mutex_lock(&audio->lock);
	audqcelp_disable(audio);
	audqcelp_flush(audio);
	audqcelp_flush_pcm_buf(audio);
	msm_adsp_put(audio->audplay);
	audio->audplay = NULL;
#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&audio->suspend_ctl.node);
#endif
	audio->opened = 0;
	audio->event_abort = 1;
	wake_up(&audio->event_wait);
	audqcelp_reset_event_queue(audio);
	if (audio->data)
		dma_free_coherent(NULL, DMASZ, audio->data, audio->phys);
	audio->data = NULL;
	if (audio->read_data) {
		dma_free_coherent(NULL,
				 audio->in[0].size * audio->pcm_buf_count,
				 audio->read_data, audio->read_phys);
		audio->read_data = NULL;
	}
	audio->pcm_feedback = 0;
	audio->rflush = 0;
	audio->wflush = 0;
	mutex_unlock(&audio->lock);
	return 0;
}

static void audqcelp_post_event(struct audio *audio, int type,
		union msm_audio_event_payload payload)
{
	struct audqcelp_event *e_node = NULL;
	unsigned long flags;

	e_node = kmalloc(sizeof(struct audqcelp_event), GFP_KERNEL);

	if (!e_node) {
		pr_err("%s: No mem to post event %d\n", __func__, type);
		return;
	}

	e_node->event_type = type;
	e_node->payload = payload;

	spin_lock_irqsave(&audio->event_queue_lock, flags);
	list_add_tail(&e_node->list, &audio->event_queue);
	spin_unlock_irqrestore(&audio->event_queue_lock, flags);
	wake_up(&audio->event_wait);
}

#ifdef CONFIG_HAS_EARLYSUSPEND
static void audqcelp_suspend(struct early_suspend *h)
{
	struct audqcelp_suspend_ctl *ctl =
		container_of(h, struct audqcelp_suspend_ctl, node);
	union msm_audio_event_payload payload;

	dprintk("%s()\n", __func__);
	audqcelp_post_event(ctl->audio, AUDIO_EVENT_SUSPEND, payload);
}

static void audqcelp_resume(struct early_suspend *h)
{
	struct audqcelp_suspend_ctl *ctl =
		container_of(h, struct audqcelp_suspend_ctl, node);
	union msm_audio_event_payload payload;

	dprintk("%s()\n", __func__);
	audqcelp_post_event(ctl->audio, AUDIO_EVENT_RESUME, payload);
}
#endif

static int audqcelp_open(struct inode *inode, struct file *file)
{
	struct audio *audio = &the_qcelp_audio;
	int rc;

	mutex_lock(&audio->lock);

	if (audio->opened) {
		pr_err("audio: busy\n");
		rc = -EBUSY;
		goto done;
	}

	audio->data = dma_alloc_coherent(NULL, DMASZ,
					 &audio->phys, GFP_KERNEL);
	if (!audio->data) {
		pr_err("audio: could not allocate DMA buffers\n");
		rc = -ENOMEM;
		goto done;
	}

	rc = audmgr_open(&audio->audmgr);
	if (rc)
		goto err;

	rc = msm_adsp_get("AUDPLAY0TASK", &audio->audplay,
		&audplay_adsp_ops_qcelp, audio);
	if (rc) {
		pr_err("audio: failed to get audplay0 dsp module\n");
		audmgr_close(&audio->audmgr);
		goto err;
	}

	audio->dec_id = 0;

	audio->out[0].data = audio->data + 0;
	audio->out[0].addr = audio->phys + 0;
	audio->out[0].size = BUFSZ;

	audio->out[1].data = audio->data + BUFSZ;
	audio->out[1].addr = audio->phys + BUFSZ;
	audio->out[1].size = BUFSZ;

	audio->volume = 0x2000;	/* Q13 1.0 */

	audqcelp_flush(audio);

	file->private_data = audio;
	audio->opened = 1;
	audio->event_abort = 0;
#ifdef CONFIG_HAS_EARLYSUSPEND
	audio->suspend_ctl.node.level = EARLY_SUSPEND_LEVEL_DISABLE_FB;
	audio->suspend_ctl.node.resume = audqcelp_resume;
	audio->suspend_ctl.node.suspend = audqcelp_suspend;
	audio->suspend_ctl.audio = audio;
	register_early_suspend(&audio->suspend_ctl.node);
#endif
	rc = 0;
done:
	mutex_unlock(&audio->lock);
	return rc;
err:
	dma_free_coherent(NULL, DMASZ, audio->data, audio->phys);
	mutex_unlock(&audio->lock);
	return rc;
}

static struct file_operations audio_qcelp_fops = {
	.owner = THIS_MODULE,
	.open = audqcelp_open,
	.release = audqcelp_release,
	.read = audqcelp_read,
	.write = audqcelp_write,
	.unlocked_ioctl = audqcelp_ioctl,
	.fsync = audqcelp_fsync,
};

struct miscdevice audio_qcelp_misc = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "msm_qcelp",
	.fops = &audio_qcelp_fops,
};

#ifdef CONFIG_DEBUG_FS
static ssize_t audqcelp_debug_open(struct inode *inode, struct file *file)
{
	file->private_data = inode->i_private;
	return 0;
}

static ssize_t audqcelp_debug_read(struct file *file, char __user *buf,
					size_t count, loff_t *ppos)
{
	const int debug_bufmax = 1024;
	static char buffer[1024];
	int n = 0, i;
	struct audio *audio = file->private_data;

	mutex_lock(&audio->lock);
	n = scnprintf(buffer, debug_bufmax, "opened %d\n", audio->opened);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"enabled %d\n", audio->enabled);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"stopped %d\n", audio->stopped);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"pcm_feedback %d\n", audio->pcm_feedback);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out_buf_sz %d\n", audio->out[0].size);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"pcm_buf_count %d \n", audio->pcm_buf_count);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"pcm_buf_sz %d \n", audio->in[0].size);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"volume %x \n", audio->volume);
	mutex_unlock(&audio->lock);
	/* Following variables are only useful for debugging when
	 * when playback halts unexpectedly. Thus, no mutual exclusion
	 * enforced
	 */
	n += scnprintf(buffer + n, debug_bufmax - n,
			"wflush %d\n", audio->wflush);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"rflush %d\n", audio->rflush);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"running %d \n", audio->running);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out_needed %d \n", audio->out_needed);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out_head %d \n", audio->out_head);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out_tail %d \n", audio->out_tail);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out[0].used %d \n", audio->out[0].used);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"out[1].used %d \n", audio->out[1].used);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"buffer_refresh %d \n", audio->buf_refresh);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"read_next %d \n", audio->read_next);
	n += scnprintf(buffer + n, debug_bufmax - n,
			"fill_next %d \n", audio->fill_next);
	for (i = 0; i < audio->pcm_buf_count; i++)
		n += scnprintf(buffer + n, debug_bufmax - n,
				"in[%d].size %d \n", i, audio->in[i].used);
	printk(KERN_INFO "size_debug_buffer %d \n", n);
	n++;
	buffer[n] = 0;
	return simple_read_from_buffer(buf, count, ppos, buffer, n);
}

static struct file_operations audqcelp_debug_fops = {
	.read = audqcelp_debug_read,
	.open = audqcelp_debug_open,
};
#endif

static int __init audqcelp_init(void)
{
#ifdef CONFIG_DEBUG_FS
	struct dentry *dentry;
#endif

	mutex_init(&the_qcelp_audio.lock);
	mutex_init(&the_qcelp_audio.write_lock);
	mutex_init(&the_qcelp_audio.read_lock);
	mutex_init(&the_qcelp_audio.get_event_lock);
	spin_lock_init(&the_qcelp_audio.dsp_lock);
	init_waitqueue_head(&the_qcelp_audio.write_wait);
	init_waitqueue_head(&the_qcelp_audio.read_wait);
	the_qcelp_audio.read_data = NULL;

#ifdef CONFIG_DEBUG_FS
	dentry = debugfs_create_file("msm_qcelp", S_IFREG | S_IRUGO, NULL,
			(void *) &the_qcelp_audio, &audqcelp_debug_fops);

	if (IS_ERR(dentry))
		dprintk("QCELP:%s:debugfs_create_file failed\n", __func__);
#endif
	INIT_LIST_HEAD(&the_qcelp_audio.event_queue);
	init_waitqueue_head(&the_qcelp_audio.event_wait);
	spin_lock_init(&the_qcelp_audio.event_queue_lock);
	return misc_register(&audio_qcelp_misc);
}

static void __exit audqcelp_exit(void)
{
	misc_deregister(&audio_qcelp_misc);
}

module_init(audqcelp_init);
module_exit(audqcelp_exit);

MODULE_DESCRIPTION("MSM QCELP 13K driver");
MODULE_LICENSE("GPL v2");
