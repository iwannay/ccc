#ifndef _OBJECT_FN_H
#define _OBJECT_FN_H

#include "utils.h"
#include "meta_obj.h"


typedef struct {
    char* fnName; // 函数名
    IntBuffer lineNo; // 行号
} FnDebug; // 在函数中的调试结构

typedef struct {
    ObjHeader objHeader;
    ByteBuffer instrStream; // 函数编译后的指令流
    ValueBuffer constants; // 函数中的常量表
    ObjModule* module; // 本函数所属的模块

    // 本函数最多需要的栈空间，是使用空间的峰值
    uint32_t maxStackSlotUsedNum;
    uint32_t upvalueNum; // 本函数所涵盖的upvalue数量
    uint8_t argNum; // 函数期望的参数个数

#if DEBUG
    FnDebug* debug;
#endif

} ObjFn; // 函数对象

typedef struct upvalue {
    ObjHeader objHeader;

    // 运行时栈是个Value类型的数组，localVarPtr 指向upvalue所关联的局部变量在运行时栈中的指针
    Value* localVarPtr;
    
    // 已被关闭的upvalue的指针
    Value closedUpvalue;

    struct upvalue* next; // 用以链接openUpvalue链表
} ObjUpvalue; // upvalue 对象

typedef struct {
    ObjHeader objHeader;
    ObjFn* fn; // 闭包中所要引用的函数
    ObjUpvalue* upvalues[0]; // 用于存储此函数的closed upvalue
} ObjClosure; // 闭包对象

typedef struct {
    uint8_t* ip; // 程序计数器 指向下一个被执行的指令

    // 在本frame中执行的闭包函数
    ObjClosure* closure;

    // iframe 是共享thread.stack
    // 此项用于指向本irame所在thread运行时栈的起始地址
    Value* stackStart;
} Frame; // 调用框架

#define INITIAL_FRAME_NUM 4

ObjUpvalue* newObjUpvalue(VM* vm, Value* localVarPtr);
ObjClosure* newObjClosure(VM* vm, ObjFn* objFn);
ObjFn* newObjFn(VM* vm, ObjModule* objModule, uint32_t maxStacksSlotUsedNum);

#endif