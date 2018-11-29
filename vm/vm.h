#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"
#include "header_obj.h"

struct vm {
    Class* stringClass;
    uint32_t allocatedBytes; // 累计已分配的内存量
    Parser* curParser; // 当前词法分析器
    ObjHeader* allObjects; // 所有已分配的对象链表
};

void initVM(VM* vm);
VM* newVM(void);
#endif
