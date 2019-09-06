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

typedef enum {
    VAR_SCOPE_INVALID,
    VAR_SCOPE_LOCAL, // 局部变量
    VAR_SCOPE_UPVALUE, // upvalue
    VAR_SCOPE_MODULE // 模块变量
} VarScopeType; // 变量作用域

typedef struct {
    VarScopeType scopeType; // 变量的作用域
    // 更具scopeType的值,index可能指向局部变量,upvalue,模块变量
    int index;
} Variable;

// 把opcode定义到数组opCodeSlotsUsed中
#define OPCODE_SLOTS(opCode, effect) effect,
static const int opCodeSlotsUsed[] = {
    #include "opcode.inc"
};
#undef OPCDE_SLOTS

typedef enum {
    BP_NONE, // 无绑定能力
    BP_LOWEST, // 最低绑定能力
    BP_ASSIGN, // =
    BP_CONDITION, // ?:
    BP_LOGIC_OR, // ||
    BP_LOGIC_AND, // &&
    BP_EQUAL, // == !=
    BP_IS, // is
    BP_CMP, // <> <= >=
    BP_BIT_OR, // |
    BP_BIT_AND, // &
    BP_BIT_SHIFT, // << >>
    BP_RANGE, // ..
    BP_TERM, // + -
    BP_FACTORE, // * / %
    BP_UNARY, // - ! ~
    BP_CALL, // . () []
    BP_HIGHEST
} BindPower; // 定义操作符的绑定权值

// 指针符函数指针
// 函数指针:函数本身为指针,需要给该函数赋值
// 指针函数:返回值为指针的函数
typedef void (*DenotationFn) (CompileUnit* cu, bool canAssign);

// 签名指针函数
typedef void (*methodSignatureFn) (CompileUnit* cu, Signature* signature);

typedef struct {
    const char* id; // 符号
    // 左绑定权值,不关注左边操作数的符号此值为0
    BindPower lbp;

    // 字面量,变量,前缀运算符等不关注左操作数的token调用方法
    DenotationFn nud;

    // 中缀运算符等关注左操作数的token调用方法
    DenotationFn led;

    // 表示本符号在类中被视为一个方法,
    // 为其生成一个方法签名
    methodSignatureFn methodSign;
} SymbolBindRule; // 符号绑定规则

// 添加常量并返回其索引
static uint32_t addConstant(CompileUnit* cu, Value constant) {
    ValueBufferAdd(cu->curParser->vm, &cu->fn->constants, constant);
    return cu->fn->constants.count - 1;
}



// 把Signature 转换为字符串,返回字符串长度
static uint32_t sign2String(Signature* sign, char* buf) {
    uint32_t pos = 0;
    // 复制方法名为xxx
    memcpy(buf+pos, sign->name, sign->length);
    pos += sign->length;

    // 处理方法名之后的部分
    switch (sign->type) {
        // SIGN_GETTER 形式:xxx, 无参数,上面memcpy已完成
        case SIGN_GETTER:
            break;
        // SIGN_SETTER 形式:xxx=(_), 之前已完成xxx
        case SIGN_SETTER:
            buf[pos++] = '=';
            // 添加=右边的赋值,仅支持一个
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';
            break;
        // SIGN_METHOD和SIGN_CONSTRUCT 形式:xxx(_,...)
        case SIGN_CONSTRUCT:
        case SIGN_METHOD: {
            buf[pos++] = "(";
            uint32_t idx = 0;
            while (idx < sign->argNum) {
               buf[pos++] = '_';
               buf[pos++] = ',';
               idx++;
            }
            if (idx == 0) {
                buf[pos++] = ')';
            } else {
                buf[pos-1] = ')';
            }
            break;
        }
        
        // SIGN_SUBSCRIPT形如:xxx[_,...]
        case SIGN_SUBSCRIPT: {
            buf[pos++] = '[';
            uint32_t idx = 0;
            while (idx < sign->argNum) {
                buf[pos++] = '_';
                buf[pos++] = ',';
                idx++;
            }
            if (idx == 0) {
                buf[pos++] = ']';
            } else {
                buf[pos-1] = ']';
            }
            break;
        }
        // SIGN_SUBSCRIPT_SETTER形如:xxx[_,...]=(_)
        case SIGN_SUBSCRIPT_SETTER: {
            buf[pos++] = '[';
            uint32_t idx = 0;
            // argNum包含右边的赋值参数,所以这里-1
            while (idx < sign->argNum -1) {
                buf[pos++] = '_';
                buf[pos++] = ',';
                idx++;
            }
            if (idx == 0) {
                buf[pos++] = ']';
            } else {
                buf[pos-1] = ']';
            }
            // 下面为等号右边的参数构造签名部分
            buf[pos++] = '=';
            buf[pos++] = '(';
            buf[pos++] = '_';
            buf[pos++] = ')';
        }
    }
    buf[pos] = '\0';
    return pos; // 返回签名函数长度
}

static void expression(CompileUnit* cu, BindPower rbp) {
    DenotationFn nud = Rules[cu->curParser->curToken.type].nud;
    // 表达式开头的要么是操作数,要么是前缀运算符,必然存在nud方法
    ASSERT(nud != NULL, "nud is NULL!");
    getNextToken(cu->curParser);
    bool canAssign = rbp < BP_ASSIGN;
    nud(cu, canAssign);
    while (rbp < Rules[cu->curParser->curToken.type].lbp) {
        DenotationFn led = Rules[cu->curParser->curToken.type].led;
        getNextToken(cu->curParser);
        led(cu, canAssign);
    }
}

// 通过签名方法编译方法调用, 包括callX和superX指令
static void emitCallBySignature(CompileUnit* cu, Signature* sign, OpCode opCode) {
    char signBuffer[MAX_SIGN_LEN];
    uint32_t length = sign2String(sign, signBuffer);

    // 确保签名录入到vm->allMethodNames中
    int symbolIndex = ensureSymbolExist(cu->curParser->vm, &cu->curParser->vm->allMethodNames, signBuffer, length);
    writeOpCodeShortOperand(cu, opCode+sign->argNum, symbolIndex);

    // 保留基类的slot
    if (opCode == OPCODE_SUPER0) {
        writeShortOperand(cu, addConstant(cu, VT_TO_VALUE(VT_NULL)));
    }
}

// 生成方法调用的指令,仅限callx指令
static void emitCall(CompileUnit* cu, int numArgs, const char* name, int length) {
    int symbolIndex = ensureSymbolExist(cu->curParser->vm, &cu->curParser->vm->allMethodNames, name, length);
    writeOpCodeShortOperand(cu, OPCODE_CALL0+numArgs, symbolIndex);
}

// 中缀运算符.led方法
static void infixOperator(CompileUnit* cu, bool canAssign UNUSED) {
    SymbolBindRule* rule = &Rules[cu->curParser->preToken.type];
    // 左右操作数绑定权值一样
    BindPower rbp = rule->lbp;
    expression(cu, rbp); // 解析右操作数

    // 生成一个参数的签名
    Signature sign = {SIGN_METHOD, rule->id, strlen(rule->id), 1};
    emitCallBySignature(cu, &sign, OPCODE_CALL0);
}

// 前缀运算符.nud方法,如-, !等
static void unaryOperator(CompileUnit* cu, bool canAssign UNUSED) {
    SymbolBindRule* rule = &Rules[cu->curParser->preToken.type];
    // BP_UNARY 作为rbp去调用express解析右操作数
    expression(cu, BP_UNARY);
    // 生成调用前缀运算符的指令
    // 0 个参数,前缀运算符都是1个字符,长度是1
    emitCall(cu, 0, rule->id, 1);

}

// 生成加载常量的指令
static void emitLoadConstant(CompileUnit* cu, Value value) {
    int index = addConstant(cu, value);
    writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, index);
}

// 数字和字符串.nud() 编译字面量
static void literal(CompileUnit* cu, bool canAssign UNUSED) {
    // literal 是常量,字符串和数字的nud方法,用来返回字面值
    emitLoadConstant(cu, cu->curParser->preToken.value);
}

// 不关注左操作数的符号称为前缀符号
// 用于入字面量,变量名,前缀符号等非运算符
// SymbolBindRule
#define PREFIX_SYMBOL(nud) {NULL, BP_NONE, nud, NULL, NULL}

// 前缀运算符,如!
#define PREFIX_OPERATOR(id) {id, BP_NONE, unaryOperator, NULL, unaryMethodsSignature}

// 关注左操作数的符号称为中缀符号
// 数组[,函数(,实例与方法之间的,等
#define INFIX_SYMBOL(lbp, led) {NULL, lbp, NULL, led, NULL}

// 中缀运算符
#define INFIX_OPERATOR(id, lbp) {id, lbp, NULL, infixOperator, infixMethodSignature}

// 即可做前缀又可做中缀的运算符,如-
#define MIX_OPERATOR(id) {id, BP_TERM, unaryOperator, infixOperator, mixMethodSignature}

// 占位
#define UNUSED_RULE {NULL, BP_NONE, NULL, NULL, NULL}

SymbolBindRule Rules[] = {
    UNUSED_RULE, // 无效的token
    PREFIX_SYMBOL(literal), // 数值token
    PREFIX_SYMBOL(literal), // 字符串token
};

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
inline static void writeShortOperand(CompileUnit* cu, int operand) {
    writeByte(cu, (operand >> 8) & 0xff); // 低地址写高位
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

// 为单运算符生成掐名
static void unaryMethodSignature(CompileUnit* cu UNUSED, Signature* sign UNUSED) {
    sign->type = SIGN_GETTER;
}

// 为中缀运算符创建签名
static void infixMethodSignature(CompileUnit* cu, Signature* sign) {
    // 在类中的运算符都是方法,类型为SIGN_METHOD
    sign->type = SIGN_METHOD;
    // 中缀运算符只有一个参数,故初始为1
    sign->argNum = 1;
    consumeCurToken(cu->curParser, TOKEN_LEFT_BRACE, "expect '(' after infix operator!");
    consumeCurToken(cu->curParser, TOKEN_ID, "expect variable name!");
    declareLocalVar(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after parameter!");
}

// 为既做单运算符又做中缀运算符的符号方法创建签名
static void mixMethodSignature(CompileUnit* cu, Signature* sign) {
    // 单运算符
    sign->type = SIGN_GETTER;
    // 若后面有'('说明为中缀运算符
    if (matchToken(cu->curParser, TOKEN_LEFT_BRACE)) {
        sign->type = SIGN_METHOD;
        consumeCurToken(cu->curParser, TOKEN_ID, "expect variable name!");
        declareLocalVar(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
        consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after parameter!");
    }
}

// 添加局部变量到cu
static uint32_t addLocalVar(CompileUnit* cu, const char* name, uint32_t length) {
    LocalVar* var = &(cu->localVars[cu->localVarNum]);
    var->name = name;
    var->length = length;
    var->scopeDepth = cu->scopeDepth;
    var->isUpvalue = false;
    return cu->localVarNum++;
}

// 声明局部变量
static int declareLocalVar(CompileUnit* cu, const char* name, uint32_t length) {
    if (cu->localVarNum > MAX_LOCAL_VAR_NUM) {
        COMPILE_ERROR(cu->curParser, "the max length of local variable of one scope is %d",MAX_LOCAL_VAR_NUM);
    }
    // 判断当前作用域中变量是否已经存在
    int idx = (int)cu->localVarNum-1;
    while (idx <= 0) {
        LocalVar* var = &cu->localVars[idx];
        // 只在当前作用域检查变量,到了父级作用域就退处
        if (var->scopeDepth < cu->scopeDepth) {
            break;
        }
        if (var->length == length && memcmp(var->name, name, length) == 0) {
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, length);
            COMPILE_ERROR(cu->curParser, "identifier \"%s\" redefinition!", id);
        }
        idx--;
    }

    return addLocalVar(cu, name, length);
}

// 根据作用域声明变量
static int declareVariable(CompileUnit* cu, const char* name, uint32_t length) {
    if (cu->scopeDepth == -1) { // 模块作用域
        int idx = defineModuleVar(cu->curParser->vm, cu->curParser->curModule, name, length, VT_TO_VALUE(VT_NULL));
        if (idx == -1) { // 重复定义报错
            char id[MAX_ID_LEN] = {'\0'};
            memcpy(id, name, length);
            COMPILE_ERROR(cu->curParser, "identifier \"%s\" redefinition!", id);
        }
        return idx;
    }

    // 局部变量
    addLocalVar(cu, name, length);
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
static void compileProgram(CompileUnit* cu) {
    ;
}

// 声明模块变量,与defineModuleVar的区别是不做重定义检查,默认为声明
static int declareModuleVar(VM* vm, ObjModule* objModule, const char* name, uint32_t length, Value value) {
    ValueBufferAdd(vm, &objModule->moduleVarValue, value);
    return addSymbol(vm, &objModule->moduleVarName, name, length);
}

// 返回包含cu->enclosingClassBK 的最近的CompileUnit
static CompileUnit* getEnclosingClassBKUnit(CompileUnit* cu) {
    while (cu != NULL) {
        if (cu->enclosingClassBK != NULL) {
            return cu;
        }
        cu = cu->enclosingUnit;
    }
    return NULL;
}

// 返回包含cu的最近的ClassBookKeep
static ClassBookKeep* GetEnclosingClassBK(CompileUnit* cu) {
    CompileUnit* cu2 = getEnclosingClassBKUnit(cu);
    if (cu2 != NULL) {
        return cu2->enclosingClassBK;
    }
    return NULL;
}

// 为实参列表中的各个实参生成加载实参的指令
static void processArgList(CompileUnit* cu, Signature* sign) {
    // 由主调放保证参数不为空
    ASSERT(cu->curParser->curToken.type != TOKEN_RIGHT_PAREN && 
        cu->curParser->curToken.type != TOKEN_RIGHT_BRACKET, "empty argument list!");

    do {
        if (++sign->argNum > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->curParser, "the max number of argment is %d!", MAX_ARG_NUM);
        }
        expression(cu, BP_LOWEST);
    } while(matchToken(cu->curParser, TOKEN_COMMA));
}

// 声明行参列表中的各个参数
static void processParaList(CompileUnit* cu, Signature* sign) {
     ASSERT(cu->curParser->curToken.type != TOKEN_RIGHT_PAREN && 
        cu->curParser->curToken.type != TOKEN_RIGHT_BRACKET, "empty argument list!");
    do {
        if (++sign->argNum > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->curParser, "the max number of params is %d!", MAX_ARG_NUM);
        }
        consumeCurToken(cu->curParser, TOKEN_ID, "expect variable name!");
        declareVariable(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
        
    } while(matchToken(cu->curParser, TOKEN_COMMA));
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