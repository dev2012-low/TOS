#pragma once

int tos_write(int fd, const char *buf, unsigned long len);
int tos_read(int fd, char *buf, unsigned long len);
int tos_getpid(void);
void tos_exit(int code);
int tos_open(const char *path, unsigned long flags);
int tos_close(int fd);
int tos_uname(char *buf, unsigned long size);

#define O_RDONLY 1
#define O_WRONLY 2
#define O_RDWR   3
#define O_CREAT  4
