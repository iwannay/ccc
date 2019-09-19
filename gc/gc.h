#ifndef _GC_GC_H
#define _GC_GC_H
#include "vm.h"
void startGC(VM* vm);
void grayObject(VM* vm, ObjHeader* obj);
void freeObject(VM* vm, ObjHeader* obj);
void grayValue(VM* vm, Value value);
#endif