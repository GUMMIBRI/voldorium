#ifndef SC1_DISC_PATHS_H
#define SC1_DISC_PATHS_H
#include <stdio.h>
#include <errno.h>
#include <string.h>

/* Existing configured disc is checked by the caller before this fallback. */
static int sc1_choose_disc(int (*probe)(const char *, void *), void *ctx,
                           char *chosen, size_t chosen_size,
                           char *first_error, size_t first_error_size)
{
    char path[256];
    const char *fixed[] = {"/app0/Soulcalibur (USA).gdi",
        "/data/PPSA99841/Soulcalibur (USA).gdi"};
    first_error[0] = 0;
    for (int i = 0; i < 10; ++i) {
        const char *candidate;
        if (i < 2) candidate = fixed[i];
        else {
            snprintf(path, sizeof(path), "/mnt/usb%d/homebrew/PPSA99841/Soulcalibur (USA).gdi", i - 2);
            candidate = path;
        }
        int result = probe(candidate, ctx);
        if (result == 0) {
            snprintf(chosen, chosen_size, "%s", candidate);
            return 1;
        }
        if (result != ENOENT && result != ENOTDIR && !first_error[0])
            snprintf(first_error, first_error_size, "%s: %s", candidate, strerror(result));
    }
    return 0;
}
#endif
