#include <dlfcn.h>
#include <stdarg.h>
#include <sys/ioctl.h>
#include <stdio.h>

#include "nv-ioctl-numbers.h"

using real_ioctl_t = int(int, unsigned long, ...);

real_ioctl_t* real_ioctl = nullptr;

extern "C" __attribute__((visibility("default")))
int ioctl(int fd, unsigned long request, ...) noexcept{
    if(real_ioctl == nullptr){
        real_ioctl = reinterpret_cast<real_ioctl_t*>(dlsym(RTLD_NEXT, "ioctl"));
    }

    va_list args;
    va_start(args, request);
    void* argp = va_arg(args, void*);
    va_end(args);

    if(((request >> 8) & 0xFF) != NV_IOCTL_MAGIC){
        return real_ioctl(fd, request, argp);
    }

    return real_ioctl(fd, request, argp);
}
