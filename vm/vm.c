#include "vm.h"
#include <stdlib.h>
#include "utils.h"
#include "core.h"
#include "compiler.h"
#include "gc.h"
#ifdef DEBUG
    #include "debug.h"
#endif


void pushTmpRoot(VM* vm, ObjHeader* obj) {
    ASSERT(obj!= NULL, "root obj is null");
    ASSERT(vm->tmpRootNum < MAX_TEMP_ROOTS_NUM, "temporary roots too much!");
    vm->tmpRoots[vm->tmpRootNum++] = obj;
}

void popTmpRoot(VM* vm) {
    ASSERT(vm->tmpRootNum < MAX_TEMP_ROOTS_NUM, "temporary roots too much!");
    vm->tmpRootNum--;
}

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
    if (opCode == OPCODE_STATIC_METHOD) {
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
    #define LOAD_CUR_FRAME()\
        curFrame = &curThread->frames[curThread->usedFrameNum-1];\
        stackStart = curFrame->stackStart;\
        ip = curFrame->ip;\
        fn = curFrame->closure->fn;
    
    #define DECODE loopStart:\
        opCode = READ_BYTE();\
        switch (opCode)
    #define CASE(shortOpCode) case OPCODE_##shortOpCode
    #define LOOP() goto loopStart

    LOAD_CUR_FRAME();
    DECODE {
        CASE(LOAD_LOCAL_VAR):
            PUSH(stackStart[READ_BYTE()]);
            LOOP();
        CASE(LOAD_THIS_FIELD):{
            uint8_t fieldIdx = READ_BYTE();
            // stackStart[0]是实例对象this
            ASSERT(VALUE_IS_OBJINSTANCE(stackStart[0]), "method receiver should be objInstance!");
            ObjInstance* objInstance = VALUE_TO_OBJINSTANCE(stackStart[0]);
            ASSERT(fieldIdx < objInstance->objHeader.class->fieldNum, "out of bounds field!");
            PUSH(objInstance->fields[fieldIdx]);
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
                argNum = opCode-OPCODE_CALL0+1;
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
                // 指令流1: 2字节的method索引
                // 指令流2：2字节的基类常量索引
                // 因为还有隐式的receiver(就是下面的args[0]),所以参数个数+1
                argNum = opCode - OPCODE_SUPER0 + 1;
                index = READ_SHORT();
                args = curThread->esp-argNum;
                class = VALUE_TO_CLASS(fn->constants.datas[READ_SHORT()]);
            invokeMethod:
                if ((uint32_t)index > class->methods.count || (method = &class->methods.datas[index])->type == MT_NONE) {
                    RUN_ERROR("method '%s' not found!", vm->allMethodNames.datas[index].str);
                }
                switch (method->type) {
                    case MT_PRIMITIVE:
                        if (method->primFn(vm, args)) { // 如果返回值为true，则进行空间回收
                            // argNum-1是为了保留args[0],args[0]是返回值，由主调方接收
                            curThread->esp -= argNum-1;
                        } else {
                            // 有两种情况
                            // 1. 出错
                            // 2. 切换了线程(vm->curThread)
                            STORE_CUR_FRAME();
                            if (vm->curThread == NULL) {
                                return VM_RESULT_SUCCESS;
                            }
                            curThread = vm->curThread;
                            LOAD_CUR_FRAME();
                            if (!VALUE_IS_NULL(curThread->errorObj)) {
                                if (VALUE_IS_OBJSTR(curThread->errorObj)) {
                                    ObjString* err = VALUE_TO_OBJSTR(curThread->errorObj);
                                    printf("%s", err->value.start);
                                }
                                PEEK() = VT_TO_VALUE(VT_NULL);
                            }

                            if (vm->curThread == NULL) {
                                return VM_RESULT_SUCCESS;
                            }
                            curThread = vm->curThread;
                            LOAD_CUR_FRAME();

                        }
                        break;
                    case MT_SCRIPT:
                        STORE_CUR_FRAME();
                        createFrame(vm, curThread, (ObjClosure*)method->obj, argNum);
                        LOAD_CUR_FRAME();
                        break;
                    case MT_FN_CALL:
                        ASSERT(VALUE_IS_OBJCLOSURE(args[0]), "instance must be a closure!");
                        ObjFn* objFn = VALUE_TO_OBJCLOSURE(args[0])->fn;
                        
                        if (argNum - 1 < objFn->argNum) {
                            RUN_ERROR("arguments less");
                        }
                        STORE_CUR_FRAME();
                        createFrame(vm, curThread, VALUE_TO_OBJCLOSURE(args[0]), argNum);
                        LOAD_CUR_FRAME();
                        break;
                    
                    default:
                        NOT_REACHED();
                }
                LOOP();
        }
        CASE(LOAD_UPVALUE):
            PUSH(*((curFrame->closure->upvalues[READ_BYTE()])->localVarPtr));
            LOOP();
        CASE(STORE_UPVALUE):
            // 栈顶：upvalue值
            // 指令流： 1字节的upvalue索引
            *(curFrame->closure->upvalues[READ_BYTE()]->localVarPtr) = PEEK();
            LOOP();
        CASE(LOAD_MODULE_VAR):
            // 指令流：两字节的模块变量索引
            PUSH(fn->module->moduleVarValue.datas[READ_SHORT()]);
            LOOP();
        CASE(STORE_MODULE_VAR):
            fn->module->moduleVarValue.datas[READ_SHORT()] = PEEK();
            LOOP();
        CASE(STORE_THIS_FIELD):{
            // 栈顶： field值
            // 指令流：1字节的field索引
            uint8_t fieldIdx = READ_BYTE();
            ASSERT(VALUE_IS_OBJINSTANCE(stackStart[0]), "receiver should be instance!");
            ObjInstance* objInstance = VALUE_TO_OBJINSTANCE(stackStart[0]);
            ASSERT(fieldIdx < objInstance->objHeader.class->fieldNum, "out of bounds field!");
            objInstance->fields[fieldIdx] = PEEK();
            LOOP();
        }
        CASE(LOAD_FIELD):{
            // 栈顶：实例对象
            // 指令流：1字节的field索引
            uint8_t fieldIdx = READ_BYTE(); // field index
            Value receiver = POP();
            ASSERT(VALUE_IS_OBJINSTANCE(receiver), "receiver should be instance!");
            ObjInstance* objInstance = VALUE_TO_OBJINSTANCE(receiver);
            ASSERT(fieldIdx < objInstance->objHeader.class->fieldNum, "out of bounds field!");
            PUSH(objInstance->fields[fieldIdx]);
            LOOP();
        }
        CASE(STORE_FIELD):{
            // 栈顶：实例对象,次栈顶：field值
            // 指令流：1字节的field索引
            uint8_t fieldIdx = READ_BYTE();
            Value receiver = POP();
            ASSERT(VALUE_IS_OBJINSTANCE(receiver), "receiver should be instance!");
            ObjInstance* objInstance = VALUE_TO_OBJINSTANCE(receiver);
            ASSERT(fieldIdx < objInstance->objHeader.class->fieldNum, "out of bounds field!");
            objInstance->fields[fieldIdx] = PEEK();
            LOOP();
        }
        CASE(JUMP): {
            // 指令流： 2字节的跳转正偏移量
            int16_t offset = READ_SHORT();
            ASSERT(offset>0, "OPCODE_JUMP's operand must be postive!");
            ip += offset;
            LOOP();
        }
        CASE(LOOP): {
            // 指令流： 2字节的跳转正偏移量
            int16_t offset = READ_SHORT();
            ASSERT(offset>0, "OPCODE_LOOP's operand must be positive!");
            ip -= offset;
            LOOP();
        }
        CASE(JUMP_IF_FALSE): {
            // 栈顶：跳转条件bool值
            // 指令流：2字节的跳转偏移量
            int16_t offset = READ_SHORT();
            ASSERT(offset > 0, "OPCODE_JUMP_IF_FALSE's operand must be positive");
            Value condition = POP();
            if (VALUE_IS_FALSE(condition) || VALUE_IS_NULL(condition)) {
                ip += offset;
            }
            LOOP();
        }
        CASE(AND):{
            // 栈顶：跳转条件bool值
            // 指令流：2字节的跳转偏移量
            uint16_t offset = READ_SHORT();
            ASSERT(offset>0, "OPCODE_AND's operand must be positive!");
            Value condition = PEEK();
            if (VALUE_IS_FALSE(condition) || VALUE_IS_NULL(condition)) {
                // 若条件为假，则不再计算and的右操作数
                ip += offset;
            } else {
                // 执行and右操作数，并丢掉栈顶的条件
                DROP();
            }
            LOOP();
        }
        CASE(OR):{
            // 栈顶：跳转条件bool值
            // 指令流：2字节的跳转偏移量
            uint16_t offset = READ_SHORT();
            ASSERT(offset>0, "OPCODE_OR's operand must be positive!");
            Value condition = PEEK();
            if (VALUE_IS_FALSE(condition) || VALUE_IS_NULL(condition)) {
                // 若条件为假 执行OR右操作数，并丢掉栈顶的条件
                DROP();
            } else {
                // 若条件为真，则不再计算AND的右操作数
                ip += offset;
            }
            LOOP();
        }
        CASE(CLOSE_UPVALUE):
            // 栈顶：相当于局部变量
            // 把地址大于栈顶局部变量的upvalue关闭
            closedUpvalue(curThread, curThread->esp - 1);
            DROP(); // 弹出栈顶局部变量
            LOOP();
        CASE(RETURN): {
            // 栈顶：返回值
            Value retVal = POP();
            // 回收堆栈框架
            curThread->usedFrameNum--;
            // 关闭此作用域内所有upvalue
            closedUpvalue(curThread, stackStart);
            if (curThread->usedFrameNum == 0) {
                // 如果不是被另一个线程调用的，直接结束
                if (curThread->caller == 0) {
                    curThread->stack[0] = retVal;
                    curThread->esp = curThread->stack+1;
                    return VM_RESULT_SUCCESS;
                }
                // 恢复主调方线程的调度
                ObjThread* callerThread = curThread->caller;
                curThread->caller = NULL;
                curThread = callerThread;
                vm->curThread = callerThread;
                // 主调线程的栈顶存储被调线程的结果
                curThread->esp[-1] = retVal;
            } else {
                // 将返回值置于运行时栈栈顶
                stackStart[0] = retVal;
                // 回收堆栈：保留出结果所在的slot即stackStart[0], 其他全部丢弃
                curThread->esp = stackStart+1;
            }
            LOAD_CUR_FRAME();
            LOOP();
        }
        CASE(CONSTRUCT):{
            // 栈底：stackStart[0]是class
            ASSERT(VALUE_IS_CLASS(stackStart[0]), "stackStart[0] should be a class for OPCODE_CONSTRUCT");
            ObjInstance* objInstance = newObjInstance(vm, VALUE_TO_CLASS(stackStart[0]));
            stackStart[0] = OBJ_TO_VALUE(objInstance);
            LOOP();
        }
        CASE(CREATE_CLOSURE):{
            // 指令流：2字节待创建闭包的函数在常量表中的索引+函数所用的upvalue数*x
            // endCompileUnit已经将闭包函数添加进了常量表
            ObjFn* objFn = VALUE_TO_OBJFN(fn->constants.datas[READ_SHORT()]);
            ObjClosure* objClosure = newObjClosure(vm, objFn);
            PUSH(OBJ_TO_VALUE(objClosure));
            uint32_t idx  = 0;
            while (idx < objFn->upvalueNum) { 
                // 读入endCompileUnit函数最后为每个upvalue写入数据对
                uint8_t isEnclosingLocalVar = READ_BYTE();
                uint8_t index = READ_BYTE();
                if (isEnclosingLocalVar) {  // 是直接外层的局部变量
                    // 创建upvalue
                    objClosure->upvalues[idx] = createOpenUpvalue(vm, curThread, curFrame->stackStart+index);
                } else {
                    // 从父编译单元中继承
                    // 当前编译的是下一层的闭包
                    objClosure->upvalues[idx] = curFrame->closure->upvalues[index];
                }
                idx++;
            }
            LOOP();
        }
        CASE(CREATE_CLASS): {
            // 指令流：1字节的field数量
            // 栈顶：基类 次栈顶：子类名
            uint32_t fieldNum = READ_BYTE();
            Value superClass = curThread->esp[-1]; // 基类名
            Value className = curThread->esp[-2]; // 子类名
            // 回收基类占用的空间，次栈顶保留，创建的类会直接用该控件
            DROP();
            validateSuperClass(vm, className, fieldNum, superClass);
            Class* class = newClass(vm, VALUE_TO_OBJSTR(className), fieldNum, VALUE_TO_CLASS(superClass));
            stackStart[0] = OBJ_TO_VALUE(class);
            LOOP();
        }
        CASE(INSTANCE_METHOD):
        CASE(STATIC_METHOD):{
            // 指令流：待绑定的方法“名字”在vim->allMethodNames中的2字节索引
            // 栈顶：带绑定的类，次栈顶：待绑定的方法
            uint32_t methodnameIdx = READ_SHORT();
            Class* class = VALUE_TO_CLASS(PEEK());
            // 获得待绑定的方法
            // 是由OPCODE_CREATE_CLOSURE操作码生成后压入到栈中的
            Value method = PEEK2();
            bindMethodAndPatch(vm, opCode, methodnameIdx, class, method);
            DROP();
            DROP();
            LOOP();
        }
        CASE(END):
            NOT_REACHED();
        default:
            printf("%d", opCode);
    }
    NOT_REACHED();
    
    #undef PUSH
    #undef POP
    #undef DROP
    #undef PEEK
    #undef PEEK2
    #undef LOAD_CUR_FRAME
    #undef STORE_CUR_FRAME
    #undef READ_BYTE
    #undef READ_SHORT
}

// 初始化虚拟机
void initVM(VM* vm) {
    vm->allocatedBytes = 0;
    vm->allObjects = NULL;
    vm->curParser = NULL;
    StringBufferInit(&vm->allMethodNames);
    vm->allModules = newObjMap(vm);
    vm->curParser = NULL;
    vm->config.heapGrowthFactor = 1.5;

    vm->config.minHeapSize = 1024*1024;
    vm->config.initialHeapSize = 1024*1024*10;
    vm->config.nextGC = vm->config.initialHeapSize;
    vm->grays.count = 0;
    vm->grays.capacity = 32;

    vm->grays.grayObjects = (ObjHeader**)malloc(vm->grays.capacity*sizeof(ObjHeader*));
}
void freeVM(VM* vm) {
    ASSERT(vm->allMethodNames.count > 0, "VM have alrady been freed!");
    ObjHeader* objHeader = vm->allObjects;
    while (objHeader != NULL) {
        ObjHeader* next = objHeader->next;
        freeObject(vm, objHeader);
        objHeader = next;
    }
    vm->grays.grayObjects = DEALLOCATE(vm, vm->grays.grayObjects);
    StringBufferClear(vm, &vm->allMethodNames);
    DEALLOCATE(vm, vm);
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