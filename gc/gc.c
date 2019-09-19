#include "gc.h"
#include "compiler.h"
#include "obj_list.h"
#include "obj_range.h"
#include "utils.h"
#if DEBUG
    #include "debug.h"
    #include <time.h>
#endif

void grayObject(VM* vm, ObjHeader* obj) {
    if (obj == NULL || obj->isDark) return;
    obj->isDark = true;
    if (vm->grays.count >= vm->grays.capacity) {
        vm->grays.capacity = vm->grays.count * 2;
        vm->grays.grayObjects = (ObjHeader**)realloc(vm->grays.grayObjects, vm->grays.capacity * sizeof(ObjHeader*));
    }
    vm->grays.grayObjects[vm->grays.count++] = obj;
}

// 标灰
void grayValue(VM* vm, Value value) {
    if (!VALUE_IS_OBJ(value)) {
        return;
    }
    grayObject(vm, VALUE_TO_OBJ(value));
}

// 标灰buffer->datas中的value
static void grayBuffer(VM* vm , ValueBuffer* buffer) {
    uint32_t idx = 0;
    while (idx < buffer->count) {
        grayValue(vm, buffer->datas[idx]);
        idx++;
    }
}

// 标黑class
static void blackClass(VM* vm, Class* class) {
    // 标灰meta类
    grayObject(vm, (ObjHeader*)class->objHeader.class);
    // 标灰父类
    grayObject(vm, (ObjHeader*)class->superClass);
    // 标灰方法
    uint32_t idx = 0;
    while (idx < class->methods.count) {
        if (class->methods.datas[idx].type == MT_SCRIPT) {
            grayObject(vm, (ObjHeader*)class->methods.datas[idx].obj);
        }
        idx++;
    }
    // 标灰类名
    grayObject(vm, (ObjHeader*)class->name);
    // 累计类大小
    vm->allocatedBytes += sizeof(Class);
    vm->allocatedBytes += sizeof(Method) * class->methods.capacity;
}

// 标灰闭包
static void blackClosure(VM* vm, ObjClosure* objClosure) {
    // 标灰闭包中的函数
    grayObject(vm, (ObjHeader*)objClosure->fn);
    // 标灰包中的upvalue
    uint32_t idx = 0;
    while (idx < objClosure->fn->upvalueNum) {
        grayObject(vm, (ObjHeader*)objClosure->upvalues[idx]);
        idx++;
    }
    // 累计闭包大小
    vm->allocatedBytes += sizeof(ObjClosure);
    vm->allocatedBytes += sizeof(ObjUpvalue*) * objClosure->fn->upvalueNum;
}

// 标黑objThread
static void blackThread(VM* vm, ObjThread* objThread) {
    // 标灰frame
    uint32_t idx = 0;
    while (idx < objThread->usedFrameNum) {
        grayObject(vm, (ObjHeader*)objThread->frames[idx].closure);
        idx++;
    }
    // 标灰运行时栈中每个slot
    Value* slot = objThread->stack;
    while (slot < objThread->esp) {
        grayValue(vm, *slot);
        slot++;
    }
    // 标灰本线程中所有的upvalue
    ObjUpvalue* upvalue = objThread->openUpvalues;
    while (upvalue != NULL) {
        grayObject(vm, (ObjHeader*)upvalue);
        upvalue = upvalue->next;
    }
    // 标灰caller
    grayObject(vm, (ObjHeader*)objThread->caller);
    grayValue(vm, objThread->errorObj);
    // 累计线程大小
    vm->allocatedBytes += sizeof(ObjThread);
    vm->allocatedBytes += objThread->frameCapacity *sizeof(Frame);
    vm->allocatedBytes += objThread->stackCapacity * sizeof(Value);
}
// 标黑fn
static void blackFn(VM* vm, ObjFn* fn) {
    // 标灰常量
    grayBuffer(vm, &fn->constants);
    // 累计Objfn的空间
    vm->allocatedBytes += sizeof(ObjFn);
    vm->allocatedBytes += sizeof(uint8_t) * fn->instrStream.capacity;
    vm->allocatedBytes += sizeof(Value) * fn->constants.capacity;
#if DEBUG
    VM->allocatedBytes += sizeof(Int) * fn->instrStream.capacity;
#endif
}

// 标黑objInstance
static void blackInstance(VM* vm, ObjInstance* objInstance) {
    // 标灰元类
    grayObject(vm, (ObjHeader*)objInstance->objHeader.class);
    // 标记实例中所有的域
    uint32_t idx = 0;
    while (idx < objInstance->objHeader.class->fieldNum) {
        grayValue(vm, objInstance->fields[idx]);
        idx++;
    }
    vm->allocatedBytes += sizeof(ObjInstance);
    vm->allocatedBytes += sizeof(Value) * objInstance->objHeader.class->fieldNum;
}

// 标黑objList
static void blackList(VM* vm, ObjList* objList) {
    grayBuffer(vm, &objList->elements);
    vm->allocatedBytes += sizeof(ObjList);
    vm->allocatedBytes += sizeof(Value) * objList->elements.capacity;
}

// 标黑objMap
static void blackMap(VM* vm, ObjMap* objMap) {
    uint32_t idx = 0;
    while (idx < objMap->capacity) {
        Entry* entry = &objMap->entries[idx];
        if (!VALUE_IS_UNDEFINED(entry->key)) {
            grayValue(vm, entry->key);
            grayValue(vm, entry->value);
        }
        idx++;
    }
    vm->allocatedBytes += sizeof(ObjMap);
    vm->allocatedBytes += sizeof(Entry) * objMap->capacity;
}

// 标黑objModule
static void blackModule(VM* vm, ObjModule* objModule) {
    uint32_t idx = 0;
    while (idx < objModule->moduleVarValue.count) {
        grayValue(vm, objModule->moduleVarValue.datas[idx]);
        idx++;
    }
    // 标记模块名
    grayObject(vm, (ObjHeader*)objModule->name);
    
    vm->allocatedBytes += sizeof(ObjModule);
    vm->allocatedBytes += sizeof(String) * objModule->moduleVarName.capacity;
    vm->allocatedBytes += sizeof(Value) * objModule->moduleVarValue.capacity;
}

static void blackRange(VM* vm) {
    vm->allocatedBytes += sizeof(ObjRange);
}

static void blackString(VM* vm, ObjString* objString) {
    vm->allocatedBytes += sizeof(ObjString) + objString->value.length + 1;
}

static void blackUpvalue(VM* vm, ObjUpvalue* objUpvalue) {
    grayValue(vm, objUpvalue->closedUpvalue);
    vm->allocatedBytes += sizeof(ObjUpvalue);
}

static void blackObject(VM* vm, ObjHeader* obj) {
#ifdef DEBUG
    printf("mark ");
    dumpValue(OBJ_TO_VALUE(obj));
    printf(" @ %p\n", obj);
#endif
    switch (obj->type) {
        case OT_CLASS:
            blackClass(vm, (Class*)obj);
            break;
        case OT_CLOSURE:
            blackClosure(vm, (ObjClosure*)obj);
            break;
        case OT_THREAD:
            blackThread(vm, (ObjThread*)obj);
            break;
        case OT_FUNCTION:
            blackFn(vm, (ObjFn*)obj);
            break;
        case OT_INSTANCE:
            blackInstance(vm, (ObjInstance*)obj);
            break;
        case OT_LIST:
            blackList(vm, (ObjList*)obj);
            break;
        case OT_MAP:
            blackMap(vm, (ObjMap*)obj);
            break;
        case OT_MODULE:
            blackModule(vm, (ObjModule*)obj);
            break;
        case OT_RANGE:
            blackRange(vm);
            break;
        case OT_STRING:
            blackString(vm, (ObjString*)obj);
            break;
        case OT_UPVALUE:
            blackUpvalue(vm, (ObjUpvalue*)obj);
            break;
    }
}

// 标黑已经标灰的对象
static void blackObjectInGray(VM* vm) {
    while (vm->grays.count > 0) {
        ObjHeader* objHeader = vm->grays.grayObjects[--vm->grays.count];
        blackObject(vm, objHeader);
    }
}

// 释放obj自身及其占用的内存
void freeObject(VM* vm, ObjHeader* obj) {
#ifdef DEBUG
    printf("free");
    dumpValue(OBJ_TO_VALUE(obj));
    printf(" @ %p\n", obj);
#endif
    switch (obj->type) {
        case OT_CLASS:
            MethodBufferClear(vm, &((Class*)obj)->methods);
            break;
        case OT_THREAD: {
            ObjThread* objThread = (ObjThread*)obj;
            DEALLOCATE(vm, objThread->frames);
            DEALLOCATE(vm, objThread->stack);
            break;
        }
        case OT_FUNCTION:{
            ObjFn* fn = (ObjFn*)obj;
            ValueBufferClear(vm, &fn->constants);
            ByteBufferClear(vm, &fn->instrStream);
        #if DEBUG
            IntBufferClear(vm, &fn->debug->lineNo);
            DEALLOCATE(vm, fn->debug->fnName);
            DEALLOCATE(vm, fn->debug);
        #endif
            break;
        }
        case OT_LIST:
            ValueBufferClear(vm, &((ObjList*)obj)->elements);
            break;
        case OT_MAP:
            DEALLOCATE(vm, ((ObjMap*)obj)->entries);
            break;
        case OT_MODULE:
            StringBufferClear(vm, &((ObjModule*)obj)->moduleVarName);
            ValueBufferClear(vm, &((ObjModule*)obj)->moduleVarValue);
            break;
        case OT_STRING:
        case OT_RANGE:
        case OT_CLOSURE:
        case OT_INSTANCE:
        case OT_UPVALUE:
            break;
    }
    DEALLOCATE(vm, obj);
}

void startGC(VM* vm) {
#ifdef DEBUG
    double startTime = (double)clock() / CLOCKS_PER_SEC;
    uint32_t before = vm->allocatedBytes;
    printf("-- gc before:%d  nextGC:%d vm:%p  --\n", before, vm->config.nextGC, vm);
#endif
    vm->allocatedBytes = 0;
    grayObject(vm, (ObjHeader*)vm->allModules);

    uint32_t idx = 0;
    while (idx < vm->tmpRootNum) {
        grayObject(vm, vm->tmpRoots[idx]);
        idx++;
    }
    
    grayObject(vm, (ObjHeader*)vm->curThread);

    // 编译过层中若申请的内存过高就标灰编译单元
    if (vm->curParser != NULL) {
        ASSERT(vm->curParser->curCompileUnit != NULL, "grayCompileUint only be called while compiling!");
        grayCompileUnit(vm, vm->curParser->curCompileUnit);
    }

    blackObjectInGray(vm);
    // 回收白色对象
    ObjHeader** obj = &vm->allObjects;
    while (*obj != NULL) {
        if (!((*obj)->isDark)) {
            ObjHeader* unreached = *obj;
            *obj = unreached->next;
            freeObject(vm, unreached);
        } else {
            // 为下一次gc重新判定,恢复为未标记状态
            (*obj)->isDark = false;
            obj = &(*obj)->next;
        }
    }
    vm->config.nextGC = vm->allocatedBytes * vm->config.heapGrowthFactor;
    if (vm->config.nextGC < vm->config.minHeapSize) {
        vm->config.nextGC = vm->config.minHeapSize;
    }
#ifdef DEBUG
    double elapsed = ((double)clock() / CLOCKS_PER_SEC) - startTime;
    printf("GC %lu before %lu after (%lu collected), next at %lu. take %.3fs.\n",
        (unsigned long)before,
        (unsigned long)vm->allocatedBytes,
        (unsigned long)(before - vm->allocatedBytes),
        (unsigned long)vm->config.nextGC,elapsed);
#endif
}
