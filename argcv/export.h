#ifndef ARGCV_EXPORT_H_
#define ARGCV_EXPORT_H_

#if defined(ARGCV_SHARED_LIBRARY)
#define ARGCV_EXPORT __attribute__((visibility("default")))
#else
#define ARGCV_EXPORT
#endif

#endif /* ARGCV_EXPORT_H_ */
