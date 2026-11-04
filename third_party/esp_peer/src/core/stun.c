/* stun implementation.
 * Copyright (c) 2021 sepfy
 * See LICENSE and LICENSES/ for copyright and licensing notices.
 */
#include "utils.h"
#include "peer_platform.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stun.h"

#define LOGE(text) esp_log((esp_log_config_t){.data=1}, "STUN", "E (%lu) %s: " text "\n", esp_log_timestamp(), "STUN")

void stun_msg_create(stun_msg_t *msg, uint16_t type, bool random_id)
{
    stun_header_t *header = (stun_header_t *)msg->buf;
    msg->stunmethod = type & 0xf;
    memset(msg->buf, 0, sizeof(msg->buf));
    header->type = htons(type);
    header->magic_cookie = htonl(MAGIC_COOKIE);
    if (random_id) {
        header->transaction_id[0] = rand();
        header->transaction_id[1] = rand();
        header->transaction_id[2] = rand();
    } else {
        header->transaction_id[0] = htonl(0x12345678);
        header->transaction_id[1] = htonl(0x90abcdef);
        header->transaction_id[2] = htonl(0x12345678);
    }
    msg->size = sizeof(stun_header_t);
}

void stun_set_mapped_address(char *value, uint8_t *mask, esp_peer_addr_t *addr)
{
    uint8_t *family = (uint8_t *)(value + 1);
    uint16_t *port = (uint16_t *)(value + 2);
    if (addr->family == AF_INET) {
        uint32_t *ipv4 = (uint32_t *)(value + 4);
        *family = 1;
        if (mask) {
            *port = htons(addr->port) ^ *(uint16_t *)mask;
            *ipv4 = *(uint32_t *)addr->ipv4 ^ *(uint32_t *)mask;
        } else {
            *port = htons(addr->port);
            *ipv4 = *(uint32_t *)addr->ipv4;
        }
    } else if (addr->family == AF_INET6) {
        uint8_t *ipv6 = (uint8_t *)(value + 4);
        *family = 2;
        if (mask) {
            *port = htons(addr->port) ^ *(uint16_t *)mask;
            for (int i = 0; i < 16; i++) {
                ipv6[i] = addr->ipv6[i] ^ mask[i];
            }
        } else {
            *port = htons(addr->port);
            memcpy(ipv6, addr->ipv6, 16);
        }
    }
}

void stun_get_mapped_address(char *value, uint8_t *mask, esp_peer_addr_t *addr)
{
    addr->family = value[1] == 2 ? AF_INET6 : AF_INET;
    switch (addr->family) {
    case AF_INET:
        addr->port = ntohs(*(uint16_t *)(value + 2) ^ *(uint16_t *)mask);
        *(uint32_t *)addr->ipv4 = *(uint32_t *)(value + 4) ^ *(uint32_t *)mask;
        break;
    case AF_INET6:
        addr->port = ntohs(*(uint16_t *)(value + 2) ^ *(uint16_t *)mask);
        for (int i = 0; i < 16; i++) {
            addr->ipv6[i] = value[4 + i] ^ mask[i];
        }
        break;
    default:
        break;
    }
}

void stun_parse_msg_buf(stun_msg_t *msg)
{
    stun_header_t *header = (stun_header_t *)msg->buf;
    int length = ntohs(header->length) + sizeof(stun_header_t);
    int pos = sizeof(stun_header_t);
    uint8_t mask[16];
    msg->stunclass = ntohs(header->type);
    if ((msg->stunclass & STUN_CLASS_ERROR) == STUN_CLASS_ERROR) {
        msg->stunclass = STUN_CLASS_ERROR;
    } else if ((msg->stunclass & STUN_CLASS_INDICATION) == STUN_CLASS_INDICATION) {
        msg->stunclass = STUN_CLASS_INDICATION;
        msg->data = NULL;
    } else if ((msg->stunclass & STUN_CLASS_RESPONSE) == STUN_CLASS_RESPONSE) {
        msg->stunclass = STUN_CLASS_RESPONSE;
    } else {
        msg->stunclass = STUN_CLASS_REQUEST;
    }
    msg->stunmethod = ntohs(header->type) & 0xf;
    msg->has_fingerprint = 0;
    msg->integrity_pos = 0;
    msg->use_candidate = 0;
    msg->username_size = 0;
    msg->realm_size = 0;
    msg->nonce_size = 0;
    while (pos < length) {
        stun_attr_t *attr = (stun_attr_t *)(msg->buf + pos);
        int attr_size = ntohs(attr->length);
        switch (ntohs(attr->type)) {
        case STUN_ATTR_TYPE_MAPPED_ADDRESS:
            memset(mask, 0, sizeof(mask));
            stun_get_mapped_address(attr->value, mask, &msg->mapped_addr);
            break;
        case STUN_ATTR_TYPE_USERNAME:
            msg->username = attr->value;
            msg->username_size = attr_size;
            break;
        case STUN_ATTR_TYPE_MESSAGE_INTEGRITY:
            msg->integrity_pos = pos;
            break;
        case STUN_ATTR_TYPE_LIFETIME:
            msg->life_time = ntohs(*(uint16_t *)attr->value);
            break;
        case STUN_ATTR_TYPE_REALM:
            msg->realm = attr->value;
            msg->realm_size = attr_size;
            break;
        case STUN_ATTR_TYPE_NONCE:
            msg->nonce = attr->value;
            msg->nonce_size = attr_size;
            break;
        case STUN_ATTR_TYPE_XOR_PEER_ADDRESS:
        case STUN_ATTR_TYPE_XOR_RELAYED_ADDRESS:
            *(uint32_t *)mask = htonl(MAGIC_COOKIE);
            memcpy(mask + 4, header->transaction_id, sizeof(header->transaction_id));
            stun_get_mapped_address(attr->value, mask, &msg->relayed_addr);
            break;
        case STUN_ATTR_TYPE_DATA:
            msg->data = (uint8_t *)attr->value;
            msg->data_size = ntohs(attr->length);
            break;
        case STUN_ATTR_TYPE_CHANNEL_NUM:
            msg->channel_num = ntohs(*(uint16_t *)attr->value);
            break;
        case STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS:
            *(uint32_t *)mask = htonl(MAGIC_COOKIE);
            memcpy(mask + 4, header->transaction_id, sizeof(header->transaction_id));
            stun_get_mapped_address(attr->value, mask, &msg->mapped_addr);
            break;
        case STUN_ATTR_TYPE_USE_CANDIDATE:
            msg->use_candidate = 1;
            break;
        case STUN_ATTR_TYPE_FINGERPRINT:
            msg->has_fingerprint = 1;
            break;
        case STUN_ATTR_TYPE_ICE_CONTROLLED:
            msg->role = STUN_ICE_ROLE_CONTROLLED;
            break;
        case STUN_ATTR_TYPE_ICE_CONTROLLING:
            msg->role = STUN_ICE_ROLE_CONTROLLING;
            break;
        case STUN_ATTR_TYPE_ERROR_CODE:
            msg->err_code = (uint8_t)(attr->value[2] & 7) * 100 + (uint8_t)attr->value[3];
            break;
        default:
            break;
        }
        pos += 4 * ((attr_size + 3) / 4) + sizeof(stun_attr_t);
    }
}

void stun_calculate_fingerprint(char *buf, size_t len, uint32_t *fingerprint)
{
    uint32_t crc = utils_crc32c_le((uint8_t *)buf, len);
    *fingerprint = htonl(crc ^ STUN_FINGERPRINT_XOR);
}

int stun_msg_write_attr(stun_msg_t *msg, stun_attr_type_t type, uint16_t length, char *value)
{
    stun_header_t *header = (stun_header_t *)msg->buf;
    stun_attr_t *stun_attr = (stun_attr_t *)(msg->buf + msg->size);
    stun_attr->type = htons(type);
    stun_attr->length = htons(length);
    if (type == STUN_ATTR_TYPE_XOR_PEER_ADDRESS || type == STUN_ATTR_TYPE_XOR_MAPPED_ADDRESS) {
        esp_peer_addr_t *addr = (esp_peer_addr_t *)value;
        length = addr->family == AF_INET6 ? 20 : 8;
        stun_attr->length = htons(length);
        uint8_t mask[16];
        *(uint32_t *)mask = htonl(MAGIC_COOKIE);
        memcpy(mask + 4, header->transaction_id, sizeof(header->transaction_id));
        stun_set_mapped_address(stun_attr->value, mask, addr);
    } else if (type == STUN_ATTR_TYPE_CHANNEL_NUM) {
        stun_attr->value[0] = value[0];
        stun_attr->value[1] = value[1];
        stun_attr->length = htons(4);
    } else if (type == STUN_ATTR_TYPE_ERROR_CODE) {
        uint16_t code = *(uint16_t *)value;
        stun_attr->value[0] = stun_attr->value[1] = 0;
        stun_attr->value[2] = code / 100;
        stun_attr->value[3] = code % 100;
        stun_attr->length = htons(4);
    } else if (type == STUN_ATTR_TYPE_LIFETIME) {
        *(uint32_t *)stun_attr->value = htonl(*(uint16_t *)value);
        stun_attr->length = htons(4);
    } else if (type == STUN_ATTR_TYPE_PRIORITY) {
        *(uint32_t *)stun_attr->value = htonl(*(uint32_t *)value);
    } else {
        memcpy(stun_attr->value, value, length);
    }
    length = 4 * ((length + 3) / 4);
    msg->size += length + sizeof(stun_attr_t);
    switch (type) {
    case STUN_ATTR_TYPE_REALM:
        msg->realm = value;
        break;
    case STUN_ATTR_TYPE_NONCE:
        msg->nonce = value;
        break;
    case STUN_ATTR_TYPE_USERNAME:
        msg->username = value;
        break;
    default:
        break;
    }
    return 0;
}

int stun_msg_finish(stun_msg_t *msg, stun_credential_t credential,
                    const char *password, size_t password_len)
{
    stun_header_t *header = (stun_header_t *)msg->buf;
    stun_attr_t *stun_attr;
    uint16_t header_length;
    if (credential == STUN_CREDENTIAL_NONE) {
        header_length = msg->size - sizeof(stun_header_t);
        header->length = htons(header_length);
        return 0;
    }
    if (password) {
        char hash_key[17];
        if (credential == STUN_CREDENTIAL_LONG_TERM) {
            if (!msg->username || !msg->realm) {
                return -1;
            }
            size_t key_size = strlen(msg->username) + strlen(msg->realm) + password_len + 3;
            char key[key_size];
            snprintf(key, sizeof(key), "%s:%s:%s", msg->username, msg->realm, password);
            utils_get_md5(key, strlen(key), (unsigned char *)hash_key);
            password = hash_key;
            password_len = 16;
        }
        stun_attr = (stun_attr_t *)(msg->buf + msg->size);
        header_length = msg->size;
        header->length = htons(header_length + 4);
        stun_attr->type = htons(STUN_ATTR_TYPE_MESSAGE_INTEGRITY);
        stun_attr->length = htons(20);
        utils_get_hmac_sha1((char *)msg->buf, msg->size, password, password_len,
                            (unsigned char *)stun_attr->value);
        msg->size += sizeof(stun_attr_t) + 20;
        if (msg->stunmethod == STUN_METHOD_ALLOCATE) {
            return 0;
        }
    }
    stun_attr = (stun_attr_t *)(msg->buf + msg->size);
    header_length = msg->size;
    header->length = htons(header_length - 12);
    stun_attr->type = htons(STUN_ATTR_TYPE_FINGERPRINT);
    stun_attr->length = htons(4);
    stun_calculate_fingerprint((char *)msg->buf, msg->size, (uint32_t *)stun_attr->value);
    msg->size += sizeof(stun_attr_t) + 4;
    return 0;
}

int stun_probe(uint8_t *buf, size_t size)
{
    stun_header_t *header;
    if (size < sizeof(stun_header_t)) {
        return -1;
    }
    header = (stun_header_t *)buf;
    if (header->magic_cookie != htonl(MAGIC_COOKIE)) {
        return -1;
    }
    return 0;
}

int stun_msg_is_valid(uint8_t *buf, size_t size, char *password)
{
    stun_msg_t msg;
    memcpy(msg.buf, buf, size);
    stun_parse_msg_buf(&msg);
    stun_header_t *header = (stun_header_t *)msg.buf;
    if (msg.has_fingerprint) {
        uint32_t fingerprint = 0;
        stun_calculate_fingerprint((char *)msg.buf, size - 8, &fingerprint);
        if (fingerprint != *(uint32_t *)(msg.buf + size - 4)) {
            LOGE("Fingerprint does not match.");
            return -1;
        }
    }
    if (msg.integrity_pos) {
        unsigned char message_integrity[20];
        header->length = htons(msg.integrity_pos + 4);
        int password_len = strlen(password);
        if (msg.realm_size && msg.username) {
            char key[256];
            char hash_key[17];
            memset(key, 0, sizeof(key));
            memset(hash_key, 0, sizeof(hash_key));
            snprintf(key, sizeof(key), "%.*s:%.*s:%s", msg.username_size, msg.username,
                      msg.realm_size, msg.realm, password);
            utils_get_md5(key, strlen(key), (unsigned char *)hash_key);
            password = hash_key;
            password_len = 16;
        }
        utils_get_hmac_sha1((char *)msg.buf, msg.integrity_pos, password, password_len,
                            message_integrity);
        if (memcmp(message_integrity, msg.buf + msg.integrity_pos + 4, 20) != 0) {
            LOGE("Message Integrity does not match.");
            return -1;
        }
    }
    return 0;
}

int stun_msg_is_valid_response(uint8_t *buf, size_t size, stun_credential_t credential,
                               char *user, char *user_realm, char *password)
{
    stun_msg_t msg;
    memcpy(msg.buf, buf, size);
    stun_parse_msg_buf(&msg);
    stun_header_t *header = (stun_header_t *)msg.buf;
    if (msg.has_fingerprint) {
        uint32_t fingerprint = 0;
        stun_calculate_fingerprint((char *)msg.buf, size - 8, &fingerprint);
        if (fingerprint != *(uint32_t *)(msg.buf + size - 4)) {
            return -1;
        }
    }
    if (msg.integrity_pos && password) {
        unsigned char message_integrity[20];
        header->length = htons(msg.integrity_pos + 4);
        int password_len = strlen(password);
        if (credential == STUN_CREDENTIAL_LONG_TERM) {
            size_t user_size = strlen(user);
            size_t realm_size = strlen(user_realm);
            char key[256] = {0};
            char hash_key[17] = {0};
            if (msg.username_size) {
                user = msg.username;
                user_size = msg.username_size;
            }
            if (msg.realm_size) {
                user_realm = msg.realm;
                realm_size = msg.realm_size;
            }
            snprintf(key, sizeof(key), "%.*s:%.*s:%s", user_size, user, realm_size,
                      user_realm, password);
            utils_get_md5(key, strlen(key), (unsigned char *)hash_key);
            password = hash_key;
            password_len = 16;
        }
        utils_get_hmac_sha1((char *)msg.buf, msg.integrity_pos, password, password_len,
                            message_integrity);
        if (memcmp(message_integrity, msg.buf + msg.integrity_pos + 4, 20) != 0) {
            return -1;
        }
    }
    return 0;
}
