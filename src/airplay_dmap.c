#include "airplay_dmap.h"

#include <string.h>
#include <stdint.h>
#include <netinet/in.h>

/* 4-byte tag interpreted as a big-endian uint32, i.e. the raw ASCII bytes
 * in order - same convention shairport-sync uses (memcpy + ntohl). */
#define DMAP_TAG(a, b, c, d) \
    (((uint32_t)(unsigned char)(a) << 24) | ((uint32_t)(unsigned char)(b) << 16) | \
     ((uint32_t)(unsigned char)(c) << 8) | (uint32_t)(unsigned char)(d))

#define DMAP_TAG_MINM DMAP_TAG('m', 'i', 'n', 'm')
#define DMAP_TAG_ASAR DMAP_TAG('a', 's', 'a', 'r')
#define DMAP_TAG_ASAL DMAP_TAG('a', 's', 'a', 'l')

void airplay_dmap_parse(const unsigned char *body, size_t len, airplay_dmap_metadata *out)
{
    size_t off = 8; /* real shairport-sync quirk - see airplay_dmap.h */

    memset(out, 0, sizeof(*out));
    if (len < 8) return;

    while (off + 8 <= len) {
        uint32_t tag_be, val_len_be, tag, val_len;

        memcpy(&tag_be, body + off, 4);
        off += 4;
        memcpy(&val_len_be, body + off, 4);
        off += 4;

        tag = ntohl(tag_be);
        val_len = ntohl(val_len_be);

        if (val_len > len - off) break; /* truncated/malformed - stop, keep what we have */

        if (tag == DMAP_TAG_MINM && !out->have_title) {
            size_t n = val_len < sizeof(out->title) - 1 ? val_len : sizeof(out->title) - 1;
            memcpy(out->title, body + off, n);
            out->title[n] = '\0';
            out->have_title = 1;
        } else if (tag == DMAP_TAG_ASAR && !out->have_artist) {
            size_t n = val_len < sizeof(out->artist) - 1 ? val_len : sizeof(out->artist) - 1;
            memcpy(out->artist, body + off, n);
            out->artist[n] = '\0';
            out->have_artist = 1;
        } else if (tag == DMAP_TAG_ASAL && !out->have_album) {
            size_t n = val_len < sizeof(out->album) - 1 ? val_len : sizeof(out->album) - 1;
            memcpy(out->album, body + off, n);
            out->album[n] = '\0';
            out->have_album = 1;
        }

        off += val_len;
    }
}
