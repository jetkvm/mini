/* SPDX-License-Identifier: MIT */
#pragma once

#include "esp_err.h"

esp_err_t tf_sd_trace_start(void);
void tf_sd_trace_dump_and_stop(void);
