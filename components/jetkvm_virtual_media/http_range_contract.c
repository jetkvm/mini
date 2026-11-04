/* SPDX-License-Identifier: MIT */

#include "http_range_contract.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

bool jetkvm_http_url_is_supported(const char *url)
{
    if (url == NULL) {
        return false;
    }
    const char *authority = NULL;
    if (strncmp(url, "http://", 7U) == 0) {
        authority = url + 7U;
    } else if (strncmp(url, "https://", 8U) == 0) {
        authority = url + 8U;
    } else {
        return false;
    }
    if (*authority == '\0' || *authority == '/' || *authority == '?' ||
        *authority == '#') {
        return false;
    }
    for (const unsigned char *cursor = (const unsigned char *)url;
         *cursor != '\0'; ++cursor) {
        if (iscntrl(*cursor) || isspace(*cursor)) {
            return false;
        }
    }
    return true;
}

static bool parse_u64(const char **cursor, uint64_t *value)
{
    if (!isdigit((unsigned char)**cursor)) {
        return false;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long parsed = strtoull(*cursor, &end, 10);
    if (errno == ERANGE || end == *cursor) {
        return false;
    }
    *cursor = end;
    *value = (uint64_t)parsed;
    return true;
}

bool jetkvm_http_parse_content_range(const char *value, uint64_t *first,
                                     uint64_t *last, uint64_t *total)
{
    if (value == NULL || first == NULL || last == NULL || total == NULL) {
        return false;
    }
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (strncasecmp(value, "bytes", 5U) != 0 ||
        !isspace((unsigned char)value[5])) {
        return false;
    }
    value += 5U;
    while (isspace((unsigned char)*value)) {
        value++;
    }
    if (!parse_u64(&value, first) || *value++ != '-' ||
        !parse_u64(&value, last) || *value++ != '/' ||
        !parse_u64(&value, total)) {
        return false;
    }
    while (isspace((unsigned char)*value)) {
        value++;
    }
    return *value == '\0' && *first <= *last && *last < *total;
}
