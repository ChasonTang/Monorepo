// Catalina fallbacks for __ulock_wait2 and positioned vector $NOCANCEL I/O.
// See README.md for the XNU references and userspace emulation limits.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

// The reserved spelling is the existing Darwin syscall ABI.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp)
extern int __ulock_wait(uint32_t, void *, uint64_t, uint32_t);
extern ssize_t pread_nocancel(int, void *, size_t,
                              off_t) __asm("_pread$NOCANCEL");
extern ssize_t pwrite_nocancel(int, const void *, size_t,
                               off_t) __asm("_pwrite$NOCANCEL");
extern int fcntl_nocancel(int, int, ...) __asm("_fcntl$NOCANCEL");

#define COMPAT_ULF_NO_ERRNO 0x01000000u
#define COMPAT_ULF_DEADLINE 0x02000000u

static int wait_error(uint32_t operation, int error) {
  if (operation & COMPAT_ULF_NO_ERRNO)
    return -error;
  errno = error;
  return -1;
}

static uint64_t ticks_to_ns(uint64_t ticks, mach_timebase_info_data_t scale) {
  __uint128_t ns = (__uint128_t)ticks * scale.numer / scale.denom;
  return ns > UINT64_MAX ? UINT64_MAX : (uint64_t)ns;
}

// Export the missing Darwin ABI entry point for binary consumers.
// NOLINTNEXTLINE(bugprone-reserved-identifier,cert-dcl37-c,cert-dcl51-cpp,misc-use-internal-linkage)
int __ulock_wait2(uint32_t operation, void *address, uint64_t value,
                  uint64_t timeout_ns, uint64_t value2) {
  // value2 is reserved and unused by the XNU implementation this replaces.
  (void)value2;
  uint64_t deadline = operation & COMPAT_ULF_DEADLINE ? timeout_ns : 0;
  operation &= ~COMPAT_ULF_DEADLINE;
  if (!timeout_ns)
    return __ulock_wait(operation, address, value, 0);

  mach_timebase_info_data_t scale;
  mach_timebase_info(&scale);
  // XNU's relative deadlines use the mach_absolute_time clock domain.
  uint64_t start = deadline ? 0 : mach_absolute_time();
  int waited = 0;
  for (;;) {
    uint64_t now = mach_absolute_time();
    uint64_t remaining = 0;
    if (deadline) {
      // Keep the original absolute deadline: conversion and scheduling time
      // must not restart its budget, including between timeout chunks.
      if (now < deadline) {
        remaining = ticks_to_ns(deadline - now, scale);
        if (!remaining)
          remaining = 1;
      }
    } else {
      uint64_t elapsed = ticks_to_ns(now - start, scale);
      if (elapsed < timeout_ns)
        remaining = timeout_ns - elapsed;
    }
    if (!remaining && waited)
      return wait_error(operation, ETIMEDOUT);

    // Round up: a nonzero sub-microsecond timeout must never become infinite.
    uint64_t micros = remaining / 1000u + (remaining % 1000u != 0);
    // Even an initially expired deadline must reach the kernel's argument
    // validation and atomic expected-value check. Zero would wait forever.
    if (!micros)
      micros = 1;
    uint32_t chunk = micros > UINT32_MAX ? UINT32_MAX : (uint32_t)micros;
    int result = __ulock_wait(operation, address, value, chunk);
    waited = 1;
    int error = operation & COMPAT_ULF_NO_ERRNO ? -result : errno;
    if (result >= 0 || error != ETIMEDOUT)
      return result;
  }
}

// XNU's readv_preadv_uio/writev_prwritev_uio copy in the descriptor array
// before looking up the fd. A direct userspace memcpy would turn EFAULT into
// a process crash. Use the current task's VM interface without signal handlers.
// This is an emulation, not a port of XNU's copyin/copyout or vnode I/O layer.
static int copy_memory(void *destination, const void *source, size_t length) {
  if (!length)
    return 0;
  if (length > UINTPTR_MAX - (uintptr_t)source ||
      length > UINTPTR_MAX - (uintptr_t)destination) {
    errno = EFAULT;
    return -1;
  }
  mach_vm_size_t copied = 0;
  kern_return_t result = mach_vm_read_overwrite(
      mach_task_self(), (mach_vm_address_t)(uintptr_t)source, length,
      (mach_vm_address_t)(uintptr_t)destination, &copied);
  if (result != KERN_SUCCESS || copied != length) {
    errno = EFAULT;
    return -1;
  }
  return 0;
}

static int copy_vectors(const struct iovec *vectors, int count,
                        struct iovec *snapshot, size_t *total) {
  if (count <= 0 || count > IOV_MAX) {
    errno = EINVAL;
    return -1;
  }
  if (copy_memory(snapshot, vectors, (size_t)count * sizeof(*vectors)))
    return -1;
  *total = 0;
  for (int i = 0; i < count; ++i) {
    // count is positive and copy_memory verified a full descriptor copy.
    // The analyzer cannot track writes through integer Mach VM addresses.
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult)
    if (snapshot[i].iov_len > (size_t)SSIZE_MAX - *total) {
      errno = EINVAL;
      return -1;
    }
    *total += snapshot[i].iov_len;
  }
  return 0;
}

// Cover the ordinary preparefileread/preparefilewrite errors before gathering
// data or allocating a large buffer. These checks cannot pin XNU's fileproc:
// close/reuse races, guarded fds and unusual descriptor types remain different.
static int check_vector_fd(int fd, int writing) {
  // Catalina's ordinary fcntl checks cancellation even for F_GETFL.
  int flags = fcntl_nocancel(fd, F_GETFL, 0);
  if (flags == -1)
    return -1;
  int mode = flags & O_ACCMODE;
  if ((writing ? mode == O_RDONLY : mode == O_WRONLY) || (flags & O_EVTONLY)) {
    errno = EBADF;
    return -1;
  }
  struct stat status;
  if (fstat(fd, &status))
    return -1;
  if (S_ISFIFO(status.st_mode) || S_ISSOCK(status.st_mode)) {
    errno = ESPIPE;
    return -1;
  }
  if (S_ISCHR(status.st_mode) && isatty(fd)) {
    errno = ENXIO;
    return -1;
  }
  return 0;
}

// These definitions are exported under their Darwin $NOCANCEL symbol names.
// NOLINTNEXTLINE(misc-use-internal-linkage)
ssize_t compat_preadv(int, const struct iovec *, int,
                      off_t) __asm("_preadv$NOCANCEL");
// NOLINTNEXTLINE(misc-use-internal-linkage)
ssize_t compat_pwritev(int, const struct iovec *, int,
                       off_t) __asm("_pwritev$NOCANCEL");

ssize_t compat_preadv(int fd, const struct iovec *vectors, int count,
                      off_t offset) {
  struct iovec snapshot[IOV_MAX];
  size_t total;
  if (copy_vectors(vectors, count, snapshot, &total))
    return -1;
  if (!total)
    return pread_nocancel(fd, NULL, 0, offset);
  if (count == 1)
    return pread_nocancel(fd, snapshot[0].iov_base, total, offset);
  if (check_vector_fd(fd, 0))
    return -1;
  // Catalina's scalar dofileread/dofilewrite reject nbyte > INT_MAX. Reject
  // before allocating; splitting the file I/O would weaken its semantics.
  if (total > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  // A single positioned read preserves the file offset and short-read behavior.
  char *buffer = malloc(total);
  if (!buffer) {
    errno = ENOMEM;
    return -1;
  }
  ssize_t result = pread_nocancel(fd, buffer, total, offset);
  int saved_errno = errno;
  size_t copied = 0;
  for (int i = 0; result > 0 && i < count && copied < (size_t)result; ++i) {
    size_t length = snapshot[i].iov_len;
    if (length > (size_t)result - copied)
      length = (size_t)result - copied;
    if (copy_memory(snapshot[i].iov_base, buffer + copied, length)) {
      saved_errno = errno;
      result = -1;
      break;
    }
    copied += length;
  }
  free(buffer);
  errno = saved_errno;
  return result;
}

ssize_t compat_pwritev(int fd, const struct iovec *vectors, int count,
                       off_t offset) {
  // XNU checks a negative write offset before copying the descriptor array.
  if (offset < 0) {
    errno = EINVAL;
    return -1;
  }
  struct iovec snapshot[IOV_MAX];
  size_t total;
  if (copy_vectors(vectors, count, snapshot, &total))
    return -1;
  if (!total)
    return pwrite_nocancel(fd, NULL, 0, offset);
  if (count == 1)
    return pwrite_nocancel(fd, snapshot[0].iov_base, total, offset);
  if (check_vector_fd(fd, 1))
    return -1;
  if (total > INT_MAX) {
    errno = EINVAL;
    return -1;
  }
  // Gather before one positioned write, avoiding a series of separate writes.
  char *buffer = malloc(total);
  if (!buffer) {
    errno = ENOMEM;
    return -1;
  }
  size_t copied = 0;
  for (int i = 0; i < count; ++i) {
    if (copy_memory(buffer + copied, snapshot[i].iov_base,
                    snapshot[i].iov_len)) {
      int saved_errno = errno;
      free(buffer);
      errno = saved_errno;
      return -1;
    }
    copied += snapshot[i].iov_len;
  }
  ssize_t result = pwrite_nocancel(fd, buffer, total, offset);
  int saved_errno = errno;
  free(buffer);
  errno = saved_errno;
  return result;
}
