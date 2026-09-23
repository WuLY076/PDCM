#ifndef PDCM_BASE_H_
#define PDCM_BASE_H_

#include <stdint.h>

#if defined(_WIN32)
#if defined(PDCM_BUILD_SHARED)
#define PDCM_API __declspec(dllexport)
#else
#define PDCM_API __declspec(dllimport)
#endif
#else
#define PDCM_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define PDCM_BEGIN_DECLS extern "C" {
#define PDCM_END_DECLS }
#else
#define PDCM_BEGIN_DECLS
#define PDCM_END_DECLS
#endif

#define PDCM_ABI_VERSION_MAJOR 0u
#define PDCM_ABI_VERSION_MINOR 1u
#define PDCM_STRUCT_VERSION_1 1u

PDCM_BEGIN_DECLS

typedef struct pdcm_struct_header {
  uint32_t struct_size;
  uint32_t version;
} pdcm_struct_header_t;

PDCM_END_DECLS

#endif // PDCM_BASE_H_
