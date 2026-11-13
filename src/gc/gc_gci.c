#include "gc_gci.h"

#include <stdio.h>
#include <string.h>

#include "gc_fs.h"

static uint32_t crc_table[256];

void gf_crc_init(void) {
    for (uint32_t i = 0; i < 256U; i++) {
        uint32_t c = i;

        for (int bit = 0; bit < 8; bit++)
            c = (c & 1U) ? 0xEDB88320U ^ (c >> 1) : c >> 1;
        crc_table[i] = c;
    }
}

uint32_t gf_crc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = data;
    uint32_t c = ~crc;

    while (len--)
        c = crc_table[(c ^ *p++) & 0xFFU] ^ (c >> 8);
    return ~c;
}

static size_t filename_length(const uint8_t *entry) {
    size_t len = 0;

    while (len < GCFS_ENT_FILENAME_LEN && entry[GCFS_ENT_FILENAME + len] != 0)
        len++;
    return len;
}

gci_result_t gci_check(const uint8_t *entry, uint32_t file_size) {
    uint32_t blocks = gcfs_get16(&entry[GCFS_ENT_BLOCK_COUNT]);

    /* 2043 is the most any card holds. A smaller limit is the card's own to report. */
    if (!gcfs_entry_used(entry) || filename_length(entry) == 0 || entry[GCFS_ENT_FILENAME] == 0xFF
        || blocks == 0 || blocks > 2043U)
        return GCI_BAD_ENTRY;

    if (file_size != GCI_HEADER_SIZE + blocks * GCFS_BLOCK_SIZE)
        return GCI_BAD_SIZE;

    return GCI_OK;
}

void gci_identity(const uint8_t *entry, gci_id_t *id) {
    size_t len = filename_length(entry);
    uint32_t fnv = 2166136261U;

    id->a = gf_crc32(0, &entry[GCFS_ENT_GAMECODE], 6);
    id->a = gf_crc32(id->a, &entry[GCFS_ENT_FILENAME], len);

    for (size_t i = 0; i < 6U + len; i++) {
        fnv ^= i < 6U ? entry[GCFS_ENT_GAMECODE + i] : entry[GCFS_ENT_FILENAME + i - 6U];
        fnv *= 16777619U;
    }
    id->b = fnv;
}

uint32_t gci_entry_crc(const uint8_t *entry) {
    static const uint8_t no_block[2] = {0, 0};
    uint32_t crc = gf_crc32(0, entry, GCFS_ENT_FIRST_BLOCK);

    crc = gf_crc32(crc, no_block, sizeof(no_block));
    return gf_crc32(crc, &entry[GCFS_ENT_FIRST_BLOCK + 2U], GCFS_ENTRY_SIZE - (GCFS_ENT_FIRST_BLOCK + 2U));
}

static char lower(char c) {
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

bool gci_is_save_name(const char *name) {
    size_t len = strlen(name);

    if (len < 5 || (name[0] == '.' && name[1] == '_'))
        return false;
    return name[len - 4] == '.' && lower(name[len - 3]) == 'g' && lower(name[len - 2]) == 'c'
        && lower(name[len - 1]) == 'i';
}

static size_t put_escaped(char *dst, size_t pos, size_t size, const uint8_t *src, size_t len) {
    static const char hex[] = "0123456789ABCDEF";

    for (size_t i = 0; i < len; i++) {
        uint8_t c = src[i];

        if (pos + 4U >= size)
            break;
        /* a space cannot start a name on a FAT card */
        if (c < 0x20 || c >= 0x7F || (c == ' ' && pos == 0) || strchr("\"*/:<>?\\|%", c)) {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0xFU];
        } else {
            dst[pos++] = (char)c;
        }
    }
    return pos;
}

void gci_default_name(const uint8_t *entry, uint32_t suffix, char *name, size_t size) {
    size_t pos = 0;
    size_t tail = 13U; /* room for the suffix and the extension, so that a long name is cut
                        * and not one that stops being a .gci */

    pos = put_escaped(name, pos, size - tail, &entry[GCFS_ENT_MAKER], 2);
    name[pos++] = '-';
    pos = put_escaped(name, pos, size - tail, &entry[GCFS_ENT_GAMECODE], 4);
    name[pos++] = '-';
    pos = put_escaped(name, pos, size - tail, &entry[GCFS_ENT_FILENAME], filename_length(entry));
    if (suffix)
        pos += (size_t)snprintf(&name[pos], size - pos, "~%08lX", (unsigned long)suffix);
    snprintf(&name[pos], size - pos, ".gci");
}

uint32_t gci_name_hash(const char *name) {
    uint32_t hash = 2166136261U;

    for (; *name; name++) {
        hash ^= (uint8_t)lower(*name);
        hash *= 16777619U;
    }
    return hash;
}
