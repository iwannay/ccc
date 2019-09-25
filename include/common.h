#ifndef _INCLUDES_COMMON_H
#define _INCLUDES_COMMON_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct vm VM;
typedef struct parser Parser;
typedef struct class Class;

#define bool char
#define true 1
#define false 0
#define UNUSED __attribute__ ((unused))

#ifdef DEBUG
    #define ASSERT(condition, errMsg) \
    do {\
        if (!(condition)) {\
            fprintf(stderr, "\033[31mASSERT failed! %s:%d In function %s(): %s\033[0m\n", \
            __FILE__, __LINE__, __func__, errMsg); \
            abort();\
        }\
    } while (0);
#else
    #define ASSERT(condition, errMsg) ((void) 0)
#endif

#define NOT_REACHED()\
    do {\
        fprintf(stderr, "\033[31mNOT_REACHED: %s:%d In function %s()\033[0m\n", __FILE__, __LINE__, __func__);\
        while (1);\
    } while(0);
#endif