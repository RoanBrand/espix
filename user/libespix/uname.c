/* uname(2), over the UNAME syscall: the kernel's own five fields, not a copy
 * an app could fall out of date with. */
#include <string.h>
#include <sys/utsname.h>

#include "sys.h"

int uname(struct utsname *buf)
{
    struct espix_utsname u;

    if (buf == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (espix_ret(espix_sys1(ESPIX_SYS_UNAME, &u)) != 0) {
        return -1;
    }
    strlcpy(buf->sysname, u.sysname, sizeof(buf->sysname));
    strlcpy(buf->nodename, u.nodename, sizeof(buf->nodename));
    strlcpy(buf->release, u.release, sizeof(buf->release));
    strlcpy(buf->version, u.version, sizeof(buf->version));
    strlcpy(buf->machine, u.machine, sizeof(buf->machine));
    return 0;
}
