/* arch/arm/mach-msm/include/mach/msm_iomap.h
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
 *
 * The MSM peripherals are spread all over across 768MB of physical
 * space, which makes just having a simple IO_ADDRESS macro to slide
 * them into the right virtual location rough.  Instead, we will
 * provide a master phys->virt mapping for peripherals here.
 *
 */

#ifndef __ASM_ARCH_MSM_IOMAP_H
#define __ASM_ARCH_MSM_IOMAP_H

#include <asm/sizes.h>

/* Physical base address and size of peripherals.
 * Ordered by the virtual base addresses they will be mapped at.
 *
 * MSM_VIC_BASE must be an value that can be loaded via a "mov"
 * instruction, otherwise entry-macro.S will not compile.
 *
 * If you add or remove entries here, you'll want to edit the
 * msm_io_desc array in arch/arm/mach-msm/io.c to reflect your
 * changes.
 *
 */

#ifdef __ASSEMBLY__
#define IOMEM(x)	x
#else
#define IOMEM(x)	((void __force __iomem *)(x))
#endif

#define MSM_VIC_BASE          IOMEM(0xE0000000)
#if defined(CONFIG_ARCH_QSD)
#define MSM_VIC_PHYS          0xAC000000
#else
#define MSM_VIC_PHYS          0xC0000000
#endif
#define MSM_VIC_SIZE          SZ_4K

#define MSM_CSR_BASE          IOMEM(0xE0001000)
#if defined(CONFIG_ARCH_QSD)
#define MSM_CSR_PHYS          0xAC100000
#else
#define MSM_CSR_PHYS          0xC0100000
#endif
#define MSM_CSR_SIZE          SZ_4K

#define MSM_GPT_PHYS          MSM_CSR_PHYS
#define MSM_GPT_BASE          MSM_CSR_BASE
#define MSM_GPT_SIZE          SZ_4K

#define MSM_DMOV_BASE         IOMEM(0xE0002000)
#define MSM_DMOV_PHYS         0xA9700000
#define MSM_DMOV_SIZE         SZ_4K

#define MSM_GPIO1_BASE        IOMEM(0xE0003000)
#if defined(CONFIG_ARCH_QSD)
#define MSM_GPIO1_PHYS        0xA9000000
#else
#define MSM_GPIO1_PHYS        0xA9200000
#endif
#define MSM_GPIO1_SIZE        SZ_4K

#define MSM_GPIO2_BASE        IOMEM(0xE0004000)
#if defined(CONFIG_ARCH_QSD)
#define MSM_GPIO2_PHYS        0xA9100000
#else
#define MSM_GPIO2_PHYS        0xA9300000
#endif
#define MSM_GPIO2_SIZE        SZ_4K

#define MSM_CLK_CTL_BASE      IOMEM(0xE0005000)
#define MSM_CLK_CTL_PHYS      0xA8600000
#define MSM_CLK_CTL_SIZE      SZ_4K

#define MSM_L2CC_BASE         IOMEM(0xE0006000)
#define MSM_L2CC_PHYS         0xC0400000
#define MSM_L2CC_SIZE         SZ_4K

#define MSM_SIRC_BASE         IOMEM(0xE1006000)
#define MSM_SIRC_PHYS         0xAC200000
#define MSM_SIRC_SIZE         SZ_4K

#define MSM_SCPLL_BASE        IOMEM(0xE1007000)
#define MSM_SCPLL_PHYS        0xA8800000
#define MSM_SCPLL_SIZE        SZ_4K

#define MSM_SHARED_RAM_BASE   IOMEM(0xE0100000)
#define MSM_SHARED_RAM_SIZE   SZ_1M

#define MSM_UART1_PHYS        0xA9A00000
#define MSM_UART1_SIZE        SZ_4K

#define MSM_UART2_PHYS        0xA9B00000
#define MSM_UART2_SIZE        SZ_4K

#define MSM_UART3_PHYS        0xA9C00000
#define MSM_UART3_SIZE        SZ_4K

#ifdef CONFIG_MSM_DEBUG_UART
#define MSM_DEBUG_UART_BASE   0xE1000000
#if CONFIG_MSM_DEBUG_UART == 1
#define MSM_DEBUG_UART_PHYS   MSM_UART1_PHYS
#elif CONFIG_MSM_DEBUG_UART == 2
#define MSM_DEBUG_UART_PHYS   MSM_UART2_PHYS
#elif CONFIG_MSM_DEBUG_UART == 3
#define MSM_DEBUG_UART_PHYS   MSM_UART3_PHYS
#endif
#define MSM_DEBUG_UART_SIZE   SZ_4K
#endif

#define MSM_I2C_PHYS          0xA9900000
#define MSM_I2C_SIZE          SZ_4K

#define MSM_HSUSB_PHYS        0xA0800000
#define MSM_HSUSB_BASE        IOMEM(0xE0009000)
#define MSM_HSUSB_SIZE        SZ_4K

#define MSM_MDC_BASE	      IOMEM(0xE0200000)
#define MSM_MDC_PHYS	      0xAA500000
#define MSM_MDC_SIZE	      SZ_1M

#define MSM_AD5_BASE          IOMEM(0xE0300000)
#define MSM_AD5_PHYS          0xAC000000
#define MSM_AD5_SIZE          (SZ_1M*13)
#if defined(CONFIG_SAMSUNG_CAPELA)
/* #define MSM_VFE_BASE          IOMEM(0xE1100000) */
#define MSM_VFE_PHYS          0xA0F00000
#define MSM_VFE_SIZE          SZ_1M
#endif /* kthur */

#define MSM_SSBI_BASE         IOMEM(0xE1004000)
#define MSM_SSBI_PHYS         0xA8100000
#define MSM_SSBI_SIZE         SZ_4K

#define MSM_TSSC_BASE         IOMEM(0xE1005000)
#define MSM_TSSC_PHYS         0xAA300000
#define MSM_TSSC_SIZE         SZ_4K

#endif
