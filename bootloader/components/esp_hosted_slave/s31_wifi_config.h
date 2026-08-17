/* SPDX-License-Identifier: Apache-2.0 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

esp_err_t s31_wifi_config_init(void);
esp_err_t s31_wifi_config_autostart(void);

