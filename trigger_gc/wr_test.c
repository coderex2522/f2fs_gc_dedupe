#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>

#define F2FS_IOCTL_MAGIC		0xf5
#define F2FS_IOC_GARBAGE_COLLECT	_IO(F2FS_IOCTL_MAGIC, 6)
int main(int argc,char *argv[])
{
    int fd;
    fd=open("/mnt/f2fs/a.txt",O_RDWR);
    ioctl(fd,F2FS_IOC_GARBAGE_COLLECT);
    close(fd);
    return 0;
}
