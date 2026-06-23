/* MIT License
 *
 * Copyright (c) 2009 Daniel Stenberg
 *
 * SPDX-License-Identifier: MIT
 *
 * Hand-tailored ares_build.h for the GN build on iOS / clang. Values mirror
 * c-ares' CMake-generated ares_build.h for the bundled iOS SDK so the GN build
 * does not depend on running CMake.
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
