#include <stdio.h>
#include "xiaoji_client.h"

int main(void) {
    uint32_t devices = 0;
    if (XiaojiGetAbiVersion() != XIAOJI_CLIENT_ABI) {
        fputs("Unsupported xiaoji client ABI\n", stderr);
        return 1;
    }
    if (XiaojiGetDevices(&devices) != XIAOJI_OK) {
        fprintf(stderr, "%s\n", XiaojiLastError());
        return 1;
    }
    printf("keyboard=%u mouse=%u\n", !!(devices & XIAOJI_KEYBOARD), !!(devices & XIAOJI_MOUSE));
    return 0;
}
