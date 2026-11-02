/* syslog() into the kernel log (SYSLOG), and dlopen(), which fails: no loader runs inside an app. */
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <syslog.h>
#include <unistd.h>

#include "sys.h"

static const char *s_ident;
static int         s_opt;
static int         s_mask = LOG_UPTO(LOG_DEBUG);

void openlog(const char *ident, int option, int facility)
{
    (void)facility;
    s_ident = ident;
    s_opt   = option;
}

void closelog(void)
{
    s_ident = NULL;
    s_opt   = 0;
}

int setlogmask(int mask)
{
    const int old = s_mask;
    if (mask != 0) {
        s_mask = mask;
    }
    return old;
}

void vsyslog(int priority, const char *format, va_list ap)
{
    if (!(s_mask & LOG_MASK(LOG_PRI(priority)))) {
        return;
    }
    char msg[121], tag[48];
    vsnprintf(msg, sizeof(msg), format, ap);
    const char *t = s_ident;
    if (s_opt & LOG_PID) {
        snprintf(tag, sizeof(tag), "%s[%d]", s_ident ? s_ident : "", (int)getpid());
        t = tag;
    }
    if (s_opt & LOG_PERROR) {
        fprintf(stderr, "%s%s%s\n", t ? t : "", t ? ": " : "", msg);
    }
    espix_sys3(ESPIX_SYS_SYSLOG, priority, t, msg);
}

void syslog(int priority, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    vsyslog(priority, format, ap);
    va_end(ap);
}

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
    return (char *)"Dynamic loading not supported";
}
