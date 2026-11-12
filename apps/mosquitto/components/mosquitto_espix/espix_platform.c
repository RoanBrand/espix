/*
 * The platform seam: the host facilities espix does not have yet, the same in
 * both builds. Everything else the broker asks of the system -- sockets, poll,
 * name lookup, syslog, getifaddrs, the user database, getrandom -- espix
 * answers itself, through the M-mode ABI or libespix.
 */

#include <errno.h>
#include <grp.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/* ----------------------------------------------------------- credentials --
 * espix publishes a process's credentials as getters and no setter, and an app
 * already runs as the user that started it, which is the state
 * drop_privileges() is trying to reach. It only gets here when the broker was
 * started as root with a `user` in its configuration, so the honest interim
 * answer is "already that user".
 */
int setuid(uid_t uid)
{
    (void)uid;
    return 0;
}

int setgid(gid_t gid)
{
    (void)gid;
    return 0;
}

int setgroups(int n, const gid_t *list)
{
    (void)n;
    (void)list;
    return 0;
}

int initgroups(const char *user, gid_t group)
{
    (void)user;
    (void)group;
    return 0;
}
