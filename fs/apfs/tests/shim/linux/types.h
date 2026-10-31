#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64; typedef int32_t s32; typedef int64_t s64;
typedef uint8_t __u8; typedef uint16_t __u16; typedef uint32_t __u32; typedef uint64_t __u64;
typedef uint16_t __le16; typedef uint32_t __le32; typedef uint64_t __le64;
typedef int ssize_t_;
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define __restrict__ __restrict
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)
#define __annotate_jump_table
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define __restrict__ __restrict
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)
#define __annotate_jump_table
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define __restrict__ __restrict
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)
#define __annotate_jump_table
#define __aligned(x) __attribute__((aligned(x)))
#define __packed __attribute__((packed))
#define __restrict__ __restrict
#ifndef EUCLEAN
#define EUCLEAN 117
#endif
#define likely(x) __builtin_expect(!!(x),1)
#define unlikely(x) __builtin_expect(!!(x),0)
