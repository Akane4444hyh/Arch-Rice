#include <errno.h>
#include <unistd.h>
int fdatasync(int fd)
{
    (void)fd;
    errno = EIO;
    return -1;
}
