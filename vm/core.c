#include "core.h"
#include <sys/stat.h>
#include "vm.h"
#include "string.h"
#include "obj_map.h"
#include "common.h"
#include "obj_thread.h"
#include "compiler.h"
#include "class.h"
#include "utils.h"

char* rootDir  = NULL; // 根目录

#define CORE_MODULE VT_TO_VALUE(VT_NULL)


// 返回值类型是Value类型，且是放在args[0], args是Value数组
// RET_VALUE的参数是VALUE类型，无需转换直接赋值
// 是后面“RET_xxx”的基础
#define RET_VALUE(value)\
    do {\
        args[0] = value;\
        return true;\
    } while(0)

#define RET_OBJ(objPtr) RET_VALUE(OBJ_TO_VALUE(objPtr))

// 将bool值转为Value后作为返回值
#define RET_BOOL(boolean) RET_VALUE(BOOL_TO_VALUE(boolean))
#define RET_NUM(num) RET_VALUE(NUM_TO_VALUE(num))
#define RET_NULL RET_VALUE(VT_TO_VALUE(VT_NULL))
#define RET_TRUE RET_VALUE(VT_TO_VALUE(VT_TRUE))
#define RET_FALSE RET_VALUE(VT_TO_VALUE(VT_FALSE))

// 设置线程报错
#define SET_ERROR_FALSE(vmPtr, errMsg)\
    do {\
        vmPtr->curThread->errorObj = \
            OBJ_TO_VALUE(newObjString(vmPtr, errMsg, strlen(errMsg)));\
            return false;\
    } while(0);

// 绑定方法func 到classPtr指向的类
#define PRIM_METHOD_BIND(classPtr, methodName, func) {\
    uint32_t length = strlen(methodName);\
    int globalIdx = getIndexFromSymbolTable(&vm->allMethodNames, methodName, length);\
    if (globalIdx == -1) {\
        globalIdx = addSymbol(vm, &vm->allMethodNames, methodName, length);\
    }\
    Method method;\
    method.type = MT_PRIMITIVE;\
    method.primFn = func;\
    bindMethod(vm, classPtr, (uint32_t)globalIdx, method);\
}

static ObjString* mum2Str(VM* vm, double num) {
    // NaN不是一个确定的值,所以如果num!=num 就可以断定num是NaN
    if (num != num) {
        return newObjString(vm, "nan", 3);
    }
    // TODO: 定义INFINITY
    if (num == INFINITY) {
        return newObjString(vm, "infinity", 8);
    }

    if (num == -INFINITY) {
        return newObjString(vm, "-infinity", 9);
    }

    // 双精度数字用24字节缓冲区足已容纳
    char buf[24] = {'\0'};
    int len = sprintf(buf, "%.14g", num);
    return newObjString(vm, buf, len);
}

// 判断arg是否为数字
static bool validateNum(VM* vm, Value arg) {
    if (VALUE_IS_NUM(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be number!");
}

static bool validateString(VM* vm, Value arg) {
    if (VALUE_IS_OBJSTR(arg)) {
        return true;
    }
    SET_ERROR_FALSE(vm, "argument must be string!");
}

// 将字符串转化为数字
static bool primNumFromString(VM* vm, Value* args) {
    if (!validateString(vm, args[1])) {
        return false;
    }
    ObjString* objString = VALUE_TO_OBJSTR(args[1]);
    
    if (objString->value.length == 0) {
        RET_NULL;
    }

    ASSERT(objString->value.start[objString->value.length] == '\0', "objString't teminate!");
    errno = 0;
    char* endPtr;
    // 将字符串转化为double类型,它会自动跳过前面的空白
    double num = strtod(objString->value.start, &endPtr);

    while (*endPtr != '\0' && isspace((unsigned char)*endPtr)) {
        endPtr++;
    }

    // 字符串过长strtod会修改errno的值,errno为全局定义的宏
    if (errno == ERANGE) {
        RUN_ERROR("string too large");
    }

    // 如果字符串不能转换的字符不全是空白,字符串非法,返回NULL
    if (endPtr < objString->value.start+objString->value.length) {
        RET_NULL;
    }

    RET_NUM(num);
}

// 返回核心模块name的value结构
static Value getCoreClassValue(ObjModule* objModule, const char* name) {
    int index = getIndexFromSymbolTable(&objModule->moduleVarName, name, strlen(name));
    if (index == -1) {
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, strlen(name));
        RUN_ERROR("something wrong occur: missing core class \"%s\"!", id);
    }
    return objModule->moduleVarValue.datas[index];
}

// 返回bool的字符串形式: true或false
static bool primBoolToString(VM* vm, Value* args) {
    ObjString* objString;
    if (VALUE_TO_BOOL(args[0])) {
        objString = newObjString(vm, "true", 4);
    } else {
        objString = newObjString(vm, "false", 5);
    }
    RET_OBJ(objString);
}

// bool值取反
static bool primBoolNot(VM* vm UNUSED, Value* args) {
    RET_BOOL(!VALUE_TO_BOOL(args[0]));
}

// 校验是否是函数
static bool validateFn(VM* vm, Value arg) {
    if (VALUE_TO_OBJCLOSURE(arg)) {
        return true;
    }

    vm->curThread->errorObj = OBJ_TO_VALUE(newObjString(vm, "argument must be a function!", 28));
    return false;
}

// 以大写字符开头的为类名,表示类(静态)方法的调用
// Thread.new(func): 创建一个thread实例
static bool primThreadNew(VM* vm, Value* args) {
    // 代码块为参数必为闭包
    if (!validateFn(vm, args[1])) {
        return false;
    }
    ObjThread* objThread = newObjThread(vm, VALUE_TO_OBJCLOSURE(args[1]));

    // stack[0]为接收者,保持栈平衡
    objThread->stack[0] = VT_TO_VALUE(VT_NULL);
    objThread->esp++;
    RET_OBJ(objThread);
}

// Thread.ablort(err): 以错误信息err为参数退出线程
static bool primThreadAbort(VM* vm, Value* args) {
    vm->curThread->errorObj = args[1];
    return VALUE_IS_NULL(args[1]);
}

// Thread.current: 返回当前线程
static bool primThreadCurrent(VM* vm, Value* args UNUSED) {
    RET_OBJ(vm->curThread);
}

// Thread.suspend(): 挂起线程,退出解析器
static bool primThreadSuspend(VM* vm, Value* args UNUSED) {
    // curThread = NULL虚拟机将推出
    vm->curThread = NULL;
    return false;
}

// Thread.yield(arg) 带参数让出cpu
static bool primThreadYieldWithArg(VM* vm, Value* args) {
    ObjThread* curThread = vm->curThread;
    vm->curThread = curThread->caller;
    curThread->caller = NULL;
    if (vm->curThread != NULL) {
        // 如果当前线程有主调方,将当前线程的返回值放在主调方的栈顶
        vm->curThread->esp[-1] = args[1];
        // 对于thread.yield(arg)来说,回收arg的空间,
        // 保留thread参数所在的空间,将来唤醒时用于存储yield结果
        curThread->esp--;
    }
    return false;
}

// Thread.yield() 无参数让出cpu
static bool primThreadYieldWithoutArg(VM* vm, Value* args UNUSED) {
    ObjThread* curThread = vm->curThread;
    vm->curThread = curThread->caller;
    curThread->caller = NULL;
    if (vm->curThread != NULL) {
        vm->curThread->esp[-1] = VT_TO_VALUE(VT_NULL);
    }
    return false;
}

// 切换到下一个线程
static bool switchThread(VM* vm, ObjThread* nextThread, Value* args, bool withArg) {
    if (nextThread->caller != NULL) {
        RUN_ERROR("thread has been called!");
    }
    nextThread->caller = vm->curThread;
    if (nextThread->usedFrameNum == 0) {
        // 只有运行完毕的thread才为0
        SET_ERROR_FALSE(vm, "a finished thread can't be switched to!");
    }
    if (!VALUE_IS_NULL(nextThread->errorObj)) {
        SET_ERROR_FALSE(vm, "a aborted thread can't be switched to!");
    }
    if (withArg) {
        // 如果call有参数,回收参数的空间
        // 只保留此栈顶用于存储nextThread返回后的结果
        vm->curThread->esp--;
    }
    ASSERT(nextThread->esp > nextThread->stack, "esp should be greater than stack!");
    nextThread->esp[-1] = withArg ? args[1] : VT_TO_VALUE(VT_NULL);

    vm->curThread = nextThread;
    return false;
}

// objThread.call()
static bool primThreadCallWithoutArg(VM* vm, Value* args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, false);
}

// objThread.call(args)
static bool primThreadCallWithArg(VM* vm, Value* args) {
    return switchThread(vm, VALUE_TO_OBJTHREAD(args[0]), args, true);
}

// 返回线程是否运行完成
static bool primThreadIsDone(VM* vm UNUSED, Value* args) {
    // 获取.isDone的调度者
    ObjThread* objThread = VALUE_TO_OBJTHREAD(args[0]);
    // TODO: return?
    RET_BOOL(objThread->usedFrameNum == 0 || !VALUE_IS_NULL(objThread->errorObj));
}

static Class* defineClass(VM* vm, ObjModule* objModule, const char* name) {
    // 1. 先创建类
    Class* class = newRawClass(vm, name, 0);
    // 2.把类作为普通变量在模块中定义
    defineModuleVar(vm, objModule, name, strlen(name), OBJ_TO_VALUE(class));
    return class;
}

// !object: object 取反，结果为false
static bool primObjectNot(VM* vm UNUSED, Value* args) {
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0] == args[1]: 返回object是否相等
static bool primObjectEqual(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] != args[1]: 返回object是否不等
// TODO
static bool primObjectNotEqual(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(!valueIsEqual(args[0], args[1]));
    RET_VALUE(boolValue);
}

// args[0] is args[1]:类args[0]是否为类args[1]的子类
static bool primObjectIs(VM* vm, Value* args) {
    // args[1] 必须是class
    if (!VALUE_IS_CLASS(args[1])) {
        RUN_ERROR("argument must be class!");
    }

    Class* thisClass = getClassOfObj(vm, args[0]);
    Class* baseClass = (Class*)(args[1].objHeader);

    // 有可能多级继承，自上而下遍历
    while (baseClass != NULL) {
        if (thisClass == baseClass) {
            RET_VALUE(VT_TO_VALUE(VT_TRUE));
        }
        baseClass = baseClass->superClass;
    }
    // 若未找到基类，说明不具备is_a关系
    RET_VALUE(VT_TO_VALUE(VT_FALSE));
}

// args[0].tostring: 返回args[0]所属class的名字
static bool primObjectToString(VM* vm UNUSED, Value* args) {
    Class* class = args[0].objHeader->class;
    Value nameValue = OBJ_TO_VALUE(class->name);
    RET_VALUE(nameValue);
}

// args[0].type: 返回对象args[0]的类
static bool primObjectType(VM* vm, Value* args) {
    Class* class = getClassOfObj(vm, args[0]);
    RET_OBJ(class);
}

// args[0].name: 返回类名
static bool primClassName(VM* vm UNUSED, Value* args) {
    RET_OBJ(VALUE_TO_CLASS(args[0])->name);
}

// args[0].supertype: 返回args[0]的基类
static bool primClassSupertype(VM* vm UNUSED, Value* args) {
    Class* class  = VALUE_TO_CLASS(args[0]);
    if (class->superClass != NULL) {
        RET_OBJ(class->superClass);
    }
    RET_VALUE(VT_TO_VALUE(VT_NULL));
}

// args[0].toString：返回类名
static bool primClassToString(VM* vm UNUSED, Value* args) {
    RET_OBJ(VALUE_TO_CLASS(args[0])->name);
}

// args[0].same(args[1], args[2]): 返回args[1]和args[2]是否相等
static bool primObjectmetaSame(VM* vm UNUSED, Value* args) {
    Value boolValue = BOOL_TO_VALUE(valueIsEqual(args[1], args[2]));
    RET_VALUE(boolValue);
}

// Fn.new(_): 新建一个函数
static bool primFnNew(VM* vm, Value* args) {
    // 代码块为参数必为闭包
    if (!validateFn(vm, args[1])) return false;

    // 直接返回函数闭包
    RET_VALUE(args[1]);
}

// 绑定fn.call的重载
static void bindFnOverloadCall(VM* vm, const char* sign) {
    uint32_t index = ensureSymbolExist(vm, &vm->allModules, sign, strlen(sign));
    Method method = {MT_FN_CALL, {0}};
    bindMethod(vm, vm->fnClass, index, method);
}

// null 取非
static bool primNullNot(VM* vm UNUSED, Value* args UNUSED) {
    RET_VALUE(BOOL_TO_VALUE(true));
}

static bool primNullToString(VM* vm, Value* args UNUSED) {
    ObjString* objString = newObjString(vm, "null", 4);
    RET_OBJ(objString);
}

// 读取源代码文件
char* readFile(const char* path) {
    FILE* file = fopen(path, "r");
    if (file == NULL) {
        IO_ERROR("Could't open file %s", path);
    }

    struct stat fileStat;
    stat(path, &fileStat);
    size_t fileSize = fileStat.st_size;
    char* fileContent = (char*)malloc(fileSize+1);

    if (fileContent == NULL) {
        MEM_ERROR("Count't allocate memory for reading file %s \n", path);
    }

    size_t numRead = fread(fileContent, sizeof(char), fileSize, file);
    if (numRead < fileSize) {
        IO_ERROR("Cound't read file %s\n", path);
    }

    fileContent[fileSize] = '\0';
    fclose(file);
    return fileContent;
}

// table中查找符号symbol, 找到后返回索引，否则返回-1
int getIndexFromSymbolTable(SymbolTable* table, const char* symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");
    uint32_t index = 0;
    while (index < table->count) {
        if (length == table->datas[index].length &&
            memcmp(table->datas[index].str, symbol, length) == 0) {
                return index;
            }
            index++;
    }
    return -1;
}

// 往table中添加符号symbol,返回其索引
int addSymbol(VM* vm, SymbolTable* table, const char* symbol, uint32_t length) {
    ASSERT(length != 0, "length of symbol is 0!");
    String string;
    string.str = ALLOCATE_ARRAY(vm, char, length+1);
    memcpy(string.str, symbol, length);
    string.str[length] = '\0';
    string.length = length;
    StringBufferAdd(vm, table, string);
    return table->count - 1;
}

int ensureSymbolExist(VM* vm, SymbolTable* table, const char* symbol, uint32_t length) {
    int symbolIndex = getIndexFromSymbolTable(table, symbol, length);
    if (symbolIndex == -1) {
        return addSymbol(vm, table, symbol, length);
    }
    return symbolIndex;
}

// 使class->methods[index]=method
void bindMethod(VM* vm, Class* class, uint32_t index, Method method) {
    if (index >= class->methods.count) {
        Method emptyPad = {MT_NONE, {0}};
        MethodBufferFillWrite(vm, &class->methods, emptyPad, index-class->methods.count + 1);
    }
    class->methods.datas[index] = method;
}
// 绑定基类
void bindSuperClass(VM* vm, Class* subClass, Class* superClass) {
    subClass->superClass = superClass;
    // 继承基类属性数
    subClass->fieldNum += superClass->fieldNum;
    //继承基类方法
    uint32_t idx = 0;
    while (idx < superClass->methods.count) {
        bindMethod(vm, subClass, idx, superClass->methods.datas[idx]);
        idx++;
    }
}

// 从modules中获取名为moduleName的模块
static ObjModule* getModule(VM* vm, Value moduleName) {
    Value value = mapGet(vm->allModules, moduleName);
    if (value.type == VT_UNDEFINED) {
        return NULL;
    }
    return (ObjModule*)(value.objHeader);
}

// 载入模块moduleName并编译
static ObjThread* loadModule(VM* vm, Value moduleName, const char* moduleCode) {
    // 确保模块已经载入到vm->allModules
    // 先查看是否已经导入了该模块，避免重新导入
    ObjModule* module = getModule(vm, moduleName);

    // 若该模块未加载先将其加载，并继承核心模块中的变量
    if (module == NULL) {
        // 创建模块并添加到vm->allModules
        ObjString* modName = VALUE_TO_OBJSTR(moduleName);
        ASSERT(modName->value.start[modName->value.length] == '\0', "string.value.start is not terminated!");
        
        module = newObjModule(vm, modName->value.start);
        mapSet(vm, vm->allModules, moduleName, OBJ_TO_VALUE(module));

        // 继承核心模块中的变量
        ObjModule* coreModule = getModule(vm, CORE_MODULE);
        uint32_t idx = 0;
        while (idx < coreModule->moduleVarName.count) {
            defineModuleVar(vm, module, 
            coreModule->moduleVarName.datas[idx].str,
            strlen(coreModule->moduleVarName.datas[idx].str),
            coreModule->moduleVarValue.datas[idx]);
            idx++;
        }
    }

    ObjFn* fn = compileModule(vm, module, moduleCode);
    ObjClosure* objClosure = newObjClosure(vm, fn);
    ObjThread* moduleThread = newObjThread(vm, objClosure);

    return moduleThread;
}

// 编译核心模块
void buildCore(VM* vm) {

    // 核心模块不需要名字，模块也允许名字为空
    ObjModule* coreModule = newObjModule(vm, NULL);

    // 创建核心模块，录入到vm->allModules
    mapSet(vm, vm->allModules, CORE_MODULE, OBJ_TO_VALUE(coreModule));

    // 创建object类并绑定方法
    vm->objectClass = defineClass(vm, coreModule, "object");
    PRIM_METHOD_BIND(vm->objectClass, "!", primObjectNot);
    PRIM_METHOD_BIND(vm->objectClass, "==(_)", primObjectEqual);
    PRIM_METHOD_BIND(vm->objectClass, "!=(_)", primObjectNotEqual);
    PRIM_METHOD_BIND(vm->objectClass, "is(_)", primObjectIs);
    PRIM_METHOD_BIND(vm->objectClass, "toString", primObjectToString);
    PRIM_METHOD_BIND(vm->objectClass, "type", primObjectType);

    // 定义classOfClass类，它是所有meta类的meta类和基类
    vm->classOfClass = defineClass(vm, coreModule, "class");

    // objectClass是任何类的基类
    bindSuperClass(vm, vm->classOfClass, vm->objectClass);

    PRIM_METHOD_BIND(vm->classOfClass, "name", primClassName);
    PRIM_METHOD_BIND(vm->classOfClass, "supertype", primClassSupertype);
    PRIM_METHOD_BIND(vm->classOfClass, "toString", primClassToString);

    // 定义object类的元信息类objectMetaclass,它无需挂载到vm
    Class* objectMetaclass = defineClass(vm, coreModule, "objectMeta");

    // classOfClass类是所有meta类的meta类和基类
    bindSuperClass(vm, objectMetaclass, vm->classOfClass);

    // 类型比较
    PRIM_METHOD_BIND(objectMetaclass, "same(_,_)", primObjectmetaSame);
    // 绑定各自的meta类
    vm->objectClass->objHeader.class = objectMetaclass;
    objectMetaclass->objHeader.class = vm->classOfClass;
    vm->classOfClass->objHeader.class = vm->classOfClass; // 元信息类回路，meta类终点
   // TODO: coreModuleCode,这里顺序有问题
    // executeModule(vm, CORE_MODULE, coreModuleCode);

    vm->boolClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Bool"));
    PRIM_METHOD_BIND(vm->boolClass, "toString", primBoolToString);
    PRIM_METHOD_BIND(vm->boolClass, "!", primBoolNot);

    // thread类在core.script.inc中定义
    // 将其挂载到vm->threadClass 并补全原生方法
    vm->threadClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Thread"));
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "new(_)", primThreadNew);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "abort(_)", primThreadAbort);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "current", primThreadCurrent);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "suspend()", primThreadSuspend);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield(_)", primThreadYieldWithArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "yield()", primThreadYieldWithoutArg);
    
    // 以下是实例方法
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "call()", primThreadCallWithoutArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "call(_)", primThreadCallWithArg);
    PRIM_METHOD_BIND(vm->threadClass->objHeader.class, "isDone", primThreadIsDone);

    // 绑定函数类
    vm->fnClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Fn"));
    PRIM_METHOD_BIND(vm->fnClass->objHeader.class, "new(_)", primFnNew);

    // 绑定call的重载方法
    bindFnOverloadCall(vm, "call()");
    bindFnOverloadCall(vm, "call(_)");
    bindFnOverloadCall(vm, "call(_,_)");
    bindFnOverloadCall(vm, "call(_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");
    bindFnOverloadCall(vm, "call(_,_,_,_,_,_,_,_,_,_,_,_,_,_,_,_)");

    // 绑定Null类的方法
    vm->nullClass = VALUE_TO_CLASS(getCoreClassValue(coreModule, "Null"));
    PRIM_METHOD_BIND(vm->nullClass, "!", primNullNot);
    PRIM_METHOD_BIND(vm->nullClass, "toString", primNullToString);

}

// 执行模块
VMResult executeModule(VM* vm, Value moduleName, const char* moduleCode) {
    ObjThread* objThread = loadModule(vm, moduleName, moduleCode);
    return executeInstruction(vm, objThread);
}