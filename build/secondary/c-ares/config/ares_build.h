/* Copyright (C) The c-ares project and its contributors
 * SPDX-License-Identifier: MIT
 *
 * Shared CMake 3.31.6 output for c-ares 1.34.6 on all repository targets:
 * macOS x64/arm64, iOS device/simulator, and Linux x64. No private GN
 * platform defines are needed by callers including this public header.
 */
#ifndef __CARES_BUILD_H
#define __CARES_BUILD_H

#define CARES_TYPEOF_ARES_SOCKLEN_T socklen_t
#define CARES_TYPEOF_ARES_SSIZE_T ssize_t

#define CARES_HAVE_SYS_TYPES_H
#define CARES_HAVE_SYS_SOCKET_H
#define CARES_HAVE_SYS_SELECT_H
#define CARES_HAVE_ARPA_NAMESER_H
#define CARES_HAVE_ARPA_NAMESER_COMPAT_H

/* Preserve the dependent includes from upstream's public CMake template. */
#ifdef CARES_HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef CARES_HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif

#ifdef CARES_HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#endif /* __CARES_BUILD_H */
