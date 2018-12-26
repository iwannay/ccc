#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"
#include "header_obj.h"
#include "meta_obj.h"

typedef enum vmResult {
    VM_RESULT_SUCCESS,
    VM_RESULT_ERROR
} VMResult; // 虚拟机执行结果

struct vm {
    Class* classOfClass;
    Class* objectClass;
    Class* stringClass;
    Class* mapClass;
    Class* rangeClass;
    Class* listClass;
    Class* nullClass;
    Class* boolClass;
    Class* numberClass;
    Class* fnClass;
    Class* threadClass;
    uint32_t allocatedBytes; // 累计已分配的内存量
    Parser* curParser; // 当前词法分析器
    ObjHeader* allObjects; // 所有已分配的对象链表
    SymbolTable allMethodName; // 所有类的方法名
    ObjMap* allModules; 
    ObjThread* curThread; // 当前正在执行的线程
};

void initVM(VM* vm);
VM* newVM(void);
#endif
