/* rand_test: SYS v4 random bytes (papp_random) and the C library's getentropy() on top of it. Prints two
 * blocks from each (they must differ) and a rough bit balance over 64 KB (about 50% ones). */
#include "papp/papp.h"
#include <stdio.h>
#include <unistd.h>

PAPP_MANIFEST(.id = "test.rand", .name = "Random test", .version = PAPP_APP_VERSION, .main_stack = 8192,
              .flags = PAPP_MF_HIDDEN);

int getentropy(void *buf, size_t len);

static void hex(const char *what, const unsigned char *b, int n)
{
    char s[80];
    int k = 0;
    for (int i = 0; i < n; i++) k += snprintf(s + k, sizeof(s) - k, "%02x", b[i]);
    papp_log("%s %s\n", what, s);
}

int main(void)
{
    unsigned char a[16] = { 0 }, b[16] = { 0 };
    papp_log("papp_random: %s\n", papp_random(a, 16) && papp_random(b, 16) ? "yes" : "NO (launcher without SYS v4)");
    hex("  1:", a, 16);
    hex("  2:", b, 16);
    papp_log("getentropy: %d\n", getentropy(a, 16));
    hex("  3:", a, 16);
    static unsigned char big[256];
    long ones = 0;
    for (int r = 0; r < 256; r++) {
        papp_random(big, sizeof(big));
        for (int i = 0; i < 256; i++) ones += __builtin_popcount(big[i]);
    }
    papp_log("ones in 64 KB: %ld of %ld (%ld.%ld%%)\n", ones, 65536L * 8, ones * 100 / (65536L * 8),
             (ones * 1000 / (65536L * 8)) % 10);
    return 0;
}
