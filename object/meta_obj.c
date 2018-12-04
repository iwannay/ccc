#include "obj_fn.h"
#include "class.h"
#include "vm.h"
#include <string.h>

// 新建模块
ObjModule* newObjModule(VM* vm, const char* modName) {
    ObjModule* objModule = ALLOCATE(vm, ObjModule);
    if (objModule == NULL) {
        MEM_ERROR("allocate ObjModule failed!");
    }

    // ObjModule 是元信息对象，不属于任何一个类
    initObjHeader(vm, &objModule->objHeader, OT_MODULE, NULL);

    StringBufferInit(&objModule->moduleVarName);
    ValueBufferInit(&objMOdule->moduleVarValue);

    objModule->name = NULL // 核心模块名为NULL
    if (modName != NULL) {
        objModule->name = newObjString(vm, modName, strlen(modName));
    }
    return objModule;
}