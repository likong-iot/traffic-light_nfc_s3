/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

#include <stdbool.h>
#include "iot_usbh_cdc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** USB Modem ID */
typedef struct {
    usb_device_match_id_t match_id; /*!< USB device match ID */

    int modem_itf_num;  /*!< USB modem interface number, (Required) */
    int at_itf_num;  /*!< USB AT interface number, (Optional, set -1 if not use) */
    const char *name;  /*!< USB Modem name, can be NULL */
} usb_modem_id_t;

/** USB modem runtime diagnostic snapshot */
typedef struct {
    bool device_seen;              /*!< A USB CDC device connection event has been seen */
    bool id_matched;               /*!< Device matched one entry from modem_id_list */
    bool dte_connected;            /*!< DTE connect callback has completed */
    bool modem_port_open;          /*!< Configured modem/PPP CDC port was opened */
    bool at_port_open;             /*!< Optional dedicated AT CDC port was opened */

    uint8_t dev_addr;              /*!< USB device address */
    uint16_t vid;                  /*!< Actual USB VID */
    uint16_t pid;                  /*!< Actual USB PID */
    uint16_t bcd_device;           /*!< Actual USB bcdDevice */
    uint8_t device_class;          /*!< Actual USB device class */
    uint8_t device_subclass;       /*!< Actual USB device subclass */
    uint8_t device_protocol;       /*!< Actual USB device protocol */
    uint8_t config_value;          /*!< Active USB configuration value */
    uint8_t config_interfaces;     /*!< Number of interfaces in active configuration */
    int cdc_matched_intf_num;      /*!< Interface matched by the CDC event layer */

    const char *matched_name;      /*!< Matched modem name from modem_id_list */
    int modem_itf_num;             /*!< Configured modem/PPP interface number */
    int at_itf_num;                /*!< Configured dedicated AT interface number, -1 if disabled */
    bool modem_itf_present;        /*!< Configured modem interface exists in active configuration */
    bool at_itf_present;           /*!< Configured AT interface exists in active configuration */
} usb_modem_diag_t;

#ifdef __cplusplus
}
#endif
