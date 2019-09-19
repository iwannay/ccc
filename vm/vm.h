#ifndef _VM_VM_H
#define _VM_VM_H
#include "common.h"
#include "class.h"
#include "obj_map.h"
#include "obj_thread.h"
#include "parser.h"


#define MAX_TEMP_ROOTS_NUM 8
#define OPCODE_SLOTS(opcode, effect) OPCODE_##opcode,
typedef enum {
    #include "opcode.inc"
} OpCode;
#undef OPCODE_SLOTS

typedef enum vmResult {
    VM_RESULT_SUCCESS,
    VM_RESULT_ERROR
} VMResult; // 虚拟机执行结果

typedef struct {
    ObjHeader** grayObjects;
    uint32_t capacity;
    uint32_t count;
} Gray;

typedef struct {
    // 堆生长因子
    int heapGrowthFactor;
    // 初始堆栈大小默认10M
    uint32_t initialHeapSize;
    // 最小堆大小,默认1M
    uint32_t minHeapSize;
    // 第一次触发gc的堆大小,默认为initialHeapSize
    uint32_t nextGC;
} Configuration;

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
    SymbolTable allMethodNames; // 所有类的方法名
    ObjMap* allModules; 
    ObjThread* curThread; // 当前正在执行的线程

    // 临时的根对象集合,存储被gc保留的对象,避免回收
    ObjHeader* tmpRoots[MAX_TEMP_ROOTS_NUM];
    uint32_t tmpRootNum;
    Gray grays;
    Configuration config;
};

void initVM(VM* vm);
VMResult executeInstruction(VM* vm, register ObjThread* curThread);
void ensureStack(VM* vm, ObjThread* objThread, uint32_t neededSots);
void pushTmpRoot(VM* vm, ObjHeader* obj);
void popTmpRoot(VM* vm);
void freeVM(VM* vm);
VM* newVM(void);
#endif
