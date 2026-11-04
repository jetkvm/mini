/* SPDX-License-Identifier: MIT */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct jetkvm_http_range_reader jetkvm_http_range_reader_t;

typedef struct {
    bool usable;
    uint64_t size;
    char reason[192];
} jetkvm_http_range_check_t;

esp_err_t jetkvm_http_range_check(const char *url,
                                  jetkvm_http_range_check_t *check);
esp_err_t jetkvm_http_range_reader_open(const char *url,
                                        jetkvm_http_range_reader_t **reader);
void jetkvm_http_range_reader_close(jetkvm_http_range_reader_t *reader);
esp_err_t jetkvm_http_range_reader_read(jetkvm_http_range_reader_t *reader,
                                        uint64_t image_size, uint64_t offset,
                                        void *buffer, size_t size);
