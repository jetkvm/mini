#pragma once

#include <stddef.h>

/*
 * The regulatory text that Menu > Regulatory shows, one paragraph per entry,
 * in the order shown. The text is fixed in the image for its board variant
 * (JETKVM_BOARD_WIFI): no setting, RPC or web UI changes or hides it. To
 * show another marking, such as an ISED number or a Japan mark, add an entry
 * in regulatory.c; the page lays out every entry.
 */
extern const char *const jetkvm_regulatory_lines[];
extern const size_t jetkvm_regulatory_line_count;
