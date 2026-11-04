#pragma once

static inline void test_log(const char *tag, const char *format, ...)
{
    (void)tag;
    (void)format;
}

#define ESP_LOGI(tag, ...) test_log(tag, __VA_ARGS__)
