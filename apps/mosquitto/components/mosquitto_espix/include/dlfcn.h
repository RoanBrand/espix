/*
 * An espix app is itself a loadable object; there is no second loader inside
 * it, so a broker plugin cannot be loaded from disk. The declarations exist so
 * mosquitto's plugin path compiles, and espix_platform.c answers each one.
 */
#ifndef ESPIX_SHIM_DLFCN_H
#define ESPIX_SHIM_DLFCN_H

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int dlclose(void *handle);
char *dlerror(void);

#endif /* ESPIX_SHIM_DLFCN_H */
