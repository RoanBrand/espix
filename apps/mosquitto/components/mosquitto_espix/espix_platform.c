/*
 * The platform seam.
 *
 * None of this is MQTT. It is the daemon-shaped part of mosquitto -- becoming
 * another user, forking into the background, writing a pid file, syslog,
 * dlopen()ing plugins, and enumerating interfaces -- none of which a
 * single-user, no-fork, no-shared-object device can honour. Each is answered
 * with the truth rather than a pretence: there is no such user, there are no
 * plugins, there is no interface list.
 *
 * That is the whole cost of running the broker unmodified. Every symbol here
 * is one espix does not publish, and the list is short on purpose: anything
 * that *could* be honest on espix -- sockets, poll, getrandom, time, files --
 * is a kernel ABI entry instead, used directly.
 */

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "dlfcn.h"
#include "espix_compat.h"
#include "ifaddrs.h"
#include "syslog.h"

/* ---------------------------------------------------------------- syslog --
 * The broker's log_dest syslog goes to stderr, which is where an espix app's
 * output belongs: the terminal or the SSH session that started it.
 */
void openlog(const char *ident, int option, int facility)
{
    (void)ident;
    (void)option;
    (void)facility;
}

void closelog(void)
{
}

void vsyslog(int priority, const char *format, va_list ap)
{
    static const char *const level[] = {
        "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"
    };
    const int p = priority & 7;

    fprintf(stderr, "mosquitto: %s: ", level[p]);
    vfprintf(stderr, format, ap);
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;

    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

/* ----------------------------------------------------------------- users --
 * The lookups are espix's now, not a shim: getpwnam, getpwuid, getpwuid_r and
 * getgrgid_r come from the kernel's espix_auth tables (components/espix_auth/
 * abi_user.c), so a broker that asks who "mosquitto" is gets the real account
 * out of /etc/passwd, home and all.
 *
 * What is left here is the half that *changes* a process. espix_proc publishes
 * the credentials as getters (espix_proc_cred_of_task) and no setter, so there
 * is nothing for setuid() to write to yet; and an espix app already runs as the
 * user that started it, which is the state drop_privileges() is trying to
 * reach. It only gets here when the broker was started as root with a `user`
 * in its configuration, so the honest interim answer is "already that user".
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

/* newlib declares the count as int, not size_t. */
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

/* ------------------------------------------------------------- processes --
 * A broker daemonises by forking and detaching. An espix app is already the
 * process the shell started and already has the console; config.daemon is
 * false by default, and -d fails loudly instead of half-working.
 */
pid_t fork(void)
{
    errno = ENOSYS;
    return -1;
}

pid_t setsid(void)
{
    return 0;
}

mode_t umask(mode_t mask)
{
    (void)mask;
    return 0;
}

/* --------------------------------------------------------- shared objects --
 * Plugins are loaded with dlopen(). espix apps are the loadable objects; there
 * is no loader inside one, so the built-in password-file and acl-file checks
 * are the security options available and a plugin listener is refused.
 */
void *dlopen(const char *filename, int flags)
{
    (void)filename;
    (void)flags;
    return NULL;
}

void *dlsym(void *handle, const char *symbol)
{
    (void)handle;
    (void)symbol;
    return NULL;
}

int dlclose(void *handle)
{
    (void)handle;
    return -1;
}

char *dlerror(void)
{
    return (char *)"espix: this app cannot load shared objects";
}

/* ------------------------------------------------------------ interfaces --
 * bind_interface resolves an interface name to an address through
 * getifaddrs(). There is no such list to publish, so a listener that names an
 * interface is refused rather than bound to the wrong one.
 */
int getifaddrs(struct ifaddrs **ifap)
{
    if (ifap != NULL) {
        *ifap = NULL;
    }
    errno = ENOSYS;
    return -1;
}

void freeifaddrs(struct ifaddrs *ifa)
{
    (void)ifa;
}

/* ------------------------------------------------------------------- DNS --
 * getaddrinfo() failure text. lwIP has the codes but not this function, and
 * espix does not publish it, so the strings live here.
 */
const char *gai_strerror(int errcode)
{
    switch (errcode) {
    case 0:      return "no error";
#ifdef EAI_BADFLAGS
    case EAI_BADFLAGS: return "invalid flags";
#endif
#ifdef EAI_NONAME
    case EAI_NONAME:   return "name or service not known";
#endif
#ifdef EAI_AGAIN
    case EAI_AGAIN:    return "temporary failure in name resolution";
#endif
#ifdef EAI_FAIL
    case EAI_FAIL:     return "non-recoverable failure in name resolution";
#endif
#ifdef EAI_FAMILY
    case EAI_FAMILY:   return "address family not supported";
#endif
#ifdef EAI_SOCKTYPE
    case EAI_SOCKTYPE: return "socket type not supported";
#endif
#ifdef EAI_SERVICE
    case EAI_SERVICE:  return "service not supported";
#endif
#ifdef EAI_MEMORY
    case EAI_MEMORY:   return "out of memory";
#endif
#ifdef EAI_SYSTEM
    case EAI_SYSTEM:   return "system error";
#endif
    default:           return "unknown name-resolution error";
    }
}
