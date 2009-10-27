/* linux/include/mach/rpc_hsusb.h
 *
 * Copyright (c) 2008 QUALCOMM USA, INC.
 *
 * All source code in this file is licensed under the following license except
 * where indicated.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you can find it at http://www.fsf.org
 */

#ifndef __ASM_ARCH_MSM_RPC_HSUSB_H
#define __ASM_ARCH_MSM_RPC_HSUSB_H

#include <mach/msm_rpcrouter.h>

int msm_hsusb_rpc_connect(void);
int msm_hsusb_phy_reset(void);
int msm_hsusb_vbus_powerup(void);
int msm_hsusb_vbus_shutdown(void);
int msm_hsusb_send_productID(uint32_t product_id);
int msm_hsusb_send_serial_number(char *serial_number);
int msm_hsusb_is_serial_num_null(uint32_t val);
int msm_hsusb_reset_rework_installed(void);
int msm_hsusb_enable_pmic_ulpidata0(void);
int msm_hsusb_disable_pmic_ulpidata0(void);
int msm_hsusb_rpc_close(void);

int msm_chg_rpc_connect(void);
int msm_chg_usb_charger_connected(uint32_t type);
int msm_chg_usb_i_is_available(uint32_t sample);
int msm_chg_usb_i_is_not_available(void);
int msm_chg_usb_charger_disconnected(void);
int msm_chg_rpc_close(void);

#endif
