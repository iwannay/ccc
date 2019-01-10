#include "core.h"
#include <sys/stat.h>
#include "vm.h"
#include "utils.h"
#include "string.h"
#include "compiler.h"

char* rootDir  = NULL; // 根目录

#define CORE_MODULE VT_TO_VALUE(VT_NULL)


// 返回值类型是Value类型，且是放在args[0], args是Value数组
// RET_VALUE的参数是VALUE类型，无需转换直接赋值
// 是后面“RET_xxx”的基础
#define RET_VALUE(value)\
    do {\
        args[0] = value;\
        return true;\
    } while(0);

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


VMResult executeModule(VM* vm, Value moduleName, const char* moduleCode) {
    return VM_RESULT_ERROR;
}

// 编译核心模块
void buildCore(VM* vm) {
    // 创建核心模块，录入到vm->allModules
    ObjModule* coreModule = newObjModule(vm, NULL); // NULL为核心模块的name
    mapSet(vm, vm->allModules, CORE_MODULE，OBJ_TO_VALUE(coreModule));
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