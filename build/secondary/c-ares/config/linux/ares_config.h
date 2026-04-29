/* Copyright (C) The c-ares project and its contributors
 * SPDX-License-Identifier: MIT
 *
 * Hand-tailored ares_config.h for the GN build on Linux x86_64
 * (Debian Bullseye sysroot, glibc 2.31, clang). Mirrors what
 * `cmake -DCARES_THREADS=ON` would generate against the same sysroot, so
 * the GN build does not depend on the autotools/cmake configure step.
 */

#ifndef CARES_CONFIG_H
#define CARES_CONFIG_H

/* Threading */
#define CARES_THREADS 1
#define HAVE_PTHREAD_H 1

/* Note: CARES_USE_LIBRESOLV is intentionally NOT defined. glibc's resolv.h
 * lacks the BSD-style res_getservers / res_ndestroy / union res_sockaddr_union
 * that ares_init_sysconfig_libresolv() requires (those are macOS/BSD only).
 * c-ares falls through to ares_init_sysconfig_files() which parses
 * /etc/resolv.conf directly — the standard Linux path. We still link -lresolv
 * via the `libs` list because libresolv exports inet_net_pton on glibc. */

/* Argument types for getnameinfo(3): POSIX/glibc signature. */
#define GETNAMEINFO_QUAL_ARG1
#define GETNAMEINFO_TYPE_ARG1   struct sockaddr *
#define GETNAMEINFO_TYPE_ARG2   socklen_t
#define GETNAMEINFO_TYPE_ARG46  socklen_t
#define GETNAMEINFO_TYPE_ARG7   int

/* Argument types for gethostname(3). */
#define GETHOSTNAME_TYPE_ARG2   size_t

/* glibc's getservby{port,name}_r take 6 args (struct + buf + buflen + result). */
#define GETSERVBYPORT_R_ARGS    6
#define GETSERVBYNAME_R_ARGS    6

/* Header files */
#define HAVE_ARPA_INET_H        1
#define HAVE_ARPA_NAMESER_COMPAT_H 1
#define HAVE_ARPA_NAMESER_H     1
#define HAVE_ASSERT_H           1
#define HAVE_DLFCN_H            1
#define HAVE_ERRNO_H            1
#define HAVE_FCNTL_H            1
#define HAVE_IFADDRS_H          1
#define HAVE_INTTYPES_H         1
#define HAVE_LIMITS_H           1
#define HAVE_MEMORY_H           1
#define HAVE_NETDB_H            1
#define HAVE_NETINET_IN_H       1
#define HAVE_NETINET_TCP_H      1
#define HAVE_NET_IF_H           1
#define HAVE_POLL_H             1
#define HAVE_SIGNAL_H           1
#define HAVE_STDBOOL_H          1
#define HAVE_STDINT_H           1
#define HAVE_STDLIB_H           1
#define HAVE_STRINGS_H          1
#define HAVE_STRING_H           1
#define HAVE_SYS_EPOLL_H        1
#define HAVE_SYS_IOCTL_H        1
#define HAVE_SYS_PARAM_H        1
#define HAVE_SYS_RANDOM_H       1
#define HAVE_SYS_SELECT_H       1
#define HAVE_SYS_SOCKET_H       1
#define HAVE_SYS_STAT_H         1
#define HAVE_SYS_TIME_H         1
#define HAVE_SYS_TYPES_H        1
#define HAVE_SYS_UIO_H          1
#define HAVE_TIME_H             1
#define HAVE_UNISTD_H           1

/* Symbols / preprocessor flags */
#define HAVE_AF_INET6           1
#define HAVE_PF_INET6           1
#define HAVE_O_NONBLOCK         1
#define HAVE_FIONBIO            1
#define HAVE_IOCTL_SIOCGIFADDR  1
#define HAVE_MSG_NOSIGNAL       1

/* Functions */
#define HAVE_CLOCK_GETTIME_MONOTONIC 1
#define HAVE_CONNECT            1
#define HAVE_EPOLL              1
#define HAVE_FCNTL              1
#define HAVE_FCNTL_O_NONBLOCK   1
#define HAVE_FREEADDRINFO       1
#define HAVE_GETADDRINFO        1
#define HAVE_GETADDRINFO_THREADSAFE 1
#define HAVE_GETENV             1
#define HAVE_GETHOSTNAME        1
#define HAVE_GETIFADDRS         1
#define HAVE_GETNAMEINFO        1
#define HAVE_GETRANDOM          1
#define HAVE_GETSERVBYNAME_R    1
#define HAVE_GETSERVBYPORT_R    1
#define HAVE_GETTIMEOFDAY       1
#define HAVE_IF_INDEXTONAME     1
#define HAVE_IF_NAMETOINDEX     1
#define HAVE_INET_NET_PTON      1
#define HAVE_INET_NTOP          1
#define HAVE_INET_PTON          1
#define HAVE_IOCTL              1
#define HAVE_IOCTL_FIONBIO      1
#define HAVE_MEMMEM             1
#define HAVE_PIPE               1
#define HAVE_PIPE2              1
#define HAVE_POLL               1
#define HAVE_RECV               1
#define HAVE_RECVFROM           1
#define HAVE_SEND               1
#define HAVE_SENDTO             1
#define HAVE_SETSOCKOPT         1
#define HAVE_SOCKET             1
#define HAVE_STAT               1
#define HAVE_STRCASECMP         1
#define HAVE_STRDUP             1
#define HAVE_STRNCASECMP        1
#define HAVE_STRNLEN            1
#define HAVE_WRITEV             1

/* Types */
#define HAVE_LONGLONG           1
#define HAVE_SOCKLEN_T          1
#define HAVE_SSIZE_T            1
#define HAVE_STRUCT_ADDRINFO    1
#define HAVE_STRUCT_IN6_ADDR    1
#define HAVE_STRUCT_SOCKADDR_IN6 1
#define HAVE_STRUCT_SOCKADDR_IN6_SIN6_SCOPE_ID 1
#define HAVE_STRUCT_SOCKADDR_STORAGE 1
#define HAVE_STRUCT_TIMEVAL     1

/* time.h + sys/time.h coexist on glibc. */
#define TIME_WITH_SYS_TIME      1

/* Source of entropy. */
#define CARES_RANDOM_FILE       "/dev/urandom"

/* recv/recvfrom/send arg types (POSIX/glibc). */
#define RECVFROM_QUAL_ARG5
#define RECVFROM_TYPE_ARG1      int
#define RECVFROM_TYPE_ARG2      void *
#define RECVFROM_TYPE_ARG3      size_t
#define RECVFROM_TYPE_ARG4      int
#define RECVFROM_TYPE_ARG5      struct sockaddr *
#define RECVFROM_TYPE_ARG6      socklen_t *
#define RECVFROM_TYPE_RETV      ssize_t
#define RECVFROM_TYPE_ARG2_IS_VOID 1
#define RECVFROM_TYPE_ARG5_IS_VOID 0
#define RECVFROM_TYPE_ARG6_IS_VOID 0

#define RECV_TYPE_ARG1          int
#define RECV_TYPE_ARG2          void *
#define RECV_TYPE_ARG3          size_t
#define RECV_TYPE_ARG4          int
#define RECV_TYPE_RETV          ssize_t

#define SEND_TYPE_ARG1          int
#define SEND_TYPE_ARG2          const void *
#define SEND_TYPE_ARG3          size_t
#define SEND_TYPE_ARG4          int
#define SEND_TYPE_RETV          ssize_t

#endif /* CARES_CONFIG_H */
