#include "diagnostics_zip.h"
#include <string.h>

static void put16(uint8_t *p, uint16_t value)
{ p[0] = value; p[1] = value >> 8; }
static void put32(uint8_t *p, uint32_t value)
{ put16(p, value); put16(p + 2, value >> 16); }
static uint32_t crc32(const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t crc = UINT32_MAX;
    while (size--) {
        crc ^= *p++;
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

bool diagnostics_zip_write(const diagnostics_zip_entry_t *entries, size_t count,
                           diagnostics_zip_write_fn write, void *ctx)
{
    if (!entries || !write || count == 0 || count > UINT16_MAX) return false;
    uint64_t total = 22;
    for (size_t i = 0; i < count; ++i) {
        if (!entries[i].name || (!entries[i].data && entries[i].size)) return false;
        size_t name_size = strlen(entries[i].name);
        if (!name_size || name_size > UINT16_MAX) return false;
#if SIZE_MAX > UINT32_MAX
        if (entries[i].size > UINT32_MAX) return false;
#endif
        total += UINT64_C(76) + 2 * name_size + entries[i].size;
        if (total > UINT32_MAX) return false;
    }
    uint32_t offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const diagnostics_zip_entry_t *e = &entries[i];
        uint16_t name_size = strlen(e->name);
        uint8_t header[30] = {0};
        put32(header, 0x04034b50); put16(header + 4, 20);
        put16(header + 12, 33); /* 1980-01-01; capture time is in JSON. */
        put32(header + 14, crc32(e->data, e->size));
        put32(header + 18, e->size); put32(header + 22, e->size);
        put16(header + 26, name_size);
        if (!write(header, sizeof(header), ctx) || !write(e->name, name_size, ctx) ||
            (e->size && !write(e->data, e->size, ctx))) return false;
        offset += sizeof(header) + name_size + e->size;
    }
    uint32_t directory_offset = offset, entry_offset = 0;
    for (size_t i = 0; i < count; ++i) {
        const diagnostics_zip_entry_t *e = &entries[i];
        uint16_t name_size = strlen(e->name);
        uint8_t header[46] = {0};
        put32(header, 0x02014b50); put16(header + 4, 20); put16(header + 6, 20);
        put16(header + 14, 33);
        put32(header + 16, crc32(e->data, e->size));
        put32(header + 20, e->size); put32(header + 24, e->size);
        put16(header + 28, name_size); put32(header + 42, entry_offset);
        if (!write(header, sizeof(header), ctx) || !write(e->name, name_size, ctx)) return false;
        offset += sizeof(header) + name_size;
        entry_offset += 30 + name_size + e->size;
    }
    uint8_t end[22] = {0};
    put32(end, 0x06054b50); put16(end + 8, count); put16(end + 10, count);
    put32(end + 12, offset - directory_offset); put32(end + 16, directory_offset);
    return write(end, sizeof(end), ctx);
}
