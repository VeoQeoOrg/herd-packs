#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <crypto.h>

static int hexload(const char *path, uint8_t *out, int n)
{
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    for (int i = 0; i < n; i++)
        if (fscanf(f, "%2hhx", &out[i]) != 1) { fclose(f); return -1; }
    fclose(f);
    return 0;
}

static void hexsave(const char *path, const uint8_t *b, int n)
{
    FILE *f = fopen(path, "w");
    for (int i = 0; i < n; i++) fprintf(f, "%02x", b[i]);
    fputc('\n', f);
    fclose(f);
}

int main(int argc, char **argv)
{
    uint8_t seed[32], pub[32], priv[64];

    if (argc == 4 && !strcmp(argv[1], "keygen")) {
        FILE *r = fopen("/dev/urandom", "rb");
        if (!r || fread(seed, 1, 32, r) != 32) { fputs("sign: no entropy\n", stderr); return 1; }
        fclose(r);
        ed25519_keypair(pub, priv, seed);
        hexsave(argv[2], seed, 32);
        hexsave(argv[3], pub, 32);
        return 0;
    }

    if (argc == 4 && !strcmp(argv[1], "pub")) {
        if (hexload(argv[2], seed, 32)) { fputs("sign: cannot read seed\n", stderr); return 1; }
        ed25519_keypair(pub, priv, seed);
        hexsave(argv[3], pub, 32);
        return 0;
    }

    if (argc == 5 && !strcmp(argv[1], "sign")) {
        if (hexload(argv[2], seed, 32)) { fputs("sign: cannot read seed\n", stderr); return 1; }
        ed25519_keypair(pub, priv, seed);

        FILE *f = fopen(argv[3], "rb");
        if (!f) { fputs("sign: cannot read input\n", stderr); return 1; }
        size_t cap = 1 << 16, n = 0;
        uint8_t *msg = malloc(cap);
        for (;;) {
            if (n + 4096 > cap) { cap *= 2; msg = realloc(msg, cap); }
            size_t got = fread(msg + n, 1, 4096, f);
            n += got;
            if (got < 4096) break;
        }
        fclose(f);

        uint8_t sig[64];
        ed25519_sign(sig, msg, n, priv);
        if (ed25519_verify(sig, msg, n, pub) != 0) { fputs("sign: self-check failed\n", stderr); return 1; }

        FILE *o = fopen(argv[4], "wb");
        if (!o) { fputs("sign: cannot write signature\n", stderr); return 1; }
        fwrite(sig, 1, 64, o);
        fclose(o);
        return 0;
    }

    fputs("usage: sign keygen SEED PUB\n"
          "       sign pub SEED PUB\n"
          "       sign sign SEED FILE SIG\n", stderr);
    return 2;
}
