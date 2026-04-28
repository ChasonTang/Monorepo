/* MIT License
 *
 * Copyright (c) 2009 Daniel Stenberg
 *
 * SPDX-License-Identifier: MIT
 *
 * Hand-tailored ares_build.h for the GN build on macOS / clang.
 * The upstream c-ares ares_build.h.dist already covers macOS via its __GNUC__
 * branch; this file fixes the relevant macros so the GN build does not depend
 * on the autotools/cmake configure step.
 */
#ifndef __CARES_BUILD_H
#define __CARES_BUILD_H

#ifdef CARES_TYPEOF_ARES_SOCKLEN_T
#  error "CARES_TYPEOF_ARES_SOCKLEN_T shall not be defined except in ares_build.h"
#endif

#define CARES_TYPEOF_ARES_SOCKLEN_T socklen_t
#define CARES_HAVE_SYS_TYPES_H      1
#define CARES_HAVE_SYS_SOCKET_H     1
#define CARES_HAVE_SYS_SELECT_H     1
#define CARES_HAVE_ARPA_NAMESER_H   1
#define CARES_HAVE_ARPA_NAMESER_COMPAT_H 1

#define CARES_TYPEOF_ARES_SSIZE_T   ssize_t

#endif /* __CARES_BUILD_H */
