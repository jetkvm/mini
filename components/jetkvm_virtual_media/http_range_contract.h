/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stdint.h>

bool jetkvm_http_url_is_supported(const char *url);
bool jetkvm_http_parse_content_range(const char *value, uint64_t *first,
                                     uint64_t *last, uint64_t *total);
