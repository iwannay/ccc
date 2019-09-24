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
    // 符号表中的存放的是所有类的所有方法
    // 查找流程为
    // 1. 生成类的方法的签名
    // 2. 根据签名从allMethodNames中查找其索引index(数组下标/指针偏移)
    // 3. 根据index从当前类中(class.methods)索引方法
    // class.methods中记录的方法
    // typedef struct method {
    //     MethodType type; // union 中的值由type的值决定
    //     union {
    //         // 指向脚本方法所关联的c实现
    //         Primitive primFn;
    //         // 指向脚本代码编译后的ObjClosure或ObjFn
    //         ObjClosure* obj;
    //     };
    // } Method;
    SymbolTable allMethodNames; 
    ObjMap* allModules;   // 记录源码中所有的模块
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
