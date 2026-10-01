#include "resource_hook.h"
#include <stdio.h>

static struct rh_rule backing[RH_RULES], staging[RH_RULES];
static struct rh_state state = {.rules = backing, .rules_capacity = RH_RULES};
static char text[RH_CONFIG_BYTES];

static int read_file(void *cookie, void *out, uint32_t size) {
    FILE *file = cookie;
    size_t n = fread(out, 1, size, file);
    return ferror(file) ? -1 : (int)n;
}

int main(int argc, char **argv) {
    FILE *file;
    char mapped[RH_PATH];
    int rc;
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "usage: check-config mappings.tsv [absolute-resource-path]\n");
        return 2;
    }
    file = fopen(argv[1], "rb");
    if (!file) { perror(argv[1]); return 1; }
    rc = rh_read_config(&state, read_file, file, text, sizeof(text), staging);
    if (fclose(file) != 0) { perror("close config"); return 1; }
    if (rc || !state.count) {
        fprintf(stderr, "Invalid configuration: %s (code %d)\n",
                !rc ? "at least one mapping is required" : "parser/path/size validation failed", rc);
        return 1;
    }
    printf("Valid configuration: %u rule(s)\n", (unsigned)state.count);
    {
        uint32_t i;
        for (i = 0; i < state.count; i++) if (rh_quickapp_package(state.rules[i].source)) {
            puts("QuickApp icon declarations require UI-owner package lookup on the device; "
                 "offline path previews do not materialize them.");
            break;
        }
    }
    if (argc == 3) {
        rc = rh_resolve(&state, argv[2], mapped);
        if (rc < 0) {
            fprintf(stderr, "Resource path is invalid or mapped path exceeds 255 bytes (code %d)\n", rc);
            return 1;
        }
        if (rc == 0) printf("No matching rule; original path: %s\n", argv[2]);
        else printf("Mapped path: %s\n", mapped);
    }
    puts("Offline path validation only: target-file existence, decoding and live UI refresh are not checked.");
    return 0;
}
