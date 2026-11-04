#ifndef ESP_PEER_CORE_UTILS_H
#define ESP_PEER_CORE_UTILS_H

#include "ice.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void utils_random_string(char *s, int len);
void utils_get_hmac_sha1(const char *input, size_t input_len, const char *key,
                         size_t key_len, unsigned char *output);
void utils_get_md5(const char *input, size_t input_len, unsigned char *output);
uint32_t utils_get_timestamp(void);
int ports_get_host_addr(esp_peer_addr_t *addr);
int ports_resolve_addr(const char *host, esp_peer_addr_t *addr);
const char *esp_peer_addr_to_str(esp_peer_addr_t *addr, char *p);
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *data, unsigned int length);
uint32_t utils_crc32c_le(const uint8_t *data, unsigned int length);
uint32_t utils_crc32c_be(uint32_t crc, const uint8_t *data, unsigned int length);
void show_measure(void);
void measure_enable(bool enable);
void measure_start(const char *tag);
void measure_stop(const char *tag);

#endif
