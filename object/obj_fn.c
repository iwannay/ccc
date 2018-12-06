#include "meta_obj.h"
#include "class.h"
#include "vm.h"

// 创建一个空函数
ObjFn* newObjFn(VM* vm, ObjModule* objModule, uint32_t slotNum) {
    ObjFn* objFn = ALLOCATE(vm, ObjFn);
    if (objFn == NULL) {
        MEM_ERROR("allocate Objfn failed!");
    }

    initObjHeader(vm, &objFn->objHeader, OT_FUNCTION, vm->fnClass);
    ByteBufferInit(&objFn->instrStream);
    ValueBufferInit(&objFn->constants);
    objFn->module = objModule;
    objFn->maxStackSlotUsedNum = slotNum;
    objFn->upvalueNum = objFn->argNum = 0;

#ifdef DEBUG
    objFn->debug = ALLOCATE(vm, FnDebug);
    objFn->debug->fnName = NULL;
    IntBufferInit(&objFn->debug->lineNo);
#endif
    return objFn;
}




