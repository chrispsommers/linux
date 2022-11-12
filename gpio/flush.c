#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include<sys/ioctl.h>
 
#include "gpio.h"

int main()
{
        int fd;
        size_t count = 0;
        int32_t dummy = 0;
 
        printf("Opening device %s...\n", "/dev/" BYTE_BUF_DEV_NAME);
        fd = open("/dev/" BYTE_BUF_DEV_NAME, O_RDWR);
        if(fd < 0) {
                printf("Cannot open device file...\n");
                return 0;
        }
 
        ioctl(fd, IOCTL_GPIO_BYTES_COUNT, &count);
        printf("Reading buffer count was %ld\n", count);

        printf("Flushing buffer...\n");
        ioctl(fd, IOCTL_GPIO_BYTES_FLUSH, &dummy); 
  
        ioctl(fd, IOCTL_GPIO_BYTES_COUNT, &count);
        printf("Reading buffer count is %ld\n", count);

        printf("Closing device %s...\n", "/dev/" BYTE_BUF_DEV_NAME);
        close(fd);
}