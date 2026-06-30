#pragma once

#include <Users/Syscall.h>

/* Создать сокет */
static inline int tos_socket(int domain, int type, int protocol) {
    return (int)Syscall3(SYS_SOCKET, domain, type, protocol);
}

/* Подключиться */
static inline int tos_connect(int sockfd, const void *addr, unsigned int addrlen) {
    return (int)Syscall3(SYS_CONNECT, sockfd, (unsigned long)addr, addrlen);
}

/* Привязать */
static inline int tos_bind(int sockfd, const void *addr, unsigned int addrlen) {
    return (int)Syscall3(SYS_BIND, sockfd, (unsigned long)addr, addrlen);
}

/* Слушать */
static inline int tos_listen(int sockfd, int backlog) {
    return (int)Syscall2(SYS_LISTEN, sockfd, backlog);
}

/* Принять соединение */
static inline int tos_accept(int sockfd, void *addr, unsigned int *addrlen) {
    return (int)Syscall3(SYS_ACCEPT, sockfd, (unsigned long)addr, (unsigned long)addrlen);
}

/* Отправить */
static inline int tos_send(int sockfd, const void *buf, unsigned int len, int flags) {
    return (int)Syscall4(SYS_SEND, sockfd, (unsigned long)buf, len, flags);
}

/* Получить */
static inline int tos_recv(int sockfd, void *buf, unsigned int len, int flags) {
    return (int)Syscall4(SYS_RECV, sockfd, (unsigned long)buf, len, flags);
}

/* Закрыть сокет */
static inline int tos_close_socket(int sockfd) {
    return (int)Syscall1(SYS_CLOSE_SOCKET, sockfd);
}

/* DNS резолвинг */
static inline int tos_getaddrinfo(const char *host, unsigned short port, void *addr) {
    return (int)Syscall3(SYS_GETADDRINFO, (unsigned long)host, port, (unsigned long)addr);
}