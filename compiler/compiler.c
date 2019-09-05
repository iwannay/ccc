#include "compiler.h"
#include "parser.h"
#include "core.h"

#if DEBUG
    #include "debug.h"
#endif

struct compileUnit {
    // 所编译的函数
    ObjFn* fn;

    // 作用域中允许的局部变量的个数上限
    LocalVar localVars[MAX_LOCAL_VAR_NUM];

    // 已分配的局部变量个数
    uint32_t localVarNum;

    // 记录本层函数所引用的upvalue
    Upvalue upvalues[MAX_UPVALUE_NUM];

    // 此项表示正在编译的代码所处的作用域
    int scopeDepth;

    // 当前使用的slot个数
    uint32_t stackSlotNum;

    // 当前正在编译的训话层
    Loop* curLoop;

    // 当前正在beanie类的编译信息
    ClassBookKeep* enclosingClassBK;

    // 包含此编译单元的编译单元,即直接外层
    struct compileUint* enclosingUnit;

    // 当前parser
    Parser* curParser;
}; // 编译单元

// 把opcode定义到数组opCodeSlotsUsed中
#define OPCODE_SLOTS(opCode, effect) effect,
static const int opCodeSlotsUsed[] = {
    #include "opcode.inc"
};
#undef OPCDE_SLOTS

static void initCompileUnit(Parser* parser, CompileUnit* cu, CompileUnit* enclosingUnit, bool isMethod) {
    parser->curlCompileUnit = cu;
    cu->curParser = parser;
    cu->enclosingUnit = enclosingUnit;
    cu->curLoop = NULL;
    cu->enclosingClassBK = NULL;
    // 若没有外层,说明当前属于模块作用域
    if (enclosingUnit == NULL) {
        // 编译代码是从上到下,从外层的作用域开始,模块作用域设为-1
        cu->scopeDepth = -1;
        // 模块作用域中没有局部变量
        cu->localVarNum = 0;
    } else { // 局部作用域
        if (isMethod) { // 类中的方法
            // this设置为第0个局部变量
            cu->localVars[0].name = "this";
            cu->localVars[0].length = 4;
        } else { // 普通函数
            // 空出第0个位置保持统一
            cu->localVars[0].name = NULL;
            cu->localVars[0].length = 4;
        }
        
        // 第0个局部变量的作用域为模块级别
        cu->localVars[0].scopeDepth = -1;
        cu->localVars[0].isUpvalue= false;
        cu->localVarNum = 1;
        // 0表示局部作用域的最外层
        cu->scopeDepth = 0;
    }

    // 局部变量保存在栈中,栈长度等于局部变量数量
    cu->stackSlotNum = cu->localVarNum;
    cu->fn = newObjFn(cu->curParser->vm, cu->curParser->curModule, cu->localVarNum);
}

// 往函数指令流中写入1字节,返回索引
static int writeByte(CompileUnit* cu, int byte) {
    #if DEBUG
        IntByfferAdd(cu->curParser->vm, &cu->fn->debug->lineNo, cu->curParser->preToken.lineNo);
    #endif
    ByteBufferAdd(cu->curParser->vm, &cu->fn->instrStream, (uint8_t)byte);
    // 从0开始,实际长度需要-1
    return cu->fn->instrStream.count - 1;
}

// 写入操作码
static void writeOpCode(CompileUnit* cu, OpCode opCode) {
    writeByte(cu, opCode);
    // 累计需要的运行时空间大小
    cu->stackSlotNum += opCodeSlotsUsed[opCode];
    if (cu->stackSlotNum > cu->fn->maxStackSlotUsedNum) {
        cu->fn->maxStackSlotUsedNum = cu->stackSlotNum;
    }
}

// 写入一字节的操作数
static int writeByteOperand(CompileUnit* cu, int operand) {
    return writeByte(cu, operand);
}

// 按大端写入两字节的操作数 
inline static void writeShortOperand(CompileUnit* cu, OpCode opCode, int operand) {
    writeByte(cu, (opCode >> 8) & 0xff); // 低地址写高位
    writeByte(cu, operand&0xff);
}

// 写入操作数为1字节的指令
static int writeOpCodeByteOperand(CompileUnit* cu, OpCode opCode, int operand) {
    writeOpCode(cu, opCode);
    return writeByteOperand(cu, operand);
}

// 写入操作数为2字节的指令
static void writeOpCodeShortOperand(CompileUnit* cu, OpCode opCode, int operand) {
    writeOpCode(cu, opCode);
    writeShortOperand(cu, operand);
}

// 在模块objModule中定义名为name,值为value的模块变量
int defineModuleVar(VM* vm, ObjModule* objModule, const char* name, uint32_t length, Value value) {
    if (length > MAX_ID_LEN) {
        // name指向的变量名不以\0结束，将其从源码中拷贝出来
        char id[MAX_ID_LEN] = {'\0'};
        memcpy(id, name, length);

        if (vm->curParser != NULL) {
            COMPILE_ERROR(vm->curParser, "length of identifier \"%s\" should be no more than %d", id, MAX_ID_LEN);
        } else {
            MEM_ERROR("length of identifier \"%s\" should be no more than %d", id, MAX_ID_LEN);
        }
    }

    // 从模块变量中查找变量，如不存在则添加
    int symbolIndex = getIndexFromSymbolTable(&objModule->moduleVarName, name, length);
    if (symbolIndex == -1) {
        // 添加变量名
        symbolIndex = addSymbol(vm, &objModule->moduleVarName, name, length);
        // 添加变量值
        ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    } else if (VALUE_IS_NUM(objModule->moduleVarValue.datas[symbolIndex])) {
        objModule->moduleVarValue.datas[symbolIndex] = value;
    } else {
        symbolIndex = -1; // 已定义则返回-1,用于判断重定义
    }
    return symbolIndex;
}


// 编译程序
static void ocmpileProgram(CompileUnit* cu) {
    ;
}

// 编译模块
ObjFn* compileModule(VM* vm, ObjModule* objModule, const char* moduleCode) {
    // 各源码模块文件需要单独的parser
    Parser parser;
    parser.parent = vm->curParser;
    vm->curParser = &parser;
    if (objModule->name == NULL) {
        // 核心模块是core.script.inc
        initParser(vm, &parser, "core.script.inc", moduleCode, objModule);
    } else {
        initParser(vm, &parser, (const char*)objModule->name->value.start, moduleCode, objModule);
    }
    CompileUnit moduleCU;
    initCompileUnit(&parser, &moduleCU, NULL, false);
    // 记录现在模块变量的数量, 后面检查预订以模块变量时可减少遍历
    uint32_t moduleVarNumBefor = objModule->moduleVarValue.count;
    // 初始的parser->curToken.type为TOKEN_UNKNOWN,下面使其指向第一个合法的token
    getNextToken(&parser);
    
    // TODO: next
    while (!matchToken(&parser, TOKEN_EOF)) {
        compileProgram(&moduleCU);
    }
    printf("to be contine....\n");exit(0);
}