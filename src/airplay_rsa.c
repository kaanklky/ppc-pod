#include "airplay_rsa.h"

#include <string.h>
#include <stdio.h>
#include <arpa/inet.h>

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"
#include "mbedtls/entropy.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/base64.h"

/* Apple's original AirPlay accessory private key - byte-for-byte the same
 * PEM shairport-sync's common.c embeds as "super_secret_key" (long-public,
 * widely-known AirPlay 1 key material - see airplay_rsa.h). */
static const char AIRPLAY_RSA_PRIVATE_KEY[] =
    "-----BEGIN RSA PRIVATE KEY-----\n"
    "MIIEpQIBAAKCAQEA59dE8qLieItsH1WgjrcFRKj6eUWqi+bGLOX1HL3U3GhC/j0Qg90u3sG/1CUt\n"
    "wC5vOYvfDmFI6oSFXi5ELabWJmT2dKHzBJKa3k9ok+8t9ucRqMd6DZHJ2YCCLlDRKSKv6kDqnw4U\n"
    "wPdpOMXziC/AMj3Z/lUVX1G7WSHCAWKf1zNS1eLvqr+boEjXuBOitnZ/bDzPHrTOZz0Dew0uowxf\n"
    "/+sG+NCK3eQJVxqcaJ/vEHKIVd2M+5qL71yJQ+87X6oV3eaYvt3zWZYD6z5vYTcrtij2VZ9Zmni/\n"
    "UAaHqn9JdsBWLUEpVviYnhimNVvYFZeCXg/IdTQ+x4IRdiXNv5hEewIDAQABAoIBAQDl8Axy9XfW\n"
    "BLmkzkEiqoSwF0PsmVrPzH9KsnwLGH+QZlvjWd8SWYGN7u1507HvhF5N3drJoVU3O14nDY4TFQAa\n"
    "LlJ9VM35AApXaLyY1ERrN7u9ALKd2LUwYhM7Km539O4yUFYikE2nIPscEsA5ltpxOgUGCY7b7ez5\n"
    "NtD6nL1ZKauw7aNXmVAvmJTcuPxWmoktF3gDJKK2wxZuNGcJE0uFQEG4Z3BrWP7yoNuSK3dii2jm\n"
    "lpPHr0O/KnPQtzI3eguhe0TwUem/eYSdyzMyVx/YpwkzwtYL3sR5k0o9rKQLtvLzfAqdBxBurciz\n"
    "aaA/L0HIgAmOit1GJA2saMxTVPNhAoGBAPfgv1oeZxgxmotiCcMXFEQEWflzhWYTsXrhUIuz5jFu\n"
    "a39GLS99ZEErhLdrwj8rDDViRVJ5skOp9zFvlYAHs0xh92ji1E7V/ysnKBfsMrPkk5KSKPrnjndM\n"
    "oPdevWnVkgJ5jxFuNgxkOLMuG9i53B4yMvDTCRiIPMQ++N2iLDaRAoGBAO9v//mU8eVkQaoANf0Z\n"
    "oMjW8CN4xwWA2cSEIHkd9AfFkftuv8oyLDCG3ZAf0vrhrrtkrfa7ef+AUb69DNggq4mHQAYBp7L+\n"
    "k5DKzJrKuO0r+R0YbY9pZD1+/g9dVt91d6LQNepUE/yY2PP5CNoFmjedpLHMOPFdVgqDzDFxU8hL\n"
    "AoGBANDrr7xAJbqBjHVwIzQ4To9pb4BNeqDndk5Qe7fT3+/H1njGaC0/rXE0Qb7q5ySgnsCb3DvA\n"
    "cJyRM9SJ7OKlGt0FMSdJD5KG0XPIpAVNwgpXXH5MDJg09KHeh0kXo+QA6viFBi21y340NonnEfdf\n"
    "54PX4ZGS/Xac1UK+pLkBB+zRAoGAf0AY3H3qKS2lMEI4bzEFoHeK3G895pDaK3TFBVmD7fV0Zhov\n"
    "17fegFPMwOII8MisYm9ZfT2Z0s5Ro3s5rkt+nvLAdfC/PYPKzTLalpGSwomSNYJcB9HNMlmhkGzc\n"
    "1JnLYT4iyUyx6pcZBmCd8bD0iwY/FzcgNDaUmbX9+XDvRA0CgYEAkE7pIPlE71qvfJQgoA9em0gI\n"
    "LAuE4Pu13aKiJnfft7hIjbK+5kyb3TysZvoyDnb3HOKvInK7vXbKuU4ISgxB2bB3HcYzQMGsz1qJ\n"
    "2gG0N5hvJpzwwhbhXqFKA4zaaSrw622wDniAK5MlIE0tIAKKP4yxNGjoD2QYjhBGuhvkWKY=\n"
    "-----END RSA PRIVATE KEY-----\0";

/* Real, verified-against-shairport-sync's rtsp.c/common.c pattern: parse the
 * key fresh for each call rather than caching a static context - this
 * project's other crypto code (dh.c, credential_blob.c) already follows the
 * same "fresh mbedtls_ctr_drbg per operation" style, and AirPlay session
 * setup only calls this a handful of times per connection, so the cost is
 * irrelevant. */
static int rsa_apply(const unsigned char *input, size_t inlen, unsigned char *out, size_t out_cap,
                      size_t *out_len, int is_decrypt)
{
    mbedtls_pk_context pkctx;
    mbedtls_rsa_context *rsa;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    const char *pers = "ppc-pod-airplay-rsa";
    size_t olen = 0;
    int rc = -1;

    mbedtls_pk_init(&pkctx);
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    if (mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                               (const unsigned char *)pers, strlen(pers)) != 0) {
        goto out;
    }

    /* mbedTLS 2.28.x mbedtls_pk_parse_key() takes no rng args (that's a
     * 3.x-only addition) - this project vendors 2.28.x, confirmed via
     * README.md's own note on the vendored mbedtls-src tag. */
    if (mbedtls_pk_parse_key(&pkctx, (const unsigned char *)AIRPLAY_RSA_PRIVATE_KEY,
                              sizeof(AIRPLAY_RSA_PRIVATE_KEY), NULL, 0) != 0) {
        fprintf(stderr, "[airplay_rsa] failed to parse embedded private key\n");
        goto out;
    }

    rsa = mbedtls_pk_rsa(pkctx);
    if (rsa == NULL) goto out;

    if (out_cap < rsa->len) {
        fprintf(stderr, "[airplay_rsa] output buffer too small (%zu < %zu)\n", out_cap, rsa->len);
        goto out;
    }

    if (is_decrypt) {
        /* RSA_MODE_KEY in shairport-sync: OAEP padding, SHA-1 hash - real
         * client behavior confirmed directly in rtsp.c's handle_announce()
         * (base64_dec then rsa_apply(..., RSA_MODE_KEY)) and common.c's
         * mbedTLS branch (MBEDTLS_RSA_PKCS_V21 + MBEDTLS_MD_SHA1). */
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V21, MBEDTLS_MD_SHA1);
        rc = mbedtls_rsa_pkcs1_decrypt(rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                       MBEDTLS_RSA_PRIVATE, &olen, input, out, out_cap);
    } else {
        /* RSA_MODE_AUTH in shairport-sync: PKCS#1 v1.5, no hash (a raw
         * private-key "encrypt" operation, i.e. a signature) - confirmed in
         * common.c's mbedTLS branch (MBEDTLS_RSA_PKCS_V15 + MBEDTLS_MD_NONE,
         * mbedtls_rsa_pkcs1_encrypt with MBEDTLS_RSA_PRIVATE). */
        mbedtls_rsa_set_padding(rsa, MBEDTLS_RSA_PKCS_V15, MBEDTLS_MD_NONE);
        rc = mbedtls_rsa_pkcs1_encrypt(rsa, mbedtls_ctr_drbg_random, &ctr_drbg,
                                       MBEDTLS_RSA_PRIVATE, inlen, input, out);
        olen = rsa->len;
    }

    if (rc != 0) {
        fprintf(stderr, "[airplay_rsa] rsa operation failed, rc=-0x%04x\n", (unsigned int)-rc);
        rc = -1;
        goto out;
    }
    *out_len = olen;
    rc = 0;

out:
    mbedtls_pk_free(&pkctx);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    return rc;
}

int airplay_rsa_decrypt_aeskey(const unsigned char *in, size_t in_len,
                                unsigned char *out, size_t *out_len)
{
    unsigned char buf[256]; /* RSA-2048 output is exactly 256 bytes before OAEP unwrap */
    size_t olen = 0;

    if (rsa_apply(in, in_len, buf, sizeof(buf), &olen, 1) != 0) return -1;
    if (olen > *out_len) return -1;
    memcpy(out, buf, olen);
    *out_len = olen;
    return 0;
}

int airplay_rsa_challenge_response(const unsigned char *challenge, size_t challenge_len,
                                    unsigned int server_ipv4_be,
                                    const unsigned char device_id6[6],
                                    char *out_b64, size_t out_b64_cap)
{
    /* Algorithm ported from shairport-sync's rtsp.c apple_challenge():
     * challenge bytes, then this server's own IPv4
     * address (4 bytes for IPv4 - this project doesn't support IPv6, unlike
     * shairport-sync's optional 16-byte branch), then the 6-byte AirPlay
     * device id, all concatenated and zero-padded up to a minimum of 0x20
     * (32) bytes before the RSA operation - not padding to a fixed 32
     * exactly, but AT LEAST 32 (shairport-sync: "if (buflen < 0x20) buflen
     * = 0x20;"). */
    unsigned char buf[48];
    unsigned char rsa_out[256];
    size_t rsa_out_len = 0;
    size_t pos = 0;
    size_t buflen;
    unsigned char b64[400];
    size_t b64_len = 0;
    size_t i;

    if (challenge_len > 16) return -1; /* "oversized Apple-Challenge" per shairport-sync */

    memset(buf, 0, sizeof(buf));
    memcpy(buf + pos, challenge, challenge_len);
    pos += challenge_len;

    memcpy(buf + pos, &server_ipv4_be, 4);
    pos += 4;

    memcpy(buf + pos, device_id6, 6);
    pos += 6;

    buflen = pos;
    if (buflen < 0x20) buflen = 0x20;

    if (rsa_apply(buf, buflen, rsa_out, sizeof(rsa_out), &rsa_out_len, 0) != 0) return -1;

    if (mbedtls_base64_encode(b64, sizeof(b64), &b64_len, rsa_out, rsa_out_len) != 0) return -1;

    /* Strip trailing '=' padding - real shairport-sync behavior
     * ("strip the padding": char *padding = strchr(encoded, '='); if
     * (padding) *padding = 0;). */
    for (i = 0; i < b64_len; i++) {
        if (b64[i] == '=') { b64_len = i; break; }
    }

    if (b64_len + 1 > out_b64_cap) return -1;
    memcpy(out_b64, b64, b64_len);
    out_b64[b64_len] = '\0';
    return 0;
}
