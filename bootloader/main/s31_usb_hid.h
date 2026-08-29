/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"

/*
 * Take ownership of the USB controller and enumerate HID devices on hart0.
 *
 * Linux must NOT claim the same controller: there is one OTG peripheral on
 * this SoC and a peripheral belongs to one hart. Disable usb_otghs and usb_phy
 * in the device tree before enabling this. See docs/usb-on-hart0-plan.md.
 */
esp_err_t s31_usb_hid_start(void);
