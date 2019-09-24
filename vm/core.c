#include "core.h"
#include <sys/stat.h>
#include "vm.h"
#include "obj_map.h"
#include "common.h"
#include "obj_thread.h"
#include "compiler.h"
#include "class.h"
#include "utils.h"
#include "obj_range.h"
#include "unicodeUtf8.h"
#include "obj_list.h"
#include "core.script.inc"
#include <time.h>
#include <string.h>
#include <math.h>
#include <ctype.h>
#include <errno.h>
#include "gc.h"

char* rootDir  = NULL; // 根目录

#define CORE_MODULE VT_TO_VALUE(VT_NULL)


// 返回值类型是Value类型，且是放在args[0], args是Value数组
// RET_VALUE的参数是VALUE类型，无需转换直接赋值
// 是后面“RET_xxx”的基础
#define RET_VALUE(value)\
    do {\
        args[0] = value;\
        return true;\
    } while(0)

#define RET_OBJ(objPtr) RET_VALUE(OBJ_TO_VALUE(objPtr))

// 将bool值转为Value后作为返回值
#define RET_BOOL(boolean) RET_VALUE(BOOL_TO_VALUE(boolean))
#define RET_NUM(num) RET_VALUE(NUM_TO_VALUE(num))
#define RET_NULL RET_VALUE(VT_TO_VALUE(VT_NULL))
#define RET_TRUE RET_VALUE(VT_TO_VALUE(VT_TRUE))
#define RET_FALSE RET_VALUE(VT_TO_VALUE(VT_FALSE))

// 设置线程报错
#define SET_ERROR_FALSE(vmPtr, errMsg)\
    do {\
        vmPtr->curThread->errorObj = \
            OBJ_TO_VALUE(newObjString(vmPtr, errMsg, strlen(errMsg)));\
            return false;\
    } while(0);

// 绑定方法func 到classPtr指向的类
#define PRIM_METHOD_BIND(classPtr, methodName, func) {\
    uint32_t length = strlen(methodName);\
    int globalIdx = getIndexFromSymbolTable(&vm->allMethodNames, methodName, length);\
    if (globalIdx == -1) {\
        globalIdx = addSymbol(vm, &vm->allMethodNames, methodName, length);\
    }\
    Method method;\
    method.type = MT_PRIMITIVE;\
    method.primFn = func;\
    bindMethod(vm, classPtr, (uint32_t)globalIdx, method);\
}

static ObjString* num2Str(VM* vm, double num) {
    // NaN不是一个确定的值,所以如果num!=num 就可以断定num是NaN
    if (num != num) {
        return newObjString(vm, "nan", 3);
    }

    if (num == INFINITY) {
        return newObjString(vm, "infinity", 8);
    }

    if (num == -INFINITY) {
        return newObjString(vm, "-infinity", 9);
    }

    // 双精度数字用24字节缓冲区足已容纳
    char buf[24] = {'\0'};
    int len = sprintf(buf, "%.14g", num);
    return newObjString(vm, buf, len);
}

// 判断arg是否为数字
static bool validateNum(VM* vm, Value arg) {
    if (VALUE_IS_NUM(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be number!");
}

static bool validateString(VM* vm, Value arg) {
    if (VALUE_IS_OBJSTR(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be string!");
}

// 将字符串转化为数字
static bool primNumFromString(VM* vm, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[1]);
    
    if (objString->value.length == 0) {
        RET_NULL;
    }

    ASSERT(objString->value.start[objString->value.length] == '\0', "objString't teminate!");
    errno = 0;
    char* endPtr;
    // 将字符串转化为double类型,它会自动跳过前面的空白
    double num = strtod(objString->value.start, &endPtr);

    while (*endPtr != '\0' && isspace((unsigned char)*endPtr)) {
        endPtr++;
    }

    // 字符串过长strtod会修改errno的值,errno为全局定义的宏
    if (errno == ERANGE) {
        RUN_ERROR("string too large");
    }

    // 如果字符串不能转换的字符不全是空白,字符串非法,返回NULL
    if (endPtr < objString->value.start+objString->value.length) {
        RET_NULL;
    }

    RET_NUM(num);
}

// 返回圆周率
static bool primNumPi(VM* vm UNUSED, Value* args UNUSED) {
    RET_NUM(3.14159265358979323846);
}

#define PRIM_NUM_INFIX(name, operator, type)\
    static bool name(VM* vm, Value* args) {\
        if (!validateNum(vm, args[1])) {\
            return false;\
        }\
        RET_##type(VALUE_TO_NUM(args[0]) operator VALUE_TO_NUM(args[1]) );\
    }

PRIM_NUM_INFIX(primNumPlus, +, NUM);
PRIM_NUM_INFIX(primNumMinus, -, NUM);
PRIM_NUM_INFIX(primNumMul, *, NUM);
PRIM_NUM_INFIX(primNumDiv, /, NUM);
PRIM_NUM_INFIX(primNumGt, >, BOOL);
PRIM_NUM_INFIX(primNumGe, >=, NUM);
PRIM_NUM_INFIX(primNumLt, <, NUM);
PRIM_NUM_INFIX(primNumLe, <=, NUM);
#undef PRIM_NUM_INFIX

#define PRIM_NUM_BIT(name, operator)\
    static bool name(VM* vm UNUSED, Value* args) {\
        if (!validateNum(vm, args[1])) {\
            return false;\
        }\
        uint32_t leftOperand = VALUE_TO_NUM(args[0]);\
        uint32_t rightOperand = VALUE_TO_NUM(args[1]);\
        RET_NUM(leftOperand operator rightOperand);\
    }

PRIM_NUM_BIT(primNumBitAnd, &);
PRIM_NUM_BIT(primNumBitOr, |);
PRIM_NUM_BIT(primNumBitShiftRight, >>);
PRIM_NUM_BIT(primNumBitShiftLeft, <<);
#undef PRIM_NUM_BIT

// 使用数学库函数
#define PRIM_NUM_MATH_FN(name, mathFn)\
    static bool name(VM* vm UNUSED, Value* args) {\
        RET_NUM(mathFn(VALUE_TO_NUM(args[0])));\
    }

PRIM_NUM_MATH_FN(primNumAbs, fabs);
PRIM_NUM_MATH_FN(primNumAcos, acos);
PRIM_NUM_MATH_FN(primNumAsin, asin);
PRIM_NUM_MATH_FN(primNumAtan, atan);
PRIM_NUM_MATH_FN(primNumCeil, ceil);
PRIM_NUM_MATH_FN(primNumCos, cos);
PRIM_NUM_MATH_FN(primNumFloor, floor);
PRIM_NUM_MATH_FN(primNumNegate, -);
PRIM_NUM_MATH_FN(primNumSin, sin);
PRIM_NUM_MATH_FN(primNumSqrt, sqrt);
PRIM_NUM_MATH_FN(primNumTan, tan);
#undef PRIM_NUM_MATH_FN

// 浮点取模fmod实现
static bool primNumMod(VM* vm UNUSED, Value* args) {
    if (!validateNum(vm, args[1])) {
        return false;
    }
    RET_NUM(fmod(VALUE_TO_NUM(args[0]), VALUE_TO_NUM(args[1])));
}

static bool primNumBitNot(VM* vm UNUSED, Value* args) {
    RET_NUM(~(uint32_t)VALUE_TO_NUM(args[0]));
}

static bool primNumRange(VM* vm UNUSED, Value* args) {
    if (!validateNum(vm, args[1])) {
        return false;
    }
    double from = VALUE_TO_NUM(args[0]);
    double to = VALUE_TO_NUM(args[1]);
    RET_OBJ(newObjRange(vm, from, to));
}

static bool primNumAtan2(VM* vm UNUSED, Value* args) {
    if (!validateNum(vm, args[1])) {
        return false;
    }
    RET_NUM(atan2(VALUE_TO_NUM(args[0]), VALUE_TO_NUM(args[1])));
}

// 返回小数部分
static bool primNumFraction(VM* vm UNUSED, Value* args) {
    double dummyInteger;
    RET_NUM(modf(VALUE_TO_NUM(args[0]), &dummyInteger));
}

// 判断数字是否无穷大
static bool primNumIsInfinity(VM* vm UNUSED, Value* args) {
    RET_BOOL(isinf(VALUE_TO_NUM(args[0])));
}

// 判断是否为数字
static bool primNumIsInteger(VM* vm UNUSED, Value* args) {
    double num = VALUE_TO_NUM(args[0]);
    if (isnan(num) || isinf(num)) {
        RET_FALSE;
    }
    RET_BOOL(trunc(num) == num);
}

// 判断数字是否为nan
static bool primNumIsNan(VM* vm UNUSED, Value* args) {
    RET_BOOL(isnan(VALUE_TO_NUM(args[0])));
}

// 数字转换为字符串
static bool primNumToString(VM* vm UNUSED, Value* args) {
    RET_OBJ(num2Str(vm, VALUE_TO_NUM(args[0])));
}

// 数字取整
static bool primNumTruncate(VM* vm UNUSED, Value* args) {
    double integer;
    modf(VALUE_TO_NUM(args[0]), &integer);
    RET_NUM(integer);
}

static bool primNumEqual(VM* vm UNUSED, Value* args) {
    if (!validateNum(vm, args[1])) {
        RET_FALSE;
    }
    RET_BOOL(VALUE_TO_NUM(args[0]) == VALUE_TO_NUM(args[1]));
}

static bool primNumNotEqual(VM* vm UNUSED, Value* args) {
    if (!validateNum(vm, args[1])) {
        RET_TRUE;
    }
    RET_BOOL(VALUE_TO_NUM(args[0]) != VALUE_TO_NUM(args[1]));
}

// 确认value是否为整数
static bool validateIntValue(VM* vm, double value) {
    if (trunc(value) == value) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be integer!");
}

static bool validateInt(VM* vm, Value arg) {
    if (!validateNum(vm, arg)) {
        return false;
    }
    return validateIntValue(vm, VALUE_TO_NUM(arg));
}

// 校验参数index是否是落在[0, length] 之间的整数
static uint32_t validateIndexValue(VM* vm, double index, uint32_t length) {
    if (!validateIntValue(vm, index)) {
        return UINT32_MAX;
    }
    // 支持负数索引,负数是由后往前索引
    if (index < 0) {
        index += length;
    }
    if (index >= 0 && index < length) {
        return (uint32_t) index;
    }
    // 超出范围
    vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, "index out of bound", 19));
    return UINT32_MAX;
}

// 验证index有效性
static uint32_t validateIndex(VM* vm, Value index, uint32_t length) {
    if (!validateNum(vm, index)) {
        return UINT32_MAX;
    }
    return validateIndexValue(vm, VALUE_TO_NUM(index), length);
}

// 从码点value创建字符串
static Value makeStringFromcodePoint(VM* vm, int value) {
    uint32_t byteNum = getByteNumOfEncodeUtf8(value);
    ASSERT(byteNum != 0, "utf8 encode bytes should be between 1 and 4!");
    // 结尾\0,+1
    ObjString* objString = ALLOCATE_EXTRA(vm, ObjString, byteNum+1);

    if (objString == NULL) {
        MEM_ERROR("allocate memory failed in runtime");
    }
    initObjHeader(vm, &objString->objHeader, OT_STRING, vm->stringClass);
    objString->value.length = byteNum;
    objString->value.start[byteNum] = '\0';
    encodeUtf8((uint8_t*)objString->value.start, value);
    hashObjString(objString);
    return OBJ_TO_VALUE(objString);
}

// 用索引index处的字节创建字符串对象
static Value stringCodePointAt(VM* vm, ObjString* objString, uint32_t index) {
    ASSERT(index < objString->value.length, "index out of bound!");
    int codePoint = decodeUtf8((uint8_t*)objString->value.start+index, objString->value.length-index);
    if (codePoint == -1) {
        return OBJ_TO_VALUE(newObjString(vm, &objString->value.start[index], 1));
    }
    return makeStringFromcodePoint(vm, codePoint);
}

// 计算objRange中元素的起始索引及索引方向
static uint32_t calculateRange(VM* vm, ObjRange* objRange, uint32_t* countPtr, int* directionPtr) {
    uint32_t from = validateIndexValue(vm, objRange->from, *countPtr);
    if (from == UINT32_MAX) {
        return UINT32_MAX;
    }
    uint32_t to = validateIndexValue(vm, objRange->to, *countPtr);
    if (to == UINT32_MAX) {
        return UINT32_MAX;
    }
    *directionPtr = from < to ? 1 : -1;
    *countPtr = abs((int)(from -to)) + 1;
    return from;
}

// 以UTF-8编码从sourceStr中起始为startIndex,方向为direction的count个字符创建字符串
static ObjString* newObjStringFromSub(VM* vm, ObjString* sourceStr, int startIndex, uint32_t count, int direction) {
    uint8_t* source = (uint8_t*)sourceStr->value.start;
    uint32_t totalLength = 0, idx = 0;
    while (idx < count) {
        totalLength += getByteNumOfDecodeUtf8(source[startIndex+idx*direction]);
        idx++;
    }
    ObjString* result = ALLOCATE_EXTRA(vm, ObjString, totalLength+1);
    if (result == NULL) {
        MEM_ERROR("allocate memory failed in runtime!");
    }
    initObjHeader(vm, &result->objHeader, OT_STRING, vm->stringClass);
    result->value.start[totalLength] = '\0';
    result->value.length = totalLength;
    
    uint8_t* dest = (uint8_t*)result->value.start;
    idx = 0;
    while (idx < count) {
        int index = startIndex + idx * direction;
        // 解码获取字符数据
        int codePoint = decodeUtf8(source+index, sourceStr->value.length-index);
        if (codePoint != -1) {
            dest += encodeUtf8(dest, codePoint);
        }
        idx++;
    }
    hashObjString(result);
    return result;
}

// 使用Boyer-Moore-horpool字符串匹配算法在haystack中查找need
static int findString(ObjString* haystack, ObjString* needle) {
    if (needle->value.length == 0) {
        return 0;
    }
    if (needle->value.length > haystack->value.length) {
        return -1;
    }

    uint32_t shift[UINT8_MAX];
    uint32_t needleEnd = needle->value.length - 1;
    uint32_t idx = 0;
    while (idx < UINT8_MAX) {
        shift[idx] = needle->value.length;
        idx++;
    }
    idx = 0;
    while (idx < needleEnd) {
        char c = needle->value.start[idx];
        shift[(uint8_t)c] = needleEnd - idx;
        idx++;
    }
    char lastChar = needle->value.start[needleEnd];
    uint32_t range = haystack->value.length - needle->value.length;
    idx = 0;
    while (idx <= range) {
        char c = haystack->value.start[idx+needleEnd];
        if (lastChar == c && memcmp(haystack->value.start+idx, needle->value.start, needleEnd) == 0) {
            return idx;
        }
        idx += shift[(uint8_t)c];
    }
    return -1;
}

static bool primStringFromCodePoint(VM* vm, Value* args) {
    if (!validateInt(vm, args[1])) {
        return false;
    }
    int codePoint = (int)VALUE_TO_NUM(args[1]);
    if (codePoint < 0) {
        SET_ERROR_FALSE(vm, "code point can't be negetive!");
    }
    if (codePoint > 0x10ffff) {
        SET_ERROR_FALSE(vm, "code point must be between 0 and 0x10ffff");
    }
    RET_VALUE(makeStringFromcodePoint(vm, codePoint));
}

// 字符串相加
static bool primStringPlus(VM* vm, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* left = VALUE_TO_OBJSTR(args[0]);
    ObjString* right = VALUE_TO_OBJSTR(args[1]);
    uint32_t totalLength = strlen(left->value.start) + strlen(right->value.start);
    ObjString* result = ALLOCATE_EXTRA(vm, ObjString, totalLength+1);
    if (result == NULL) {
        MEM_ERROR("allocate memory failed in runtime!");
    }
    initObjHeader(vm, &result->objHeader, OT_STRING, vm->stringClass);
    memcpy(result->value.start, left->value.start, strlen(left->value.start));
    memcpy(result->value.start+strlen(left->value.start), right->value.start, strlen(right->value.start));
    result->value.start[totalLength] = '\0';
    hashObjString(result);
    RET_OBJ(result);
}

// objString[_]:用数字或objRange对象做字符串的subscript
static bool primStringSubscript(VM* vm, Value* args) {
    ObjString* objstring = VALUE_TO_OBJSTR(args[0]);
    if (VALUE_IS_NUM(args[1])) {
        uint32_t index = validateIndex(vm, args[1], objstring->value.length);
        if (index == UINT32_MAX) {
            return false;
        }
        RET_VALUE(stringCodePointAt(vm, objstring, index));
    }
    if (!VALUE_IS_OBJRANGE(args[1])) {
        SET_ERROR_FALSE(vm, "subscript should be integer or range!");
    }
    int direction;
    uint32_t count = objstring->value.length;
    uint32_t startIndx = calculateRange(vm, VALUE_TO_OBJRANGE(args[1]), &count, &direction);
    if (startIndx == UINT32_MAX) {
        return false;
    }
    RET_OBJ(newObjStringFromSub(vm, objstring, startIndx, count, direction));
}

// objString.byteAt_():返回指定索引的字节
static bool primStringByteAt(VM* vm UNUSED, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    uint32_t index = validateIndex(vm, args[1], objString->value.length);
    if (index == UINT32_MAX) {
        return false;
    }
    RET_NUM((uint8_t)objString->value.start[index]);
}

// objstring.byteCount_:返回字节数
static bool primStringByteCount(VM* vm UNUSED, Value* args) {
    RET_NUM(VALUE_TO_OBJSTR(args[0])->value.length);
}

// objString.codePointAt_(_):返回指定的CodePoint
static bool primStringCodePointAt(VM* vm UNUSED, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    uint32_t index = validateIndex(vm, args[1], objString->value.length);
    if (index == UINT32_MAX) {
        return false;
    }
    const uint8_t* bytes = (uint8_t*)objString->value.start;
    if ((bytes[index] & 0xc0) == 0x80) {
        // 如果index指向的并不是utf8编码的最高字节,而是后面的低字节,返回-1提示用户
        RET_NUM(-1);
    }
    // 返回解码
    RET_NUM(decodeUtf8((uint8_t*)objString->value.start+index, objString->value.length-index));
}

// objString.contains(_): 判断字符串args[0]中是否包含子字符串args[1]
static bool primStringContains(VM* vm UNUSED, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    ObjString* pattern = VALUE_TO_OBJSTR(args[1]);
    RET_BOOL(findString(objString, pattern) != -1);
}

// objString.endsWith(_): 返回字符串是否以args[1]为结束
static bool primStringEndsWith(VM* vm UNUSED, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    ObjString* pattern = VALUE_TO_OBJSTR(args[1]);
    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }
    char* cmpIdx = objString->value.start + objString->value.length-pattern->value.length;
    RET_BOOL(memcmp(cmpIdx, pattern->value.start, pattern->value.length) == 0);
}

// objString.indexOf(_): 检索字符串args[0]中子串args[1]的起始下标
static bool primStringIndexOf(VM* vm UNUSED, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    ObjString* pattern = VALUE_TO_OBJSTR(args[1]);
    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }
    int index = findString(objString, pattern);
    RET_NUM(index);
}

// objString.iterate(_):返回下一个utf-8字符串(不是字节)的迭代器
static bool primStringIterate(VM* vm UNUSED, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    if (VALUE_IS_NULL(args[1])) { // 第一次迭代为null
        if (objString->value.length == 0) {
            RET_FALSE;
        }
        RET_NUM(0);
    }
    // 迭代器必须是正整数
    if (!validateInt(vm, args[1])) {
        return false;
    }
    double iter = VALUE_TO_NUM(args[1]);
    if (iter < 0) {
        RET_FALSE;
    }
    uint32_t index = (uint32_t)iter;
    do {
        index++;
        if (index >= objString->value.length) RET_FALSE;
    } while((objString->value.start[index] & 0xc0) == 0x80);
    RET_NUM(index);
}

// objString.iterateByte_(_): 迭代索引,内部使用
static bool primStringIterateByte(VM* vm UNUSED, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    if (VALUE_IS_NULL(args[1])) {
        if (objString->value.length == 0) {
            RET_FALSE;
        }
        RET_NUM(0);
    }
    if (!validateInt(vm, args[1])) {
        return false;
    }
    double iter = VALUE_TO_NUM(args[1]);
    if (iter < 0) {
        RET_FALSE;
    }

    uint32_t index = (uint32_t)iter;
    index++; // 下一个字节的索引
    if (index >= objString->value.length) {
        RET_FALSE;
    }
    RET_NUM(index);
}

// objString.itervatorValue(_): 返回迭代器对应的value
static bool primStringIteratorValue(VM* vm, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    uint32_t index = validateIndex(vm, args[1], objString->value.length);
    if (index == UINT32_MAX) {
        return false;
    }
    RET_VALUE(stringCodePointAt(vm, objString, index));
}
// objString.startsWith(_): 返回args[0]是否以args[1]为起始
static bool primStringStartsWith(VM* vm UNUSED, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[0]);
    ObjString* pattern = VALUE_TO_OBJSTR(args[1]);

    if (pattern->value.length > objString->value.length) {
        RET_FALSE;
    }
    RET_BOOL(memcmp(objString->value.start, pattern->value.start, pattern->value.length) == 0);
}

// objString.toString: 获得自己的字符串
static bool primStringToString(VM* vm UNUSED, Value* args) {
    RET_VALUE(args[0]);
}

// objList.New(): 创建一个新的list
static bool primListNew(VM* vm, Value* args UNUSED) {
    RET_OBJ(newObjList(vm, 0));
}

// objList[_]: 索引list元素
static bool primListSubscript(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    if (VALUE_IS_NUM(args[1])) {
        uint32_t index = validateIndex(vm, args[1], objList->elements.count);
        if (index == UINT32_MAX) {
            return false;
        }
        RET_VALUE(objList->elements.datas[index]);
    }

    if (!VALUE_IS_OBJRANGE(args[1])) {
        SET_ERROR_FALSE(vm, "subscript should be integer or range!");
    }
    int direction;
    uint32_t count = objList->elements.count;
    uint32_t startIndex = calculateRange(vm, VALUE_TO_OBJRANGE(args[1]), &count, &direction);

    ObjList* result = newObjList(vm, count);
    uint32_t idx = 0;
    while (idx < count) {
        result->elements.datas[idx] = objList->elements.datas[startIndex+idx*direction];
        idx++;
    }
    RET_OBJ(result);
}

// objList[_]=(_): 值支持数字作为subscript
static bool primListSubscriptSetter(VM* vm UNUSED, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    uint32_t index = validateIndex(vm, args[1], objList->elements.count);
    if (index == UINT32_MAX) {
        return false;
    }
    objList->elements.datas[index] = args[2];
    RET_VALUE(args[2]);
}

// objList.add(_): 直接追加到list中
static bool primListAdd(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferAdd(vm, &objList->elements, args[1]);
    RET_VALUE(args[1]); // 参数作为返回值
}

// objList.addCore_(_): 编译列表直接量
static bool primListAddCore(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferAdd(vm, &objList->elements, args[1]);
    RET_VALUE(args[0]); // 返回自身
}

// objList.clear(): 清空list
static bool primListClear(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    ValueBufferClear(vm, &objList->elements);
    RET_NULL;
}

// objList.count: 返回list中元素个数
static bool primListCount(VM* vm UNUSED, Value* args) {
    RET_NUM(VALUE_TO_OBJLIST(args[0])->elements.count);
}

// objList.insert(_,_): 插入元素
static bool primListInsert(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    // +1 确保可以在最后插入
    uint32_t index = validateIndex(vm, args[1], objList->elements.count+1);
    if (index == UINT32_MAX) {
        return false;
    }
    insertElement(vm, objList, index, args[2]);
    RET_VALUE(args[2]);
}

// objList.iterate(_):返回list
static bool primListIterate(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    if (VALUE_IS_NULL(args[1])) {
        if (objList->elements.count == 0) {
            RET_FALSE;
        }
        RET_NUM(0);
    }
    if (!validateInt(vm, args[1])) {
        return false;
    }
    double iter = VALUE_TO_NUM(args[1]);
    if (iter<0 || iter >= objList->elements.count - 1) {
        RET_FALSE;
    }
    RET_NUM(iter+1);
}

// objList.iteratorValue(_): 返回迭代值
static bool primListIteratorValue(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    uint32_t index = validateIndex(vm, args[1], objList->elements.count);
    if (index == UINT32_MAX) {
        return false;
    }
    RET_VALUE(objList->elements.datas[index]);
}

// objList.remoteAt(_): 删除指定位置的元素
static bool primListRemoveAt(VM* vm, Value* args) {
    ObjList* objList = VALUE_TO_OBJLIST(args[0]);
    uint32_t index = validateIndex(vm, args[1], objList->elements.count);
    if (index == UINT32_MAX) {
        return false;
    }
    RET_VALUE(removeElement(vm, objList, index));
}

// 校验key合法性
static bool validateKey(VM* vm, Value arg) {
    if (VALUE_IS_TRUE(arg) || 
        VALUE_IS_FALSE(arg) ||
        VALUE_IS_NULL(arg) ||
        VALUE_IS_NUM(arg) ||
        VALUE_IS_OBJSTR(arg) ||
        VALUE_IS_OBJRANGE(arg) ||
        VALUE_IS_CLASS(arg)) {
            return true;
    }
    SET_ERROR_FALSE(vm, "key must be value type!");
}

// objMap.new(): 创建map对象
static bool primMapNew(VM* vm, Value* args UNUSED) {
    RET_OBJ(newObjMap(vm));
}
// objMap[_]: 返回map[key]对应的value
static bool primMapSubscript(VM* vm, Value* args) {
    if (!validateKey(vm, args[1])) {
        return false;
    }
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    Value value = mapGet(objMap, args[1]);
    if (VALUE_IS_UNDEFINED(value)) {
        RET_NULL;
    }
    RET_VALUE(value);
}

// objMap[_]=(_): map[key]=value
static bool primMapSubscriptSetter(VM* vm, Value* args) {
    if (!validateKey(vm, args[1])) {
        return false;
    }
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    mapSet(vm, objMap, args[1], args[2]);
    RET_VALUE(args[2]);
}

// objMap.addCore_(_,_): 编译map字面量时内部使用
static bool primMapAddCore(VM* vm, Value* args) {
    if (!validateKey(vm, args[1])) {
        return false;
    }
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    mapSet(vm, objMap, args[1], args[2]);
    RET_VALUE(args[0]);
}

// objMap.clear(): 清除map
static bool primMapClear(VM* vm, Value* args) {
    clearMap(vm, VALUE_TO_OBJMAP(args[0]));
    RET_NULL;
}

// objMap.containsKey(_): 判断map即args[0]是否包含key即args[1]
static bool primMapContainsKey(VM* vm, Value* args) {
    if (!validateKey(vm, args[1])) {
        return false;
    }
    RET_BOOL(!VALUE_IS_UNDEFINED(mapGet(VALUE_TO_OBJMAP(args[0]), args[1])));
}

// objMap.count: 返回map中entry个数
static bool primMapCount(VM* vm UNUSED, Value* args) {
    RET_NUM(VALUE_TO_OBJMAP(args[0])->count);
}

// objMap.remove(_): 删除map[key]
static bool primMapRemove(VM* vm, Value* args) {
    if (!validateKey(vm, args[1])) {
        return false;
    }
    RET_VALUE(removeKey(vm, VALUE_TO_OBJMAP(args[0]), args[1]));
}
// objMap.iterate_(_): 迭代map中的entry
// 返回entry的索引供keyIteratorValue_和valueIteratorValue_做迭代器
static bool primMapIterate(VM* vm, Value* args) {
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    if (objMap->count == 0) {
        RET_FALSE;
    }
    uint32_t index = 0;
    if (!VALUE_IS_NULL(args[1])) {
        if (!validateInt(vm, args[1])) {
            return false;
        }
        if (VALUE_TO_NUM(args[1]) < 0) {
            RET_FALSE;
        }
        index = (uint32_t)VALUE_TO_NUM(args[1]);
        if (index >= objMap->capacity) {
            RET_FALSE;
        }
        index++;
    }
    while (index < objMap->capacity) {
        // entries十个数组,元素是哈希槽
        // 哈希值散布在这些槽中并不连续,因此逐个判断是否在用
        if (!VALUE_IS_UNDEFINED(objMap->entries[index].key)) {
            RET_NUM(index);
        }
        index++;
    }
    RET_FALSE;
}

static bool primMapKeyIteratorValue(VM* vm, Value* args) {
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    uint32_t index = validateIndex(vm, args[1], objMap->capacity);
    if (index == UINT32_MAX) {
        return false;
    }
    Entry* entry = &objMap->entries[index];
    if (VALUE_IS_UNDEFINED(entry->key)) {
        SET_ERROR_FALSE(vm, "invalid iterator!");
    }
    RET_VALUE(entry->key);
}

// objMap.valueIteratorValue_(_)
// value = map.valueIteratorValue_(iter)
static bool primMapValueIteratorValue(VM* vm, Value* args) {
    ObjMap* objMap = VALUE_TO_OBJMAP(args[0]);
    uint32_t index = validateIndex(vm, args[1], objMap->capacity);
    if (index == UINT32_MAX) {
        return false;
    }
    Entry* entry = &objMap->entries[index];
    if (VALUE_IS_UNDEFINED(entry->key)) {
        SET_ERROR_FALSE(vm, "invalid iterator!");
    }
    RET_VALUE(entry->value);

}

// objRange.from: 返回range的from
static bool primRangeFrom(VM* vm UNUSED, Value* args) {
    RET_NUM(VALUE_TO_OBJRANGE(args[0])->from);
}
// objRange.to: 返回range的to
static bool primRangeTo(VM* vm UNUSED, Value* args) {
    RET_NUM(VALUE_TO_OBJRANGE(args[0])->to);
}
// objRange.min: 返回range中from和to较小的值
static bool primRangeMin(VM* vm UNUSED, Value* args) {
    ObjRange* objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(fmin(objRange->from, objRange->to));
}
// objRange.max: 返回range中from和to较大的值
static bool primRangeMax(VM* vm UNUSED, Value* args) {
    ObjRange* objRange = VALUE_TO_OBJRANGE(args[0]);
    RET_NUM(fmax(objRange->from, objRange->to));
}
// objRange.iterate(_): 返回range中的值,并不索引
static bool primRangeIterate(VM* vm, Value* args) {
    ObjRange* objRange  = VALUE_TO_OBJRANGE(args[0]);
    if (VALUE_IS_NULL(args[1])) {
        RET_NUM(objRange->from);
    }
    if (!validateNum(vm, args[1])) {
        return false;
    }
    double iter = VALUE_TO_NUM(args[1]);
    if (objRange->from < objRange->to) {
        iter++;
        if (iter > objRange->to) {
            RET_FALSE;
        }
    } else { // 反向迭代
        iter--;
        if (iter < objRange->to) {
            RET_FALSE;
        }
    }
    RET_NUM(iter);
}

// objRange.iteratorValue(_): range的迭代就是range中从from到to之间的值
// 因此直接返回迭代器就是range的值
static bool primRangeIteratorValue(VM* vm UNUSED, Value* args) {
    ObjRange* objRange = VALUE_TO_OBJRANGE(args[0]);
    double value = VALUE_TO_NUM(args[1]);
    if (objRange->from < objRange->to) {
        if (value >= objRange->from && value <= objRange->to) {
            RET_VALUE(args[1]);
        }
    } else {    // 反向迭代
        if (value <= objRange->from && value >= objRange->to) {
            RET_VALUE(args[1]);
        }
    }
    RET_FALSE;
}

// 获取文件全路径
static char* getFilePath(const char* moduleName) {
    uint32_t rootDirLength = rootDir == NULL ? 0 : strlen(rootDir);
    uint32_t nameLength = strlen(moduleName);
    uint32_t pathLength = rootDirLength + nameLength + strlen(".sp");
    char* path = (char*)malloc(pathLength + 1);
    if (rootDir != NULL) {
        memmove(path, rootDir, rootDirLength);
    }
    memmove(path + rootDirLength, moduleName, nameLength);
    memmove(path + rootDirLength + nameLength, ".sp", 3);
    path[pathLength] = '\0';
    return path;
}
// 读取模块
static char* readModule(const char* moduleName) {
    char* modulePath = getFilePath(moduleName);
    char* moduleCode = readFile(modulePath);
    free(modulePath);
    return moduleCode;
}
// 输出字符串
static void printString(const char* str) {
    printf("%s", str);
    fflush(stdout);
}

// 从modules中获取名为moduleName的模块
static ObjModule* getModule(VM* vm, Value moduleName) {
    Value value = mapGet(vm->allModules, moduleName);
    if (value.type == VT_UNDEFINED) {
        return NULL;
    }
    return (ObjModule*)(value.objHeader);
}

// 载入模块moduleName并编译
static ObjThread* loadModule(VM* vm, Value moduleName, const char* moduleCode) {
    // 确保模块已经载入到vm->allModules
    // 先查看是否已经导入了该模块，避免重新导入
    ObjModule* module = getModule(vm, moduleName);

    // 若该模块未加载先将其加载，并继承核心模块中的变量
    if (module == NULL) {
        // 创建模块并添加到vm->allModules
        ObjString* modName = VALUE_TO_OBJSTR(moduleName);
        ASSERT(modName->value.start[modName->value.length] == '\0', "string.value.start is not terminated!");
        module = newObjModule(vm, modName->value.start);
        mapSet(vm, vm->allModules, moduleName, OBJ_TO_VALUE(module));

        // 继承核心模块中的变量
        ObjModule* coreModule = getModule(vm, CORE_MODULE);
        uint32_t idx = 0;
        while (idx < coreModule->moduleVarName.count) {
            defineModuleVar(vm, module, 
            coreModule->moduleVarName.datas[idx].str,
            strlen(coreModule->moduleVarName.datas[idx].str),
            coreModule->moduleVarValue.datas[idx]);
            idx++;
        }
    }

    ObjFn* fn = compileModule(vm, module, moduleCode);
    ObjClosure* objClosure = newObjClosure(vm, fn);
    ObjThread* moduleThread = newObjThread(vm, objClosure);

    return moduleThread;
}

// 导入模块moduleName,主要是编译模块加载到vm->allModules
static Value importModule(VM* vm, Value moduleName) {
    // 若已经存在则返回null_val
    if (!(VALUE_IS_UNDEFINED(mapGet(vm->allModules, moduleName)))) {
        return VT_TO_VALUE(VT_NULL);
    }
    ObjString* objString = VALUE_TO_OBJSTR(moduleName);
    const char* sourceCode = readModule(objString->value.start);

    ObjThread* moduleThread = loadModule(vm, moduleName, sourceCode);
    return OBJ_TO_VALUE(moduleThread);
}
// 在模块moduleName中获取模块变量variableName
static Value getModuleVariable(VM* vm, Value moduleName, Value variableName) {
    ObjModule* objModule = getModule(vm, moduleName);
    if (objModule == NULL) {
        ObjString* modName = VALUE_TO_OBJSTR(moduleName);
        // 24是下面sprintf中fmt中除%s的字符个数
        ASSERT(modName->value.length < 512-24, "id's buffer not big enough!");
        char id[512] = {'\0'};
        int len = sprintf(id, "module \'%s\' is not loaded!", modName->value.start);
        vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }
    ObjString* varName = VALUE_TO_OBJSTR(variableName);
    int index = getIndexFromSymbolTable(&objModule->moduleVarName, varName->value.start, varName->value.length);
    if (index == -1) {
        // 32是下面sprintf中fmt中除%s的字符个数
        ASSERT(varName->value.length < 512-32, "id's buffer not big enough!");
        ObjString* modName = VALUE_TO_OBJSTR(moduleName);
        char id[512] = {'\0'};
        int len = sprintf(id, "variable \'%s\' is not in module \'%s\'!", varName->value.start, modName->value.start);
        vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, id, len));
        return VT_TO_VALUE(VT_NULL);
    }
    return objModule->moduleVarValue.datas[index];
}
// System.clock: 返回秒为单位的系统时钟
static bool primSystemClock(VM* vm UNUSED, Value* args UNUSED) {
    RET_NUM((double)time(NULL));
}
// System.gc()
static bool primSystemGC(VM* vm, Value* args) {
    startGC(vm);
    RET_NULL;
}
// System.importModule(_): 导入未编译模块args[1], 把模块挂载到vm->allModules
static bool primSystemImportModule(VM* vm, Value* args) {
    // args[1]模块名
    if (!validateString(vm, args[1])) {
        return false;
    }
    Value result = importModule(vm, args[1]);
    if (VALUE_IS_NULL(result)) { // 已经导入返回null
        RET_NULL;
    }
    // 若编译过程除了问题,切换到下一线程
    if (!VALUE_IS_NULL(vm->curThread->errorObj)) {
        return false;
    }
    // 回收一个slot
    // TODO: check
    vm->curThread->esp--;
    ObjThread* nextThread = VALUE_TO_OBJTHREAD(result);
    nextThread->caller = vm->curThread;
    vm->curThread = nextThread;
    return false;
}

// System.getModuleVariable(_,_): 获得模块args[1]中的模块变量args[2]
static bool primSystemGetModuleVariable(VM* vm, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    if (!validateString(vm, args[2])) {
        return false;
    }
    Value result = getModuleVariable(vm, args[1], args[2]);
    if (VALUE_IS_NULL(result)) {
        // 出错给vm返回false切换线程
        return false;
    }
    RET_VALUE(result);
}
// System.writeString_(_): 输出字符串args[1]
static bool primSystemWriteString(VM* vm UNUSED, Value* args) {
    ObjString* objString = VALUE_TO_OBJSTR(args[1]);
    ASSERT(objString->value.start[objString->value.length] == '\0', "string isn't terminated!");
    printString(objString->value.start);
    RET_VALUE(args[1]);
}










// 返回核心模块name的value结构
static Value getCoreClassValue(ObjModule* objModule, const char* name) {
    int index = getIndexFromSymbolTable(&objModule->moduleVarName, name, strlen(name));
    if (index == -1) {
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, strlen(name));
        RUN_ERROR("something wrong occur: missing core class \"%s\"!", id);
    }
    return objModule->moduleVarValue.datas[index];
}

// 返回bool的字符串形式: true或false
static bool primBoolToString(VM* vm, Value* args) {
    ObjString* objString;
    if (VALUE_TO_BOOL(args[0])) {
        objString = newObjString(vm, "true", 4);
    } else {
        objString = newObjString(vm, "false", 5);
    }
    RET_OBJ(objString);
}

// bool值取反
static bool primBoolNot(VM* vm UNUSED, Value* args) {
    RET_BOOL(!VALUE_TO_BOOL(args[0]));
}

// 校验是否是函数
static bool validateFn(VM* vm, Value arg) {
    if (VALUE_TO_OBJCLOSURE(arg)) {
        return true;
    }

    vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, "argument must be a function!", 28));
    return false;
}

// 以大写字符开头的为类名,表示类(静态)方法的调用
// Thread.new(func): 创建一个thread实例
static bool primThreadNew(VM* vm, Value* args) {
    // 代码块为参数必为闭包
    if (!validateFn(vm, args[1])) {
        return false;
    }
    ObjThread* objThread = newObjThread(vm, VALUE_TO_OBJCLOSURE(args[1]));

    // stack[0]为接收者,保持栈平衡
    objThread->stack[0] = VT_TO_VALUE(VT_NULL);
    objThread->esp++;
    RET_OBJ(objThread);
}

// Thread.ablort(err): 以错误信息err为参数退出线程
static bool primThreadAbort(VM* vm, Value* args) {
    vm->curThread->errorObj = args[1];
    return VALUE_IS_NULL(args[1]);
}

// Thread.current: 返回当前线程
static bool primThreadCurrent(VM* vm, Value* args UNUSED) {
    RET_OBJ(vm->curThread);
}

// Thread.suspend(): 挂起线程,退出解析器
static bool primThreadSuspend(VM* vm, Value* args UNUSED) {
    // curThread = NULL虚拟机将推出
    vm->curThread = NULL;
    return false;
}

// Thread.yield(arg) 带参数让出cpu
static bool primThreadYieldWithArg(VM* vm, Value* args) {
    ObjThread* curThread = vm->curThread;
    vm->curThread = curThread->caller;
    curThread->caller = NULL;
    if (vm->curThread != NULL) {
        // 如果当前线程有主调方,将当前线程的返回值放在主调方的栈顶
        vm->curThread->esp[-1] = args[1];
        // 对于thread.yield(arg)来说,回收arg的空间,
        // 保留thread参数所在的空间,将来唤醒时用于存储yield结果
        curThread->esp--;
    }
    return false;
}

// Thread.yield() 无参数让出cpu
static bool primThreadYieldWithoutArg(VM* vm, Value* args UNUSED) {
    ObjThread* curThread = vm->curThread;
    vm->curThread = curThread->caller;
    curThread->caller = NULL;
    if (vm->curThread != NULL) {
        vm->curThread->esp[-1] = VT_TO_VALUE(VT_NULL);
    }
    return false;
}

// 切换到下一个线程
static bool switchThread(VM* vm, ObjThread* nextThread, Value* args, bool withArg) {
    if (nextThread->caller != NULL) {
        RUN_ERROR("thread has been called!");
    }
    nextThread->caller = vm->curThread;
    if (nextThread->usedFrameNum == 0) {
        // 只有运行完毕的thread才为0
        SET_ERROR_FALSE(vm, "a finished thread can't be switched to!");
    }
    if (!VALUE_IS_NULL(nextThread->errorObj)) {
        SET_ERROR_FALSE(vm, "a aborted thread can't be switched to!");
    }
    if (withArg) {
        // 如果call有参数,回收参数的空间
        // 只保留此栈顶用于存储nextThread返回后的结果
        vm->curThread->esp--;
    }
    ASSERT(nextThread->esp > nextThread->stack, "esp should be greater than stack!");
    nextThread->esp[-1] = withArg ? args[1] : VT_TO_VALUE(VT_NULL);

    vm->curThread = nextThread;
    return false;
}

// objThread.call()
static bool primThreadCallWithoutArg(VM* vm, Value* args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, false);
}

// objThread.call(args)
static bool primThreadCallWithArg(VM* vm, Value* args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, true);
}

// 返回线程是否运行完成
static bool primThreadIsDone(VM* vm UNUSED, Value* args) {
    // 获取.isDone的调度者
    ObjThread* objThread = VALUE_TO_OBJTHREAD(args[0]);
    RET_BOOL(objThread->usedFrameNum == 0 || !VALUE_IS_NULL(objThread->errorObj));
}

static Class* defineClass(VM* vm, ObjModule* objModule, const char* name) {
    // 1. 先创建类
    Class* class = newRawClass(vm, name, 0);
    // 2.把类作为普通变量在模块中定义
    defineModuleVar(vm, objModule, name, strlen(name), OBJ_TO_VALUE(class));
    return class;
}

// !object: object 取反，结果为false
static bool primObjectNot(VM* vm UNUSED, Value* args) {
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0] == args[1]: 返回object是否相等
static bool primObjectEqual(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] != args[1]: 返回object是否不等
static bool primObjectNotEqual(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(!valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] is args[1]:类args[0]是否为类args[1]的子类
static bool primObjectIs(VM* vm, Value* args) {
    // args[1] 必须是class
    if (!VALUE_IS_CLASS(args[1])) {
        RUN_ERROR("argument must be class!");
    }

    Class* thisClass = getClassOfObj(vm, args[0]);
    Class* baseClass = (Class*)(args[1].objHeader);

    // 有可能多级继承，自上而下遍历
    while (baseClass != NULL) {
        if (thisClass == baseClass) {
            RET_VALUE(VT_TO_VALUE(VT_TRUE));
        }
        baseClass = baseClass->superClass;
    }
    // 若未找到基类，说明不具备is_a关系
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0].tostring: 返回args[0]所属class的名字
static bool primObjectToString(VM* vm UNUSED, Value* args) {
    Class* class = args[0].objHeader->class;
    Value nameValue = OBJ_TO_VALUE(class->name);
    RET_VALUE(nameValue);
}

// args[0].type: 返回对象args[0]的类
static bool primObjectType(VM* vm, Value* args) {
    Class* class = getClassOfObj(vm, args[0]);
    RET_OBJ(class);
}

// args[0].name: 返回类名
static bool primClassName(VM* vm UNUSED, Value* args) {
    RET_OBJ(VALUE_TO_CLASS(args[0])->name);
}

// args[0].supertype: 返回args[0]的基类
static bool primClassSupertype(VM* vm UNUSED, Value* args) {
    Class* class  = VALUE_TO_CLASS(args[0]);
    if (class->superClass != NULL) {
        RET_OBJ(class->superClass);
    }
    RET_VALUE(VT_TO_VALUE(VT_NULL));
}

// args[0].toString：返回类名
static bool primClassToString(VM* vm UNUSED, Value* args) {
    RET_OBJ(VALUE_TO_CLASS(args[0])->name);
}

// args[0].same(args[1], args[2]): 返回args[1]和args[2]是否相等
static bool primObjectmetaSame(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[1], args[2]));
    RET_VALUE(boolValue);
}

// Fn.new(_): 新建一个函数
static bool primFnNew(VM* vm, Value* args) {
    // 代码块为参数必为闭包
    if (!validateFn(vm, args[1])) return false;

    // 直接返回函数闭包
    RET_VALUE(args[1]);
}

// 绑定fn.call的重载
static void bindFnOverloadCall(VM* vm, const char* sign) {
    uint32_t index = ensureSymbolExist(vm, &vm->allMethodNames, sign, strlen(sign));
    Method method = {MT_FN_CALL, {0}};
    bindMethod(vm, vm->fnClass, index, method);
}

// null 取非
static bool primNullNot(VM* vm UNUSED, Value* args UNUSED) {
    RET_VALUE(BOOL_TO_VALUE(true));
}

static bool primNullToString(VM* vm, Value* args UNUSED) {
    ObjString* objString = newObjString(vm, "null", 4);
    RET_OBJ(objString);
}

// 读取源代码文件
char* readFile(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        IO_ERROR("Could't open file %s", path);
    }

    struct stat fileStat;
    stat(path, &fileStat);
    size_t fileSize = fileStat.st_size;
    char* fileContent = (char*)malloc(fileSize+1);

    if (fileContent == NULL) {
        MEM_ERROR("Count't allocate memory for reading file %s \n", path);
    }

    size_t numRead = fread(fileContent, sizeof(char), fileSize, file);
    if (numRead < fileSize) {
        IO_ERROR("Cound't read file %s\n", path);
    }

    fileContent[fileSize] = '\0';
    fclose(file);
    return fileContent;
}

// table中查找符号symbol, 找到后返回索引，否则返回-1
int getIndexFromSymbolTable(SymbolTable* table, const char* symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");
    uint32_t index = 0;
    while (index < table->count) {
        if (length == table->datas[index].length &&
            memcmp(table->datas[index].str, symbol, length) == 0) {
                return index;
            }
            index++;
    }
    return -1;
}

// 往table中添加符号symbol,返回其索引
int addSymbol(VM* vm, SymbolTable* table, const char* symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");
    String string;
    string.str = ALLOCATE_ARRAY(vm, char, length+1);
    memcpy(string.str, symbol, length);
    string.str[length] = '\0';
    string.length = length;
    StringBufferAdd(vm, table, string);
    return table->count - 1;
}

int ensureSymbolExist(VM* vm, SymbolTable* table, const char* symbol, uint32_t length) {
    int symbolIndex = getIndexFromSymbolTable(table, symbol, length);
    if (symbolIndex == -1) {
        return addSymbol(vm, table, symbol, length);
    }
    return symbolIndex;
}

// 使class->methods[index]=method
void bindMethod(VM* vm, Class* class, uint32_t index, Method method) {
    if (index >= class->methods.count) {
        Method emptyPad = {MT_NONE, {0}};
        MethodBufferFillWrite(vm, &class->methods, emptyPad, index-class->methods.count + 1);
    }
    class->methods.datas[index] = method;
}
// 绑定基类
void bindSuperClass(VM* vm, Class* subClass, Class* superClass) {
    subClass->superClass = superClass;
    // 继承基类属性数
    subClass->fieldNum += superClass->fieldNum;
    //继承基类方法
    uint32_t idx = 0;
    while (idx < superClass->methods.count) {
        bindMethod(vm, subClass, idx, superClass->methods.datas[idx]);
        idx++;
    }
}

// 编译核心模块
void buildCore(VM* vm) {

    // 核心模块不需要名字，模块也允许名字为空
    ObjModule* coreModule = newObjModule(vm, NULL);

    // 创建核心模块，录入到vm->allModules
    mapSet(vm, vm->allModules, CORE_MODULE, OBJ_TO_VALUE(coreModule));

    // 创建object类并绑定方法
    // 根据签名从vm->allMethodNames中查找方法签名
    // 不存在签名时把签名记录到allMethodNames中,然后根据把allMethodNames中的索引记录到当前class.methods中
    vm->objectClass = defineClass(vm, coreModule, "object");
    PRIM_METHOD_BIND(vm->objectClass, "!", primObjectNot);
    PRIM_METHOD_BIND(vm->objectClass, "==(_)", primObjectEqual);
    PRIM_METHOD_BIND(vm->objectClass, "!=(_)", primObjectNotEqual);
    PRIM_METHOD_BIND(vm->objectClass, "is(_)", primObjectIs);
    PRIM_METHOD_BIND(vm->objectClass, "toString", primObjectToString);
    PRIM_METHOD_BIND(vm->objectClass, "type", primObjectType);

    // 定义classOfClass类，它是所有meta类的meta类和基类
    vm->classOfClass = defineClass(vm, coreModule, "class");

    // objectClass是任何类的基类
    bindSuperClass(vm, vm->classOfClass, vm->objectClass);

    PRIM_METHOD_BIND(vm->classOfClass, "name", primClassName);
    PRIM_METHOD_BIND(vm->classOfClass, "supertype", primClassSupertype);
    PRIM_METHOD_BIND(vm->classOfClass, "toString", primClassToString);

    // 定义object类的元信息类objectMetaclass,它无需挂载到vm
    Class* objectMetaclass = defineClass(vm, coreModule, "objectMeta");

    // classOfClass类是所有meta类的meta类和基类
    bindSuperClass(vm, objectMetaclass, vm->classOfClass);

    // 类型比较
    PRIM_METHOD_BIND(objectMetaclass, "same(_,_)", primObjectmetaSame);
    // 绑定各自的meta类
    vm->objectClass->objHeader.class = objectMetaclass;
    objectMetaclass->objHeader.class = vm->classOfClass;
    vm->classOfClass->objHeader.class = vm->classOfClass; // 元信息类回路，meta类终点

    
    executeModule(vm, CORE_MODULE, coreModuleCode);

    vm->boolClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Bool"));
    PRIM_METHOD_BIND(vm->boolClass, "toString", primBoolToString);
    PRIM_METHOD_BIND(vm->boolClass, "!", primBoolNot);

    // thread类在core.script.inc中定义
    // 将其挂载到vm->threadClass 并补全原生方法
    vm->threadClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Thread"));
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "new(_)", primThreadNew);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "abort(_)", primThreadAbort);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "current", primThreadCurrent);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "suspend()", primThreadSuspend);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield(_)", primThreadYieldWithArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield()", primThreadYieldWithoutArg);
    
    // 以下是实例方法
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "call()", primThreadCallWithoutArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "call(_)", primThreadCallWithArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "isDone", primThreadIsDone);

    // 绑定函数类
    vm->fnClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Fn"));
    PRIM_METHOD_BIND(vm->fnClass->objHeader.class, "new(_)", primFnNew);

    // 绑定call的重载方法
    bindFnOverloadCall(vm, "call()");
    bindFnOverloadCall(vm, "call(_)");
    bindFnOverloadCall(vm, "call(_,_)");
    bindFnOverloadCall(vm, "call(_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");

    // 绑定Null类的方法
    vm->nullClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Null"));
    PRIM_METHOD_BIND(vm->nullClass, "!", primNullNot);
    PRIM_METHOD_BIND(vm->nullClass, "toString", primNullToString);

    vm->numberClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Num"));
    // 类方法
    PRIM_METHOD_BIND(vm->numberClass, "fromString(_)", primNumFromString);
    PRIM_METHOD_BIND(vm->numberClass, "pi", primNumPi);
    // 实例方法
    PRIM_METHOD_BIND(vm->numberClass, "+(_)", primNumPlus);
    PRIM_METHOD_BIND(vm->numberClass, "-(_)", primNumMinus);
    PRIM_METHOD_BIND(vm->numberClass, "*(_)", primNumMul);
    PRIM_METHOD_BIND(vm->numberClass, "/(_)", primNumDiv);
    PRIM_METHOD_BIND(vm->numberClass, ">(_)", primNumGt);
    PRIM_METHOD_BIND(vm->numberClass, ">=(_)", primNumGe);
    PRIM_METHOD_BIND(vm->numberClass, "<(_)", primNumLt);
    PRIM_METHOD_BIND(vm->numberClass, "<=(_)", primNumLe);
    // 位运算
    PRIM_METHOD_BIND(vm->numberClass, "&(_)", primNumBitAnd);
    PRIM_METHOD_BIND(vm->numberClass, "|(_)", primNumBitOr);
    PRIM_METHOD_BIND(vm->numberClass, ">>(_)", primNumBitShiftRight);
    PRIM_METHOD_BIND(vm->numberClass, "<<(_)", primNumBitShiftLeft);
    // 以上通过rules infix_operator来解析

    // 下面大部分通过rules中对应的led(callEntry)来解析
    // 少数依然那是infix_operator解析
    PRIM_METHOD_BIND(vm->numberClass, "abs", primNumAbs);
    PRIM_METHOD_BIND(vm->numberClass, "acos", primNumAcos);
    PRIM_METHOD_BIND(vm->numberClass, "asin", primNumAsin);
    PRIM_METHOD_BIND(vm->numberClass, "atan", primNumAtan);
    PRIM_METHOD_BIND(vm->numberClass, "ceil", primNumCeil);
    PRIM_METHOD_BIND(vm->numberClass, "cos", primNumCos);
    PRIM_METHOD_BIND(vm->numberClass, "floor", primNumFloor);
    PRIM_METHOD_BIND(vm->numberClass, "-", primNumNegate);
    PRIM_METHOD_BIND(vm->numberClass, "sin", primNumSin);
    PRIM_METHOD_BIND(vm->numberClass, "sqrt", primNumSqrt);
    PRIM_METHOD_BIND(vm->numberClass, "tan", primNumTan);
    PRIM_METHOD_BIND(vm->numberClass, "%(_)", primNumMod);
    PRIM_METHOD_BIND(vm->numberClass, "~", primNumBitNot);
    PRIM_METHOD_BIND(vm->numberClass, "..(_)", primNumRange);
    PRIM_METHOD_BIND(vm->numberClass, "atan(_)", primNumAtan2);
    PRIM_METHOD_BIND(vm->numberClass, "fraction", primNumFraction);
    PRIM_METHOD_BIND(vm->numberClass, "isInfinity", primNumIsInfinity);
    PRIM_METHOD_BIND(vm->numberClass, "isInteger", primNumIsInteger);
    PRIM_METHOD_BIND(vm->numberClass, "isNan", primNumIsNan);
    PRIM_METHOD_BIND(vm->numberClass, "toString", primNumToString);
    PRIM_METHOD_BIND(vm->numberClass, "truncate", primNumTruncate);
    PRIM_METHOD_BIND(vm->numberClass, "==(_)", primNumEqual);
    PRIM_METHOD_BIND(vm->numberClass, "!=(_)", primNumNotEqual);
    
    // 字符串类
    vm->stringClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "String"));
    // 类方法
    PRIM_METHOD_BIND(vm->stringClass->objHeader.class, "fromCodePoint(_)", primStringFromCodePoint);
    // 实例方法
    PRIM_METHOD_BIND(vm->stringClass, "+(_)", primStringPlus);
    PRIM_METHOD_BIND(vm->stringClass, "[_]", primStringSubscript);
    PRIM_METHOD_BIND(vm->stringClass, "byteAt_(_)", primStringByteAt);
    PRIM_METHOD_BIND(vm->stringClass, "byteCount_", primStringByteCount);
    PRIM_METHOD_BIND(vm->stringClass, "codePointAt_(_)", primStringCodePointAt);
    PRIM_METHOD_BIND(vm->stringClass, "contains(_)", primStringContains);
    PRIM_METHOD_BIND(vm->stringClass, "endsWith(_)", primStringEndsWith);
    PRIM_METHOD_BIND(vm->stringClass, "indexOf(_)", primStringIndexOf);
    PRIM_METHOD_BIND(vm->stringClass, "iterate(_)", primStringIterate);
    PRIM_METHOD_BIND(vm->stringClass, "iterateByte_(_)", primStringIterateByte);
    PRIM_METHOD_BIND(vm->stringClass, "iterateValue_(_)", primStringIteratorValue);
    PRIM_METHOD_BIND(vm->stringClass, "startsWith(_)", primStringStartsWith);
    PRIM_METHOD_BIND(vm->stringClass, "toString", primStringToString);
    PRIM_METHOD_BIND(vm->stringClass, "count", primStringByteCount);

    // list类
    vm->listClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "List"));
    PRIM_METHOD_BIND(vm->listClass->objHeader.class, "new()", primListNew);
    PRIM_METHOD_BIND(vm->listClass, "[_]", primListSubscript);
    PRIM_METHOD_BIND(vm->listClass, "[_]=(_)", primListSubscriptSetter);
    PRIM_METHOD_BIND(vm->listClass, "add(_)", primListAdd);
    PRIM_METHOD_BIND(vm->listClass, "addCore_(_)", primListAddCore);
    PRIM_METHOD_BIND(vm->listClass, "clear()", primListClear);
    PRIM_METHOD_BIND(vm->listClass, "count()", primListCount);
    PRIM_METHOD_BIND(vm->listClass, "insert(_,_)", primListInsert);
    PRIM_METHOD_BIND(vm->listClass, "iterate(_)", primListIterate);
    PRIM_METHOD_BIND(vm->listClass, "iteratorValue(_)", primListIteratorValue);
    PRIM_METHOD_BIND(vm->listClass, "removeAt(_)", primListRemoveAt);

    // map
    vm->mapClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Map"));
    PRIM_METHOD_BIND(vm->mapClass->objHeader.class, "new()", primMapNew);
    PRIM_METHOD_BIND(vm->mapClass, "[_]", primMapSubscript);
    PRIM_METHOD_BIND(vm->mapClass, "[_]=(_)", primMapSubscriptSetter);
    PRIM_METHOD_BIND(vm->mapClass, "addCore_(_,_)", primMapAddCore);
    PRIM_METHOD_BIND(vm->mapClass, "clear()", primMapClear);
    PRIM_METHOD_BIND(vm->mapClass, "containsKey(_)", primMapContainsKey);
    PRIM_METHOD_BIND(vm->mapClass, "count", primMapCount);
    PRIM_METHOD_BIND(vm->mapClass, "remove(_)", primMapRemove);
    PRIM_METHOD_BIND(vm->mapClass, "iterate_(_)", primMapIterate);
    PRIM_METHOD_BIND(vm->mapClass, "keyIteratorValue_(_)", primMapKeyIteratorValue);
    PRIM_METHOD_BIND(vm->mapClass, "valueIteratorValue_(_)", primMapValueIteratorValue);
    
    // range 类
    vm->rangeClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Range"));
    PRIM_METHOD_BIND(vm->rangeClass, "from", primRangeFrom);
    PRIM_METHOD_BIND(vm->rangeClass, "to", primRangeTo);
    PRIM_METHOD_BIND(vm->rangeClass, "min", primRangeMin);
    PRIM_METHOD_BIND(vm->rangeClass, "max", primRangeMax);
    PRIM_METHOD_BIND(vm->rangeClass, "iterate(_)", primRangeIterate);
    PRIM_METHOD_BIND(vm->rangeClass, "iteratorValue(_)", primRangeIteratorValue);

    // system类
    Class* systemClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "System"));
    PRIM_METHOD_BIND(systemClass->objHeader.class, "clock", primSystemClock);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "gc()", primSystemGC);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "importModule(_)", primSystemImportModule);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "getModuleVariable(_,_)", primSystemGetModuleVariable);
    PRIM_METHOD_BIND(systemClass->objHeader.class, "writeString_(_)", primSystemWriteString);

    // 核心自举过程中穿件了很多ObjString对象,创建过程中调用initObjHeader初始化对象头
    // 使其class指向vm->stringClass,但那时vm->stringClass未初始化,现在更正
    ObjHeader* objHeader = vm->allObjects;
    while (objHeader != NULL) {
        if (objHeader->type == OT_STRING) {
            objHeader->class = vm->stringClass;
        }
        objHeader = objHeader->next;
    }
}

// 执行模块
VMResult executeModule(VM* vm, Value moduleName, const char* moduleCode) {
    ObjThread* objThread = loadModule(vm, moduleName, moduleCode);
    return executeInstruction(vm, objThread);
}