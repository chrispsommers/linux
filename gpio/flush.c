#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include<sys/ioctl.h>
 
#define IOCTL_GPIO_FLUSH _IOW('g','f',int32_t*)
#define IOCTL_GPIO_COUNT _IOR('g','b',size_t*)
#define DEV_NAME "fake_gpio"

int main()
{
        int fd;
        size_t count = 0;
        int32_t dummy = 0;
 
        printf("Opening device %s...\n", "/dev/" DEV_NAME);
        fd = open("/dev/" DEV_NAME, O_RDWR);
        if(fd < 0) {
                printf("Cannot open device file...\n");
                return 0;
        }
 
        ioctl(fd, IOCTL_GPIO_COUNT, &count);
        printf("Reading buffer count was %ld\n", count);

        printf("Flushing buffer...\n");
        ioctl(fd, IOCTL_GPIO_FLUSH, &dummy); 
  
        ioctl(fd, IOCTL_GPIO_COUNT, &count);
        printf("Reading buffer count is %ld\n", count);

        printf("Closing device %s...\n", "/dev/" DEV_NAME);
        close(fd);
}