#include "unicodeUtf8.h"
#include "common.h"

// 返回value按照UTF-8编码后的字节数
uint32_t getByteNumOfEncodeUtf8(int value) {
    ASSERT(value > 0, "Can't encode negative values!");

    // 单个ASCII字符需要一个字节
    if (value <= 0x7f) {
        return 1;
    }

    // 此范围编码需要2字节
    if (value <= 0x7ff) {
        return 2;
    }

    // 3个字节
    if (value <= 0xffff) {
        return 3;
    }

    // 4个字节
    if (value <= 0x10ffff) {
        return 4;
    }

    return 0; // 超过范围返回0
}

// 把value编码为UTF-8后写入缓冲区buf, 返回写入的字节数
uint8_t encodeUtf8(uint8_t* buf, int value) {
    ASSERT(value > 0, "Can't encode negative value!");
    // 按照大端字节序写入缓冲区
    if (value <= 0x7f) {
        *buf = value & 0x7f;
        return 1;
    } else if (value <= 0x7ff) {
        // 先写入高字节
        *buf++ = 0xc0 | ((value & 0x7c0) >> 6);
        // 再写入低字节
        *buf = 0x80 | (value & 0x3f);
        return 2;
    }
}