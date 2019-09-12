#include "vm.h"
#include <stdlib.h>
#include <utils.h>
#include "core.h"
#include "compiler.h"
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

// 校验基类合法性
static void validateSuperClass(VM* vm, Value classNameValue, uint32_t fieldNum, Value superClassValue) {
    if (!VALUE_IS_CLASS(superClassValue)) {
        ObjString* classNameString = VALUE_TO_OBJSTR(classNameValue);
        RUN_ERROR("class '%s' 's superClass is not a valid class!", classNameString->value.start);
    }
    Class* superClass = VALUE_TO_CLASS(superClassValue);
    // 基类不允许为内建类
    if (superClass == vm->stringClass ||
        superClass == vm->mapClass ||
        superClass == vm->rangeClass ||
        superClass == vm->listClass ||
        superClass == vm->nullClass ||
        superClass == vm->boolClass ||
        superClass == vm->numberClass ||
        superClass == vm->fnClass ||
        superClass == vm->threadClass) {
        RUN_ERROR("superClass mustn't be a buildin class!");
    }
    if (superClass->fieldNum+fieldNum > MAX_FIELD_NUM) {
        RUN_ERROR("number of field including super exceed %d!", MAX_FIELD_NUM);
    }
}

// 修正操作数
static void patchOperand(Class* class, ObjFn* fn) {
    int ip = 0;
    OpCode opCode;
    while (true) {
        opCode = (OpCode)fn->instrStream.datas[ip++];
        switch (opCode) {
            case OPCODE_LOAD_FIELD:
            case OPCODE_STORE_FIELD:
            case OPCODE_LOAD_THIS_FIELD:
            case OPCODE_STORE_THIS_FIELD:
                // 修正子类的field数目,参数是1字节
                fn->instrStream.datas[ip++] += class->superClass->fieldNum;
                break;
            case OPCODE_SUPER0:
            case OPCODE_SUPER1:
            case OPCODE_SUPER2:
            case OPCODE_SUPER3:
            case OPCODE_SUPER4:
            case OPCODE_SUPER5:
            case OPCODE_SUPER6:
            case OPCODE_SUPER7:
            case OPCODE_SUPER8:
            case OPCODE_SUPER9:
            case OPCODE_SUPER10:
            case OPCODE_SUPER11:
            case OPCODE_SUPER12:
            case OPCODE_SUPER13:
            case OPCODE_SUPER14:
            case OPCODE_SUPER15:
            case OPCODE_SUPER16:{
                // 指令1: 2字节的method索引
                // 指令2: 2字节的基类常量索引
                ip += 2; // 跳过两字节的method索引
                uint32_t superClassIdx = (fn->instrStream.datas[ip] << 8) | fn->instrStream.datas[ip+1];
                // 回填在函数emitCallBySignature中的占位VT_TO_VALUE(VT_NULL)
                fn->constants.datas[superClassIdx] = OBJ_TO_VALUE(class->superClass);
                ip +=2 ;// 跳过基类索引
                break;
            }
            case OPCODE_CREATE_CLOSURE:{
                // 指令流: 2字节待创建闭包的函数在常量表中的索引+函数所用的upvalue数*2
                // 函数是存储到常量表中,获取待创建闭包的函数在常量表中的索引
                uint32_t fnIdx = (fn->instrStream.datas[ip]<<8) | fn->instrStream.datas[ip+1];
                // 递归进入该函数的指令流,继续为其中的super和field修正操作数
                patchOperand(class, VALUE_TO_OBJFN(fn->constants.datas[fnIdx]));
                // ip-1 返回OPCODE_CREATE_CLOSURE指令
                ip += getBytesOfOperands(fn->instrStream.datas, fn->constants.datas, ip-1);
                break;
            }
            case OPCODE_END:
                return;
            default:
                ip += getBytesOfOperands(fn->instrStream.datas, fn->constants.datas, ip-1);
                break;
        }
    }
}

// 绑定方法和修正操作数
static void bindMethodAndPatch(VM* vm, OpCode opCode, uint32_t methodIndex, Class* class, Value methodValue) {
    if (opCode == OPCODE_STATIC_MEHOD) {
        class = class->objHeader.class;
    }
    Method method;
    method.type = MT_SCRIPT;
    method.obj = VALUE_TO_OBJCLOSURE(methodValue);
    patchOperand(class, method.obj->fn);
    bindMethod(vm, class, methodIndex, method);
}
// 执行指令
VMResult executeInstruction(VM* vm, register ObjThread* curThread) {
    vm->curThread = curThread;
    register Frame* curFrame;
    register Value* stackStart;
    register uint8_t* ip;
    register ObjFn* fn;
    OpCode opCode;

    // 定义操作运行时栈的宏
    #define PUSH(value) (*curThread->esp++ = value) // 压栈
    #define POP() (*(--curThread->esp)) // 出栈
    #define DROP() (curThread->esp--)
    #define PEEK() (*(curThread->esp-1))    // 获得栈顶数据
    #define PEEK2() (*(curThread->esp-2))   // 获得次栈顶数据
    // 读取objFn.instrStream.datas中的数据
    #define READ_BYTE() (*ip++) // 从指令流中读取一字节
    // 读取两字节
    #define READ_SHORT() (ip+=2,(uint16_t)((ip[-2])<<8 | ip[-1]))
    
    #define STORE_CUR_FRAME() curFrame->ip = ip // 备份ip以便返回

    // 加载最新的frame
    #define LOAD_CUR_FRAME() \
        curFrame = &curThread->frames[curThread->usedFrameNum-1];   \
        stackStart = curFrame->stackStart;  \
        ip = curFrame->ip;  \
        fn = curFrame->closure->fn;
    #define DECODE loopStart:   \
        opCode = READ_BYTE();   \
        switch (opCode);
    #define CASE(shortOpCode) case OPCODE_##shortOpCode
    #define LOOP() goto loopStart

    LOAD_CUR_FRAME();
    DECODE {
        case(LOAD_LOCAL_VAR):
            PUSH(stackStart[READ_BYTE()]);
            LOOP();
        CASE(LOAD_THIS_FIELD):{
            uint8_t fieldIdx = READ_BYTE();
            // stackStart[0]是实例对象this
            ASSERT(VALUE_IS_OBJINSTANCE(stackStart[0]), "method receiver should be objInstance!");
            ObjInstance* ObjInstance = VALUE_TO_OBJINSTANCE(stackStart[0]);
            ASSERT(fieldIdx < ObjInstance->objHeader.class->fieldNum, "out of bounds field!");
            PUSH(ObjInstance->fields[fieldIdx]);
            LOOP();
        }
        CASE(POP):
            DROP();
            LOOP();
        CASE(PUSH_NULL):
            PUSH(VT_TO_VALUE(VT_NULL));
            LOOP();
        CASE(PUSH_FALSE):
            PUSH(VT_TO_VALUE(VT_FALSE));
            LOOP();
        CASE(PUSH_TRUE):
            PUSH(VT_TO_VALUE(VT_TRUE));
            LOOP();
        CASE(STORE_LOCAL_VAR):
            // 栈顶: 局部变量值
            // 指令流: 1 字节局部变量索引
            stackStart[READ_BYTE()] = PEEK();
            LOOP();
        CASE(LOAD_CONSTANT):
            // 指令流: 2字节的常量索引
            PUSH(fn->constants.datas[READ_SHORT()]);
            LOOP();
        {
            int argNum, index;
            Value* args;
            Class* class;
            Method* method;

            CASE(CALL0):
            CASE(CALL1):
            CASE(CALL2):
            CASE(CALL3):
            CASE(CALL4):
            CASE(CALL5):
            CASE(CALL6):
            CASE(CALL7):
            CASE(CALL8):
            CASE(CALL9):
            CASE(CALL10):
            CASE(CALL11):
            CASE(CALL12):
            CASE(CALL13):
            CASE(CALL14):
            CASE(CALL15):
            CASE(CALL16):
                // 指令流: 2字节的method索引
                // 因为还有隐式的receiver(就是下面的args[0]),所以参数个数+1
                argNum = opCode-OPCODE_CALL0+1
                index = READ_SHORT(); // 方法名索引
                args = curThread->esp - argNum;
                // 获得方法所在的类
                class = getClassOfObj(vm, args[0]);
                goto invokeMethod;
            CASE(SUPER0):
            CASE(SUPER1):
            CASE(SUPER2):
            CASE(SUPER3):
            CASE(SUPER4):
            CASE(SUPER5):
            CASE(SUPER6):
            CASE(SUPER7):
            CASE(SUPER8):
            CASE(SUPER9):
            CASE(SUPER10):
            CASE(SUPER11):
            CASE(SUPER12):
            CASE(SUPER13):
            CASE(SUPER14):
            CASE(SUPER15):
            CASE(SUPER16):
        }
    }

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