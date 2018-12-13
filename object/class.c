#include "class.h"
#include "common.h"
#include "string.h"
#include "obj_range.h"
#include "core.h"
#include "vm.h"

DEFINE_BUFFER_METHOD(Method)

// 判断a和b是否相等
bool valueIsEqual(Value a, Value b) {
    // 类型不同则无需进行后面的比较
    if (a.type != b.type) {
        return false;
    }

    // 同为数字，比较数值
    if (a.type == VT_NUM) {
        return a.num == b.num;
    }

    

}