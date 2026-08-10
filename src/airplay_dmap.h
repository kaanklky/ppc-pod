#ifndef PPC_SPOTI_AIRPLAY_DMAP_H
#define PPC_SPOTI_AIRPLAY_DMAP_H

#include <stddef.h>

/*
 * Bounded DMAP/DAAP tag reader for AirPlay 1's `SET_PARAMETER` metadata
 * push (Content-Type: application/x-dmap-tagged). Only the three tags
 * this project's UI shows are extracted; everything else is skipped.
 */

#define AIRPLAY_DMAP_TITLE_MAX 256
#define AIRPLAY_DMAP_ARTIST_MAX 256
#define AIRPLAY_DMAP_ALBUM_MAX 256

typedef struct {
    char title[AIRPLAY_DMAP_TITLE_MAX];   /* DMAP tag 'minm' */
    char artist[AIRPLAY_DMAP_ARTIST_MAX]; /* 'asar' */
    char album[AIRPLAY_DMAP_ALBUM_MAX];   /* 'asal' */
    int have_title;
    int have_artist;
    int have_album;
} airplay_dmap_metadata;

/* The first 8 bytes of an x-dmap-tagged body are an outer DMAP
 * listing-item wrapper (its own tag+length), skipped unchecked - parsing
 * begins at byte 8. From there it's a flat sequence of {4-byte big-endian
 * tag, 4-byte big-endian length, raw value bytes} records. Bounds-checked
 * throughout - a short/truncated body yields whichever fields were fully
 * present before parsing stopped; `out` is always fully zeroed first so
 * unset fields are unambiguous. */
void airplay_dmap_parse(const unsigned char *body, size_t len, airplay_dmap_metadata *out);

#endif
