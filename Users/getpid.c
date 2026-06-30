#include "libtos.h"

static unsigned long utoa(unsigned long v, char *buf) {
    char tmp[20];
    int i = 0, j = 0;
    if (v == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    while (v > 0) {
        tmp[i++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
    return (unsigned long)j;
}

static unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}