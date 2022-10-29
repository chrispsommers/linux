// read one byte from /dev/fake_gpio to drain buffer in controlled fashion

#include <stdio.h>
#include <errno.h>
#include <string.h>

#define DEVICE "/dev/fake_gpio"
int main(int argc, char **argv) {

    int rc;
    int n;
    char c;
    FILE *fp;

    fp = fopen("/dev/fake_gpio", "r");
    if(fp == NULL) {
        rc = errno;
        fprintf(stderr, "Error %d: '%s' opening %s\n", rc, strerror(rc), DEVICE);
        return rc;
    }
    if (feof(fp)) {
        fprintf(stderr, "%s is empty\n", DEVICE);
    }
    n = fread(&c, 1, 1, fp);
    if (n == 0) {
        rc = ferror(fp);
        if (rc) {
            fprintf(stderr, "Error %d: '%s' reading %s\n", rc, strerror(rc), DEVICE);
            return rc;
        }
    }
    printf("%c\n", c);
    return 0;
}