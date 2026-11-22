#include "runtime.h"
#include <stdio.h>
#include <string.h>

int main(int argc, char *argv[])
{
    if (argc == 2 &&
        (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        printf("Usage: %s <ethernet-interface>\n", argv[0]);
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <ethernet-interface>\n", argv[0]);
        return 1;
    }
    return cdp_run(argv[1]);
}
