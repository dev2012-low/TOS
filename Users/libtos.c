#include <User/Syscall.h>

int tos_write(int fd, const char *buf, unsigned long len) {
    return (int)SyscallWrite(fd, buf, len);
}

int tos_read(int fd, char *buf, unsigned long len) {
    return (int)SyscallRead(fd, buf, len);
}

int tos_getpid(void) {
    return (int)SyscallGetPid();
}

void tos_exit(int code) {
    SyscallExit((unsigned long)code);
}

int tos_open(const char *path, unsigned long flags) {
    return (int)SyscallOpen(path, flags);
}

int tos_close(int fd) {
    return (int)SyscallClose(fd);
}

int tos_uname(char *buf, unsigned long size) {
    return (int)SyscallUname(buf, size);
}
