/*
 *  cx18 ioctl system call
 *
 *  Derived from ivtv-ioctl.c
 *
 *  Copyright (C) 2007  Hans Verkuil <hverkuil@xs4all.nl>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA
 */

#include "cx18-driver.h"
#include "cx18-version.h"
#include "cx18-mailbox.h"
#include "cx18-i2c.h"
#include "cx18-queue.h"
#include "cx18-fileops.h"
#include "cx18-vbi.h"
#include "cx18-audio.h"
#include "cx18-video.h"
#include "cx18-streams.h"
#include "cx18-ioctl.h"
#include "cx18-gpio.h"
#include "cx18-controls.h"
#include "cx18-cards.h"
#include "cx18-av-core.h"
#include <media/tveeprom.h>
#include <media/v4l2-chip-ident.h>
#include <linux/i2c-id.h>

u16 cx18_service2vbi(int type)
{
	switch (type) {
	case V4L2_SLICED_TELETEXT_B:
		return CX18_SLICED_TYPE_TELETEXT_B;
	case V4L2_SLICED_CAPTION_525:
		return CX18_SLICED_TYPE_CAPTION_525;
	case V4L2_SLICED_WSS_625:
		return CX18_SLICED_TYPE_WSS_625;
	case V4L2_SLICED_VPS:
		return CX18_SLICED_TYPE_VPS;
	default:
		return 0;
	}
}

static int valid_service_line(int field, int line, int is_pal)
{
	return (is_pal && line >= 6 && (line != 23 || field == 0)) ||
	       (!is_pal && line >= 10 && line < 22);
}

static u16 select_service_from_set(int field, int line, u16 set, int is_pal)
{
	u16 valid_set = (is_pal ? V4L2_SLICED_VBI_625 : V4L2_SLICED_VBI_525);
	int i;

	set = set & valid_set;
	if (set == 0 || !valid_service_line(field, line, is_pal))
		return 0;
	if (!is_pal) {
		if (line == 21 && (set & V4L2_SLICED_CAPTION_525))
			return V4L2_SLICED_CAPTION_525;
	} else {
		if (line == 16 && field == 0 && (set & V4L2_SLICED_VPS))
			return V4L2_SLICED_VPS;
		if (line == 23 && field == 0 && (set & V4L2_SLICED_WSS_625))
			return V4L2_SLICED_WSS_625;
		if (line == 23)
			return 0;
	}
	for (i = 0; i < 32; i++) {
		if ((1 << i) & set)
			return 1 << i;
	}
	return 0;
}

void cx18_expand_service_set(struct v4l2_sliced_vbi_format *fmt, int is_pal)
{
	u16 set = fmt->service_set;
	int f, l;

	fmt->service_set = 0;
	for (f = 0; f < 2; f++) {
		for (l = 0; l < 24; l++)
			fmt->service_lines[f][l] = select_service_from_set(f, l, set, is_pal);
	}
}


u16 cx18_get_service_set(struct v4l2_sliced_vbi_format *fmt)
{
	int f, l;
	u16 set = 0;

	for (f = 0; f < 2; f++) {
		for (l = 0; l < 24; l++)
			set |= fmt->service_lines[f][l];
	}
	return set;
}

static int cx18_g_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	struct v4l2_pix_format *pixfmt = &fmt->fmt.pix;

	pixfmt->width = cx->params.width;
	pixfmt->height = cx->params.height;
	pixfmt->colorspace = V4L2_COLORSPACE_SMPTE170M;
	pixfmt->field = V4L2_FIELD_INTERLACED;
	pixfmt->priv = 0;
	if (id->type == CX18_ENC_STREAM_TYPE_YUV) {
		pixfmt->pixelformat = V4L2_PIX_FMT_HM12;
		/* YUV size is (Y=(h*w) + UV=(h*(w/2))) */
		pixfmt->sizeimage =
			pixfmt->height * pixfmt->width +
			pixfmt->height * (pixfmt->width / 2);
		pixfmt->bytesperline = 720;
	} else {
		pixfmt->pixelformat = V4L2_PIX_FMT_MPEG;
		pixfmt->sizeimage = 128 * 1024;
		pixfmt->bytesperline = 0;
	}
	return 0;
}

static int cx18_g_fmt_vbi_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;
	struct v4l2_vbi_format *vbifmt = &fmt->fmt.vbi;

	vbifmt->sampling_rate = 27000000;
	vbifmt->offset = 248;
	vbifmt->samples_per_line = cx->vbi.raw_decoder_line_size - 4;
	vbifmt->sample_format = V4L2_PIX_FMT_GREY;
	vbifmt->start[0] = cx->vbi.start[0];
	vbifmt->start[1] = cx->vbi.start[1];
	vbifmt->count[0] = vbifmt->count[1] = cx->vbi.count;
	vbifmt->flags = 0;
	vbifmt->reserved[0] = 0;
	vbifmt->reserved[1] = 0;
	return 0;
}

static int cx18_g_fmt_sliced_vbi_cap(struct file *file, void *fh,
					struct v4l2_format *fmt)
{
	return -EINVAL;
}

static int cx18_try_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;

	int w = fmt->fmt.pix.width;
	int h = fmt->fmt.pix.height;

	w = min(w, 720);
	w = max(w, 1);
	h = min(h, cx->is_50hz ? 576 : 480);
	h = max(h, 2);
	cx18_g_fmt_vid_cap(file, fh, fmt);
	fmt->fmt.pix.width = w;
	fmt->fmt.pix.height = h;
	return 0;
}

static int cx18_try_fmt_vbi_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	return cx18_g_fmt_vbi_cap(file, fh, fmt);
}

static int cx18_try_fmt_sliced_vbi_cap(struct file *file, void *fh,
					struct v4l2_format *fmt)
{
	return -EINVAL;
}

static int cx18_s_fmt_vid_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;
	int w = fmt->fmt.pix.width;
	int h = fmt->fmt.pix.height;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	ret = cx18_try_fmt_vid_cap(file, fh, fmt);
	if (ret)
		return ret;

	if (cx->params.width == w && cx->params.height == h)
		return 0;

	if (atomic_read(&cx->ana_capturing) > 0)
		return -EBUSY;

	cx->params.width = w;
	cx->params.height = h;
	cx18_av_cmd(cx, VIDIOC_S_FMT, fmt);
	return cx18_g_fmt_vid_cap(file, fh, fmt);
}

static int cx18_s_fmt_vbi_cap(struct file *file, void *fh,
				struct v4l2_format *fmt)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if (id->type == CX18_ENC_STREAM_TYPE_VBI &&
			cx->vbi.sliced_in->service_set &&
			atomic_read(&cx->ana_capturing) > 0)
		return -EBUSY;

	cx->vbi.sliced_in->service_set = 0;
	cx18_av_cmd(cx, VIDIOC_S_FMT, &cx->vbi.in);
	return cx18_g_fmt_vbi_cap(file, fh, fmt);
}

static int cx18_s_fmt_sliced_vbi_cap(struct file *file, void *fh,
					struct v4l2_format *fmt)
{
	return -EINVAL;
}

static int cx18_g_chip_ident(struct file *file, void *fh,
				struct v4l2_chip_ident *chip)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	chip->ident = V4L2_IDENT_NONE;
	chip->revision = 0;
	if (chip->match_type == V4L2_CHIP_MATCH_HOST) {
		if (v4l2_chip_match_host(chip->match_type, chip->match_chip))
			chip->ident = V4L2_IDENT_CX23418;
		return 0;
	}
	if (chip->match_type == V4L2_CHIP_MATCH_I2C_DRIVER)
		return cx18_i2c_id(cx, chip->match_chip, VIDIOC_G_CHIP_IDENT,
					chip);
	if (chip->match_type == V4L2_CHIP_MATCH_I2C_ADDR)
		return cx18_call_i2c_client(cx, chip->match_chip,
						VIDIOC_G_CHIP_IDENT, chip);
	return -EINVAL;
}

#ifdef CONFIG_VIDEO_ADV_DEBUG
static int cx18_cxc(struct cx18 *cx, unsigned int cmd, void *arg)
{
	struct v4l2_register *regs = arg;
	unsigned long flags;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (regs->reg >= CX18_MEM_OFFSET + CX18_MEM_SIZE)
		return -EINVAL;

	spin_lock_irqsave(&cx18_cards_lock, flags);
	if (cmd == VIDIOC_DBG_G_REGISTER)
		regs->val = read_enc(regs->reg);
	else
		write_enc(regs->val, regs->reg);
	spin_unlock_irqrestore(&cx18_cards_lock, flags);
	return 0;
}

static int cx18_g_register(struct file *file, void *fh,
				struct v4l2_register *reg)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (v4l2_chip_match_host(reg->match_type, reg->match_chip))
		return cx18_cxc(cx, VIDIOC_DBG_G_REGISTER, reg);
	if (reg->match_type == V4L2_CHIP_MATCH_I2C_DRIVER)
		return cx18_i2c_id(cx, reg->match_chip, VIDIOC_DBG_G_REGISTER,
					reg);
	return cx18_call_i2c_client(cx, reg->match_chip, VIDIOC_DBG_G_REGISTER,
					reg);
}

static int cx18_s_register(struct file *file, void *fh,
				struct v4l2_register *reg)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (v4l2_chip_match_host(reg->match_type, reg->match_chip))
		return cx18_cxc(cx, VIDIOC_DBG_S_REGISTER, reg);
	if (reg->match_type == V4L2_CHIP_MATCH_I2C_DRIVER)
		return cx18_i2c_id(cx, reg->match_chip, VIDIOC_DBG_S_REGISTER,
					reg);
	return cx18_call_i2c_client(cx, reg->match_chip, VIDIOC_DBG_S_REGISTER,
					reg);
}
#endif

static int cx18_g_priority(struct file *file, void *fh, enum v4l2_priority *p)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	*p = v4l2_prio_max(&cx->prio);
	return 0;
}

static int cx18_s_priority(struct file *file, void *fh, enum v4l2_priority prio)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;

	return v4l2_prio_change(&cx->prio, &id->prio, prio);
}

static int cx18_querycap(struct file *file, void *fh,
				struct v4l2_capability *vcap)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	strlcpy(vcap->driver, CX18_DRIVER_NAME, sizeof(vcap->driver));
	strlcpy(vcap->card, cx->card_name, sizeof(vcap->card));
	strlcpy(vcap->bus_info, pci_name(cx->dev), sizeof(vcap->bus_info));
	vcap->version = CX18_DRIVER_VERSION; 	    /* version */
	vcap->capabilities = cx->v4l2_cap; 	    /* capabilities */
	return 0;
}

static int cx18_enumaudio(struct file *file, void *fh, struct v4l2_audio *vin)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	return cx18_get_audio_input(cx, vin->index, vin);
}

static int cx18_g_audio(struct file *file, void *fh, struct v4l2_audio *vin)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	vin->index = cx->audio_input;
	return cx18_get_audio_input(cx, vin->index, vin);
}

static int cx18_s_audio(struct file *file, void *fh, struct v4l2_audio *vout)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (vout->index >= cx->nof_audio_inputs)
		return -EINVAL;
	cx->audio_input = vout->index;
	cx18_audio_set_io(cx);
	return 0;
}

static int cx18_enum_input(struct file *file, void *fh, struct v4l2_input *vin)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	/* set it to defaults from our table */
	return cx18_get_input(cx, vin->index, vin);
}

static int cx18_cropcap(struct file *file, void *fh,
			struct v4l2_cropcap *cropcap)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (cropcap->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	cropcap->bounds.top = cropcap->bounds.left = 0;
	cropcap->bounds.width = 720;
	cropcap->bounds.height = cx->is_50hz ? 576 : 480;
	cropcap->pixelaspect.numerator = cx->is_50hz ? 59 : 10;
	cropcap->pixelaspect.denominator = cx->is_50hz ? 54 : 11;
	cropcap->defrect = cropcap->bounds;
	return 0;
}

static int cx18_s_crop(struct file *file, void *fh, struct v4l2_crop *crop)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	return cx18_av_cmd(cx, VIDIOC_S_CROP, crop);
}

static int cx18_g_crop(struct file *file, void *fh, struct v4l2_crop *crop)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (crop->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	return cx18_av_cmd(cx, VIDIOC_G_CROP, crop);
}

static int cx18_enum_fmt_vid_cap(struct file *file, void *fh,
					struct v4l2_fmtdesc *fmt)
{
	static struct v4l2_fmtdesc formats[] = {
		{ 0, V4L2_BUF_TYPE_VIDEO_CAPTURE, 0,
		  "HM12 (YUV 4:1:1)", V4L2_PIX_FMT_HM12, { 0, 0, 0, 0 }
		},
		{ 1, V4L2_BUF_TYPE_VIDEO_CAPTURE, V4L2_FMT_FLAG_COMPRESSED,
		  "MPEG", V4L2_PIX_FMT_MPEG, { 0, 0, 0, 0 }
		}
	};

	if (fmt->index > 1)
		return -EINVAL;
	*fmt = formats[fmt->index];
	return 0;
}

static int cx18_g_input(struct file *file, void *fh, unsigned int *i)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	*i = cx->active_input;
	return 0;
}

int cx18_s_input(struct file *file, void *fh, unsigned int inp)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if (inp < 0 || inp >= cx->nof_inputs)
		return -EINVAL;

	if (inp == cx->active_input) {
		CX18_DEBUG_INFO("Input unchanged\n");
		return 0;
	}

	CX18_DEBUG_INFO("Changing input from %d to %d\n",
			cx->active_input, inp);

	cx->active_input = inp;
	/* Set the audio input to whatever is appropriate for the input type. */
	cx->audio_input = cx->card->video_inputs[inp].audio_index;

	/* prevent others from messing with the streams until
	   we're finished changing inputs. */
	cx18_mute(cx);
	cx18_video_set_io(cx);
	cx18_audio_set_io(cx);
	cx18_unmute(cx);
	return 0;
}

static int cx18_g_frequency(struct file *file, void *fh,
				struct v4l2_frequency *vf)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (vf->tuner != 0)
		return -EINVAL;

	cx18_call_i2c_clients(cx, VIDIOC_G_FREQUENCY, vf);
	return 0;
}

int cx18_s_frequency(struct file *file, void *fh, struct v4l2_frequency *vf)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if (vf->tuner != 0)
		return -EINVAL;

	cx18_mute(cx);
	CX18_DEBUG_INFO("v4l2 ioctl: set frequency %d\n", vf->frequency);
	cx18_call_i2c_clients(cx, VIDIOC_S_FREQUENCY, vf);
	cx18_unmute(cx);
	return 0;
}

static int cx18_g_std(struct file *file, void *fh, v4l2_std_id *std)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	*std = cx->std;
	return 0;
}

int cx18_s_std(struct file *file, void *fh, v4l2_std_id *std)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if ((*std & V4L2_STD_ALL) == 0)
		return -EINVAL;

	if (*std == cx->std)
		return 0;

	if (test_bit(CX18_F_I_RADIO_USER, &cx->i_flags) ||
	    atomic_read(&cx->ana_capturing) > 0) {
		/* Switching standard would turn off the radio or mess
		   with already running streams, prevent that by
		   returning EBUSY. */
		return -EBUSY;
	}

	cx->std = *std;
	cx->is_60hz = (*std & V4L2_STD_525_60) ? 1 : 0;
	cx->params.is_50hz = cx->is_50hz = !cx->is_60hz;
	cx->params.width = 720;
	cx->params.height = cx->is_50hz ? 576 : 480;
	cx->vbi.count = cx->is_50hz ? 18 : 12;
	cx->vbi.start[0] = cx->is_50hz ? 6 : 10;
	cx->vbi.start[1] = cx->is_50hz ? 318 : 273;
	cx->vbi.sliced_decoder_line_size = cx->is_60hz ? 272 : 284;
	CX18_DEBUG_INFO("Switching standard to %llx.\n",
			(unsigned long long) cx->std);

	/* Tuner */
	cx18_call_i2c_clients(cx, VIDIOC_S_STD, &cx->std);
	return 0;
}

static int cx18_s_tuner(struct file *file, void *fh, struct v4l2_tuner *vt)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;
	int ret;

	ret = v4l2_prio_check(&cx->prio, &id->prio);
	if (ret)
		return ret;

	if (vt->index != 0)
		return -EINVAL;

	/* Setting tuner can only set audio mode */
	cx18_call_i2c_clients(cx, VIDIOC_S_TUNER, vt);

	return 0;
}

static int cx18_g_tuner(struct file *file, void *fh, struct v4l2_tuner *vt)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	if (vt->index != 0)
		return -EINVAL;

	cx18_call_i2c_clients(cx, VIDIOC_G_TUNER, vt);

	if (test_bit(CX18_F_I_RADIO_USER, &cx->i_flags)) {
		strlcpy(vt->name, "cx18 Radio Tuner", sizeof(vt->name));
		vt->type = V4L2_TUNER_RADIO;
	} else {
		strlcpy(vt->name, "cx18 TV Tuner", sizeof(vt->name));
		vt->type = V4L2_TUNER_ANALOG_TV;
	}

	return 0;
}

static int cx18_g_sliced_vbi_cap(struct file *file, void *fh,
					struct v4l2_sliced_vbi_cap *cap)
{
	return -EINVAL;
}

static int cx18_g_enc_index(struct file *file, void *fh,
				struct v4l2_enc_idx *idx)
{
	return -EINVAL;
}

static int cx18_encoder_cmd(struct file *file, void *fh,
				struct v4l2_encoder_cmd *enc)
{
	struct cx18_open_id *id = fh;
	struct cx18 *cx = id->cx;

	switch (enc->cmd) {
	case V4L2_ENC_CMD_START:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_START\n");
		enc->flags = 0;
		return cx18_start_capture(id);

	case V4L2_ENC_CMD_STOP:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_STOP\n");
		enc->flags &= V4L2_ENC_CMD_STOP_AT_GOP_END;
		cx18_stop_capture(id,
				  enc->flags & V4L2_ENC_CMD_STOP_AT_GOP_END);
		break;

	case V4L2_ENC_CMD_PAUSE:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_PAUSE\n");
		enc->flags = 0;
		if (!atomic_read(&cx->ana_capturing))
			return -EPERM;
		if (test_and_set_bit(CX18_F_I_ENC_PAUSED, &cx->i_flags))
			return 0;
		cx18_mute(cx);
		cx18_vapi(cx, CX18_CPU_CAPTURE_PAUSE, 1, cx18_find_handle(cx));
		break;

	case V4L2_ENC_CMD_RESUME:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_RESUME\n");
		enc->flags = 0;
		if (!atomic_read(&cx->ana_capturing))
			return -EPERM;
		if (!test_and_clear_bit(CX18_F_I_ENC_PAUSED, &cx->i_flags))
			return 0;
		cx18_vapi(cx, CX18_CPU_CAPTURE_RESUME, 1, cx18_find_handle(cx));
		cx18_unmute(cx);
		break;

	default:
		CX18_DEBUG_IOCTL("Unknown cmd %d\n", enc->cmd);
		return -EINVAL;
	}
	return 0;
}

static int cx18_try_encoder_cmd(struct file *file, void *fh,
				struct v4l2_encoder_cmd *enc)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	switch (enc->cmd) {
	case V4L2_ENC_CMD_START:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_START\n");
		enc->flags = 0;
		break;

	case V4L2_ENC_CMD_STOP:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_STOP\n");
		enc->flags &= V4L2_ENC_CMD_STOP_AT_GOP_END;
		break;

	case V4L2_ENC_CMD_PAUSE:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_PAUSE\n");
		enc->flags = 0;
		break;

	case V4L2_ENC_CMD_RESUME:
		CX18_DEBUG_IOCTL("V4L2_ENC_CMD_RESUME\n");
		enc->flags = 0;
		break;

	default:
		CX18_DEBUG_IOCTL("Unknown cmd %d\n", enc->cmd);
		return -EINVAL;
	}
	return 0;
}

static int cx18_log_status(struct file *file, void *fh)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;
	struct v4l2_input vidin;
	struct v4l2_audio audin;
	int i;

	CX18_INFO("=================  START STATUS CARD #%d  =================\n", cx->num);
	if (cx->hw_flags & CX18_HW_TVEEPROM) {
		struct tveeprom tv;

		cx18_read_eeprom(cx, &tv);
	}
	cx18_call_i2c_clients(cx, VIDIOC_LOG_STATUS, NULL);
	cx18_get_input(cx, cx->active_input, &vidin);
	cx18_get_audio_input(cx, cx->audio_input, &audin);
	CX18_INFO("Video Input: %s\n", vidin.name);
	CX18_INFO("Audio Input: %s\n", audin.name);
	mutex_lock(&cx->gpio_lock);
	CX18_INFO("GPIO:  direction 0x%08x, value 0x%08x\n",
		cx->gpio_dir, cx->gpio_val);
	mutex_unlock(&cx->gpio_lock);
	CX18_INFO("Tuner: %s\n",
		test_bit(CX18_F_I_RADIO_USER, &cx->i_flags) ?  "Radio" : "TV");
	cx2341x_log_status(&cx->params, cx->name);
	CX18_INFO("Status flags: 0x%08lx\n", cx->i_flags);
	for (i = 0; i < CX18_MAX_STREAMS; i++) {
		struct cx18_stream *s = &cx->streams[i];

		if (s->v4l2dev == NULL || s->buffers == 0)
			continue;
		CX18_INFO("Stream %s: status 0x%04lx, %d%% of %d KiB (%d buffers) in use\n",
			  s->name, s->s_flags,
			  (s->buffers - s->q_free.buffers) * 100 / s->buffers,
			  (s->buffers * s->buf_size) / 1024, s->buffers);
	}
	CX18_INFO("Read MPEG/VBI: %lld/%lld bytes\n",
			(long long)cx->mpg_data_received,
			(long long)cx->vbi_data_inserted);
	CX18_INFO("==================  END STATUS CARD #%d  ==================\n", cx->num);
	return 0;
}

static int cx18_default(struct file *file, void *fh, int cmd, void *arg)
{
	struct cx18 *cx = ((struct cx18_open_id *)fh)->cx;

	switch (cmd) {
	case VIDIOC_INT_S_AUDIO_ROUTING: {
		struct v4l2_routing *route = arg;

		CX18_DEBUG_IOCTL("VIDIOC_INT_S_AUDIO_ROUTING(%d, %d)\n",
			route->input, route->output);
		cx18_audio_set_route(cx, route);
		break;
	}

	case VIDIOC_INT_RESET: {
		u32 val = *(u32 *)arg;

		if ((val == 0) || (val & 0x01))
			cx18_reset_ir_gpio(&cx->i2c_algo_cb_data[0]);
		break;
	}

	default:
		return -EINVAL;
	}
	return 0;
}

int cx18_v4l2_ioctl(struct inode *inode, struct file *filp, unsigned int cmd,
		    unsigned long arg)
{
	struct video_device *vfd = video_devdata(filp);
	struct cx18_open_id *id = (struct cx18_open_id *)filp->private_data;
	struct cx18 *cx = id->cx;
	int res;

	mutex_lock(&cx->serialize_lock);

	if (cx18_debug & CX18_DBGFLG_IOCTL)
		vfd->debug = V4L2_DEBUG_IOCTL | V4L2_DEBUG_IOCTL_ARG;
	res = video_ioctl2(inode, filp, cmd, arg);
	vfd->debug = 0;
	mutex_unlock(&cx->serialize_lock);
	return res;
}

static const struct v4l2_ioctl_ops cx18_ioctl_ops = {
	.vidioc_querycap                = cx18_querycap,
	.vidioc_g_priority              = cx18_g_priority,
	.vidioc_s_priority              = cx18_s_priority,
	.vidioc_s_audio                 = cx18_s_audio,
	.vidioc_g_audio                 = cx18_g_audio,
	.vidioc_enumaudio               = cx18_enumaudio,
	.vidioc_enum_input              = cx18_enum_input,
	.vidioc_cropcap                 = cx18_cropcap,
	.vidioc_s_crop                  = cx18_s_crop,
	.vidioc_g_crop                  = cx18_g_crop,
	.vidioc_g_input                 = cx18_g_input,
	.vidioc_s_input                 = cx18_s_input,
	.vidioc_g_frequency             = cx18_g_frequency,
	.vidioc_s_frequency             = cx18_s_frequency,
	.vidioc_s_tuner                 = cx18_s_tuner,
	.vidioc_g_tuner                 = cx18_g_tuner,
	.vidioc_g_enc_index             = cx18_g_enc_index,
	.vidioc_g_std                   = cx18_g_std,
	.vidioc_s_std                   = cx18_s_std,
	.vidioc_log_status              = cx18_log_status,
	.vidioc_enum_fmt_vid_cap        = cx18_enum_fmt_vid_cap,
	.vidioc_encoder_cmd             = cx18_encoder_cmd,
	.vidioc_try_encoder_cmd         = cx18_try_encoder_cmd,
	.vidioc_g_fmt_vid_cap           = cx18_g_fmt_vid_cap,
	.vidioc_g_fmt_vbi_cap           = cx18_g_fmt_vbi_cap,
	.vidioc_g_fmt_sliced_vbi_cap    = cx18_g_fmt_sliced_vbi_cap,
	.vidioc_s_fmt_vid_cap           = cx18_s_fmt_vid_cap,
	.vidioc_s_fmt_vbi_cap           = cx18_s_fmt_vbi_cap,
	.vidioc_s_fmt_sliced_vbi_cap    = cx18_s_fmt_sliced_vbi_cap,
	.vidioc_try_fmt_vid_cap         = cx18_try_fmt_vid_cap,
	.vidioc_try_fmt_vbi_cap         = cx18_try_fmt_vbi_cap,
	.vidioc_try_fmt_sliced_vbi_cap  = cx18_try_fmt_sliced_vbi_cap,
	.vidioc_g_sliced_vbi_cap        = cx18_g_sliced_vbi_cap,
	.vidioc_g_chip_ident            = cx18_g_chip_ident,
#ifdef CONFIG_VIDEO_ADV_DEBUG
	.vidioc_g_register              = cx18_g_register,
	.vidioc_s_register              = cx18_s_register,
#endif
	.vidioc_default                 = cx18_default,
	.vidioc_queryctrl               = cx18_queryctrl,
	.vidioc_querymenu               = cx18_querymenu,
	.vidioc_g_ext_ctrls             = cx18_g_ext_ctrls,
	.vidioc_s_ext_ctrls             = cx18_s_ext_ctrls,
	.vidioc_try_ext_ctrls           = cx18_try_ext_ctrls,
};

void cx18_set_funcs(struct video_device *vdev)
{
	vdev->ioctl_ops = &cx18_ioctl_ops;
}
