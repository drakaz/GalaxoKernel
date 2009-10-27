/* arch/arm/mach-msm/io.c
 *
 * MSM7K, QSD io support
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2008-2009 QUALCOMM USA, INC.
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
#include <linux/init.h>
#include <linux/io.h>
#include <linux/module.h>

#include <mach/hardware.h>
#include <asm/page.h>
#include <mach/msm_iomap.h>
#include <asm/mach/map.h>

#include <mach/board.h>

#define MSM_DEVICE(name) { \
		.virtual = (unsigned long) MSM_##name##_BASE, \
		.pfn = __phys_to_pfn(MSM_##name##_PHYS), \
		.length = MSM_##name##_SIZE, \
		.type = MT_DEVICE_NONSHARED, \
	 }

/* msm_shared_ram_phys default value of 0x00100000 is the most common value
 * and should work as-is for any target without stacked memory.
 */
int msm_shared_ram_phys = 0x00100000;

static struct map_desc msm_io_desc[] __initdata = {
	MSM_DEVICE(VIC),
	MSM_DEVICE(CSR),
	MSM_DEVICE(GPT),
	MSM_DEVICE(DMOV),
	MSM_DEVICE(GPIO1),
	MSM_DEVICE(GPIO2),
	MSM_DEVICE(CLK_CTL),
	MSM_DEVICE(AD5),
	MSM_DEVICE(MDC),
#ifdef CONFIG_MSM_DEBUG_UART
	MSM_DEVICE(DEBUG_UART),
#endif
#ifdef CONFIG_CACHE_L2X0
	{
		.virtual =  (unsigned long) MSM_L2CC_BASE,
		.pfn =      __phys_to_pfn(MSM_L2CC_PHYS),
		.length =   MSM_L2CC_SIZE,
		.type =     MT_DEVICE,
	},
#endif
	{
		.virtual =  (unsigned long) MSM_SHARED_RAM_BASE,
		.length =   MSM_SHARED_RAM_SIZE,
		.type =     MT_DEVICE,
	},
};

static struct map_desc qsd8x50_io_desc[] __initdata = {
	MSM_DEVICE(VIC),
	MSM_DEVICE(CSR),
	MSM_DEVICE(GPT),
	MSM_DEVICE(DMOV),
	MSM_DEVICE(GPIO1),
	MSM_DEVICE(GPIO2),
	MSM_DEVICE(CLK_CTL),
	MSM_DEVICE(SIRC),
	MSM_DEVICE(SCPLL),
	MSM_DEVICE(AD5),
	MSM_DEVICE(MDC),
#ifdef CONFIG_MSM_DEBUG_UART
	MSM_DEVICE(DEBUG_UART),
#endif
	{
		.virtual =  (unsigned long) MSM_SHARED_RAM_BASE,
		.length =   MSM_SHARED_RAM_SIZE,
		.type =     MT_DEVICE,
	},
};

static struct map_desc comet_io_desc[] __initdata = {
	MSM_DEVICE(VIC),
	MSM_DEVICE(CSR),
	MSM_DEVICE(GPT),
	MSM_DEVICE(DMOV),
	MSM_DEVICE(GPIO1),
	MSM_DEVICE(GPIO2),
	MSM_DEVICE(CLK_CTL),
	MSM_DEVICE(SIRC),
	MSM_DEVICE(SCPLL),
	MSM_DEVICE(AD5),
	MSM_DEVICE(MDC),
#ifdef CONFIG_MSM_DEBUG_UART
	MSM_DEVICE(DEBUG_UART),
#endif
	{
		.virtual =  (unsigned long) MSM_SHARED_RAM_BASE,
		.length =   MSM_SHARED_RAM_SIZE,
		.type =     MT_DEVICE,
	},
};

void __init msm_map_common_io(void)
{
	int i;

	/* Make sure the peripheral register window is closed, since
	 * we will use PTE flags (TEX[1]=1,B=0,C=1) to determine which
	 * pages are peripheral interface or not.
	 */
	asm("mcr p15, 0, %0, c15, c2, 4" : : "r" (0));

	BUG_ON(!ARRAY_SIZE(msm_io_desc));
	for (i = 0; i < ARRAY_SIZE(msm_io_desc); i++)
		if (msm_io_desc[i].virtual ==
				(unsigned long)MSM_SHARED_RAM_BASE)
			msm_io_desc[i].pfn =
				__phys_to_pfn(msm_shared_ram_phys);

	iotable_init(msm_io_desc, ARRAY_SIZE(msm_io_desc));
}

void __init msm_map_qsd8x50_io(void)
{
	int i;

	BUG_ON(!ARRAY_SIZE(qsd8x50_io_desc));
	for (i = 0; i < ARRAY_SIZE(qsd8x50_io_desc); i++)
		if (qsd8x50_io_desc[i].virtual ==
				(unsigned long)MSM_SHARED_RAM_BASE)
			qsd8x50_io_desc[i].pfn =
				__phys_to_pfn(msm_shared_ram_phys);

	iotable_init(qsd8x50_io_desc, ARRAY_SIZE(qsd8x50_io_desc));
}

void __init msm_map_comet_io(void)
{
	int i;

	BUG_ON(!ARRAY_SIZE(comet_io_desc));
	for (i = 0; i < ARRAY_SIZE(comet_io_desc); i++)
		if (comet_io_desc[i].virtual ==
				(unsigned long)MSM_SHARED_RAM_BASE)
			comet_io_desc[i].pfn =
				__phys_to_pfn(msm_shared_ram_phys);

	iotable_init(comet_io_desc, ARRAY_SIZE(comet_io_desc));
}

void __iomem *
__msm_ioremap(unsigned long phys_addr, size_t size, unsigned int mtype)
{
	if (mtype == MT_DEVICE) {
		/* The peripherals in the 88000000 - D0000000 range
		 * are only accessable by type MT_DEVICE_NONSHARED.
		 * Adjust mtype as necessary to make this "just work."
		 */
		if ((phys_addr >= 0x88000000) && (phys_addr < 0xD0000000))
			mtype = MT_DEVICE_NONSHARED;
	}

	return __arm_ioremap(phys_addr, size, mtype);
}

EXPORT_SYMBOL(__msm_ioremap);
