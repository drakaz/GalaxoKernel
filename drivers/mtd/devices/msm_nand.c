/* drivers/mtd/devices/msm_nand.c
 *
 * Copyright (c) 2008-2009 QUALCOMM USA, INC.
 * Copyright (C) 2007 Google, Inc.
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
#include <linux/module.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/platform_device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/crc16.h>
#include <linux/bitrev.h>

#include <asm/dma.h>
#include <asm/mach/flash.h>
#include <mach/partition.h> // gtuo.park for BBM

#define MSM_NAND_BASE 0xA0A00000
#include "msm_nand.h"

#define MSM_NAND_DMA_BUFFER_SIZE SZ_4K
#define MSM_NAND_DMA_BUFFER_SLOTS \
	(MSM_NAND_DMA_BUFFER_SIZE / (sizeof(((atomic_t *)0)->counter) * 8))

#define NAND_CFG0_RAW 0xA80420C0
#define NAND_CFG1_RAW 0x5045D

#define NAND_CFG0_RAW_ONFI_IDENTIFIER 0x88000800
#define NAND_CFG0_RAW_ONFI_PARAM_INFO 0x88040000
#define NAND_CFG1_RAW_ONFI_IDENTIFIER 0x0005045d
#define NAND_CFG1_RAW_ONFI_PARAM_INFO 0x0005045d

#define ONFI_IDENTIFIER_LENGTH 0x0004
#define ONFI_PARAM_INFO_LENGTH 0x0200
#define ONFI_PARAM_PAGE_LENGTH 0x0100

#define ONFI_PARAMETER_PAGE_SIGNATURE 0x49464E4F

#define FLASH_READ_ONFI_IDENTIFIER_COMMAND 0x90
#define FLASH_READ_ONFI_IDENTIFIER_ADDRESS 0x20
#define FLASH_READ_ONFI_PARAMETERS_COMMAND 0xEC
#define FLASH_READ_ONFI_PARAMETERS_ADDRESS 0x00

#define VERBOSE 0

struct msm_nand_chip {
	struct device *dev;
	wait_queue_head_t wait_queue;
	atomic_t dma_buffer_busy;
	unsigned dma_channel;
	uint8_t *dma_buffer;
	dma_addr_t dma_addr;
	unsigned CFG0, CFG1;
	uint32_t ecc_buf_cfg;
};

struct mtd_info *current_mtd;



#define CFG1_WIDE_FLASH (1U << 1)

/* TODO: move datamover code out */

#define SRC_CRCI_NAND_CMD  CMD_SRC_CRCI(DMOV_NAND_CRCI_CMD)
#define DST_CRCI_NAND_CMD  CMD_DST_CRCI(DMOV_NAND_CRCI_CMD)
#define SRC_CRCI_NAND_DATA CMD_SRC_CRCI(DMOV_NAND_CRCI_DATA)
#define DST_CRCI_NAND_DATA CMD_DST_CRCI(DMOV_NAND_CRCI_DATA)

#define msm_virt_to_dma(chip, vaddr) \
	((void)(*(vaddr)), (chip)->dma_addr + \
	 ((uint8_t *)(vaddr) - (chip)->dma_buffer))

/**
 * msm_nand_oob_64 - oob info for large (2KB) page
 */
static struct nand_ecclayout msm_nand_oob_64 = {
	.eccbytes	= 40,
	.eccpos		= {
		0,  1,  2,  3,  4,  5,  6,  7,  8,  9,
		10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
		20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
		30, 31, 32, 33, 34, 35, 36, 37, 38, 39,
		},
	.oobavail	= 16,
	.oobfree	= {
		{40, 16},
	}
};


/**
 * msm_onenand_oob_64 - oob info for large (2KB) page
 */
static struct nand_ecclayout msm_onenand_oob_64 = {
	.eccbytes	= 20,
	.eccpos		= {
		8, 9, 10, 11, 12,
		24, 25, 26, 27, 28,
		40, 41, 42, 43, 44,
		56, 57, 58, 59, 60,
		},
	.oobavail	= 20,
	.oobfree	= {
		{2, 3}, {14, 2}, {18, 3}, {30, 2},
		{34, 3}, {46, 2}, {50, 3}, {62, 2}
	}
};

static void *msm_nand_get_dma_buffer(struct msm_nand_chip *chip, size_t size)
{
	unsigned int bitmask, free_bitmask, old_bitmask;
	unsigned int need_mask, current_need_mask;
	int free_index;

	need_mask = (1UL << DIV_ROUND_UP(size, MSM_NAND_DMA_BUFFER_SLOTS)) - 1;
	bitmask = atomic_read(&chip->dma_buffer_busy);
	free_bitmask = ~bitmask;
	do {
		free_index = __ffs(free_bitmask);
		current_need_mask = need_mask << free_index;
		if ((bitmask & current_need_mask) == 0) {
			old_bitmask =
				atomic_cmpxchg(&chip->dma_buffer_busy,
					       bitmask,
					       bitmask | current_need_mask);
			if (old_bitmask == bitmask)
				return chip->dma_buffer +
					free_index * MSM_NAND_DMA_BUFFER_SLOTS;
			free_bitmask = 0; /* force return */
		}
		/* current free range was too small, clear all free bits */
		/* below the top busy bit within current_need_mask */
		free_bitmask &=
			~(~0U >> (32 - fls(bitmask & current_need_mask)));
	} while (free_bitmask);

	return NULL;
}

static void msm_nand_release_dma_buffer(struct msm_nand_chip *chip,
					void *buffer, size_t size)
{
	int index;
	unsigned int used_mask;

	used_mask = (1UL << DIV_ROUND_UP(size, MSM_NAND_DMA_BUFFER_SLOTS)) - 1;
	index = ((uint8_t *)buffer - chip->dma_buffer) /
		MSM_NAND_DMA_BUFFER_SLOTS;
	atomic_sub(used_mask << index, &chip->dma_buffer_busy);

	wake_up(&chip->wait_queue);
}


unsigned flash_rd_reg(struct msm_nand_chip *chip, unsigned addr)
{
	struct {
		dmov_s cmd;
		unsigned cmdptr;
		unsigned data;
	} *dma_buffer;
	unsigned rv;

	wait_event(chip->wait_queue,
		   (dma_buffer = msm_nand_get_dma_buffer(
			    chip, sizeof(*dma_buffer))));

	dma_buffer->cmd.cmd = CMD_LC | CMD_OCB | CMD_OCU;
	dma_buffer->cmd.src = addr;
	dma_buffer->cmd.dst = msm_virt_to_dma(chip, &dma_buffer->data);
	dma_buffer->cmd.len = 4;

	dma_buffer->cmdptr =
		(msm_virt_to_dma(chip, &dma_buffer->cmd) >> 3) | CMD_PTR_LP;
	dma_buffer->data = 0xeeeeeeee;

	msm_dmov_exec_cmd(
		chip->dma_channel, DMOV_CMD_PTR_LIST |
		DMOV_CMD_ADDR(msm_virt_to_dma(chip, &dma_buffer->cmdptr)));
	rv = dma_buffer->data;

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	return rv;
}

void flash_wr_reg(struct msm_nand_chip *chip, unsigned addr, unsigned val)
{
	struct {
		dmov_s cmd;
		unsigned cmdptr;
		unsigned data;
	} *dma_buffer;

	wait_event(chip->wait_queue,
		   (dma_buffer = msm_nand_get_dma_buffer(
			    chip, sizeof(*dma_buffer))));

	dma_buffer->cmd.cmd = CMD_LC | CMD_OCB | CMD_OCU;
	dma_buffer->cmd.src = msm_virt_to_dma(chip, &dma_buffer->data);
	dma_buffer->cmd.dst = addr;
	dma_buffer->cmd.len = 4;

	dma_buffer->cmdptr =
		(msm_virt_to_dma(chip, &dma_buffer->cmd) >> 3) | CMD_PTR_LP;
	dma_buffer->data = val;

	msm_dmov_exec_cmd(
		chip->dma_channel, DMOV_CMD_PTR_LIST |
		DMOV_CMD_ADDR(msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));
}


uint32_t flash_read_id(struct msm_nand_chip *chip)
{
	struct {
		dmov_s cmd[7];
		unsigned cmdptr;
		unsigned data[7];
	} *dma_buffer;
	uint32_t rv;

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	dma_buffer->data[0] = 0 | 4;
	dma_buffer->data[1] = NAND_CMD_FETCH_ID;
	dma_buffer->data[2] = 1;
	dma_buffer->data[3] = 0xeeeeeeee;
	dma_buffer->data[4] = 0xeeeeeeee;
	dma_buffer->data[5] = flash_rd_reg(chip, NAND_SFLASHC_BURST_CFG);
	dma_buffer->data[6] = 0x00000000;
	BUILD_BUG_ON(6 != ARRAY_SIZE(dma_buffer->data) - 1);

	dma_buffer->cmd[0].cmd = 0 | CMD_OCB;
	dma_buffer->cmd[0].src = msm_virt_to_dma(chip, &dma_buffer->data[6]);
	dma_buffer->cmd[0].dst = NAND_SFLASHC_BURST_CFG;
	dma_buffer->cmd[0].len = 4;

	dma_buffer->cmd[1].cmd = 0;
	dma_buffer->cmd[1].src = msm_virt_to_dma(chip, &dma_buffer->data[0]);
	dma_buffer->cmd[1].dst = NAND_FLASH_CHIP_SELECT;
	dma_buffer->cmd[1].len = 4;

	dma_buffer->cmd[2].cmd = DST_CRCI_NAND_CMD;
	dma_buffer->cmd[2].src = msm_virt_to_dma(chip, &dma_buffer->data[1]);
	dma_buffer->cmd[2].dst = NAND_FLASH_CMD;
	dma_buffer->cmd[2].len = 4;

	dma_buffer->cmd[3].cmd = 0;
	dma_buffer->cmd[3].src = msm_virt_to_dma(chip, &dma_buffer->data[2]);
	dma_buffer->cmd[3].dst = NAND_EXEC_CMD;
	dma_buffer->cmd[3].len = 4;

	dma_buffer->cmd[4].cmd = SRC_CRCI_NAND_DATA;
	dma_buffer->cmd[4].src = NAND_FLASH_STATUS;
	dma_buffer->cmd[4].dst = msm_virt_to_dma(chip, &dma_buffer->data[3]);
	dma_buffer->cmd[4].len = 4;

	dma_buffer->cmd[5].cmd = 0;
	dma_buffer->cmd[5].src = NAND_READ_ID;
	dma_buffer->cmd[5].dst = msm_virt_to_dma(chip, &dma_buffer->data[4]);
	dma_buffer->cmd[5].len = 4;

	dma_buffer->cmd[6].cmd = CMD_OCU | CMD_LC;
	dma_buffer->cmd[6].src = msm_virt_to_dma(chip, &dma_buffer->data[5]);
	dma_buffer->cmd[6].dst = NAND_SFLASHC_BURST_CFG;
	dma_buffer->cmd[6].len = 4;

	BUILD_BUG_ON(6 != ARRAY_SIZE(dma_buffer->cmd) - 1);

	dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd) >> 3
			) | CMD_PTR_LP;

	msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST | DMOV_CMD_ADDR
			(msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

	pr_info("status: %x\n", dma_buffer->data[3]);
	pr_info("nandid: %x maker %02x device %02x\n",
	       dma_buffer->data[4], dma_buffer->data[4] & 0xff,
	       (dma_buffer->data[4] >> 8) & 0xff);
	rv = dma_buffer->data[4];
	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));
	return rv;
}

struct flash_identification {
	uint32_t flash_id;
	uint32_t mask;
	uint32_t density;
	uint32_t widebus;
	uint32_t pagesize;
	uint32_t blksize;
	uint32_t oobsize;
};

static struct flash_identification supported_flash[] =
{
	/* Flash ID   ID Mask Density(MB)  Wid Pgsz   Blksz   oobsz    Manuf */
	{0x00000000, 0xFFFFFFFF,         0, 0,    0,         0,  0, }, /*ONFI*/
	{0x1500aaec, 0xFF00FFFF, (256<<20), 0, 2048, (2048<<6), 64, }, /*Sams*/
	{0x5500baec, 0xFF00FFFF, (256<<20), 1, 2048, (2048<<6), 64, }, /*Sams*/
	{0x1500aa98, 0xFFFFFFFF, (256<<20), 0, 2048, (2048<<6), 64, }, /*Tosh*/
	{0x5500ba98, 0xFFFFFFFF, (256<<20), 1, 2048, (2048<<6), 64, }, /*Tosh*/
	{0xd580b12c, 0xFFFFFFFF, (128<<20), 1, 2048, (2048<<6), 64, }, /*Micr*/
	{0x5580baad, 0xFFFFFFFF, (256<<20), 1, 2048, (2048<<6), 64, }, /*Hynx*/
	{0x5510baad, 0xFFFFFFFF, (256<<20), 1, 2048, (2048<<6), 64, }, /*Hynx*/
	/* Note: Width flag is 0 for 8 bit Flash and 1 for 16 bit flash      */
	/* Note: The First row will be filled at runtime during ONFI probe   */
};

uint16_t flash_onfi_crc_check(uint8_t *buffer, uint16_t count)
{
	int i;
	uint16_t result;

	for (i = 0; i < count; i++)
		buffer[i] = bitrev8(buffer[i]);

	result = bitrev16(crc16(bitrev16(0x4f4e), buffer, count));

	for (i = 0; i < count; i++)
		buffer[i] = bitrev8(buffer[i]);

	return result;
}


uint32_t flash_onfi_probe(struct msm_nand_chip *chip)
{
	struct onfi_param_page {
		uint32_t parameter_page_signature;
		uint16_t revision_number;
		uint16_t features_supported;
		uint16_t optional_commands_supported;
		uint8_t  reserved0[22];
		uint8_t  device_manufacturer[12];
		uint8_t  device_model[20];
		uint8_t  jedec_manufacturer_id;
		uint16_t date_code;
		uint8_t  reserved1[13];
		uint32_t number_of_data_bytes_per_page;
		uint16_t number_of_spare_bytes_per_page;
		uint32_t number_of_data_bytes_per_partial_page;
		uint16_t number_of_spare_bytes_per_partial_page;
		uint32_t number_of_pages_per_block;
		uint32_t number_of_blocks_per_logical_unit;
		uint8_t  number_of_logical_units;
		uint8_t  number_of_address_cycles;
		uint8_t  number_of_bits_per_cell;
		uint16_t maximum_bad_blocks_per_logical_unit;
		uint16_t block_endurance;
		uint8_t  guaranteed_valid_begin_blocks;
		uint16_t guaranteed_valid_begin_blocks_endurance;
		uint8_t  number_of_programs_per_page;
		uint8_t  partial_program_attributes;
		uint8_t  number_of_bits_ecc_correctaility;
		uint8_t  number_of_interleaved_address_bits;
		uint8_t  interleaved_operation_attributes;
		uint8_t  reserved2[13];
		uint8_t  io_pin_capacitance;
		uint16_t timing_mode_support;
		uint16_t program_cache_timing_mode_support;
		uint16_t maximum_page_programming_time;
		uint16_t maximum_block_erase_time;
		uint16_t maximum_page_read_time;
		uint16_t maximum_change_column_setup_time;
		uint8_t  reserved3[23];
		uint16_t vendor_specific_revision_number;
		uint8_t  vendor_specific[88];
		uint16_t integrity_crc;

	} __attribute__((__packed__));

	struct onfi_param_page *onfi_param_page_ptr;
	uint8_t *onfi_identifier_buf = NULL;
	uint8_t *onfi_param_info_buf = NULL;

	struct {
		dmov_s cmd[11];
		unsigned cmdptr;
		struct {
			uint32_t cmd;
			uint32_t addr0;
			uint32_t addr1;
			uint32_t cfg0;
			uint32_t cfg1;
			uint32_t exec;
			uint32_t flash_status;
			uint32_t devcmd1_orig;
			uint32_t devcmdvld_orig;
			uint32_t devcmd1_mod;
			uint32_t devcmdvld_mod;
			uint32_t sflash_bcfg_orig;
			uint32_t sflash_bcfg_mod;
		} data;
	} *dma_buffer;
	dmov_s *cmd;

	unsigned page_address = 0;
	int err = 0;
	dma_addr_t dma_addr_param_info = 0;
	dma_addr_t dma_addr_identifier = 0;
	unsigned cmd_set_count = 2;
	unsigned crc_chk_count = 0;

	if (msm_nand_data.nr_parts) {
		page_address = ((msm_nand_data.parts[0]).offset >> 11);
	} else {
		pr_err("flash_onfi_probe: "
				"No partition info available\n");
		err = -EIO;
		goto err_free_and_return;
	}

	onfi_identifier_buf = kmalloc(ONFI_IDENTIFIER_LENGTH, GFP_KERNEL);
	if (!onfi_identifier_buf) {
		pr_err("flash_onfi_probe: "
				"failed to alloc identifier buffer\n");
		err = -ENOMEM;
		goto err_free_and_return;
	}
	onfi_param_info_buf = kmalloc(ONFI_PARAM_INFO_LENGTH, GFP_KERNEL);
	if (!onfi_param_info_buf) {
		pr_err("flash_onfi_probe: "
				"failed to alloc param info buffer\n");
		err = -ENOMEM;
		goto err_free_and_return;
	}
	dma_addr_identifier = dma_map_single(chip->dev, onfi_identifier_buf,
			ONFI_IDENTIFIER_LENGTH, DMA_FROM_DEVICE);
	if (dma_mapping_error(chip->dev, dma_addr_identifier)) {
		pr_err("flash_onfi_probe: "
				"failed to get dma addr for %p\n",
				onfi_identifier_buf);
		err = -EIO;
		goto err_free_and_return;
	}

	dma_addr_param_info = dma_map_single(chip->dev, onfi_param_info_buf,
			ONFI_PARAM_INFO_LENGTH, DMA_FROM_DEVICE);
	if (dma_mapping_error(chip->dev, dma_addr_param_info)) {
		pr_err("flash_onfi_probe: failed to get dma addr for %p\n",
				onfi_param_info_buf);
		dma_unmap_single(chip->dev, dma_addr_identifier,
				ONFI_IDENTIFIER_LENGTH, DMA_FROM_DEVICE);
		err = -EIO;
		goto err_free_and_return;
	}

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	dma_buffer->data.sflash_bcfg_orig = flash_rd_reg
				(chip, NAND_SFLASHC_BURST_CFG);
	dma_buffer->data.devcmd1_orig = flash_rd_reg(chip, NAND_DEV_CMD1);
	dma_buffer->data.devcmdvld_orig = flash_rd_reg(chip, NAND_DEV_CMD_VLD);

	while (cmd_set_count-- > 0) {
		cmd = dma_buffer->cmd;

		dma_buffer->data.devcmd1_mod = (dma_buffer->data.devcmd1_orig &
				0xFFFFFF00) | (cmd_set_count
				? FLASH_READ_ONFI_IDENTIFIER_COMMAND
				: FLASH_READ_ONFI_PARAMETERS_COMMAND);
		dma_buffer->data.cmd = NAND_CMD_PAGE_READ;
		dma_buffer->data.addr0 = (page_address << 16) | (cmd_set_count
				? FLASH_READ_ONFI_IDENTIFIER_ADDRESS
				: FLASH_READ_ONFI_PARAMETERS_ADDRESS);
		dma_buffer->data.addr1 = (page_address >> 16) & 0xFF;
		dma_buffer->data.cfg0 =	(cmd_set_count
				? NAND_CFG0_RAW_ONFI_IDENTIFIER
				: NAND_CFG0_RAW_ONFI_PARAM_INFO);
		dma_buffer->data.cfg1 =	(cmd_set_count
				? NAND_CFG1_RAW_ONFI_IDENTIFIER
				: NAND_CFG1_RAW_ONFI_PARAM_INFO);
		dma_buffer->data.sflash_bcfg_mod = 0x00000000;
		dma_buffer->data.devcmdvld_mod = (dma_buffer->
				data.devcmdvld_orig & 0xFFFFFFFE);
		dma_buffer->data.exec = 1;
		dma_buffer->data.flash_status = 0xeeeeeeee;

		/* Put the Nand ctlr in Async mode and disable SFlash ctlr */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.sflash_bcfg_mod);
		cmd->dst = NAND_SFLASHC_BURST_CFG;
		cmd->len = 4;
		cmd++;

		/* Block on cmd ready, & write CMD,ADDR0,ADDR1,CHIPSEL regs */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.cmd);
		cmd->dst = NAND_FLASH_CMD;
		cmd->len = 12;
		cmd++;

		/* Configure the CFG0 and CFG1 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.cfg0);
		cmd->dst = NAND_DEV0_CFG0;
		cmd->len = 8;
		cmd++;

		/* Configure the DEV_CMD_VLD register */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.devcmdvld_mod);
		cmd->dst = NAND_DEV_CMD_VLD;
		cmd->len = 4;
		cmd++;

		/* Configure the DEV_CMD1 register */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.devcmd1_mod);
		cmd->dst = NAND_DEV_CMD1;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.exec);
		cmd->dst = NAND_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the two status registers */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_FLASH_STATUS;
		cmd->dst = msm_virt_to_dma(chip,
				&dma_buffer->data.flash_status);
		cmd->len = 4;
		cmd++;

		/* Read data block - valid only if status says success */
		cmd->cmd = 0;
		cmd->src = NAND_FLASH_BUFFER;
		cmd->dst = (cmd_set_count ? dma_addr_identifier :
				dma_addr_param_info);
		cmd->len = (cmd_set_count ? ONFI_IDENTIFIER_LENGTH :
				ONFI_PARAM_INFO_LENGTH);
		cmd++;

		/* Restore the DEV_CMD1 register */
		cmd->cmd = 0 ;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.devcmd1_orig);
		cmd->dst = NAND_DEV_CMD1;
		cmd->len = 4;
		cmd++;

		/* Restore the DEV_CMD_VLD register */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.devcmdvld_orig);
		cmd->dst = NAND_DEV_CMD_VLD;
		cmd->len = 4;
		cmd++;

		/* Restore the SFLASH_BURST_CONFIG register */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip,
				&dma_buffer->data.sflash_bcfg_orig);
		cmd->dst = NAND_SFLASHC_BURST_CFG;
		cmd->len = 4;
		cmd++;

		BUILD_BUG_ON(11 != ARRAY_SIZE(dma_buffer->cmd));
		BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
		dma_buffer->cmd[0].cmd |= CMD_OCB;
		cmd[-1].cmd |= CMD_OCU | CMD_LC;

		dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd)
				>> 3) | CMD_PTR_LP;

		msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST
				| DMOV_CMD_ADDR(msm_virt_to_dma(chip,
				&dma_buffer->cmdptr)));

		/* Check for errors, protection violations etc */
		if (dma_buffer->data.flash_status & 0x110) {
			pr_info("MPU/OP error (0x%x) during "
					"ONFI probe\n",
					dma_buffer->data.flash_status);
			err = -EIO;
			break;
		}

		if (cmd_set_count) {
			onfi_param_page_ptr = (struct onfi_param_page *)
				(&(onfi_identifier_buf[0]));
			if (onfi_param_page_ptr->parameter_page_signature !=
					ONFI_PARAMETER_PAGE_SIGNATURE) {
				pr_info("ONFI probe : Found a non"
						"ONFI Compliant device \n");
				err = -EIO;
				break;
			}
		} else {
			for (crc_chk_count = 0; crc_chk_count <
					ONFI_PARAM_INFO_LENGTH
					/ ONFI_PARAM_PAGE_LENGTH;
					crc_chk_count++) {
				onfi_param_page_ptr =
					(struct onfi_param_page *)
					(&(onfi_param_info_buf
					[ONFI_PARAM_PAGE_LENGTH *
					crc_chk_count]));
				if (flash_onfi_crc_check(
					(uint8_t *)onfi_param_page_ptr,
					ONFI_PARAM_PAGE_LENGTH - 2) ==
					onfi_param_page_ptr->integrity_crc) {
					break;
				}
			}
			if (crc_chk_count >= ONFI_PARAM_INFO_LENGTH
					/ ONFI_PARAM_PAGE_LENGTH) {
				pr_info("ONFI probe : CRC Check "
						"failed on ONFI Parameter "
						"data \n");
				err = -EIO;
				break;
			} else {
				supported_flash[0].flash_id =
					flash_read_id(chip);
				supported_flash[0].widebus  =
					onfi_param_page_ptr->
					features_supported & 0x01;
				supported_flash[0].pagesize =
					onfi_param_page_ptr->
					number_of_data_bytes_per_page;
				supported_flash[0].blksize  =
					onfi_param_page_ptr->
					number_of_pages_per_block *
					supported_flash[0].pagesize;
				supported_flash[0].oobsize  =
					onfi_param_page_ptr->
					number_of_spare_bytes_per_page;
				supported_flash[0].density  =
					onfi_param_page_ptr->
					number_of_blocks_per_logical_unit
					* supported_flash[0].blksize;

				pr_info("ONFI probe : Found an ONFI "
					"compliant device %s\n",
					onfi_param_page_ptr->device_model);
			}
		}
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));
	dma_unmap_single(chip->dev, dma_addr_identifier,
			ONFI_IDENTIFIER_LENGTH, DMA_FROM_DEVICE);
	dma_unmap_single(chip->dev, dma_addr_param_info,
			ONFI_PARAM_INFO_LENGTH, DMA_FROM_DEVICE);

err_free_and_return:
	kfree(onfi_identifier_buf);
	kfree(onfi_param_info_buf);
	return err;
}

static int msm_nand_read_oob(struct mtd_info *mtd, loff_t from,
			     struct mtd_oob_ops *ops)
{
	struct msm_nand_chip *chip = mtd->priv;

	struct {
		dmov_s cmd[4 * 5 + 2];
		unsigned cmdptr;
		struct {
			uint32_t cmd;
			uint32_t addr0;
			uint32_t addr1;
			uint32_t chipsel;
			uint32_t cfg0;
			uint32_t cfg1;
			uint32_t exec;
			uint32_t ecccfg;
			struct {
				uint32_t flash_status;
				uint32_t buffer_status;
			} result[4];
		} data;
	} *dma_buffer;
	dmov_s *cmd;
	unsigned n;
	unsigned page = from / 2048;
	uint32_t oob_len = ops->ooblen;
	uint32_t sectordatasize;
	uint32_t sectoroobsize;
	int err, pageerr, rawerr;
	dma_addr_t data_dma_addr = 0;
	dma_addr_t oob_dma_addr = 0;
	dma_addr_t data_dma_addr_curr = 0;
	dma_addr_t oob_dma_addr_curr = 0;
	uint32_t oob_col = 0;
	unsigned page_count;
	unsigned pages_read = 0;
	unsigned start_sector = 0;
	uint32_t ecc_errors;
	uint32_t total_ecc_errors = 0;

	
	if (from & (mtd->writesize - 1)) {
		pr_err("%s: unsupported from, 0x%llx\n",
		       __func__, from);
		return -EINVAL;
	}
	if (ops->mode != MTD_OOB_RAW) {
		if (ops->datbuf != NULL && (ops->len % mtd->writesize) != 0) {
			/* when ops->datbuf is NULL, ops->len can be ooblen */
			pr_err("%s: unsupported ops->len, %d\n",
			       __func__, ops->len);
			return -EINVAL;
		}
	} else {
		if (ops->datbuf != NULL &&
			(ops->len % (mtd->writesize + mtd->oobsize)) != 0) {
			pr_err("%s: unsupported ops->len,"
				" %d for MTD_OOB_RAW\n", __func__, ops->len);
			return -EINVAL;
		}
	}

	if (ops->mode != MTD_OOB_RAW && ops->ooblen != 0 && ops->ooboffs != 0) {
		pr_err("%s: unsupported ops->ooboffs, %d\n",
		       __func__, ops->ooboffs);
		return -EINVAL;
	}

	if (ops->oobbuf && !ops->datbuf && ops->mode == MTD_OOB_AUTO)
		start_sector = 3;

	if (ops->oobbuf && !ops->datbuf)
		page_count = ops->ooblen / ((ops->mode == MTD_OOB_AUTO) ?
			mtd->oobavail : mtd->oobsize);
	else if (ops->mode != MTD_OOB_RAW)
		page_count = ops->len / mtd->writesize;
	else
		page_count = ops->len / (mtd->writesize + mtd->oobsize);

#if 0 /* yaffs reads more oob data than it needs */
	if (ops->ooblen >= sectoroobsize * 4) {
		pr_err("%s: unsupported ops->ooblen, %d\n",
		       __func__, ops->ooblen);
		return -EINVAL;
	}
#endif
#if VERBOSE
	pr_info("msm_nand_read_oob %llx %p %x %p %x\n",
		from, ops->datbuf, ops->len, ops->oobbuf, ops->ooblen);
#endif
	if (ops->datbuf) {
		/* memset(ops->datbuf, 0x55, ops->len); */
		data_dma_addr_curr = data_dma_addr =
			dma_map_single(chip->dev, ops->datbuf, ops->len,
				       DMA_FROM_DEVICE);
		if (dma_mapping_error(chip->dev, data_dma_addr)) {
			pr_err("msm_nand_read_oob: failed to get dma addr "
			       "for %p\n", ops->datbuf);
			return -EIO;
		}
	}
	if (ops->oobbuf) {
		memset(ops->oobbuf, 0xff, ops->ooblen);
		oob_dma_addr_curr = oob_dma_addr =
			dma_map_single(chip->dev, ops->oobbuf,
				       ops->ooblen, DMA_BIDIRECTIONAL);
		if (dma_mapping_error(chip->dev, oob_dma_addr)) {
			pr_err("msm_nand_read_oob: failed to get dma addr "
			       "for %p\n", ops->oobbuf);
			err = -EIO;
			goto err_dma_map_oobbuf_failed;
		}
	}

	wait_event(chip->wait_queue,
		   (dma_buffer = msm_nand_get_dma_buffer(
			    chip, sizeof(*dma_buffer))));

	oob_col = start_sector * 0x210;
	if (chip->CFG1 & CFG1_WIDE_FLASH)
		oob_col >>= 1;

	err = 0;
	while (page_count-- > 0) {
		cmd = dma_buffer->cmd;

		/* CMD / ADDR0 / ADDR1 / CHIPSEL program values */
		if (ops->mode != MTD_OOB_RAW) {
			dma_buffer->data.cmd = NAND_CMD_PAGE_READ_ECC;
			dma_buffer->data.cfg0 =
			(chip->CFG0 & ~(7U << 6)) | ((3U - start_sector) << 6);
			dma_buffer->data.cfg1 = chip->CFG1;
		} else {
			dma_buffer->data.cmd = NAND_CMD_PAGE_READ;
			dma_buffer->data.cfg0 = NAND_CFG0_RAW;
			dma_buffer->data.cfg1 = NAND_CFG1_RAW |
						(chip->CFG1 & CFG1_WIDE_FLASH);
		}

		dma_buffer->data.addr0 = (page << 16) | oob_col;
		/* qc example is (page >> 16) && 0xff !? */
		dma_buffer->data.addr1 = (page >> 16) & 0xff;
		/* flash0 + undoc bit */
		dma_buffer->data.chipsel = 0 | 4;


		/* GO bit for the EXEC register */
		dma_buffer->data.exec = 1;


		BUILD_BUG_ON(4 != ARRAY_SIZE(dma_buffer->data.result));

		for (n = start_sector; n < 4; n++) {
			/* flash + buffer status return words */
			dma_buffer->data.result[n].flash_status = 0xeeeeeeee;
			dma_buffer->data.result[n].buffer_status = 0xeeeeeeee;

			/* block on cmd ready, then
			 * write CMD / ADDR0 / ADDR1 / CHIPSEL
			 * regs in a burst
			 */
			cmd->cmd = DST_CRCI_NAND_CMD;
			cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.cmd);
			cmd->dst = NAND_FLASH_CMD;
			if (n == start_sector)
				cmd->len = 16;
			else
				cmd->len = 4;
			cmd++;

			if (n == start_sector) {
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
							&dma_buffer->data.cfg0);
				cmd->dst = NAND_DEV0_CFG0;
				cmd->len = 8;
				cmd++;

				dma_buffer->data.ecccfg = chip->ecc_buf_cfg;
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.ecccfg);
				cmd->dst = NAND_EBI2_ECC_BUF_CFG;
				cmd->len = 4;
				cmd++;
			}

			/* kick the execute register */
			cmd->cmd = 0;
			cmd->src =
				msm_virt_to_dma(chip, &dma_buffer->data.exec);
			cmd->dst = NAND_EXEC_CMD;
			cmd->len = 4;
			cmd++;

			/* block on data ready, then
			 * read the status register
			 */
			cmd->cmd = SRC_CRCI_NAND_DATA;
			cmd->src = NAND_FLASH_STATUS;
			cmd->dst = msm_virt_to_dma(chip,
						   &dma_buffer->data.result[n]);
			/* NAND_FLASH_STATUS + NAND_BUFFER_STATUS */
			cmd->len = 8;
			cmd++;

			/* read data block
			 * (only valid if status says success)
			 */
			if (ops->datbuf) {
				if (ops->mode != MTD_OOB_RAW)
					sectordatasize = (n < 3) ? 516 : 500;
				else
					sectordatasize = 528;

				cmd->cmd = 0;
				cmd->src = NAND_FLASH_BUFFER;
				cmd->dst = data_dma_addr_curr;
				data_dma_addr_curr += sectordatasize;
				cmd->len = sectordatasize;
				cmd++;
			}

			if (ops->oobbuf &&
			    (n == 3 || ops->mode != MTD_OOB_AUTO)) {
				cmd->cmd = 0;
				if (n == 3) {
					cmd->src = NAND_FLASH_BUFFER + 500;
					sectoroobsize = 16;
					if (ops->mode != MTD_OOB_AUTO)
						sectoroobsize += 10;
				} else {
					cmd->src = NAND_FLASH_BUFFER + 516;
					sectoroobsize = 10;
				}

				cmd->dst = oob_dma_addr_curr;
				if (sectoroobsize < oob_len)
					cmd->len = sectoroobsize;
				else
					cmd->len = oob_len;
				oob_dma_addr_curr += cmd->len;
				oob_len -= cmd->len;
				if (cmd->len > 0)
					cmd++;
			}
		}

		BUILD_BUG_ON(4 * 5 + 2 != ARRAY_SIZE(dma_buffer->cmd));
		BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
		dma_buffer->cmd[0].cmd |= CMD_OCB;
		cmd[-1].cmd |= CMD_OCU | CMD_LC;

		dma_buffer->cmdptr =
			(msm_virt_to_dma(chip, dma_buffer->cmd) >> 3)
			| CMD_PTR_LP;

		msm_dmov_exec_cmd(
			chip->dma_channel, DMOV_CMD_PTR_LIST | DMOV_CMD_ADDR(
				msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

		/* if any of the writes failed (0x10), or there
		 * was a protection violation (0x100), we lose
		 */
		pageerr = rawerr = 0;
		for (n = start_sector; n < 4; n++) {
			if (dma_buffer->data.result[n].flash_status & 0x110) {
				rawerr = -EIO;
				break;
			}
		}
		if (rawerr) {
			if (ops->datbuf && ops->mode != MTD_OOB_RAW) {
				uint8_t *datbuf =
					ops->datbuf + pages_read * 2048;
				for (n = 0; n < 2048; n++) {
					/* empty blocks read 0x54 at
					 * these offsets
					 */
					if (n % 516 == 3 && datbuf[n] == 0x54)
						datbuf[n] = 0xff;
					if (datbuf[n] != 0xff) {
						pageerr = rawerr;
						break;
					}
				}
			}
			if (ops->oobbuf) {
				for (n = 0; n < ops->ooblen; n++) {
					if (ops->oobbuf[n] != 0xff) {
						pageerr = rawerr;
						break;
					}
				}
			}
		}
		if (pageerr) {
			for (n = start_sector; n < 4; n++) {
				if (dma_buffer->data.result[n].buffer_status
						& 0x8) {
					/* not thread safe */
					mtd->ecc_stats.failed++;
					pageerr = -EBADMSG;
					break;
				}
			}
		}
		if (!rawerr) { /* check for corretable errors */
			for (n = start_sector; n < 4; n++) {
				ecc_errors = dma_buffer->data.
					result[n].buffer_status & 0x7;
				if (ecc_errors) {
					total_ecc_errors += ecc_errors;
					/* not thread safe */
					mtd->ecc_stats.corrected += ecc_errors;
					if (ecc_errors > 1)
						pageerr = -EUCLEAN;
				}
			}
		}
		if (pageerr && (pageerr != -EUCLEAN || err == 0))
			err = pageerr;

#if VERBOSE
//#if 1
		if (rawerr && !pageerr) {
			pr_err("msm_nand_read_oob %llx %x %x empty page\n",
			       (loff_t)page * mtd->writesize, ops->len,
			       ops->ooblen);
		} else {
			pr_info("status: %x %x %x %x %x %x %x %x\n",
				dma_buffer->data.result[0].flash_status,
				dma_buffer->data.result[0].buffer_status,
				dma_buffer->data.result[1].flash_status,
				dma_buffer->data.result[1].buffer_status,
				dma_buffer->data.result[2].flash_status,
				dma_buffer->data.result[2].buffer_status,
				dma_buffer->data.result[3].flash_status,
				dma_buffer->data.result[3].buffer_status);
		}
#endif
		if (err && err != -EUCLEAN && err != -EBADMSG)
			break;
		pages_read++;
		page++;
	}
	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	if (ops->oobbuf) {
		dma_unmap_single(chip->dev, oob_dma_addr,
				 ops->ooblen, DMA_FROM_DEVICE);
	}
err_dma_map_oobbuf_failed:
	if (ops->datbuf) {
		dma_unmap_single(chip->dev, data_dma_addr,
				 ops->len, DMA_FROM_DEVICE);
	}

	if (ops->mode != MTD_OOB_RAW)
		ops->retlen = mtd->writesize * pages_read;
	else
		ops->retlen = (mtd->writesize +  mtd->oobsize) *
							pages_read;
	ops->oobretlen = ops->ooblen - oob_len;
	if (err)
		pr_err("msm_nand_read_oob %llx %x %x failed %d, corrected %d\n",
		       from, ops->datbuf ? ops->len : 0, ops->ooblen, err,
		       total_ecc_errors);
	return err;
}

static int
msm_nand_read(struct mtd_info *mtd, loff_t from, size_t len,
	      size_t *retlen, u_char *buf)
{
	int ret;
	struct mtd_oob_ops ops;

	 printk("msm_nand_read %llx %x\n", from, len); 

	ops.mode = MTD_OOB_PLACE;
	ops.len = len;
	ops.retlen = 0;
	ops.ooblen = 0;
	ops.datbuf = buf;
	ops.oobbuf = NULL;
	ret =  msm_nand_read_oob(mtd, from, &ops);
	*retlen = ops.retlen;
	return ret;
}

static int
msm_nand_write_oob(struct mtd_info *mtd, loff_t to, struct mtd_oob_ops *ops)
{
	struct msm_nand_chip *chip = mtd->priv;
	struct {
		dmov_s cmd[4 * 5 + 2];
		unsigned cmdptr;
		struct {
			uint32_t cmd;
			uint32_t addr0;
			uint32_t addr1;
			uint32_t chipsel;
			uint32_t cfg0;
			uint32_t cfg1;
			uint32_t exec;
			uint32_t ecccfg;
			uint32_t flash_status[4];
		} data;
	} *dma_buffer;
	dmov_s *cmd;
	unsigned n;
	unsigned page = to / 2048;
	uint32_t oob_len = ops->ooblen;
	uint32_t sectordatawritesize;
	int err;
	dma_addr_t data_dma_addr = 0;
	dma_addr_t oob_dma_addr = 0;
	dma_addr_t data_dma_addr_curr = 0;
	dma_addr_t oob_dma_addr_curr = 0;
	unsigned page_count;
	unsigned pages_written = 0;

	if (to & (mtd->writesize - 1)) {
		pr_err("%s: unsupported to, 0x%llx\n", __func__, to);
		return -EINVAL;
	}

	if (ops->mode != MTD_OOB_RAW) {
		if (ops->ooblen != 0 && ops->mode != MTD_OOB_AUTO) {
			pr_err("%s: unsupported ops->mode,%d\n",
					 __func__, ops->mode);
			return -EINVAL;
		}
		if ((ops->len % mtd->writesize) != 0) {
			pr_err("%s: unsupported ops->len, %d\n",
					__func__, ops->len);
			return -EINVAL;
		}
	} else {
		if ((ops->len % (mtd->writesize + mtd->oobsize)) != 0) {
			pr_err("%s: unsupported ops->len, "
				"%d for MTD_OOB_RAW mode\n",
				 __func__, ops->len);
			return -EINVAL;
		}
	}

	if (ops->datbuf == NULL) {
		pr_err("%s: unsupported ops->datbuf == NULL\n", __func__);
		return -EINVAL;
	}
#if 0 /* yaffs writes more oob data than it needs */
	if (ops->ooblen >= sectoroobsize * 4) {
		pr_err("%s: unsupported ops->ooblen, %d\n",
		       __func__, ops->ooblen);
		return -EINVAL;
	}
#endif
	if (ops->mode != MTD_OOB_RAW && ops->ooblen != 0 && ops->ooboffs != 0) {
		pr_err("%s: unsupported ops->ooboffs, %d\n",
		       __func__, ops->ooboffs);
		return -EINVAL;
	}

	if (ops->datbuf) {
		data_dma_addr_curr = data_dma_addr =
			dma_map_single(chip->dev, ops->datbuf,
				       ops->len, DMA_TO_DEVICE);
		if (dma_mapping_error(chip->dev, data_dma_addr)) {
			pr_err("msm_nand_write_oob: failed to get dma addr "
			       "for %p\n", ops->datbuf);
			return -EIO;
		}
	}
	if (ops->oobbuf) {
		oob_dma_addr_curr = oob_dma_addr =
			dma_map_single(chip->dev, ops->oobbuf,
				       ops->ooblen, DMA_TO_DEVICE);
		if (dma_mapping_error(chip->dev, oob_dma_addr)) {
			pr_err("msm_nand_write_oob: failed to get dma addr "
			       "for %p\n", ops->oobbuf);
			err = -EIO;
			goto err_dma_map_oobbuf_failed;
		}
	}
	if (ops->mode != MTD_OOB_RAW)
		page_count = ops->len / mtd->writesize;
	else
		page_count = ops->len / (mtd->writesize + mtd->oobsize);

	wait_event(chip->wait_queue, (dma_buffer =
			msm_nand_get_dma_buffer(chip, sizeof(*dma_buffer))));

	while (page_count-- > 0) {
		cmd = dma_buffer->cmd;

		/* CMD / ADDR0 / ADDR1 / CHIPSEL program values */
		if (ops->mode != MTD_OOB_RAW) {
			dma_buffer->data.cfg0 = chip->CFG0;
			dma_buffer->data.cfg1 = chip->CFG1;
		} else {
			dma_buffer->data.cfg0 = NAND_CFG0_RAW;
			dma_buffer->data.cfg1 = NAND_CFG1_RAW |
						(chip->CFG1 & CFG1_WIDE_FLASH);
		}

		dma_buffer->data.cmd = NAND_CMD_PRG_PAGE;
		dma_buffer->data.addr0 = page << 16;
		dma_buffer->data.addr1 = (page >> 16) & 0xff;
		dma_buffer->data.chipsel = 0 | 4; /* flash0 + undoc bit */


			/* GO bit for the EXEC register */
		dma_buffer->data.exec = 1;

		BUILD_BUG_ON(4 != ARRAY_SIZE(dma_buffer->data.flash_status));

		for (n = 0; n < 4; n++) {
			/* status return words */
			dma_buffer->data.flash_status[n] = 0xeeeeeeee;
			/* block on cmd ready, then
			 * write CMD / ADDR0 / ADDR1 / CHIPSEL regs in a burst
			 */
			cmd->cmd = DST_CRCI_NAND_CMD;
			cmd->src =
				msm_virt_to_dma(chip, &dma_buffer->data.cmd);
			cmd->dst = NAND_FLASH_CMD;
			if (n == 0)
				cmd->len = 16;
			else
				cmd->len = 4;
			cmd++;

			if (n == 0) {
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
							&dma_buffer->data.cfg0);
				cmd->dst = NAND_DEV0_CFG0;
				cmd->len = 8;
				cmd++;

				dma_buffer->data.ecccfg = chip->ecc_buf_cfg;
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						 &dma_buffer->data.ecccfg);
				cmd->dst = NAND_EBI2_ECC_BUF_CFG;
				cmd->len = 4;
				cmd++;
			}

				/* write data block */
			if (ops->mode != MTD_OOB_RAW)
				sectordatawritesize = (n < 3) ? 516 : 500;
			else
				sectordatawritesize = 528;

			cmd->cmd = 0;
			cmd->src = data_dma_addr_curr;
			data_dma_addr_curr += sectordatawritesize;
			cmd->dst = NAND_FLASH_BUFFER;
			cmd->len = sectordatawritesize;
			cmd++;

			if (ops->oobbuf) {
				if (n == 3) {
					cmd->cmd = 0;
					cmd->src = oob_dma_addr_curr;
					cmd->dst = NAND_FLASH_BUFFER + 500;
					if (16 < oob_len)
						cmd->len = 16;
					else
						cmd->len = oob_len;
					oob_dma_addr_curr += cmd->len;
					oob_len -= cmd->len;
					if (cmd->len > 0)
						cmd++;
				}
				if (ops->mode != MTD_OOB_AUTO) {
					/* skip ecc bytes in oobbuf */
					if (oob_len < 10) {
						oob_dma_addr_curr += 10;
						oob_len -= 10;
					} else {
						oob_dma_addr_curr += oob_len;
						oob_len = 0;
					}
				}
			}

			/* kick the execute register */
			cmd->cmd = 0;
			cmd->src =
				msm_virt_to_dma(chip, &dma_buffer->data.exec);
			cmd->dst = NAND_EXEC_CMD;
			cmd->len = 4;
			cmd++;

			/* block on data ready, then
			 * read the status register
			 */
			cmd->cmd = SRC_CRCI_NAND_DATA;
			cmd->src = NAND_FLASH_STATUS;
			cmd->dst = msm_virt_to_dma(chip,
					     &dma_buffer->data.flash_status[n]);
			cmd->len = 4;
			cmd++;
		}

		dma_buffer->cmd[0].cmd |= CMD_OCB;
		cmd[-1].cmd |= CMD_OCU | CMD_LC;
		BUILD_BUG_ON(4 * 5 + 2 != ARRAY_SIZE(dma_buffer->cmd));
		BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
		dma_buffer->cmdptr =
			(msm_virt_to_dma(chip, dma_buffer->cmd) >> 3) |
			CMD_PTR_LP;

		msm_dmov_exec_cmd(chip->dma_channel,
			DMOV_CMD_PTR_LIST | DMOV_CMD_ADDR(
				msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

		/* if any of the writes failed (0x10), or there was a
		 * protection violation (0x100), or the program success
		 * bit (0x80) is unset, we lose
		 */
		err = 0;
		for (n = 0; n < 4; n++) {
			if (dma_buffer->data.flash_status[n] & 0x110) {
				err = -EIO;
				break;
			}
			if (!(dma_buffer->data.flash_status[n] & 0x80)) {
				err = -EIO;
				break;
			}
		}

#if VERBOSE
		pr_info("write page %d: status: %x %x %x %x\n", page,
			dma_buffer->data.flash_status[0],
			dma_buffer->data.flash_status[1],
			dma_buffer->data.flash_status[2],
			dma_buffer->data.flash_status[3]);
#endif
		if (err)
			break;
		pages_written++;
		page++;
	}
	if (ops->mode != MTD_OOB_RAW)
		ops->retlen = mtd->writesize * pages_written;
	else
		ops->retlen = (mtd->writesize + mtd->oobsize) * pages_written;

	ops->oobretlen = ops->ooblen - oob_len;

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	if (ops->oobbuf)
		dma_unmap_single(chip->dev, oob_dma_addr,
				 ops->ooblen, DMA_TO_DEVICE);
err_dma_map_oobbuf_failed:
	if (ops->datbuf)
		dma_unmap_single(chip->dev, data_dma_addr, 2048, DMA_TO_DEVICE);
	if (err)
		pr_err("msm_nand_write_oob %llx %x %x failed %d\n",
		       to, ops->len, ops->ooblen, err);
	return err;
}

static int msm_nand_write(struct mtd_info *mtd, loff_t to, size_t len,
			  size_t *retlen, const u_char *buf)
{
	int ret;
	struct mtd_oob_ops ops;

	ops.mode = MTD_OOB_PLACE;
	ops.len = len;
	ops.retlen = 0;
	ops.ooblen = 0;
	ops.datbuf = (uint8_t *)buf;
	ops.oobbuf = NULL;
	ret =  msm_nand_write_oob(mtd, to, &ops);
	*retlen = ops.retlen;
	return ret;
}

static int
msm_nand_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	int err;
	struct msm_nand_chip *chip = mtd->priv;
	struct {
		dmov_s cmd[4];
		unsigned cmdptr;
		unsigned data[8];
	} *dma_buffer;
	unsigned page = instr->addr / 2048;

	if (instr->addr & (mtd->erasesize - 1)) {
		pr_err("%s: unsupported erase address, 0x%x\n",
		       __func__, instr->addr);
		return -EINVAL;
	}
	if (instr->len != mtd->erasesize) {
		pr_err("%s: unsupported erase len, %d\n",
		       __func__, instr->len);
		return -EINVAL;
	}

	wait_event(chip->wait_queue,
		   (dma_buffer = msm_nand_get_dma_buffer(
			    chip, sizeof(*dma_buffer))));

	dma_buffer->data[0] = NAND_CMD_BLOCK_ERASE;
	dma_buffer->data[1] = page;
	dma_buffer->data[2] = 0;
	dma_buffer->data[3] = 0 | 4;
	dma_buffer->data[4] = 1;
	dma_buffer->data[5] = 0xeeeeeeee;
	dma_buffer->data[6] = chip->CFG0 & (~(7 << 6));  /* CW_PER_PAGE = 0 */
	dma_buffer->data[7] = chip->CFG1;
	BUILD_BUG_ON(7 != ARRAY_SIZE(dma_buffer->data) - 1);

	dma_buffer->cmd[0].cmd = DST_CRCI_NAND_CMD | CMD_OCB;
	dma_buffer->cmd[0].src = msm_virt_to_dma(chip, &dma_buffer->data[0]);
	dma_buffer->cmd[0].dst = NAND_FLASH_CMD;
	dma_buffer->cmd[0].len = 16;

	dma_buffer->cmd[1].cmd = 0;
	dma_buffer->cmd[1].src = msm_virt_to_dma(chip, &dma_buffer->data[6]);
	dma_buffer->cmd[1].dst = NAND_DEV0_CFG0;
	dma_buffer->cmd[1].len = 8;

	dma_buffer->cmd[2].cmd = 0;
	dma_buffer->cmd[2].src = msm_virt_to_dma(chip, &dma_buffer->data[4]);
	dma_buffer->cmd[2].dst = NAND_EXEC_CMD;
	dma_buffer->cmd[2].len = 4;

	dma_buffer->cmd[3].cmd = SRC_CRCI_NAND_DATA | CMD_OCU | CMD_LC;;
	dma_buffer->cmd[3].src = NAND_FLASH_STATUS;
	dma_buffer->cmd[3].dst = msm_virt_to_dma(chip, &dma_buffer->data[5]);
	dma_buffer->cmd[3].len = 4;

	BUILD_BUG_ON(3 != ARRAY_SIZE(dma_buffer->cmd) - 1);

	dma_buffer->cmdptr =
		(msm_virt_to_dma(chip, dma_buffer->cmd) >> 3) | CMD_PTR_LP;

	msm_dmov_exec_cmd(
		chip->dma_channel, DMOV_CMD_PTR_LIST |
		DMOV_CMD_ADDR(msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

	/* we fail if there was an operation error, a mpu error, or the
	 * erase success bit was not set.
	 */

	if (dma_buffer->data[5] & 0x110 || !(dma_buffer->data[5] & 0x80))
		err = -EIO;
	else
		err = 0;

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));
	if (err) {
		pr_err("%s: erase failed, 0x%x\n", __func__, instr->addr);
		instr->fail_addr = instr->addr;
		instr->state = MTD_ERASE_FAILED;
	} else {
		instr->state = MTD_ERASE_DONE;
		instr->fail_addr = 0xffffffff;
		mtd_erase_callback(instr);
	}
	return err;
}

static int
msm_nand_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct msm_nand_chip *chip = mtd->priv;
	int ret;
	struct {
		dmov_s cmd[5];
		unsigned cmdptr;
		struct {
			uint32_t cmd;
			uint32_t addr0;
			uint32_t addr1;
			uint32_t chipsel;
			uint32_t cfg0;
			uint32_t cfg1;
			uint32_t exec;
			uint32_t ecccfg;
			struct {
				uint32_t flash_status;
				uint32_t buffer_status;
			} result;
		} data;
	} *dma_buffer;
	dmov_s *cmd;
	uint8_t *buf;
	unsigned page = ofs / 2048;

	/* Check for invalid offset */
	if (ofs > mtd->size)
		return -EINVAL;
	if (ofs & (mtd->erasesize - 1)) {
		pr_err("%s: unsupported block address, 0x%x\n",
			 __func__, (uint32_t)ofs);
		return -EINVAL;
	}

	wait_event(chip->wait_queue,
		(dma_buffer = msm_nand_get_dma_buffer(chip ,
					 sizeof(*dma_buffer) + 4)));
	buf = (uint8_t *)dma_buffer + sizeof(*dma_buffer);

	/* Read 4 bytes starting from the bad block marker location
	 * in the last code word of the page
	 */

	cmd = dma_buffer->cmd;

	dma_buffer->data.cmd = NAND_CMD_PAGE_READ;
	dma_buffer->data.cfg0 = NAND_CFG0_RAW & ~(7U << 6);
	dma_buffer->data.cfg1 = NAND_CFG1_RAW | (chip->CFG1 & CFG1_WIDE_FLASH);

	if (chip->CFG1 & CFG1_WIDE_FLASH)
		dma_buffer->data.addr0 = (page << 16) | (0x630 >> 1);
	else
		dma_buffer->data.addr0 = (page << 16) | 0x630;

	dma_buffer->data.addr1 = (page >> 16) & 0xff;
	dma_buffer->data.chipsel = 0 | 4;

	dma_buffer->data.exec = 1;

	dma_buffer->data.result.flash_status = 0xeeeeeeee;
	dma_buffer->data.result.buffer_status = 0xeeeeeeee;

	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.cmd);
	cmd->dst = NAND_FLASH_CMD;
	cmd->len = 16;
	cmd++;

	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.cfg0);
	cmd->dst = NAND_DEV0_CFG0;
	cmd->len = 8;
	cmd++;

	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.exec);
	cmd->dst = NAND_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_FLASH_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.result);
	cmd->len = 8;
	cmd++;

	cmd->cmd = 0;
	cmd->src = NAND_FLASH_BUFFER + 464;
	cmd->dst = msm_virt_to_dma(chip, buf);
	cmd->len = 4;
	cmd++;

	BUILD_BUG_ON(5 != ARRAY_SIZE(dma_buffer->cmd));
	BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
	dma_buffer->cmd[0].cmd |= CMD_OCB;
	cmd[-1].cmd |= CMD_OCU | CMD_LC;

	dma_buffer->cmdptr = (msm_virt_to_dma(chip,
				dma_buffer->cmd) >> 3) | CMD_PTR_LP;

	msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST |
		DMOV_CMD_ADDR(msm_virt_to_dma(chip, &dma_buffer->cmdptr)));

	ret = 0;
	if (dma_buffer->data.result.flash_status & 0x110)
		ret = -EIO;

	if (!ret) {
		/* Check for bad block marker byte */
		if (chip->CFG1 & CFG1_WIDE_FLASH) {
			if (buf[0] != 0xFF || buf[1] != 0xFF)
				ret = 1;
		} else {
			if (buf[0] != 0xFF)
				ret = 1;
		}
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer) + 4);


	return ret;
}


static int
msm_nand_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_oob_ops ops;
	int ret;
	uint8_t *buf;

	/* Check for invalid offset */
	if (ofs > mtd->size)
		return -EINVAL;
	if (ofs & (mtd->erasesize - 1)) {
		pr_err("%s: unsupported block address, 0x%x\n",
				 __func__, (uint32_t)ofs);
		return -EINVAL;
	}

	/*
	Write all 0s to the first page
	This will set the BB marker to 0
	*/
	buf = page_address(ZERO_PAGE());

	ops.mode = MTD_OOB_RAW;
	ops.len = mtd->writesize + mtd->oobsize;
	ops.retlen = 0;
	ops.ooblen = 0;
	ops.datbuf = buf;
	ops.oobbuf = NULL;
	ret =  msm_nand_write_oob(mtd, ofs, &ops);

	return ret;
}

/**
 * msm_nand_suspend - [MTD Interface] Suspend the msm_nand flash
 * @param mtd		MTD device structure
 */
static int msm_nand_suspend(struct mtd_info *mtd)
{
	return 0;
}

/**
 * msm_nand_resume - [MTD Interface] Resume the msm_nand flash
 * @param mtd		MTD device structure
 */
static void msm_nand_resume(struct mtd_info *mtd)
{
}

struct onenand_information {
	uint16_t manufacturer_id;
	uint16_t device_id;
	uint16_t version_id;
	uint16_t data_buf_size;
	uint16_t boot_buf_size;
	uint16_t num_of_buffers;
	uint16_t technology;
};

static struct onenand_information onenand_info;

uint32_t flash_onenand_probe(struct msm_nand_chip *chip)
{
	struct {
		dmov_s cmd[7];
		unsigned cmdptr;
		struct {
			uint32_t bcfg;
			uint32_t cmd;
			uint32_t exec;
			uint32_t status;
			uint32_t addr0;
			uint32_t addr1;
			uint32_t addr2;
			uint32_t addr3;
			uint32_t addr4;
			uint32_t addr5;
			uint32_t addr6;
			uint32_t data0;
			uint32_t data1;
			uint32_t data2;
			uint32_t data3;
			uint32_t data4;
			uint32_t data5;
			uint32_t data6;
		} data;
	} *dma_buffer;
	dmov_s *cmd;

	int err = 0;

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	cmd = dma_buffer->cmd;

	dma_buffer->data.bcfg = SFLASH_BCFG;
	dma_buffer->data.cmd = SFLASH_PREPCMD(7, 0, 0,
						NAND_SFCMD_DATXS,
						NAND_SFCMD_ASYNC,
						NAND_SFCMD_REGRD);
	dma_buffer->data.exec = 1;
	dma_buffer->data.status = CLEAN_DATA_32;
	dma_buffer->data.addr0 = (ONENAND_DEVICE_ID << 16) |
						(ONENAND_MANUFACTURER_ID);
	dma_buffer->data.addr1 = (ONENAND_DATA_BUFFER_SIZE << 16) |
						(ONENAND_VERSION_ID);
	dma_buffer->data.addr2 = (ONENAND_AMOUNT_OF_BUFFERS << 16) |
						(ONENAND_BOOT_BUFFER_SIZE);
	dma_buffer->data.addr3 = (CLEAN_DATA_16 << 16) |
						(ONENAND_TECHNOLOGY << 0);
	dma_buffer->data.data0 = CLEAN_DATA_32;
	dma_buffer->data.data1 = CLEAN_DATA_32;
	dma_buffer->data.data2 = CLEAN_DATA_32;
	dma_buffer->data.data3 = CLEAN_DATA_32;

	/* Enable and configure the SFlash controller */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.bcfg);
	cmd->dst = NAND_SFLASHC_BURST_CFG;
	cmd->len = 4;
	cmd++;

	/* Block on cmd ready and write CMD register */
	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.cmd);
	cmd->dst = NAND_SFLASHC_CMD;
	cmd->len = 4;
	cmd++;

	/* Configure the ADDR0 and ADDR1 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr0);
	cmd->dst = NAND_ADDR0;
	cmd->len = 8;
	cmd++;

	/* Configure the ADDR2 and ADDR3 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr2);
	cmd->dst = NAND_ADDR2;
	cmd->len = 8;
	cmd++;

	/* Kick the execute command */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.exec);
	cmd->dst = NAND_SFLASHC_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	/* Block on data ready, and read the two status registers */
	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_SFLASHC_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.status);
	cmd->len = 4;
	cmd++;

	/* Read data registers - valid only if status says success */
	cmd->cmd = 0;
	cmd->src = NAND_GENP_REG0;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data0);
	cmd->len = 16;
	cmd++;

	BUILD_BUG_ON(7 != ARRAY_SIZE(dma_buffer->cmd));
	BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
	dma_buffer->cmd[0].cmd |= CMD_OCB;
	cmd[-1].cmd |= CMD_OCU | CMD_LC;

	dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd)
			>> 3) | CMD_PTR_LP;

	msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST
			| DMOV_CMD_ADDR(msm_virt_to_dma(chip,
			&dma_buffer->cmdptr)));

	/* Check for errors, protection violations etc */
	if (dma_buffer->data.status & 0x110) {
		pr_info("%s: MPU/OP error"
				"(0x%x) during Onenand probe\n",
				__func__, dma_buffer->data.status);
		err = -EIO;
	} else {

		onenand_info.manufacturer_id =
			(dma_buffer->data.data0 >> 0) & 0x0000FFFF;
		onenand_info.device_id =
			(dma_buffer->data.data0 >> 16) & 0x0000FFFF;
		onenand_info.version_id =
			(dma_buffer->data.data1 >> 0) & 0x0000FFFF;
		onenand_info.data_buf_size =
			(dma_buffer->data.data1 >> 16) & 0x0000FFFF;
		onenand_info.boot_buf_size =
			(dma_buffer->data.data2 >> 0) & 0x0000FFFF;
		onenand_info.num_of_buffers =
			(dma_buffer->data.data2 >> 16) & 0x0000FFFF;
		onenand_info.technology =
			(dma_buffer->data.data3 >> 0) & 0x0000FFFF;


		pr_info("======================================="
				"==========================\n");

		pr_info("%s: manufacturer_id = 0x%x\n"
				, __func__, onenand_info.manufacturer_id);
		pr_info("%s: device_id = 0x%x\n"
				, __func__, onenand_info.device_id);
		pr_info("%s: version_id = 0x%x\n"
				, __func__, onenand_info.version_id);
		pr_info("%s: data_buf_size = 0x%x\n"
				, __func__, onenand_info.data_buf_size);
		pr_info("%s: boot_buf_size = 0x%x\n"
				, __func__, onenand_info.boot_buf_size);
		pr_info("%s: num_of_buffers = 0x%x\n"
				, __func__, onenand_info.num_of_buffers);
		pr_info("%s: technology = 0x%x\n"
				, __func__, onenand_info.technology);

		pr_info("======================================="
				"==========================\n");

		if ((onenand_info.manufacturer_id != 0x00EC)
			|| ((onenand_info.device_id & 0x0040) != 0x0040)
			|| (onenand_info.data_buf_size != 0x0800)
			|| (onenand_info.boot_buf_size != 0x0200)
			|| (onenand_info.num_of_buffers != 0x0201)
			|| (onenand_info.technology != 0)) {

			pr_info("%s: Detected an unsupported device\n"
				, __func__);
			err = -EIO;
		}
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	return err;
}

int msm_onenand_read_oob(struct mtd_info *mtd,
		loff_t from, struct mtd_oob_ops *ops)
{
	struct msm_nand_chip *chip = mtd->priv;

	struct {
		dmov_s cmd[53];
		unsigned cmdptr;
		struct {
			uint32_t sfbcfg;
			uint32_t sfcmd[9];
			uint32_t sfexec;
			uint32_t sfstat[9];
			uint32_t addr0;
			uint32_t addr1;
			uint32_t addr2;
			uint32_t addr3;
			uint32_t addr4;
			uint32_t addr5;
			uint32_t addr6;
			uint32_t data0;
			uint32_t data1;
			uint32_t data2;
			uint32_t data3;
			uint32_t data4;
			uint32_t data5;
			uint32_t data6;
			uint32_t macro[5];
		} data;
	} *dma_buffer;
	dmov_s *cmd;

	int err = 0;
	int i;
	dma_addr_t data_dma_addr = 0;
	dma_addr_t oob_dma_addr = 0;
	dma_addr_t data_dma_addr_curr = 0;
	dma_addr_t oob_dma_addr_curr = 0;

	loff_t from_curr = 0;
	unsigned page_count;
	unsigned pages_read = 0;

	uint16_t onenand_startaddr1;
	uint16_t onenand_startaddr8;
	uint16_t onenand_startaddr2;
	uint16_t onenand_startbuffer;
	uint16_t onenand_sysconfig1;
	uint16_t controller_status;
	uint16_t interrupt_status;
	uint16_t ecc_status;
#if VERBOSE
	pr_info("================================================="
			"================\n");
	pr_info("%s: from 0x%llx mode %d \ndatbuf 0x%p datlen 0x%x"
			"\noobbuf 0x%p ooblen 0x%x\n",
			__func__, from, ops->mode, ops->datbuf, ops->len,
			ops->oobbuf, ops->ooblen);
#endif
	if (!mtd) {
		pr_err("%s: invalid mtd pointer, 0x%x\n", __func__,
				(uint32_t)mtd);
		return -EINVAL;
	}
	if (from & (mtd->writesize - 1)) {
		pr_err("%s: unsupported from, 0x%llx\n", __func__,
				from);
		return -EINVAL;
	}

	if ((ops->mode != MTD_OOB_PLACE) && (ops->mode != MTD_OOB_AUTO) &&
			(ops->mode != MTD_OOB_RAW)) {
		pr_err("%s: unsupported ops->mode, %d\n", __func__,
				ops->mode);
		return -EINVAL;
	}

	if (((ops->datbuf == NULL) || (ops->len == 0)) &&
			((ops->oobbuf == NULL) || (ops->ooblen == 0))) {
		pr_err("%s: incorrect ops fields - nothing to do\n",
				__func__);
		return -EINVAL;
	}

	if ((ops->datbuf != NULL) && (ops->len == 0)) {
		pr_err("%s: data buffer passed but length 0\n",
				__func__);
		return -EINVAL;
	}

	if ((ops->oobbuf != NULL) && (ops->ooblen == 0)) {
		pr_err("%s: oob buffer passed but length 0\n",
				__func__);
		return -EINVAL;
	}

	if (ops->mode != MTD_OOB_RAW) {
		if (ops->datbuf != NULL && (ops->len % mtd->writesize) != 0) {
			/* when ops->datbuf is NULL, ops->len can be ooblen */
			pr_err("%s: unsupported ops->len, %d\n", __func__,
					ops->len);
			return -EINVAL;
		}
	} else {
		if (ops->datbuf != NULL &&
			(ops->len % (mtd->writesize + mtd->oobsize)) != 0) {
			pr_err("%s: unsupported ops->len,"
				" %d for MTD_OOB_RAW\n", __func__, ops->len);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_RAW) && (ops->oobbuf)) {
		pr_err("%s: unsupported operation, oobbuf pointer "
				"passed in for RAW mode, %x\n", __func__,
				(uint32_t)ops->oobbuf);
		return -EINVAL;
	}

	if (ops->oobbuf && !ops->datbuf)
		page_count = ops->ooblen / ((ops->mode == MTD_OOB_AUTO) ?
			mtd->oobavail : mtd->oobsize);
	else if (ops->mode != MTD_OOB_RAW)
		page_count = ops->len / mtd->writesize;
	else
		page_count = ops->len / (mtd->writesize + mtd->oobsize);

	if ((ops->mode == MTD_OOB_AUTO) && (ops->oobbuf != NULL)) {
		if (page_count * mtd->oobavail > ops->ooblen) {
			pr_err("%s: unsupported ops->ooblen for "
				"AUTO, %d\n", __func__, ops->ooblen);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_PLACE) && (ops->oobbuf != NULL)) {
		if (page_count * mtd->oobsize > ops->ooblen) {
			pr_err("%s: unsupported ops->ooblen for "
				"PLACE, %d\n", __func__, ops->ooblen);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_PLACE) && (ops->ooblen != 0) &&
							(ops->ooboffs != 0)) {
		pr_err("%s: unsupported ops->ooboffs, %d\n", __func__,
				ops->ooboffs);
		return -EINVAL;
	}

	if (ops->datbuf) {
		memset(ops->datbuf, 0x55, ops->len);
		data_dma_addr_curr = data_dma_addr = dma_map_single(chip->dev,
				ops->datbuf, ops->len, DMA_FROM_DEVICE);
		if (dma_mapping_error(chip->dev, data_dma_addr)) {
			pr_err("%s: failed to get dma addr for %p\n",
					__func__, ops->datbuf);
			return -EIO;
		}
	}
	if (ops->oobbuf) {
		memset(ops->oobbuf, 0x55, ops->ooblen);
		oob_dma_addr_curr = oob_dma_addr = dma_map_single(chip->dev,
				ops->oobbuf, ops->ooblen, DMA_FROM_DEVICE);
		if (dma_mapping_error(chip->dev, oob_dma_addr)) {
			pr_err("%s: failed to get dma addr for %p\n",
					__func__, ops->oobbuf);
			err = -EIO;
			goto err_dma_map_oobbuf_failed;
		}
	}

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	from_curr = from;

	while (page_count-- > 0) {

		cmd = dma_buffer->cmd;

		onenand_startaddr1 = DEVICE_FLASHCORE_0 |
				((uint32_t)from_curr / mtd->erasesize) ;
		onenand_startaddr8 = (((uint32_t)from_curr &
				(mtd->erasesize - 1)) / mtd->writesize) << 2;
		onenand_startaddr2 = DEVICE_BUFFERRAM_0 << 15;
		onenand_startbuffer = DATARAM0_0 << 8;
		onenand_sysconfig1 = (ops->mode == MTD_OOB_RAW) ?
			ONENAND_SYSCFG1_ECCDIS : ONENAND_SYSCFG1_ECCENA;

		dma_buffer->data.sfbcfg = SFLASH_BCFG;
		dma_buffer->data.sfcmd[0] =  SFLASH_PREPCMD(7, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGWR);
		dma_buffer->data.sfcmd[1] =  SFLASH_PREPCMD(0, 0, 32,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_INTHI);
		dma_buffer->data.sfcmd[2] =  SFLASH_PREPCMD(3, 7, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGRD);
		dma_buffer->data.sfcmd[3] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATRD);
		dma_buffer->data.sfcmd[4] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATRD);
		dma_buffer->data.sfcmd[5] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATRD);
		dma_buffer->data.sfcmd[6] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATRD);
		dma_buffer->data.sfcmd[7] =  SFLASH_PREPCMD(32, 0, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATRD);
		dma_buffer->data.sfcmd[8] =  SFLASH_PREPCMD(4, 10, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGWR);
		dma_buffer->data.sfexec = 1;
		dma_buffer->data.sfstat[0] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[1] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[2] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[3] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[4] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[5] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[6] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[7] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[8] = CLEAN_DATA_32;
		dma_buffer->data.addr0 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
		dma_buffer->data.addr1 = (ONENAND_START_ADDRESS_8 << 16) |
						(ONENAND_START_ADDRESS_1);
		dma_buffer->data.addr2 = (ONENAND_START_BUFFER << 16) |
						(ONENAND_START_ADDRESS_2);
		dma_buffer->data.addr3 = (ONENAND_ECC_STATUS << 16) |
						(ONENAND_COMMAND);
		dma_buffer->data.addr4 = (ONENAND_CONTROLLER_STATUS << 16) |
						(ONENAND_INTERRUPT_STATUS);
		dma_buffer->data.addr5 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
		dma_buffer->data.addr6 = (ONENAND_START_ADDRESS_3 << 16) |
						(ONENAND_START_ADDRESS_1);
		dma_buffer->data.data0 = (ONENAND_CLRINTR << 16) |
						(onenand_sysconfig1);
		dma_buffer->data.data1 = (onenand_startaddr8 << 16) |
						(onenand_startaddr1);
		dma_buffer->data.data2 = (onenand_startbuffer << 16) |
						(onenand_startaddr2);
		dma_buffer->data.data3 = (CLEAN_DATA_16 << 16) |
						(ONENAND_CMDLOADSPARE);
		dma_buffer->data.data4 = (CLEAN_DATA_16 << 16) |
						(CLEAN_DATA_16);
		dma_buffer->data.data5 = (ONENAND_CLRINTR << 16) |
						(ONENAND_SYSCFG1_ECCENA);
		dma_buffer->data.data6 = (ONENAND_STARTADDR3_RES << 16) |
						(ONENAND_STARTADDR1_RES);
		dma_buffer->data.macro[0] = 0x0200;
		dma_buffer->data.macro[1] = 0x0300;
		dma_buffer->data.macro[2] = 0x0400;
		dma_buffer->data.macro[3] = 0x0500;
		dma_buffer->data.macro[4] = 0x8010;

		/*************************************************************/
		/* Write necessary address registers in the onenand device   */
		/*************************************************************/

		/* Enable and configure the SFlash controller */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfbcfg);
		cmd->dst = NAND_SFLASHC_BURST_CFG;
		cmd->len = 4;
		cmd++;

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[0]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Write the ADDR0 and ADDR1 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr0);
		cmd->dst = NAND_ADDR0;
		cmd->len = 8;
		cmd++;

		/* Write the ADDR2 ADDR3 ADDR4 ADDR5 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr2);
		cmd->dst = NAND_ADDR2;
		cmd->len = 16;
		cmd++;

		/* Write the ADDR6 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr6);
		cmd->dst = NAND_ADDR6;
		cmd->len = 4;
		cmd++;

		/* Write the GENP0, GENP1, GENP2, GENP3 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data0);
		cmd->dst = NAND_GENP_REG0;
		cmd->len = 16;
		cmd++;

		/* Write the FLASH_DEV_CMD4,5,6 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data4);
		cmd->dst = NAND_DEV_CMD4;
		cmd->len = 12;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[0]);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Wait for the interrupt from the Onenand device controller */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[1]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[1]);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Read necessary status registers from the onenand device   */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[2]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[2]);
		cmd->len = 4;
		cmd++;

		/* Read the GENP3 register */
		cmd->cmd = 0;
		cmd->src = NAND_GENP_REG3;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data3);
		cmd->len = 4;
		cmd++;

		/* Read the DEVCMD4 register */
		cmd->cmd = 0;
		cmd->src = NAND_DEV_CMD4;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data4);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Read the data ram area from the onenand buffer ram        */
		/*************************************************************/

		if (ops->datbuf) {

			dma_buffer->data.data3 = (CLEAN_DATA_16 << 16) |
							(ONENAND_CMDLOAD);

			for (i = 0; i < 4; i++) {

				/* Block on cmd ready and write CMD register */
				cmd->cmd = DST_CRCI_NAND_CMD;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.sfcmd[3+i]);
				cmd->dst = NAND_SFLASHC_CMD;
				cmd->len = 4;
				cmd++;

				/* Write the MACRO1 register */
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.macro[i]);
				cmd->dst = NAND_MACRO1_REG;
				cmd->len = 4;
				cmd++;

				/* Kick the execute command */
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.sfexec);
				cmd->dst = NAND_SFLASHC_EXEC_CMD;
				cmd->len = 4;
				cmd++;

				/* Block on data rdy, & read status register */
				cmd->cmd = SRC_CRCI_NAND_DATA;
				cmd->src = NAND_SFLASHC_STATUS;
				cmd->dst = msm_virt_to_dma(chip,
						&dma_buffer->data.sfstat[3+i]);
				cmd->len = 4;
				cmd++;

				/* Transfer nand ctlr buf contents to usr buf */
				cmd->cmd = 0;
				cmd->src = NAND_FLASH_BUFFER;
				cmd->dst = data_dma_addr_curr;
				cmd->len = 512;
				data_dma_addr_curr += 512;
				cmd++;
			}
		}

		if ((ops->oobbuf) || (ops->mode == MTD_OOB_RAW)) {

			/* Block on cmd ready and write CMD register */
			cmd->cmd = DST_CRCI_NAND_CMD;
			cmd->src = msm_virt_to_dma(chip,
					&dma_buffer->data.sfcmd[7]);
			cmd->dst = NAND_SFLASHC_CMD;
			cmd->len = 4;
			cmd++;

			/* Write the MACRO1 register */
			cmd->cmd = 0;
			cmd->src = msm_virt_to_dma(chip,
					&dma_buffer->data.macro[4]);
			cmd->dst = NAND_MACRO1_REG;
			cmd->len = 4;
			cmd++;

			/* Kick the execute command */
			cmd->cmd = 0;
			cmd->src = msm_virt_to_dma(chip,
					&dma_buffer->data.sfexec);
			cmd->dst = NAND_SFLASHC_EXEC_CMD;
			cmd->len = 4;
			cmd++;

			/* Block on data ready, and read status register */
			cmd->cmd = SRC_CRCI_NAND_DATA;
			cmd->src = NAND_SFLASHC_STATUS;
			cmd->dst = msm_virt_to_dma(chip,
					&dma_buffer->data.sfstat[7]);
			cmd->len = 4;
			cmd++;

			/* Transfer nand ctlr buffer contents into usr buf */
			if (ops->mode == MTD_OOB_AUTO) {
				for (i = 0; i < MTD_MAX_OOBFREE_ENTRIES; i++) {
					cmd->cmd = 0;
					cmd->src = NAND_FLASH_BUFFER +
					mtd->ecclayout->oobfree[i].offset;
					cmd->dst = oob_dma_addr_curr;
					cmd->len =
					mtd->ecclayout->oobfree[i].length;
					oob_dma_addr_curr +=
					mtd->ecclayout->oobfree[i].length;
					cmd++;
				}
			}
			if (ops->mode == MTD_OOB_PLACE) {
					cmd->cmd = 0;
					cmd->src = NAND_FLASH_BUFFER;
					cmd->dst = oob_dma_addr_curr;
					cmd->len = mtd->oobsize;
					oob_dma_addr_curr += mtd->oobsize;
					cmd++;
			}
			if (ops->mode == MTD_OOB_RAW) {
					cmd->cmd = 0;
					cmd->src = NAND_FLASH_BUFFER;
					cmd->dst = data_dma_addr_curr;
					cmd->len = mtd->oobsize;
					data_dma_addr_curr += mtd->oobsize;
					cmd++;
			}
		}

		/*************************************************************/
		/* Restore the necessary registers to proper values          */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[8]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[8]);
		cmd->len = 4;
		cmd++;


		BUILD_BUG_ON(53 != ARRAY_SIZE(dma_buffer->cmd));
		BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
		dma_buffer->cmd[0].cmd |= CMD_OCB;
		cmd[-1].cmd |= CMD_OCU | CMD_LC;

		dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd)
				>> 3) | CMD_PTR_LP;

		msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST
				| DMOV_CMD_ADDR(msm_virt_to_dma(chip,
				&dma_buffer->cmdptr)));

		ecc_status = (dma_buffer->data.data3 >> 16) &
							0x0000FFFF;
		interrupt_status = (dma_buffer->data.data4 >> 0) &
							0x0000FFFF;
		controller_status = (dma_buffer->data.data4 >> 16) &
							0x0000FFFF;

#if VERBOSE
		pr_info("\n%s: sflash status %x %x %x %x %x %x %x"
				"%x %x\n", __func__,
					dma_buffer->data.sfstat[0],
					dma_buffer->data.sfstat[1],
					dma_buffer->data.sfstat[2],
					dma_buffer->data.sfstat[3],
					dma_buffer->data.sfstat[4],
					dma_buffer->data.sfstat[5],
					dma_buffer->data.sfstat[6],
					dma_buffer->data.sfstat[7],
					dma_buffer->data.sfstat[8]);

		pr_info("%s: controller_status = %x\n", __func__,
					controller_status);
		pr_info("%s: interrupt_status = %x\n", __func__,
					interrupt_status);
		pr_info("%s: ecc_status = %x\n", __func__,
					ecc_status);
#endif
		/* Check for errors, protection violations etc */
		if ((controller_status != 0)
				|| (dma_buffer->data.sfstat[0] & 0x110)
				|| (dma_buffer->data.sfstat[1] & 0x110)
				|| (dma_buffer->data.sfstat[2] & 0x110)
				|| (dma_buffer->data.sfstat[8] & 0x110)
				|| ((dma_buffer->data.sfstat[3] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[4] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[5] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[6] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[7] & 0x110) &&
								((ops->oobbuf)
					|| (ops->mode == MTD_OOB_RAW)))) {
			pr_info("%s: ECC/MPU/OP error\n", __func__);
			err = -EIO;
		}

		if (err)
			break;
		pages_read++;
		from_curr += mtd->writesize;
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	if (ops->oobbuf) {
		dma_unmap_single(chip->dev, oob_dma_addr, ops->ooblen,
				DMA_FROM_DEVICE);
	}
err_dma_map_oobbuf_failed:
	if (ops->datbuf) {
		dma_unmap_single(chip->dev, data_dma_addr, ops->len,
				DMA_FROM_DEVICE);
	}

	if (err) {
		pr_err("%s: %llx %x %x failed\n", __func__, from_curr,
				ops->datbuf ? ops->len : 0, ops->ooblen);
	} else {
		ops->retlen = ops->oobretlen = 0;
		if (ops->datbuf != NULL) {
			if (ops->mode != MTD_OOB_RAW)
				ops->retlen = mtd->writesize * pages_read;
			else
				ops->retlen = (mtd->writesize +  mtd->oobsize)
							* pages_read;
		}
		if (ops->oobbuf != NULL) {
			if (ops->mode == MTD_OOB_AUTO)
				ops->oobretlen = mtd->oobavail * pages_read;
			else
				ops->oobretlen = mtd->oobsize * pages_read;
		}
	}

#if VERBOSE
	pr_info("\n%s: ret %d, retlen %d oobretlen %d\n",
			__func__, err, ops->retlen, ops->oobretlen);

	pr_info("==================================================="
			"==============\n");
#endif
	return err;
}

int msm_onenand_read(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, u_char *buf)
{
	int ret;
	struct mtd_oob_ops ops;

	ops.mode = MTD_OOB_PLACE;
	ops.datbuf = buf;
	ops.len = len;
	ops.retlen = 0;
	ops.oobbuf = NULL;
	ops.ooblen = 0;
	ops.oobretlen = 0;
	ret =  msm_onenand_read_oob(mtd, from, &ops);
	*retlen = ops.retlen;

	return ret;
}

static int msm_onenand_write_oob(struct mtd_info *mtd, loff_t to,
		struct mtd_oob_ops *ops)
{
	struct msm_nand_chip *chip = mtd->priv;

	struct {
		dmov_s cmd[53];
		unsigned cmdptr;
		struct {
			uint32_t sfbcfg;
			uint32_t sfcmd[9];
			uint32_t sfexec;
			uint32_t sfstat[9];
			uint32_t addr0;
			uint32_t addr1;
			uint32_t addr2;
			uint32_t addr3;
			uint32_t addr4;
			uint32_t addr5;
			uint32_t addr6;
			uint32_t data0;
			uint32_t data1;
			uint32_t data2;
			uint32_t data3;
			uint32_t data4;
			uint32_t data5;
			uint32_t data6;
			uint32_t macro[5];
		} data;
	} *dma_buffer;
	dmov_s *cmd;

	int err = 0;
	int i, j, k;
	dma_addr_t data_dma_addr = 0;
	dma_addr_t oob_dma_addr = 0;
	dma_addr_t init_dma_addr = 0;
	dma_addr_t data_dma_addr_curr = 0;
	dma_addr_t oob_dma_addr_curr = 0;
	static uint8_t init_spare_bytes[64];

	loff_t to_curr = 0;
	unsigned page_count;
	unsigned pages_written = 0;

	uint16_t onenand_startaddr1;
	uint16_t onenand_startaddr8;
	uint16_t onenand_startaddr2;
	uint16_t onenand_startbuffer;
	uint16_t onenand_sysconfig1;

	uint16_t controller_status;
	uint16_t interrupt_status;
	uint16_t ecc_status;

#if VERBOSE
	pr_info("================================================="
			"================\n");
	pr_info("%s: to 0x%llx mode %d \ndatbuf 0x%p datlen 0x%x"
			"\noobbuf 0x%p ooblen 0x%x\n",
			__func__, to, ops->mode, ops->datbuf, ops->len,
			ops->oobbuf, ops->ooblen);
#endif
	if (!mtd) {
		pr_err("%s: invalid mtd pointer, 0x%x\n", __func__,
				(uint32_t)mtd);
		return -EINVAL;
	}
	if (to & (mtd->writesize - 1)) {
		pr_err("%s: unsupported to, 0x%llx\n", __func__, to);
		return -EINVAL;
	}

	if ((ops->mode != MTD_OOB_PLACE) && (ops->mode != MTD_OOB_AUTO) &&
			(ops->mode != MTD_OOB_RAW)) {
		pr_err("%s: unsupported ops->mode, %d\n", __func__,
				ops->mode);
		return -EINVAL;
	}

	if (((ops->datbuf == NULL) || (ops->len == 0)) &&
			((ops->oobbuf == NULL) || (ops->ooblen == 0))) {
		pr_err("%s: incorrect ops fields - nothing to do\n",
				__func__);
		return -EINVAL;
	}

	if ((ops->datbuf != NULL) && (ops->len == 0)) {
		pr_err("%s: data buffer passed but length 0\n",
				__func__);
		return -EINVAL;
	}

	if ((ops->oobbuf != NULL) && (ops->ooblen == 0)) {
		pr_err("%s: oob buffer passed but length 0\n",
				__func__);
		return -EINVAL;
	}

	if (ops->mode != MTD_OOB_RAW) {
		if (ops->datbuf != NULL && (ops->len % mtd->writesize) != 0) {
			/* when ops->datbuf is NULL, ops->len can be ooblen */
			pr_err("%s: unsupported ops->len, %d\n", __func__,
					ops->len);
			return -EINVAL;
		}
	} else {
		if (ops->datbuf != NULL &&
			(ops->len % (mtd->writesize + mtd->oobsize)) != 0) {
			pr_err("%s: unsupported ops->len,"
				" %d for MTD_OOB_RAW\n", __func__, ops->len);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_RAW) && (ops->oobbuf)) {
		pr_err("%s: unsupported operation, oobbuf pointer "
				"passed in for RAW mode, %x\n", __func__,
				(uint32_t)ops->oobbuf);
		return -EINVAL;
	}

	if (ops->oobbuf && !ops->datbuf)
		page_count = ops->ooblen / ((ops->mode == MTD_OOB_AUTO) ?
			mtd->oobavail : mtd->oobsize);
	else if (ops->mode != MTD_OOB_RAW)
		page_count = ops->len / mtd->writesize;
	else
		page_count = ops->len / (mtd->writesize + mtd->oobsize);

	if ((ops->mode == MTD_OOB_AUTO) && (ops->oobbuf != NULL)) {
		if (page_count > 1) {
			pr_err("%s: unsupported ops->ooblen for"
				"AUTO, %d\n", __func__, ops->ooblen);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_PLACE) && (ops->oobbuf != NULL)) {
		if (page_count * mtd->oobsize > ops->ooblen) {
			pr_err("%s: unsupported ops->ooblen for"
				"PLACE,	%d\n", __func__, ops->ooblen);
			return -EINVAL;
		}
	}

	if ((ops->mode == MTD_OOB_PLACE) && (ops->ooblen != 0) &&
						(ops->ooboffs != 0)) {
		pr_err("%s: unsupported ops->ooboffs, %d\n",
				__func__, ops->ooboffs);
		return -EINVAL;
	}

	for (i = 0; i < 64; i++)
		init_spare_bytes[i] = 0xFF;
	init_dma_addr = dma_map_single(chip->dev, init_spare_bytes, 64,
			DMA_TO_DEVICE);
	if (dma_mapping_error(chip->dev, init_dma_addr)) {
		pr_err("%s: failed to get dma addr for %p\n",
				__func__, init_spare_bytes);
		return -EIO;
	}

	if (ops->datbuf) {
		data_dma_addr_curr = data_dma_addr = dma_map_single(chip->dev,
				ops->datbuf, ops->len, DMA_TO_DEVICE);
		if (dma_mapping_error(chip->dev, data_dma_addr)) {
			pr_err("%s: failed to get dma addr for %p\n",
					__func__, ops->datbuf);
			return -EIO;
		}
	}
	if (ops->oobbuf) {
		oob_dma_addr_curr = oob_dma_addr = dma_map_single(chip->dev,
				ops->oobbuf, ops->ooblen, DMA_TO_DEVICE);
		if (dma_mapping_error(chip->dev, oob_dma_addr)) {
			pr_err("%s: failed to get dma addr for %p\n",
					__func__, ops->oobbuf);
			err = -EIO;
			goto err_dma_map_oobbuf_failed;
		}
	}

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	to_curr = to;

	while (page_count-- > 0) {
		cmd = dma_buffer->cmd;

		onenand_startaddr1 = DEVICE_FLASHCORE_0 |
					((uint32_t)to_curr / mtd->erasesize);
		onenand_startaddr8 = (((uint32_t)to_curr &
				(mtd->erasesize - 1)) / mtd->writesize) << 2;
		onenand_startaddr2 = DEVICE_BUFFERRAM_0 << 15;
		onenand_startbuffer = DATARAM0_0 << 8;
		onenand_sysconfig1 = (ops->mode == MTD_OOB_RAW) ?
			ONENAND_SYSCFG1_ECCDIS : ONENAND_SYSCFG1_ECCENA;

		dma_buffer->data.sfbcfg = SFLASH_BCFG;
		dma_buffer->data.sfcmd[0] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATWR);
		dma_buffer->data.sfcmd[1] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATWR);
		dma_buffer->data.sfcmd[2] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATWR);
		dma_buffer->data.sfcmd[3] =  SFLASH_PREPCMD(256, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATWR);
		dma_buffer->data.sfcmd[4] =  SFLASH_PREPCMD(32, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_DATWR);
		dma_buffer->data.sfcmd[5] =  SFLASH_PREPCMD(7, 0, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGWR);
		dma_buffer->data.sfcmd[6] =  SFLASH_PREPCMD(0, 0, 32,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_INTHI);
		dma_buffer->data.sfcmd[7] =  SFLASH_PREPCMD(3, 7, 0,
							NAND_SFCMD_DATXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGRD);
		dma_buffer->data.sfcmd[8] =  SFLASH_PREPCMD(4, 10, 0,
							NAND_SFCMD_CMDXS,
							NAND_SFCMD_ASYNC,
							NAND_SFCMD_REGWR);
		dma_buffer->data.sfexec = 1;
		dma_buffer->data.sfstat[0] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[1] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[2] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[3] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[4] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[5] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[6] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[7] = CLEAN_DATA_32;
		dma_buffer->data.sfstat[8] = CLEAN_DATA_32;
		dma_buffer->data.addr0 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
		dma_buffer->data.addr1 = (ONENAND_START_ADDRESS_8 << 16) |
						(ONENAND_START_ADDRESS_1);
		dma_buffer->data.addr2 = (ONENAND_START_BUFFER << 16) |
						(ONENAND_START_ADDRESS_2);
		dma_buffer->data.addr3 = (ONENAND_ECC_STATUS << 16) |
						(ONENAND_COMMAND);
		dma_buffer->data.addr4 = (ONENAND_CONTROLLER_STATUS << 16) |
						(ONENAND_INTERRUPT_STATUS);
		dma_buffer->data.addr5 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
		dma_buffer->data.addr6 = (ONENAND_START_ADDRESS_3 << 16) |
						(ONENAND_START_ADDRESS_1);
		dma_buffer->data.data0 = (ONENAND_CLRINTR << 16) |
						(onenand_sysconfig1);
		dma_buffer->data.data1 = (onenand_startaddr8 << 16) |
						(onenand_startaddr1);
		dma_buffer->data.data2 = (onenand_startbuffer << 16) |
						(onenand_startaddr2);
		dma_buffer->data.data3 = (CLEAN_DATA_16 << 16) |
						(ONENAND_CMDPROGSPARE);
		dma_buffer->data.data4 = (CLEAN_DATA_16 << 16) |
						(CLEAN_DATA_16);
		dma_buffer->data.data5 = (ONENAND_CLRINTR << 16) |
						(ONENAND_SYSCFG1_ECCENA);
		dma_buffer->data.data6 = (ONENAND_STARTADDR3_RES << 16) |
						(ONENAND_STARTADDR1_RES);
		dma_buffer->data.macro[0] = 0x0200;
		dma_buffer->data.macro[1] = 0x0300;
		dma_buffer->data.macro[2] = 0x0400;
		dma_buffer->data.macro[3] = 0x0500;
		dma_buffer->data.macro[4] = 0x8010;


		/*************************************************************/
		/* Write the data ram area in the onenand buffer ram         */
		/*************************************************************/

		/* Enable and configure the SFlash controller */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfbcfg);
		cmd->dst = NAND_SFLASHC_BURST_CFG;
		cmd->len = 4;
		cmd++;

		if (ops->datbuf) {
			dma_buffer->data.data3 = (CLEAN_DATA_16 << 16) |
							(ONENAND_CMDPROG);

			for (i = 0; i < 4; i++) {

				/* Block on cmd ready and write CMD register */
				cmd->cmd = DST_CRCI_NAND_CMD;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.sfcmd[i]);
				cmd->dst = NAND_SFLASHC_CMD;
				cmd->len = 4;
				cmd++;

				/* Trnsfr usr buf contents to nand ctlr buf */
				cmd->cmd = 0;
				cmd->src = data_dma_addr_curr;
				cmd->dst = NAND_FLASH_BUFFER;
				cmd->len = 512;
				data_dma_addr_curr += 512;
				cmd++;

				/* Write the MACRO1 register */
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.macro[i]);
				cmd->dst = NAND_MACRO1_REG;
				cmd->len = 4;
				cmd++;

				/* Kick the execute command */
				cmd->cmd = 0;
				cmd->src = msm_virt_to_dma(chip,
						&dma_buffer->data.sfexec);
				cmd->dst = NAND_SFLASHC_EXEC_CMD;
				cmd->len = 4;
				cmd++;

				/* Block on data rdy, & read status register */
				cmd->cmd = SRC_CRCI_NAND_DATA;
				cmd->src = NAND_SFLASHC_STATUS;
				cmd->dst = msm_virt_to_dma(chip,
						&dma_buffer->data.sfstat[i]);
				cmd->len = 4;
				cmd++;

			}
		}

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[4]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		if ((ops->oobbuf) || (ops->mode == MTD_OOB_RAW)) {

			/* Transfer user buf contents into nand ctlr buffer */
			if (ops->mode == MTD_OOB_AUTO) {
				for (i = 0, k = 0;
					i < MTD_MAX_OOBFREE_ENTRIES; i++)
					for (j = 0;
					j < mtd->ecclayout->oobfree[i].length;
					j++) {
					init_spare_bytes[j +
					mtd->ecclayout->oobfree[i].offset]
					= (ops->oobbuf)[k];
					k++;
					}
				cmd->cmd = 0;
				cmd->src = init_dma_addr;
				cmd->dst = NAND_FLASH_BUFFER;
				cmd->len = mtd->oobsize;
				cmd++;
			}
			if (ops->mode == MTD_OOB_PLACE) {
				cmd->cmd = 0;
				cmd->src = oob_dma_addr_curr;
				cmd->dst = NAND_FLASH_BUFFER;
				cmd->len = mtd->oobsize;
				oob_dma_addr_curr += mtd->oobsize;
				cmd++;
			}
			if (ops->mode == MTD_OOB_RAW) {
				cmd->cmd = 0;
				cmd->src = data_dma_addr_curr;
				cmd->dst = NAND_FLASH_BUFFER;
				cmd->len = mtd->oobsize;
				data_dma_addr_curr += mtd->oobsize;
				cmd++;
			}
		}

		/* Write the MACRO1 register */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.macro[4]);
		cmd->dst = NAND_MACRO1_REG;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[4]);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Write necessary address registers in the onenand device   */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[5]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Write the ADDR0 and ADDR1 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr0);
		cmd->dst = NAND_ADDR0;
		cmd->len = 8;
		cmd++;

		/* Write the ADDR2 ADDR3 ADDR4 ADDR5 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr2);
		cmd->dst = NAND_ADDR2;
		cmd->len = 16;
		cmd++;

		/* Write the ADDR6 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr6);
		cmd->dst = NAND_ADDR6;
		cmd->len = 4;
		cmd++;

		/* Write the GENP0, GENP1, GENP2, GENP3 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data0);
		cmd->dst = NAND_GENP_REG0;
		cmd->len = 16;
		cmd++;

		/* Write the FLASH_DEV_CMD4,5,6 registers */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data4);
		cmd->dst = NAND_DEV_CMD4;
		cmd->len = 12;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[5]);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Wait for the interrupt from the Onenand device controller */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[6]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[6]);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Read necessary status registers from the onenand device   */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[7]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[7]);
		cmd->len = 4;
		cmd++;

		/* Read the GENP3 register */
		cmd->cmd = 0;
		cmd->src = NAND_GENP_REG3;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data3);
		cmd->len = 4;
		cmd++;

		/* Read the DEVCMD4 register */
		cmd->cmd = 0;
		cmd->src = NAND_DEV_CMD4;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data4);
		cmd->len = 4;
		cmd++;

		/*************************************************************/
		/* Restore the necessary registers to proper values          */
		/*************************************************************/

		/* Block on cmd ready and write CMD register */
		cmd->cmd = DST_CRCI_NAND_CMD;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[8]);
		cmd->dst = NAND_SFLASHC_CMD;
		cmd->len = 4;
		cmd++;

		/* Kick the execute command */
		cmd->cmd = 0;
		cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
		cmd->dst = NAND_SFLASHC_EXEC_CMD;
		cmd->len = 4;
		cmd++;

		/* Block on data ready, and read the status register */
		cmd->cmd = SRC_CRCI_NAND_DATA;
		cmd->src = NAND_SFLASHC_STATUS;
		cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[8]);
		cmd->len = 4;
		cmd++;


		BUILD_BUG_ON(53 != ARRAY_SIZE(dma_buffer->cmd));
		BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
		dma_buffer->cmd[0].cmd |= CMD_OCB;
		cmd[-1].cmd |= CMD_OCU | CMD_LC;

		dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd)
				>> 3) | CMD_PTR_LP;

		msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST
				| DMOV_CMD_ADDR(msm_virt_to_dma(chip,
				&dma_buffer->cmdptr)));

		ecc_status = (dma_buffer->data.data3 >> 16) & 0x0000FFFF;
		interrupt_status = (dma_buffer->data.data4 >> 0)&0x0000FFFF;
		controller_status = (dma_buffer->data.data4 >> 16)&0x0000FFFF;

#if VERBOSE
		pr_info("\n%s: sflash status %x %x %x %x %x %x %x
				%x %x\n", __func__,
					dma_buffer->data.sfstat[0],
					dma_buffer->data.sfstat[1],
					dma_buffer->data.sfstat[2],
					dma_buffer->data.sfstat[3],
					dma_buffer->data.sfstat[4],
					dma_buffer->data.sfstat[5],
					dma_buffer->data.sfstat[6],
					dma_buffer->data.sfstat[7],
					dma_buffer->data.sfstat[8]);

		pr_info("%s: controller_status = %x\n", __func__,
					controller_status);
		pr_info("%s: interrupt_status = %x\n", __func__,
					interrupt_status);
		pr_info("%s: ecc_status = %x\n", __func__,
					ecc_status);
#endif
		/* Check for errors, protection violations etc */
		if ((controller_status != 0)
				|| (dma_buffer->data.sfstat[5] & 0x110)
				|| (dma_buffer->data.sfstat[6] & 0x110)
				|| (dma_buffer->data.sfstat[7] & 0x110)
				|| (dma_buffer->data.sfstat[8] & 0x110)
				|| ((dma_buffer->data.sfstat[0] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[1] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[2] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[3] & 0x110) &&
								(ops->datbuf))
				|| ((dma_buffer->data.sfstat[4] & 0x110) &&
								((ops->oobbuf)
					|| (ops->mode == MTD_OOB_RAW)))) {
			pr_info("%s: ECC/MPU/OP error\n", __func__);
			err = -EIO;
		}

		if (err)
			break;
		pages_written++;
		to_curr += mtd->writesize;
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	if (ops->oobbuf) {
		dma_unmap_single(chip->dev, oob_dma_addr, ops->ooblen,
							DMA_TO_DEVICE);
	}
err_dma_map_oobbuf_failed:
	if (ops->datbuf) {
		dma_unmap_single(chip->dev, data_dma_addr, ops->len,
							DMA_TO_DEVICE);
	}

	dma_unmap_single(chip->dev, init_dma_addr, 64, DMA_TO_DEVICE);

	if (err) {
		pr_err("%s: %llx %x %x failed\n", __func__, to_curr,
				ops->datbuf ? ops->len : 0, ops->ooblen);
	} else {
		ops->retlen = ops->oobretlen = 0;
		if (ops->datbuf != NULL) {
			if (ops->mode != MTD_OOB_RAW)
				ops->retlen = mtd->writesize * pages_written;
			else
				ops->retlen = (mtd->writesize +  mtd->oobsize)
							* pages_written;
		}
		if (ops->oobbuf != NULL) {
			if (ops->mode == MTD_OOB_AUTO)
				ops->oobretlen = mtd->oobavail * pages_written;
			else
				ops->oobretlen = mtd->oobsize * pages_written;
		}
	}

#if VERBOSE
	pr_info("\n%s: ret %d, retlen %d oobretlen %d\n",
			__func__, err, ops->retlen, ops->oobretlen);

	pr_info("================================================="
			"================\n");
#endif
	return err;
}

static int msm_onenand_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	int ret;
	struct mtd_oob_ops ops;

	ops.mode = MTD_OOB_PLACE;
	ops.datbuf = (uint8_t *)buf;
	ops.len = len;
	ops.retlen = 0;
	ops.oobbuf = NULL;
	ops.ooblen = 0;
	ops.oobretlen = 0;
	ret =  msm_onenand_write_oob(mtd, to, &ops);
	*retlen = ops.retlen;

	return ret;
}

static int msm_onenand_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct msm_nand_chip *chip = mtd->priv;

	struct {
		dmov_s cmd[20];
		unsigned cmdptr;
		struct {
			uint32_t sfbcfg;
			uint32_t sfcmd[4];
			uint32_t sfexec;
			uint32_t sfstat[4];
			uint32_t addr0;
			uint32_t addr1;
			uint32_t addr2;
			uint32_t addr3;
			uint32_t addr4;
			uint32_t addr5;
			uint32_t addr6;
			uint32_t data0;
			uint32_t data1;
			uint32_t data2;
			uint32_t data3;
			uint32_t data4;
			uint32_t data5;
			uint32_t data6;
		} data;
	} *dma_buffer;
	dmov_s *cmd;

	int err = 0;

	uint16_t onenand_startaddr1;
	uint16_t onenand_startaddr8;
	uint16_t onenand_startaddr2;
	uint16_t onenand_startbuffer;

	uint16_t controller_status;
	uint16_t interrupt_status;
	uint16_t ecc_status;

#if VERBOSE
	pr_info("================================================="
			"================\n");
	pr_info("%s: addr 0x%x len 0x%x\n",
			__func__, instr->addr, instr->len);
#endif
	if (instr->addr & (mtd->erasesize - 1)) {
		pr_err("%s: Unsupported erase address, 0x%x\n",
				__func__, instr->addr);
		return -EINVAL;
	}
	if (instr->len != mtd->erasesize) {
		pr_err("%s: Unsupported erase len, %d\n",
				__func__, instr->len);
		return -EINVAL;
	}

	wait_event(chip->wait_queue, (dma_buffer = msm_nand_get_dma_buffer
				(chip, sizeof(*dma_buffer))));

	cmd = dma_buffer->cmd;

	onenand_startaddr1 = DEVICE_FLASHCORE_0 |
					(instr->addr / (mtd->erasesize));
	onenand_startaddr8 = 0x0000;
	onenand_startaddr2 = DEVICE_BUFFERRAM_0 << 15;
	onenand_startbuffer = DATARAM0_0 << 8;

	dma_buffer->data.sfbcfg = SFLASH_BCFG;
	dma_buffer->data.sfcmd[0] = SFLASH_PREPCMD(7, 0, 0,
						NAND_SFCMD_CMDXS,
						NAND_SFCMD_ASYNC,
						NAND_SFCMD_REGWR);
	dma_buffer->data.sfcmd[1] = SFLASH_PREPCMD(0, 0, 32,
						NAND_SFCMD_CMDXS,
						NAND_SFCMD_ASYNC,
						NAND_SFCMD_INTHI);
	dma_buffer->data.sfcmd[2] = SFLASH_PREPCMD(3, 7, 0,
						NAND_SFCMD_DATXS,
						NAND_SFCMD_ASYNC,
						NAND_SFCMD_REGRD);
	dma_buffer->data.sfcmd[3] = SFLASH_PREPCMD(4, 10, 0,
						NAND_SFCMD_CMDXS,
						NAND_SFCMD_ASYNC,
						NAND_SFCMD_REGWR);
	dma_buffer->data.sfexec = 1;
	dma_buffer->data.sfstat[0] = CLEAN_DATA_32;
	dma_buffer->data.sfstat[1] = CLEAN_DATA_32;
	dma_buffer->data.sfstat[2] = CLEAN_DATA_32;
	dma_buffer->data.sfstat[3] = CLEAN_DATA_32;
	dma_buffer->data.addr0 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
	dma_buffer->data.addr1 = (ONENAND_START_ADDRESS_8 << 16) |
						(ONENAND_START_ADDRESS_1);
	dma_buffer->data.addr2 = (ONENAND_START_BUFFER << 16) |
						(ONENAND_START_ADDRESS_2);
	dma_buffer->data.addr3 = (ONENAND_ECC_STATUS << 16) |
						(ONENAND_COMMAND);
	dma_buffer->data.addr4 = (ONENAND_CONTROLLER_STATUS << 16) |
						(ONENAND_INTERRUPT_STATUS);
	dma_buffer->data.addr5 = (ONENAND_INTERRUPT_STATUS << 16) |
						(ONENAND_SYSTEM_CONFIG_1);
	dma_buffer->data.addr6 = (ONENAND_START_ADDRESS_3 << 16) |
						(ONENAND_START_ADDRESS_1);
	dma_buffer->data.data0 = (ONENAND_CLRINTR << 16) |
						(ONENAND_SYSCFG1_ECCENA);
	dma_buffer->data.data1 = (onenand_startaddr8 << 16) |
						(onenand_startaddr1);
	dma_buffer->data.data2 = (onenand_startbuffer << 16) |
						(onenand_startaddr2);
	dma_buffer->data.data3 = (CLEAN_DATA_16 << 16) |
						(ONENAND_CMDERAS);
	dma_buffer->data.data4 = (CLEAN_DATA_16 << 16) |
						(CLEAN_DATA_16);
	dma_buffer->data.data5 = (ONENAND_CLRINTR << 16) |
						(ONENAND_SYSCFG1_ECCENA);
	dma_buffer->data.data6 = (ONENAND_STARTADDR3_RES << 16) |
						(ONENAND_STARTADDR1_RES);

	/***************************************************************/
	/* Write the necessary address registers in the onenand device */
	/***************************************************************/

	/* Enable and configure the SFlash controller */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfbcfg);
	cmd->dst = NAND_SFLASHC_BURST_CFG;
	cmd->len = 4;
	cmd++;

	/* Block on cmd ready and write CMD register */
	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[0]);
	cmd->dst = NAND_SFLASHC_CMD;
	cmd->len = 4;
	cmd++;

	/* Write the ADDR0 and ADDR1 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr0);
	cmd->dst = NAND_ADDR0;
	cmd->len = 8;
	cmd++;

	/* Write the ADDR2 ADDR3 ADDR4 ADDR5 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr2);
	cmd->dst = NAND_ADDR2;
	cmd->len = 16;
	cmd++;

	/* Write the ADDR6 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.addr6);
	cmd->dst = NAND_ADDR6;
	cmd->len = 4;
	cmd++;

	/* Write the GENP0, GENP1, GENP2, GENP3, GENP4 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data0);
	cmd->dst = NAND_GENP_REG0;
	cmd->len = 16;
	cmd++;

	/* Write the FLASH_DEV_CMD4,5,6 registers */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.data4);
	cmd->dst = NAND_DEV_CMD4;
	cmd->len = 12;
	cmd++;

	/* Kick the execute command */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
	cmd->dst = NAND_SFLASHC_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	/* Block on data ready, and read the status register */
	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_SFLASHC_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[0]);
	cmd->len = 4;
	cmd++;

	/***************************************************************/
	/* Wait for the interrupt from the Onenand device controller   */
	/***************************************************************/

	/* Block on cmd ready and write CMD register */
	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[1]);
	cmd->dst = NAND_SFLASHC_CMD;
	cmd->len = 4;
	cmd++;

	/* Kick the execute command */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
	cmd->dst = NAND_SFLASHC_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	/* Block on data ready, and read the status register */
	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_SFLASHC_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[1]);
	cmd->len = 4;
	cmd++;

	/***************************************************************/
	/* Read the necessary status registers from the onenand device */
	/***************************************************************/

	/* Block on cmd ready and write CMD register */
	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[2]);
	cmd->dst = NAND_SFLASHC_CMD;
	cmd->len = 4;
	cmd++;

	/* Kick the execute command */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
	cmd->dst = NAND_SFLASHC_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	/* Block on data ready, and read the status register */
	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_SFLASHC_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[2]);
	cmd->len = 4;
	cmd++;

	/* Read the GENP3 register */
	cmd->cmd = 0;
	cmd->src = NAND_GENP_REG3;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data3);
	cmd->len = 4;
	cmd++;

	/* Read the DEVCMD4 register */
	cmd->cmd = 0;
	cmd->src = NAND_DEV_CMD4;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.data4);
	cmd->len = 4;
	cmd++;

	/***************************************************************/
	/* Restore the necessary registers to proper values            */
	/***************************************************************/

	/* Block on cmd ready and write CMD register */
	cmd->cmd = DST_CRCI_NAND_CMD;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfcmd[3]);
	cmd->dst = NAND_SFLASHC_CMD;
	cmd->len = 4;
	cmd++;

	/* Kick the execute command */
	cmd->cmd = 0;
	cmd->src = msm_virt_to_dma(chip, &dma_buffer->data.sfexec);
	cmd->dst = NAND_SFLASHC_EXEC_CMD;
	cmd->len = 4;
	cmd++;

	/* Block on data ready, and read the status register */
	cmd->cmd = SRC_CRCI_NAND_DATA;
	cmd->src = NAND_SFLASHC_STATUS;
	cmd->dst = msm_virt_to_dma(chip, &dma_buffer->data.sfstat[3]);
	cmd->len = 4;
	cmd++;


	BUILD_BUG_ON(20 != ARRAY_SIZE(dma_buffer->cmd));
	BUG_ON(cmd - dma_buffer->cmd > ARRAY_SIZE(dma_buffer->cmd));
	dma_buffer->cmd[0].cmd |= CMD_OCB;
	cmd[-1].cmd |= CMD_OCU | CMD_LC;

	dma_buffer->cmdptr = (msm_virt_to_dma(chip, dma_buffer->cmd)
			>> 3) | CMD_PTR_LP;

	msm_dmov_exec_cmd(chip->dma_channel, DMOV_CMD_PTR_LIST
			| DMOV_CMD_ADDR(msm_virt_to_dma(chip,
			&dma_buffer->cmdptr)));


	ecc_status = (dma_buffer->data.data3 >> 16) & 0x0000FFFF;
	interrupt_status = (dma_buffer->data.data4 >> 0) & 0x0000FFFF;
	controller_status = (dma_buffer->data.data4 >> 16) & 0x0000FFFF;

#if VERBOSE
	pr_info("\n%s: sflash status %x %x %x %x\n", __func__,
				dma_buffer->data.sfstat[0],
				dma_buffer->data.sfstat[1],
				dma_buffer->data.sfstat[2],
				dma_buffer->data.sfstat[3]);

	pr_info("%s: controller_status = %x\n", __func__,
				controller_status);
	pr_info("%s: interrupt_status = %x\n", __func__,
				interrupt_status);
	pr_info("%s: ecc_status = %x\n", __func__,
				ecc_status);
#endif
	/* Check for errors, protection violations etc */
	if ((controller_status != 0)
			|| (dma_buffer->data.sfstat[0] & 0x110)
			|| (dma_buffer->data.sfstat[1] & 0x110)
			|| (dma_buffer->data.sfstat[2] & 0x110)
			|| (dma_buffer->data.sfstat[3] & 0x110)) {
		pr_err("%s: ECC/MPU/OP error\n", __func__);
		err = -EIO;
	}

	msm_nand_release_dma_buffer(chip, dma_buffer, sizeof(*dma_buffer));

	if (err) {
		pr_err("%s: Erase failed, 0x%x\n", __func__,
				instr->addr);
		instr->fail_addr = instr->addr;
		instr->state = MTD_ERASE_FAILED;
	} else {
		instr->state = MTD_ERASE_DONE;
		instr->fail_addr = 0xffffffff;
		mtd_erase_callback(instr);
	}

#if VERBOSE
	pr_info("\n%s: ret %d\n", __func__, err);
	pr_info("===================================================="
			"=============\n");
#endif
	return err;
}

static int msm_onenand_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_oob_ops ops;
	int rval, i;
	int ret = 0;
	uint8_t *buffer;
	uint8_t *oobptr;

	if ((ofs > mtd->size) || (ofs & (mtd->erasesize - 1))) {
		pr_err("%s: unsupported block address, 0x%x\n",
			 __func__, (uint32_t)ofs);
		return -EINVAL;
	}

	buffer = kmalloc(2112, GFP_KERNEL|GFP_DMA);
	if (buffer == 0) {
		pr_err("%s: Could not kmalloc for buffer\n",
				__func__);
		return -ENOMEM;
	}

	memset(buffer, 0x00, 2112);
	oobptr = &(buffer[2048]);

	ops.mode = MTD_OOB_RAW;
	ops.len = 2112;
	ops.retlen = 0;
	ops.ooblen = 0;
	ops.oobretlen = 0;
	ops.ooboffs = 0;
	ops.datbuf = buffer;
	ops.oobbuf = NULL;

	for (i = 0; i < 2; i++) {
		ofs = ofs + i*mtd->writesize;
		rval = msm_onenand_read_oob(mtd, ofs, &ops);
		if (rval) {
			pr_err("%s: Error in reading bad blk info\n",
					__func__);
			ret = rval;
			break;
		}
		if ((oobptr[0] != 0xFF) || (oobptr[1] != 0xFF) ||
		    (oobptr[16] != 0xFF) || (oobptr[17] != 0xFF) ||
		    (oobptr[32] != 0xFF) || (oobptr[33] != 0xFF) ||
		    (oobptr[48] != 0xFF) || (oobptr[49] != 0xFF)
		   ) {
			ret = 1;
			break;
		}
	}

	kfree(buffer);

#if VERBOSE
	if (ret == 1)
		pr_info("%s : Block containing 0x%x is bad\n",
				__func__, (unsigned int)ofs);
#endif
	return ret;
}

static int msm_onenand_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_oob_ops ops;
	int rval, i;
	int ret = 0;
	uint8_t *buffer;

	if ((ofs > mtd->size) || (ofs & (mtd->erasesize - 1))) {
		pr_err("%s: unsupported block address, 0x%x\n",
			 __func__, (uint32_t)ofs);
		return -EINVAL;
	}

	buffer = page_address(ZERO_PAGE());

	ops.mode = MTD_OOB_RAW;
	ops.len = 2112;
	ops.retlen = 0;
	ops.ooblen = 0;
	ops.oobretlen = 0;
	ops.ooboffs = 0;
	ops.datbuf = buffer;
	ops.oobbuf = NULL;

	for (i = 0; i < 2; i++) {
		ofs = ofs + i*mtd->writesize;
		rval = msm_onenand_write_oob(mtd, ofs, &ops);
		if (rval) {
			pr_err("%s: Error in writing bad blk info\n",
					__func__);
			ret = rval;
			break;
		}
	}

	return ret;
}

/* static int msm_nand_unlock(struct mtd_info *mtd, loff_t ofs, size_t len) */
static int msm_onenand_suspend(struct mtd_info *mtd)
{
	return 0;
}

static void msm_onenand_resume(struct mtd_info *mtd)
{
}

int msm_onenand_scan(struct mtd_info *mtd, int maxchips)
{
	struct msm_nand_chip *chip = mtd->priv;

	/* Probe and check whether onenand device is present */
	if (flash_onenand_probe(chip))
		return -ENODEV;

	mtd->size      = (256<<20);
	mtd->writesize = (2048);
	mtd->oobsize   = (64);
	mtd->erasesize = (2048<<6);
	mtd->oobavail = msm_onenand_oob_64.oobavail;
	mtd->ecclayout = &msm_onenand_oob_64;

	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	mtd->erase = msm_onenand_erase;
	mtd->point = NULL;
	mtd->unpoint = NULL;
	mtd->read = msm_onenand_read;
	mtd->write = msm_onenand_write;
	mtd->read_oob = msm_onenand_read_oob;
	mtd->write_oob = msm_onenand_write_oob;
	mtd->lock = NULL;
	mtd->suspend = msm_onenand_suspend;
	mtd->resume = msm_onenand_resume;
	mtd->block_isbad = msm_onenand_block_isbad;
	mtd->block_markbad = msm_onenand_block_markbad;
	mtd->owner = THIS_MODULE;

	pr_info("Found a supported onenand device\n");

	return 0;
}


/**
 * msm_read_param	-	[msm nand Interface] read parameter data from param block
 * @param	orion_param	
 *
 */
void msm_read_param(struct orion_param *orion_param_data)
{
	int ret;
	char data_buf[2048] = { 0, };
	char spare_buf[64] = { 0, };
    struct mtd_oob_ops ops;

	
/*
	ret = msm_nand_block_isbad(current_mtd, (PARAMETER_START_BLOCK + 1) * 64 * 2048);
	if(!ret) {
		printk("PARAMETER BLOCK is BAD\n");
		return;
	}
*/
	memset(data_buf,0x0,2048);
	memset(spare_buf,0x0,64);

	ops.mode = MTD_OOB_RAW;
	ops.len = 2048+64;
	ops.retlen = 0;
	ops.ooblen = 64;
	ops.datbuf = data_buf;
	ops.oobbuf = spare_buf;
	
	ret =  msm_nand_read_oob(current_mtd, ((PARAMETER_START_BLOCK+1) * 64)*2048, &ops);
	memcpy(orion_param_data,data_buf,sizeof(struct orion_param));
}
EXPORT_SYMBOL(msm_read_param);
/**
 * msm_write_param 		-	[msm nand Interface] write parameter data to param block
 * @param	orion_param
 *
 */
void msm_write_param(struct orion_param *orion_param_data)
{
	int ret;
	char data_buf[2048] = { 0, };
	char spare_buf[64] = { 0, };
    struct mtd_oob_ops ops;
	struct erase_info *param_erase_info;

	param_erase_info = kzalloc(sizeof(struct erase_info), GFP_KERNEL);
	param_erase_info->mtd = current_mtd;
	param_erase_info->addr = (PARAMETER_START_BLOCK + 1) * 64 * 2048;
	param_erase_info->len = current_mtd->erasesize;
	if(!msm_nand_erase(current_mtd, param_erase_info)) {
		printk("ERASE OK!\n");
	}

	memset(data_buf,0x0,2048);
	memset(spare_buf,0xFF,64);
	memcpy(data_buf,orion_param_data,sizeof(struct orion_param));

	ops.mode = MTD_OOB_RAW;
	ops.len = 2048+64;
	ops.retlen = 0;
	ops.ooblen = 64;
	ops.datbuf = data_buf;
	ops.oobbuf = spare_buf;
	
	ret =  msm_nand_write_oob(current_mtd, ((PARAMETER_START_BLOCK+1) * 64)*2048, &ops);

}
EXPORT_SYMBOL(msm_write_param);

/**
 * msm_nand_scan - [msm_nand Interface] Scan for the msm_nand device
 * @param mtd		MTD device structure
 * @param maxchips	Number of chips to scan for
 *
 * This fills out all the not initialized function pointers
 * with the defaults.
 * The flash ID is read and the mtd/chip structures are
 * filled with the appropriate values.
 */
int msm_nand_scan(struct mtd_info *mtd, int maxchips)
{
	struct msm_nand_chip *chip = mtd->priv;
	uint32_t flash_id = 0;
	uint8_t dev_found = 0;
	uint8_t wide_bus;
	uint8_t index;

	/* Probe the Flash device for ONFI compliance */
	if (!flash_onfi_probe(chip)) {

		index = 0;
		dev_found = 1;

	} else {

		/* Read the Flash ID from the Nand Flash Device */
		flash_id = flash_read_id(chip);

		for (index = 1;	index < ARRAY_SIZE(supported_flash); index++)
			if ((flash_id & supported_flash[index].mask) ==
				(supported_flash[index].flash_id &
				(supported_flash[index].mask))) {
				dev_found = 1;
				break;
			}
	}

	if (dev_found) {
		wide_bus       = supported_flash[index].widebus;
		mtd->size      = supported_flash[index].density;
		mtd->writesize = supported_flash[index].pagesize;
		mtd->oobsize   = supported_flash[index].oobsize;
		mtd->erasesize = supported_flash[index].blksize;

		pr_info("Found a supported NAND device\n");
		pr_info("NAND Id  : 0x%x\n", supported_flash[index].
				flash_id);
		pr_info("Buswidth : %d Bits \n", (wide_bus) ? 16 : 8);
		pr_info("Density  : %d MByte\n", (mtd->size>>20));
		pr_info("Pagesize : %d Bytes\n", mtd->writesize);
		pr_info("Erasesize: %d Bytes\n", mtd->erasesize);
		pr_info("Oobsize  : %d Bytes\n", mtd->oobsize);
	} else {
		pr_err("Unsupported Nand,Id: 0x%x \n", flash_id);
		return -ENODEV;
	}

	chip->CFG0 = (3 <<  6)  /* 4 codeword per page for 2k nand */
		|  (516 <<  9)  /* 516 user data bytes */
		|   (10 << 19)  /* 10 parity bytes */
		|    (5 << 27)  /* 5 address cycles */
		|    (1 << 30)  /* Read status before data */
		|    (1 << 31)  /* Send read cmd */
		/* 0 spare bytes for 16 bit nand or 1 spare bytes for 8 bit */
		| ((wide_bus) ? (0 << 23) : (1 << 23));

	chip->CFG1 = (0 <<  0)  /* Enable ecc */
		|    (7 <<  2)  /* 8 recovery cycles */
		|    (0 <<  5)  /* Allow CS deassertion */
		|  (465 <<  6)  /* Bad block marker location */
		|    (0 << 16)  /* Bad block in user data area */
		|    (2 << 17)  /* 6 cycle tWB/tRB */
		| (wide_bus << 1); /* Wide flash bit */

	chip->ecc_buf_cfg = 0x203;

	pr_info("CFG0 Init  : 0x%08x \n", chip->CFG0);
	pr_info("CFG1 Init  : 0x%08x \n", chip->CFG1);
	pr_info("ECCBUFCFG  : 0x%08x \n", chip->ecc_buf_cfg);

	if (mtd->oobsize == 64) {
		mtd->oobavail = msm_nand_oob_64.oobavail;
		mtd->ecclayout = &msm_nand_oob_64;
	} else {
		pr_err("Unsupported Nand, oobsize: 0x%x \n",
				mtd->oobsize);
		return -ENODEV;
	}

	/* Fill in remaining MTD driver data */
	mtd->type = MTD_NANDFLASH;
	mtd->flags = MTD_CAP_NANDFLASH;
	/* mtd->ecctype = MTD_ECC_SW; */
	mtd->erase = msm_nand_erase;
	mtd->point = NULL;
	mtd->unpoint = NULL;
	mtd->read = msm_nand_read;
	mtd->write = msm_nand_write;
	mtd->read_oob = msm_nand_read_oob;
	mtd->write_oob = msm_nand_write_oob;
	/* mtd->sync = msm_nand_sync; */
	mtd->lock = NULL;
	/* mtd->unlock = msm_nand_unlock; */
	mtd->suspend = msm_nand_suspend;
	mtd->resume = msm_nand_resume;
	mtd->block_isbad = msm_nand_block_isbad;
	mtd->block_markbad = msm_nand_block_markbad;
	mtd->owner = THIS_MODULE;

	/* Unlock whole block */
	/* msm_nand_unlock_all(mtd); */

	/* return this->scan_bbt(mtd); */
	//msm_scan_bbt(mtd);
	current_mtd = mtd; // for PARAMETER block
	return 0;
}
EXPORT_SYMBOL_GPL(msm_nand_scan);

/**
 * msm_nand_release - [msm_nand Interface] Free resources held by the msm_nand device
 * @param mtd		MTD device structure
 */
void msm_nand_release(struct mtd_info *mtd)
{
	/* struct msm_nand_chip *this = mtd->priv; */

#ifdef CONFIG_MTD_PARTITIONS
	/* Deregister partitions */
	del_mtd_partitions(mtd);
#endif
	/* Deregister the device */
	del_mtd_device(mtd);
}
EXPORT_SYMBOL_GPL(msm_nand_release);

#ifdef CONFIG_MTD_PARTITIONS
static const char *part_probes[] = { "cmdlinepart", NULL,  };
#endif

struct msm_nand_info {
	struct mtd_info		mtd;
	struct mtd_partition	*parts;
	struct msm_nand_chip	msm_nand;
};

static int __devinit msm_nand_probe(struct platform_device *pdev)
{
	struct msm_nand_info *info;
	struct flash_platform_data *pdata = pdev->dev.platform_data;
	int err;

	if (pdev->num_resources != 1) {
		pr_err("invalid num_resources");
		return -ENODEV;
	}
	if (pdev->resource[0].flags != IORESOURCE_DMA) {
		pr_err("invalid resource type");
		return -ENODEV;
	}

	info = kzalloc(sizeof(struct msm_nand_info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->msm_nand.dev = &pdev->dev;

	init_waitqueue_head(&info->msm_nand.wait_queue);

	info->msm_nand.dma_channel = pdev->resource[0].start;
	/* this currently fails if dev is passed in */
	info->msm_nand.dma_buffer =
		dma_alloc_coherent(/*dev*/ NULL, MSM_NAND_DMA_BUFFER_SIZE,
				   &info->msm_nand.dma_addr, GFP_KERNEL);
	if (info->msm_nand.dma_buffer == NULL) {
		err = -ENOMEM;
		goto out_free_info;
	}

	pr_info("allocated dma buffer at %p, dma_addr %x\n",
		info->msm_nand.dma_buffer, info->msm_nand.dma_addr);

	info->mtd.name = pdev->dev.bus_id;
	info->mtd.priv = &info->msm_nand;
	info->mtd.owner = THIS_MODULE;

	if (msm_nand_scan(&info->mtd, 1))
		if (msm_onenand_scan(&info->mtd, 1)) {
			err = -ENXIO;
			goto out_free_dma_buffer;
		}

#ifdef CONFIG_MTD_PARTITIONS
	err = parse_mtd_partitions(&info->mtd, part_probes, &info->parts, 0);
	if (err > 0)
		add_mtd_partitions(&info->mtd, info->parts, err);
	else if (err <= 0 && pdata && pdata->parts)
		add_mtd_partitions(&info->mtd, pdata->parts, pdata->nr_parts);
	else
#endif
		err = add_mtd_device(&info->mtd);

	dev_set_drvdata(&pdev->dev, info);

	return 0;

out_free_dma_buffer:
	dma_free_coherent(/*dev*/ NULL, SZ_4K, info->msm_nand.dma_buffer,
			  info->msm_nand.dma_addr);
out_free_info:
	kfree(info);

	return err;
}

static int __devexit msm_nand_remove(struct platform_device *pdev)
{
	struct msm_nand_info *info = dev_get_drvdata(&pdev->dev);

	dev_set_drvdata(&pdev->dev, NULL);

	if (info) {
#ifdef CONFIG_MTD_PARTITIONS
		if (info->parts)
			del_mtd_partitions(&info->mtd);
		else
#endif
			del_mtd_device(&info->mtd);

		msm_nand_release(&info->mtd);
		dma_free_coherent(/*dev*/ NULL, SZ_4K,
				  info->msm_nand.dma_buffer,
				  info->msm_nand.dma_addr);
		kfree(info);
	}

	return 0;
}

#define DRIVER_NAME "msm_nand"

static struct platform_driver msm_nand_driver = {
	.probe		= msm_nand_probe,
	.remove		= __devexit_p(msm_nand_remove),
	.driver = {
		.name		= DRIVER_NAME,
	}
};

MODULE_ALIAS(DRIVER_NAME);

static int __init msm_nand_init(void)
{
	return platform_driver_register(&msm_nand_driver);
}

static void __exit msm_nand_exit(void)
{
	platform_driver_unregister(&msm_nand_driver);
}

module_init(msm_nand_init);
module_exit(msm_nand_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("msm_nand flash driver code");
