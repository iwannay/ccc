#ifndef _OBJECT_METAOBJ_H
#define _OBJECT_METAOBJ_H

#include "obj_string.h"
typedef struct {
    ObjHeader objHeader;
    // 具体的字段
    Value fields[0];
} ObjInstance; // 对象实例

ObjModule* newObjModule(VM* vm, const char* modName);
ObjInstance* newObjInstance(VM* vm, Class* class);


#endif