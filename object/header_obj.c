#include "header_obj.h"
#include "class.h"
#include "vm.h"
#include "common.h"
#include "utils.h"

DEFINE_BUFFER_METHOD(Value)

// 初始化对象头
void initObjHeader(VM* vm, ObjHeader* objHeader, ObjType objType, Class* class) {
    objHeader->type = objType;
    objHeader->isDark = false;
    objHeader->class = class;
    objHeader->next = vm->allObjects;
    vm->allObjects = objHeader;
}