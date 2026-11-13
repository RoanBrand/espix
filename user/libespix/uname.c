/* uname(2): the kernel fills the struct, Linux's layout, so nothing here can fall out of date with it. */
#include <stddef.h>
#include <sys/utsname.h>

#include "sys.h"

_Static_assert(sizeof(struct utsname) == sizeof(struct espix_utsname), "struct utsname is the kernel's");

int uname(struct utsname *buf)
{
    return (int)espix_ret(espix_sys1(ESPIX_SYS_UNAME, buf));
}
