#include "bench.h"
#include <string.h>

int main(int argc, char **argv)
{
    if (argc < 2) {
        goto out;
    }

    if (strcmp(argv[1], "pub") == 0) {
        client(argc - 2, argv + 2, PUB);
    } else if (strcmp(argv[1], "sub") == 0) {
        client(argc - 2, argv + 2, SUB);
    } else if (strcmp(argv[1], "conn") == 0) {
        client(argc - 2, argv + 2, CONN);
    } else {
        goto out;
    }

    return 0;

out:
    fatal("\nUsage: %s { pub | sub | conn } [--help]\n", argv[0]);
}