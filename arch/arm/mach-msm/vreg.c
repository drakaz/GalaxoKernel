/* arch/arm/mach-msm/vreg.c
 *
 * Copyright (C) 2008 Google, Inc.
 * Author: Brian Swetland <swetland@google.com>
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/init.h>
#include <linux/debugfs.h>
#include <mach/vreg.h>

#include "proc_comm.h"

#if defined(CONFIG_MSM_VREG_SWITCH_INVERTED)
#define VREG_SWITCH_ENABLE 0
#define VREG_SWITCH_DISABLE 1
#else
#define VREG_SWITCH_ENABLE 1
#define VREG_SWITCH_DISABLE 0
#endif

struct vreg {
	const char *name;
	unsigned id;
	int status;
};

#define VREG(_name, _id, _status) \
	{ .name = _name, .id = _id, .status = _status }

static struct vreg vregs[] = {
	VREG("msma",	0, 0),
	VREG("msmp",	1, 0),
	VREG("msme1",	2, 0),
	VREG("msmc1",	3, 0),
	VREG("msmc2",	4, 0),
	VREG("gp3",	5, 0),
	VREG("msme2",	6, 0),
	VREG("gp4",	7, 0),
	VREG("gp1",	8, 0),
	VREG("tcxo",	9, 0),
	VREG("pa",	10, 0),
	VREG("rftx",	11, 0),
	VREG("rfrx1",	12, 0),
	VREG("rfrx2",	13, 0),
	VREG("synt",	14, 0),
	VREG("wlan",	15, 0),
	VREG("usb",	16, 0),
	VREG("boost",	17, 0),
	VREG("mmc",	18, 0),
	VREG("ruim",	19, 0),
	VREG("msmc0",	20, 0),
	VREG("gp2",	21, 0),
	VREG("gp5",	22, 0),
	VREG("gp6",	23, 0),
	VREG("rf",	24, 0),
	VREG("rf_vco",	26, 0),
	VREG("mpll",	27, 0),
	VREG("s2",	28, 0),
	VREG("s3",	29, 0),
	VREG("rfubm",	30, 0),
	VREG("ncp",	31, 0),
};

struct vreg *vreg_get(struct device *dev, const char *id)
{
	int n;
	for (n = 0; n < ARRAY_SIZE(vregs); n++) {
		if (!strcmp(vregs[n].name, id))
			return vregs + n;
	}
	return ERR_PTR(-ENOENT);
}
EXPORT_SYMBOL(vreg_get);

void vreg_put(struct vreg *vreg)
{
}

int vreg_enable(struct vreg *vreg)
{
	unsigned id = vreg->id;
	int enable = VREG_SWITCH_ENABLE;

	vreg->status = msm_proc_comm(PCOM_VREG_SWITCH, &id, &enable);
	return vreg->status;
}
EXPORT_SYMBOL(vreg_enable);

int vreg_disable(struct vreg *vreg)
{
	unsigned id = vreg->id;
	int disable = VREG_SWITCH_DISABLE;

	vreg->status = msm_proc_comm(PCOM_VREG_SWITCH, &id, &disable);
	return vreg->status;
}
EXPORT_SYMBOL(vreg_disable);

int vreg_set_level(struct vreg *vreg, unsigned mv)
{
	unsigned id = vreg->id;

	vreg->status = msm_proc_comm(PCOM_VREG_SET_LEVEL, &id, &mv);
	return vreg->status;
}
EXPORT_SYMBOL(vreg_set_level);

#if defined(CONFIG_DEBUG_FS)

static int vreg_debug_set(void *data, u64 val)
{
	struct vreg *vreg = data;
	switch (val) {
	case 0:
		vreg_disable(vreg);
		break;
	case 1:
		vreg_enable(vreg);
		break;
	default:
		vreg_set_level(vreg, val);
		break;
	}
	return 0;
}

static int vreg_debug_get(void *data, u64 *val)
{
	struct vreg *vreg = data;

	if (!vreg->status)
		*val = 0;
	else
		*val = 1;

	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(vreg_fops, vreg_debug_get, vreg_debug_set, "%llu\n");

static int __init vreg_debug_init(void)
{
	struct dentry *dent;
	int n;

	dent = debugfs_create_dir("vreg", 0);
	if (IS_ERR(dent))
		return 0;

	for (n = 0; n < ARRAY_SIZE(vregs); n++)
		(void) debugfs_create_file(vregs[n].name, 0644,
					   dent, vregs + n, &vreg_fops);

	return 0;
}

device_initcall(vreg_debug_init);
#endif
