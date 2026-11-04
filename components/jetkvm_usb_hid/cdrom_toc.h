#pragma once

#include <stdint.h>

/* READ TOC/PMA/ATIP (0x43) for a CD-ROM image: one Mode 1 data track that
 * starts at LBA 0 and ends at the lead-out, LBA block_count (2048-byte
 * blocks). Formats 0 (TOC), 1 (session information) and 2 (full TOC) are
 * supported, from CDB byte 2 or, when that is 0, from the SFF-8020i bits 7-6
 * of byte 9 that macOS and Linux use.
 *
 * Writes at most the allocation length (CDB bytes 7-8) and out_size bytes
 * and returns that count, or returns -1 for an unsupported format or track
 * (ILLEGAL REQUEST, INVALID FIELD IN CDB). */
int32_t jetkvm_cdrom_read_toc(const uint8_t cdb[10], uint32_t block_count,
                              uint8_t *out, uint32_t out_size);
