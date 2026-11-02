/* No shared objects load inside an app: dlopen() fails and says so, as in a static musl build. Both builds. */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#define RTLD_LAZY   0x1
#define RTLD_NOW    0x2
#define RTLD_LOCAL  0x0
#define RTLD_GLOBAL 0x100

void *dlopen(const char *filename, int flags);
void *dlsym(void *handle, const char *symbol);
int   dlclose(void *handle);
char *dlerror(void);

#ifdef __cplusplus
}
#endif
