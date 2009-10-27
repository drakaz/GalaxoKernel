/* linux/arch/arm/mach-msm/irq.c
 *
 * Copyright (C) 2007 Google, Inc.
 * Copyright (c) 2009 QUALCOMM USA, INC.
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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/interrupt.h>
#include <linux/ptrace.h>
#include <linux/timer.h>
#include <linux/irq.h>
#include <linux/io.h>

#include <asm/cacheflush.h>

#include <mach/hardware.h>

#include <mach/msm_iomap.h>
#include <mach/fiq.h>

#include "smd_private.h"

enum {
	IRQ_DEBUG_SLEEP_INT_TRIGGER = 1U << 0,
	IRQ_DEBUG_SLEEP_INT = 1U << 1,
	IRQ_DEBUG_SLEEP_ABORT = 1U << 2,
	IRQ_DEBUG_SLEEP = 1U << 3,
	IRQ_DEBUG_SLEEP_REQUEST = 1U << 4,
};
static int msm_irq_debug_mask = IRQ_DEBUG_SLEEP_ABORT;
module_param_named(debug_mask, msm_irq_debug_mask, int, S_IRUGO | S_IWUSR | S_IWGRP);
#if defined(CONFIG_SAMSUNG_CAPELA)
extern int uart_arm9; /* gtuo.park for UART Diag */
#endif

#define VIC_REG(off) (MSM_VIC_BASE + (off))

#define VIC_INT_SELECT0     VIC_REG(0x0000)  /* 1: FIQ, 0: IRQ */
#define VIC_INT_SELECT1     VIC_REG(0x0004)  /* 1: FIQ, 0: IRQ */
#define VIC_INT_EN0         VIC_REG(0x0010)
#define VIC_INT_EN1         VIC_REG(0x0014)
#define VIC_INT_ENCLEAR0    VIC_REG(0x0020)
#define VIC_INT_ENCLEAR1    VIC_REG(0x0024)
#define VIC_INT_ENSET0      VIC_REG(0x0030)
#define VIC_INT_ENSET1      VIC_REG(0x0034)
#define VIC_INT_TYPE0       VIC_REG(0x0040)  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_TYPE1       VIC_REG(0x0044)  /* 1: EDGE, 0: LEVEL  */
#define VIC_INT_POLARITY0   VIC_REG(0x0050)  /* 1: NEG, 0: POS */
#define VIC_INT_POLARITY1   VIC_REG(0x0054)  /* 1: NEG, 0: POS */
#define VIC_NO_PEND_VAL     VIC_REG(0x0060)

#if defined(CONFIG_ARCH_QSD)
#define VIC_NO_PEND_VAL_FIQ VIC_REG(0x0064)
#define VIC_INT_MASTEREN    VIC_REG(0x0068)  /* 1: IRQ, 2: FIQ     */
#define VIC_CONFIG          VIC_REG(0x006C)  /* 1: USE SC VIC */
#else
#define VIC_INT_MASTEREN    VIC_REG(0x0064)  /* 1: IRQ, 2: FIQ     */
#define VIC_PROTECTION      VIC_REG(0x006C)  /* 1: ENABLE          */
#define VIC_CONFIG          VIC_REG(0x0068)  /* 1: USE ARM1136 VIC */
#endif

#define VIC_IRQ_STATUS0     VIC_REG(0x0080)
#define VIC_IRQ_STATUS1     VIC_REG(0x0084)
#define VIC_FIQ_STATUS0     VIC_REG(0x0090)
#define VIC_FIQ_STATUS1     VIC_REG(0x0094)
#define VIC_RAW_STATUS0     VIC_REG(0x00A0)
#define VIC_RAW_STATUS1     VIC_REG(0x00A4)
#define VIC_INT_CLEAR0      VIC_REG(0x00B0)
#define VIC_INT_CLEAR1      VIC_REG(0x00B4)
#define VIC_SOFTINT0        VIC_REG(0x00C0)
#define VIC_SOFTINT1        VIC_REG(0x00C4)
#define VIC_IRQ_VEC_RD      VIC_REG(0x00D0)  /* pending int # */
#define VIC_IRQ_VEC_PEND_RD VIC_REG(0x00D4)  /* pending vector addr */
#define VIC_IRQ_VEC_WR      VIC_REG(0x00D8)

#if defined(CONFIG_ARCH_QSD)
#define VIC_FIQ_VEC_RD      VIC_REG(0x00DC)
#define VIC_FIQ_VEC_PEND_RD VIC_REG(0x00E0)
#define VIC_FIQ_VEC_WR      VIC_REG(0x00E4)
#define VIC_IRQ_IN_SERVICE  VIC_REG(0x00E8)
#define VIC_IRQ_IN_STACK    VIC_REG(0x00EC)
#define VIC_FIQ_IN_SERVICE  VIC_REG(0x00F0)
#define VIC_FIQ_IN_STACK    VIC_REG(0x00F4)
#define VIC_TEST_BUS_SEL    VIC_REG(0x00F8)
#define VIC_IRQ_CTRL_CONFIG VIC_REG(0x00FC)
#else
#define VIC_IRQ_IN_SERVICE  VIC_REG(0x00E0)
#define VIC_IRQ_IN_STACK    VIC_REG(0x00E4)
#define VIC_TEST_BUS_SEL    VIC_REG(0x00E8)
#endif

#define VIC_VECTPRIORITY(n) VIC_REG(0x0200+((n) * 4))
#define VIC_VECTADDR(n)     VIC_REG(0x0400+((n) * 4))

static uint32_t msm_irq_smsm_wake_enable[2];
static struct {
	uint32_t int_en[2];
	uint32_t int_type;
	uint32_t int_polarity;
	uint32_t int_select;
} msm_irq_shadow_reg[2];
static uint32_t msm_irq_idle_disable[2];

#define SMSM_FAKE_IRQ (0xff)
static uint8_t msm_irq_to_smsm[NR_IRQS] = { 0, }; // gtuo.park
static uint8_t msm_irq_to_smsm_uart[NR_IRQS] = {
	[INT_MDDI_EXT] = 1,
	[INT_MDDI_PRI] = 2,
	[INT_MDDI_CLIENT] = 3,
	[INT_USB_OTG] = 4,

	[INT_PWB_I2C] = 5,
	[INT_SDC1_0] = 6,
	[INT_SDC1_1] = 7,
	[INT_SDC2_0] = 8,

	[INT_SDC2_1] = 9,
	[INT_ADSP_A9_A11] = 10,
	[INT_UART1] = 11,
	[INT_UART2] = 12,

#if 1
	[INT_UART3] = 13,
#endif
	[INT_UART1_RX] = 14,
	[INT_UART2_RX] = 15,
#if 1
	[INT_UART3_RX] = 16,
#endif

	[INT_UART1DM_IRQ] = 17,
	[INT_UART1DM_RX] = 18,
	[INT_KEYSENSE] = 19,
	[INT_AD_HSSD] = 20,

	[INT_NAND_WR_ER_DONE] = 21,
	[INT_NAND_OP_DONE] = 22,
	[INT_TCHSCRN1] = 23,
	[INT_TCHSCRN2] = 24,

	[INT_TCHSCRN_SSBI] = 25,
	[INT_USB_HS] = 26,
	[INT_UART2DM_RX] = 27,
	[INT_UART2DM_IRQ] = 28,

	[INT_SDC4_1] = 29,
	[INT_SDC4_0] = 30,
	[INT_SDC3_1] = 31,
	[INT_SDC3_0] = 32,

	/* fake wakeup interrupts */
	[INT_GPIO_GROUP1] = SMSM_FAKE_IRQ,
	[INT_GPIO_GROUP2] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_0] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_1] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_2] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_5] = SMSM_FAKE_IRQ,
	[INT_GP_TIMER_EXP] = SMSM_FAKE_IRQ,
	[INT_DEBUG_TIMER_EXP] = SMSM_FAKE_IRQ,
	[INT_ADSP_A11] = SMSM_FAKE_IRQ,
#if defined(CONFIG_ARCH_QSD)
	[INT_SIRC_0] = SMSM_FAKE_IRQ,
	[INT_SIRC_1] = SMSM_FAKE_IRQ,
#endif
};
static uint8_t msm_irq_to_smsm_nouart[NR_IRQS] = {
	[INT_MDDI_EXT] = 1,
	[INT_MDDI_PRI] = 2,
	[INT_MDDI_CLIENT] = 3,
	[INT_USB_OTG] = 4,

	[INT_PWB_I2C] = 5,
	[INT_SDC1_0] = 6,
	[INT_SDC1_1] = 7,
	[INT_SDC2_0] = 8,

	[INT_SDC2_1] = 9,
	[INT_ADSP_A9_A11] = 10,
	[INT_UART1] = 11,
	[INT_UART2] = 12,

#if 0
	[INT_UART3] = 13,
#endif
	[INT_UART1_RX] = 14,
	[INT_UART2_RX] = 15,
#if 0
	[INT_UART3_RX] = 16,
#endif

	[INT_UART1DM_IRQ] = 17,
	[INT_UART1DM_RX] = 18,
	[INT_KEYSENSE] = 19,
	[INT_AD_HSSD] = 20,

	[INT_NAND_WR_ER_DONE] = 21,
	[INT_NAND_OP_DONE] = 22,
	[INT_TCHSCRN1] = 23,
	[INT_TCHSCRN2] = 24,

	[INT_TCHSCRN_SSBI] = 25,
	[INT_USB_HS] = 26,
	[INT_UART2DM_RX] = 27,
	[INT_UART2DM_IRQ] = 28,

	[INT_SDC4_1] = 29,
	[INT_SDC4_0] = 30,
	[INT_SDC3_1] = 31,
	[INT_SDC3_0] = 32,

	/* fake wakeup interrupts */
	[INT_GPIO_GROUP1] = SMSM_FAKE_IRQ,
	[INT_GPIO_GROUP2] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_0] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_1] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_2] = SMSM_FAKE_IRQ,
	[INT_A9_M2A_5] = SMSM_FAKE_IRQ,
	[INT_GP_TIMER_EXP] = SMSM_FAKE_IRQ,
	[INT_DEBUG_TIMER_EXP] = SMSM_FAKE_IRQ,
	[INT_ADSP_A11] = SMSM_FAKE_IRQ,
#if defined(CONFIG_ARCH_QSD)
	[INT_SIRC_0] = SMSM_FAKE_IRQ,
	[INT_SIRC_1] = SMSM_FAKE_IRQ,
#endif
};

static void msm_irq_ack(unsigned int irq)
{
	void __iomem *reg = VIC_INT_CLEAR0 + ((irq & 32) ? 4 : 0);
	irq = 1 << (irq & 31);
	writel(irq, reg);
}

static void msm_irq_mask(unsigned int irq)
{
	void __iomem *reg = VIC_INT_ENCLEAR0 + ((irq & 32) ? 4 : 0);
	unsigned index = (irq >> 5) & 1;
	uint32_t mask = 1UL << (irq & 31);
	int smsm_irq = msm_irq_to_smsm[irq];

	msm_irq_shadow_reg[index].int_en[0] &= ~mask;
	writel(mask, reg);
	if (smsm_irq == 0)
		msm_irq_idle_disable[index] &= ~mask;
	else {
		mask = 1UL << (smsm_irq - 1);
		msm_irq_smsm_wake_enable[0] &= ~mask;
	}
}

static void msm_irq_unmask(unsigned int irq)
{
	void __iomem *reg = VIC_INT_ENSET0 + ((irq & 32) ? 4 : 0);
	unsigned index = (irq >> 5) & 1;
	uint32_t mask = 1UL << (irq & 31);
	int smsm_irq = msm_irq_to_smsm[irq];

	msm_irq_shadow_reg[index].int_en[0] |= mask;
	writel(mask, reg);

	if (smsm_irq == 0)
		msm_irq_idle_disable[index] |= mask;
	else {
		mask = 1UL << (smsm_irq - 1);
		msm_irq_smsm_wake_enable[0] |= mask;
	}
}

static int msm_irq_set_wake(unsigned int irq, unsigned int on)
{
	unsigned index = (irq >> 5) & 1;
	uint32_t mask = 1UL << (irq & 31);
	int smsm_irq = msm_irq_to_smsm[irq];

	if (smsm_irq == 0) {
		printk(KERN_ERR "msm_irq_set_wake: bad wakeup irq %d\n", irq);
		return -EINVAL;
	}
	if (on)
		msm_irq_shadow_reg[index].int_en[1] |= mask;
	else
		msm_irq_shadow_reg[index].int_en[1] &= ~mask;

	if (smsm_irq == SMSM_FAKE_IRQ)
		return 0;

	mask = 1UL << (smsm_irq - 1);
	if (on)
		msm_irq_smsm_wake_enable[1] |= mask;
	else
		msm_irq_smsm_wake_enable[1] &= ~mask;
	return 0;
}

static int msm_irq_set_type(unsigned int irq, unsigned int flow_type)
{
	void __iomem *treg = VIC_INT_TYPE0 + ((irq & 32) ? 4 : 0);
	void __iomem *preg = VIC_INT_POLARITY0 + ((irq & 32) ? 4 : 0);
	unsigned index = (irq >> 5) & 1;
	int b = 1 << (irq & 31);
	uint32_t polarity;
	uint32_t type;

	polarity = msm_irq_shadow_reg[index].int_polarity;
	if (flow_type & (IRQF_TRIGGER_FALLING | IRQF_TRIGGER_LOW))
		polarity |= b;
	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_HIGH))
		polarity &= ~b;
	writel(polarity, preg);
	msm_irq_shadow_reg[index].int_polarity = polarity;

	type = msm_irq_shadow_reg[index].int_type;
	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) {
		type |= b;
		irq_desc[irq].handle_irq = handle_edge_irq;
	}
	if (flow_type & (IRQF_TRIGGER_HIGH | IRQF_TRIGGER_LOW)) {
		type &= ~b;
		irq_desc[irq].handle_irq = handle_level_irq;
	}
	writel(type, treg);
	msm_irq_shadow_reg[index].int_type = type;
	return 0;
}

int msm_irq_pending(void)
{
	return readl(VIC_IRQ_STATUS0) || readl(VIC_IRQ_STATUS1);
}

int msm_irq_idle_sleep_allowed(void)
{
	if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP_REQUEST)
		printk(KERN_INFO "msm_irq_idle_sleep_allowed: disable %x %x\n",
		msm_irq_idle_disable[0], msm_irq_idle_disable[1]);
	return !(msm_irq_idle_disable[0] || msm_irq_idle_disable[1]);
}

/*
 * Prepare interrupt subsystem for entering sleep -- phase 1.
 * If arm9_wake is true, return currently enabled interrupts in *irq_mask.
 */
void msm_irq_enter_sleep1(bool arm9_wake, int from_idle, uint32_t *irq_mask)
{
	if (arm9_wake) {
		*irq_mask = msm_irq_smsm_wake_enable[!from_idle];
		if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP)
			printk(KERN_INFO
				"%s irq_mask %x\n", __func__, *irq_mask);
	}
}

/*
 * Prepare interrupt subsystem for entering sleep -- phase 2.
 * Detect any pending interrupts and configure interrupt hardware.
 *
 * Return value:
 * -EAGAIN: there are pending interrupt(s); interrupt configuration
 *          is not changed.
 *       0: success
 */
int msm_irq_enter_sleep2(bool arm9_wake, int from_idle)
{
	int limit = 10;
	uint32_t pending0, pending1;

	if (from_idle && !arm9_wake)
		return 0;

	/* edge triggered interrupt may get lost if this mode is used */
	WARN_ON_ONCE(!arm9_wake && !from_idle);

	if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP)
		printk(KERN_INFO "%s change irq, pend %x %x\n", __func__,
			readl(VIC_IRQ_STATUS0), readl(VIC_IRQ_STATUS1));

	pending0 = readl(VIC_IRQ_STATUS0);
	pending1 = readl(VIC_IRQ_STATUS1);
	pending0 &= msm_irq_shadow_reg[0].int_en[!from_idle];
	/* Clear INT_A9_M2A_5 since requesting sleep triggers it */
	pending0 &= ~((1U << INT_A9_M2A_5) | (1U << INT_A9_M2A_3));
	
	pending1 &= msm_irq_shadow_reg[1].int_en[!from_idle];

	if (pending0 || pending1) {
		if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP_ABORT)
			printk(KERN_INFO "%s abort %x %x\n", __func__,
				pending0, pending1);
		return -EAGAIN;
	}

	writel(0, VIC_INT_EN0);
	writel(0, VIC_INT_EN1);

	while (limit-- > 0) {
		int pend_irq;
		int irq = readl(VIC_IRQ_VEC_RD);
		if (irq == -1)
			break;
		pend_irq = readl(VIC_IRQ_VEC_PEND_RD);
		if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP_INT)
			printk(KERN_INFO "%s cleared int %d (%d)\n",
				__func__, irq, pend_irq);
	}

	if (arm9_wake) {
		msm_irq_set_type(INT_A9_M2A_6, IRQF_TRIGGER_RISING);
		writel(1U << INT_A9_M2A_6, VIC_INT_ENSET0);
	} else {
		writel(msm_irq_shadow_reg[0].int_en[1], VIC_INT_ENSET0);
		writel(msm_irq_shadow_reg[1].int_en[1], VIC_INT_ENSET1);
	}

	return 0;
}

/*
 * Restore interrupt subsystem from sleep -- phase 1.
 * Configure interrupt hardware.
 */
void msm_irq_exit_sleep1(uint32_t irq_mask, uint32_t wakeup_reason,
	uint32_t pending_irqs)
{
	int i;

	msm_irq_ack(INT_A9_M2A_6);

	for (i = 0; i < 2; i++) {
		writel(msm_irq_shadow_reg[i].int_type,
			VIC_INT_TYPE0 + i * 4);
		writel(msm_irq_shadow_reg[i].int_polarity,
			VIC_INT_POLARITY0 + i * 4);
		writel(msm_irq_shadow_reg[i].int_en[0],
			VIC_INT_EN0 + i * 4);
		writel(msm_irq_shadow_reg[i].int_select,
			VIC_INT_SELECT0 + i * 4);
	}

	writel(3, VIC_INT_MASTEREN);

	if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP)
		printk(KERN_INFO "%s %x %x %x now %x %x\n",
			__func__, irq_mask, pending_irqs, wakeup_reason,
			readl(VIC_IRQ_STATUS0), readl(VIC_IRQ_STATUS1));
}

/*
 * Restore interrupt subsystem from sleep -- phase 2.
 * Poke the specified pending interrupts into interrupt hardware.
 */
void msm_irq_exit_sleep2(uint32_t irq_mask, uint32_t wakeup_reason,
	uint32_t pending)
{
	int i;

	if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP)
		printk(KERN_INFO "%s %x %x %x now %x %x\n",
			__func__, irq_mask, pending, wakeup_reason,
			readl(VIC_IRQ_STATUS0), readl(VIC_IRQ_STATUS1));

	for (i = 0; pending && i < ARRAY_SIZE(msm_irq_to_smsm); i++) {
		unsigned reg_offset = (i & 32) ? 4 : 0;
		uint32_t reg_mask = 1UL << (i & 31);
		int smsm_irq = msm_irq_to_smsm[i];
		uint32_t smsm_mask;

		if (smsm_irq == 0)
			continue;

		smsm_mask = 1U << (smsm_irq - 1);
		if (!(pending & smsm_mask))
			continue;

		pending &= ~smsm_mask;
		if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP_INT)
			printk(KERN_INFO
				"%s: irq %d still pending %x now %x %x\n",
				__func__, i, pending,
				readl(VIC_IRQ_STATUS0),
				readl(VIC_IRQ_STATUS1));
#if 0 /* debug intetrrupt trigger */
		if (readl(VIC_IRQ_STATUS0 + reg_offset) & reg_mask)
			writel(reg_mask, VIC_INT_CLEAR0 + reg_offset);
#endif
		if (readl(VIC_IRQ_STATUS0 + reg_offset) & reg_mask)
			continue;

		writel(reg_mask, VIC_SOFTINT0 + reg_offset);

		if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP_INT_TRIGGER)
			printk(KERN_INFO
				"%s: irq %d need trigger, now %x %x\n",
				__func__, i,
				readl(VIC_IRQ_STATUS0),
				readl(VIC_IRQ_STATUS1));
	}
}

/*
 * Restore interrupt subsystem from sleep -- phase 3.
 * Print debug information.
 */
void msm_irq_exit_sleep3(uint32_t irq_mask, uint32_t wakeup_reason,
	uint32_t pending_irqs)
{
	if (msm_irq_debug_mask & IRQ_DEBUG_SLEEP)
		printk(KERN_INFO "%s %x %x %x now %x %x state %x\n",
			__func__, irq_mask, pending_irqs, wakeup_reason,
			readl(VIC_IRQ_STATUS0), readl(VIC_IRQ_STATUS1),
			smsm_get_state(SMSM_MODEM_STATE));
}

static struct irq_chip msm_irq_chip = {
	.name      = "msm",
	.disable   = msm_irq_mask,
	.ack       = msm_irq_ack,
	.mask      = msm_irq_mask,
	.unmask    = msm_irq_unmask,
	.set_wake  = msm_irq_set_wake,
	.set_type  = msm_irq_set_type,
};

void __init msm_init_irq(void)
{
	unsigned n;

#if defined(CONFIG_SAMSUNG_CAPELA)
	if(uart_arm9) {
		printk(KERN_ERR "%s, uart_arm9 : %d, uart diag mode ON!!\n",__FUNCTION__,uart_arm9);
	}else {
		printk(KERN_ERR "%s, uart_arm9 : %d, uart diag mode OFF!!\n",__FUNCTION__,uart_arm9);
	}

	if(uart_arm9) {
		memcpy(msm_irq_to_smsm, msm_irq_to_smsm_nouart, ARRAY_SIZE(msm_irq_to_smsm_nouart));
	} else {
		memcpy(msm_irq_to_smsm, msm_irq_to_smsm_uart, ARRAY_SIZE(msm_irq_to_smsm_uart));
	}
#endif
	/* select level interrupts */
	writel(0, VIC_INT_TYPE0);
	writel(0, VIC_INT_TYPE1);

	/* select highlevel interrupts */
	writel(0, VIC_INT_POLARITY0);
	writel(0, VIC_INT_POLARITY1);

	/* select IRQ for all INTs */
	writel(0, VIC_INT_SELECT0);
	writel(0, VIC_INT_SELECT1);

	/* disable all INTs */
	writel(0, VIC_INT_EN0);
	writel(0, VIC_INT_EN1);

	/* don't use 1136 vic */
	writel(0, VIC_CONFIG);

	/* enable interrupt controller */
	writel(3, VIC_INT_MASTEREN);

	for (n = 0; n < NR_MSM_IRQS; n++) {
		set_irq_chip(n, &msm_irq_chip);
		set_irq_handler(n, handle_level_irq);
		set_irq_flags(n, IRQF_VALID);
	}
}

#if defined(CONFIG_MSM_FIQ_SUPPORT)
void msm_trigger_irq(int irq)
{
	void __iomem *reg = VIC_SOFTINT0 + ((irq & 32) ? 4 : 0);
	uint32_t mask = 1UL << (irq & 31);
	writel(mask, reg);
}

void msm_fiq_enable(int irq)
{
	unsigned long flags;
	local_irq_save(flags);
	msm_irq_unmask(irq);
	local_irq_restore(flags);
}

void msm_fiq_disable(int irq)
{
	unsigned long flags;
	local_irq_save(flags);
	msm_irq_mask(irq);
	local_irq_restore(flags);
}

void msm_fiq_select(int irq)
{
	void __iomem *reg = VIC_INT_SELECT0 + ((irq & 32) ? 4 : 0);
	unsigned index = (irq >> 5) & 1;
	uint32_t mask = 1UL << (irq & 31);
	unsigned long flags;

	local_irq_save(flags);
	msm_irq_shadow_reg[index].int_select |= mask;
	writel(msm_irq_shadow_reg[index].int_select, reg);
	local_irq_restore(flags);
}

void msm_fiq_unselect(int irq)
{
	void __iomem *reg = VIC_INT_SELECT0 + ((irq & 32) ? 4 : 0);
	unsigned index = (irq >> 5) & 1;
	uint32_t mask = 1UL << (irq & 31);
	unsigned long flags;

	local_irq_save(flags);
	msm_irq_shadow_reg[index].int_select &= (!mask);
	writel(msm_irq_shadow_reg[index].int_select, reg);
	local_irq_restore(flags);
}
/* set_fiq_handler originally from arch/arm/kernel/fiq.c */
static void set_fiq_handler(void *start, unsigned int length)
{
	memcpy((void *)0xffff001c, start, length);
	flush_icache_range(0xffff001c, 0xffff001c + length);
	if (!vectors_high())
		flush_icache_range(0x1c, 0x1c + length);
}

extern unsigned char fiq_glue, fiq_glue_end;

static void (*fiq_func)(void *data, void *regs);
static unsigned long long fiq_stack[256];

void fiq_glue_setup(void *func, void *data, void *sp);

int msm_fiq_set_handler(void (*func)(void *data, void *regs), void *data)
{
	unsigned long flags;
	int ret = -ENOMEM;

	local_irq_save(flags);
	if (fiq_func == 0) {
		fiq_func = func;
		fiq_glue_setup(func, data, fiq_stack + 255);
		set_fiq_handler(&fiq_glue, (&fiq_glue_end - &fiq_glue));
		ret = 0;
	}
	local_irq_restore(flags);
	return ret;
}
#endif
