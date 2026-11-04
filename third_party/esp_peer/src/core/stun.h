/* stun interface.
 * Copyright (c) 2021 sepfy
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#ifndef ESP_PEER_CORE_STUN_H
#define ESP_PEER_CORE_STUN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "ice.h"

#define MAGIC_COOKIE 0x2112A442
#define STUN_FINGERPRINT_XOR 0x5354554e

typedef enum {
    STUN_CLASS_REQUEST = 0,
    STUN_CLASS_INDICATION = 0x10,
    STUN_CLASS_RESPONSE = 0x100,
    STUN_CLASS_ERROR = 0x110,
} stun_class_t;

typedef enum {
    STUN_METHOD_BINDING = 1,
    STUN_METHOD_ALLOCATE = 3,
    STUN_METHOD_REFRESH = 4,
    STUN_METHOD_SEND = 6,
    STUN_METHOD_DATA = 7,
    STUN_METHOD_CREATE_PERMISSION = 8,
    STUN_METHOD_CHANNEL_BIND = 9,
} stun_method_t;

typedef enum {
    STUN_ATTR_TYPE_MAPPED_ADDRESS = 1,
    STUN_ATTR_TYPE_USERNAME = 6,
    STUN_ATTR_TYPE_MESSAGE_INTEGRITY = 8,
    STUN_ATTR_TYPE_ERROR_CODE = 9,
    STUN_ATTR_TYPE_CHANNEL_NUM = 12,
    STUN_ATTR_TYPE_LIFETIME = 13,
    STUN_ATTR_TYPE_XOR_PEER_ADDRESS = 18,
    STUN_ATTR_TYPE_DATA = 19,
    STUN_ATTR_TYPE_REALM = 20,
    STUN_ATTR_TYPE_NONCE = 21,
    STUN_ATTR_TYPE_XOR_RELAYED_ADDRESS = 22,
    STUN_ATTR_TYPE_EVEN_PORT = 24,
    STUN_ATTR_TYPE_REQUESTED_TRANSPORT = 25,
    STUN_ATTR_TYPE_DONT_FRAGMENT = 26,
    STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS = 32,
    STUN_ATTR_TYPE_PRIORITY = 36,
    STUN_ATTR_TYPE_USE_CANDIDATE = 37,
    STUN_ATTR_TYPE_SOFTWARE = 0x8022,
    STUN_ATTR_ALTERNATE_SERVER = 0x8023,
    STUN_ATTR_TYPE_FINGERPRINT = 0x8028,
    STUN_ATTR_TYPE_ICE_CONTROLLED = 0x8029,
    STUN_ATTR_TYPE_ICE_CONTROLLING = 0x802a,
    STUN_ATTR_TYPE_NETWORK_COST = 0xc057,
} stun_attr_type_t;

typedef enum {
    STUN_CREDENTIAL_NONE,
    STUN_CREDENTIAL_SHORT_TERM,
    STUN_CREDENTIAL_LONG_TERM,
} stun_credential_t;

typedef enum {
    STUN_ICE_ROLE_CONTROLLING = 1,
    STUN_ICE_ROLE_CONTROLLED = 2,
} stun_ice_role_t;

typedef struct {
    uint16_t type;
    uint16_t length;
    uint32_t magic_cookie;
    uint32_t transaction_id[3];
} stun_header_t;

typedef struct {
    uint16_t type;
    uint16_t length;
    char value[0];
} stun_attr_t;

typedef struct {
    stun_class_t stunclass;
    stun_method_t stunmethod;
    uint8_t *data;
    uint16_t data_size;
    char *username;
    char *realm;
    char *nonce;
    uint16_t username_size;
    uint16_t realm_size;
    uint16_t nonce_size;
    uint16_t err_code;
    uint16_t life_time;
    uint32_t channel_num;
    esp_peer_addr_t mapped_addr;
    esp_peer_addr_t relayed_addr;
    stun_ice_role_t role;
    uint8_t use_candidate : 1;
    uint8_t has_fingerprint : 1;
    uint16_t integrity_pos;
    uint8_t buf[1400];
    size_t size;
} stun_msg_t;

_Static_assert(sizeof(stun_header_t) == 20, "vendor STUN header size");
_Static_assert(sizeof(stun_msg_t) == 1496, "vendor STUN message size");
_Static_assert(offsetof(stun_msg_t, buf) == 92, "vendor STUN buffer offset");
_Static_assert(offsetof(stun_msg_t, size) == 1492, "vendor STUN size offset");

void stun_msg_create(stun_msg_t *msg, uint16_t type, bool random_id);
void stun_set_mapped_address(char *value, uint8_t *mask, esp_peer_addr_t *addr);
void stun_get_mapped_address(char *value, uint8_t *mask, esp_peer_addr_t *addr);
void stun_parse_msg_buf(stun_msg_t *msg);
void stun_calculate_fingerprint(char *buf, size_t len, uint32_t *fingerprint);
int stun_msg_write_attr(stun_msg_t *msg, stun_attr_type_t type, uint16_t length, char *value);
int stun_msg_finish(stun_msg_t *msg, stun_credential_t credential,
                    const char *password, size_t password_len);
int stun_probe(uint8_t *buf, size_t size);
int stun_msg_is_valid(uint8_t *buf, size_t size, char *password);
int stun_msg_is_valid_response(uint8_t *buf, size_t size, stun_credential_t credential,
                               char *user, char *user_realm, char *password);
#endif
