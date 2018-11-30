#include "obj_string.h"
#include <string.h>
#include "vm.h"
#include "utils.h"
#include "common.h"
#include <stdlib.h>

// fnv-la算法
uint32_t hashString(char* str, uint32_t length) {
    uint32_t hashCode = 2366136261, idx = 0;
    while (idx < length) {
        hashCode ^= str[idx];
        hashCode  *= 16777619;
        idx++;
    }
    return hashCode;
}