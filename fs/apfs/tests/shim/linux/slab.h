#pragma once
#include <linux/types.h>
#define GFP_KERNEL 0
#define kmalloc(n,f) malloc(n)
#define kfree(p) free(p)
#include <stdlib.h>
#define vmalloc(n) malloc(n)
#define vfree(p) free(p)
#define kvmalloc(n,f) malloc(n)
#define kvfree(p) free(p)
