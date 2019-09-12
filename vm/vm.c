#include "vm.h"
#include <stdlib.h>
#include <utils.h>
#include "core.h"

// 确保stack有效
void ensureStack(VM* vm, ObjThread* objThread, uint32_t neededSots) {
    if (objThread->stackCapacity >= neededSots) {
        return;
    }
    uint32_t newStackCapacity = ceilToPowerOf2(neededSots);
    ASSERT(newStackCapacity > objThread->stackCapacity, "newStackCapacity error!");
    Value* oldStackBottom = objThread->stack;
    uint32_t slotSize = sizeof(Value);
    objThread->stack = (Value*)memManager(vm, objThread->stack, objThread->stackCapacity*slotSize, newStackCapacity*slotSize);
    objThread->stackCapacity = newStackCapacity;

    long offset = objThread->stack - oldStackBottom;
    
    // 说明os无法满足内存需求,重新分配了起始地址
    // stack是共享的,所以需要同步修正偏移
    if (offset != 0) {
        uint32_t idx = 0;
        while(idx < objThread->usedFrameNum) {
            objThread->frames[idx++].stackStart += offset;
        }

        // 调整openvalue
        ObjUpvalue* upvalue = objThread->openUpvalues;
        while (upvalue != NULL) {
            upvalue->localVarPtr += offset;
            upvalue = upvalue->next;
        }
        // 更新栈顶
        objThread->esp += offset;
    }
}

// 为objClosure在objThread中创建运行时栈
inline static void createFrame(VM* vm, ObjThread* objThread, ObjClosure* ObjClosure, int argNum) {
    if (objThread->usedFrameNum+1 > objThread->frameCapacity) {
        uint32_t newCapacity = objThread->frameCapacity*2;
        uint32_t frameSize = sizeof(Frame);
        objThread->frames = (Frame*)memManager(vm, objThread->frames, frameSize*objThread->frameCapacity, frameSize*newCapacity);
        objThread->frameCapacity = newCapacity;
    }
    // 栈大小等于栈顶-栈底
    uint32_t stackSlots = (uint32_t)(objThread->esp - objThread->stack);
    // 总共需要的栈大小
    uint32_t neededSlots = stackSlots+ObjClosure->fn->maxStackSlotUsedNum;
    ensureStack(vm, objThread, neededSlots);
    // 准备上cpu
    prepareFrame(objThread, ObjClosure, objThread->esp-argNum);
}

// 关闭在栈中slot为lastSlot及之上的upvalue
// 关闭即upvalue.localVarPtr修改指针地址由运行时栈,改为自身upvalue.closedUpvalue
static void closedUpvalue(ObjThread* objThread, Value* lastSlot) {
    ObjUpvalue* upvalue = objThread->openUpvalues;
    while (upvalue != NULL && upvalue->localVarPtr >= lastSlot) {
        upvalue->closedUpvalue = *(upvalue->localVarPtr);
        // 关闭后把指向运行时栈的指针改为指向自身的closedUpvalue
        upvalue->localVarPtr = &(upvalue->closedUpvalue);
        upvalue = upvalue->next;
    }
    objThread->openUpvalues = upvalue;
}

// 创建线程已打开的upvalue链表,并将localVarPtr所属的upvalue以降序插入到该链表
static ObjUpvalue* createOpenUpvalue(VM* vm, ObjThread* objThread, Value* localVarPtr) {
    // 如果openUpvalues链表为空就创建
    if (objThread->openUpvalues == NULL) {
        objThread->openUpvalues = newObjUpvalue(vm, localVarPtr);
        return objThread->openUpvalues;
    }
    // 下面已以upvalue.localVarPtr降序组织openUpvalues
    ObjUpvalue*  preUpvalue = NULL;
    ObjUpvalue* upvalue = objThread->openUpvalues;

    // 保证降序
    while(upvalue != NULL && upvalue->localVarPtr > localVarPtr) {
        preUpvalue = upvalue;
        upvalue = upvalue->next;
    }
    if (upvalue != NULL && upvalue->localVarPtr == localVarPtr) {
        return upvalue;
    }
    ObjUpvalue* newUpvalue = newObjUpvalue(vm, localVarPtr);
    if (preUpvalue == NULL) {
        objThread->openUpvalues = newUpvalue;
    } else {
        preUpvalue->next = newUpvalue;
    }
    newUpvalue->next = upvalue;
    return newUpvalue;
}

// 初始化虚拟机
void initVM(VM* vm) {
    vm->allocatedBytes = 0;
    vm->allObjects = NULL;
    vm->curParser = NULL;
    StringBufferInit(&vm->allMethodNames);
    vm->allModules = newObjMap(vm);
}

VM* newVM() {
    VM* vm = (VM*)malloc(sizeof(VM));
    if (vm == NULL) {
        MEM_ERROR("allocate VM failed!");
    }
    initVM(vm);
    buildCore(vm);
    return vm;
}