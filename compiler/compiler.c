#include "compiler.h"
#include "parser.h"
#include "core.h"
#include "utils.h"

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
    struct compileUnit* enclosingUnit;

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
    if (matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
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

// 查找局部变量并返回索引
static int findLocal(CompileUnit* cu, const char* name, uint32_t length) {
    // 从内往外查找变量
    int index = cu->localVarNum - 1;
    while (index >= 0) {
        if (cu->localVars[index].length = length && memcmp(cu->localVars[index].name, name, length)) {
            return index;
        }
        index--;
    }
    return -1;
}

// 添加upvalue到cu->upvalues,返回其索引,若已存在则只返回索引
static int addUpvalue(CompileUnit* cu, bool isEnclosingLocalVar, uint32_t index) {
    uint32_t idx = 0;
    while (idx < cu->fn->upvalueNum) {
        if (cu->upvalues[idx].index == index && 
            cu->upvalues[idx].isEnclosingLocalVar == isEnclosingLocalVar) {
            return idx;
        }
        idx++;
    }
    // 若没有找到则添加
    cu->upvalues[cu->fn->upvalueNum].isEnclosingLocalVar = isEnclosingLocalVar;
    cu->upvalues[cu->fn->upvalueNum].index = index;
    return cu->fn->upvalueNum++;
}

// 查找name指代的upvalue后添加到cu->upvalues,返回其索引,否则返回-1
static int findUpvalue(CompileUnit* cu, const char* name, uint32_t length) {
    if (cu->enclosingUnit == NULL) { // 如果已经到了最外层仍未找到,返回-1
        return -1;
    }
    // 进入了方法的cu且查找的不是静态域即不是方法的Upvalue,则停止查找
    if (!strchr(name, ' ') && cu->enclosingUnit->enclosingClassBK != NULL) {
        return -1;
    }

    // 查看name是否为直接外层的局部变量
    int directOuterLocalIndex = findLocal(cu->enclosingUnit, name, length);
    // 若是,将该外层局部变量重置为upvalue
    if (directOuterLocalIndex != -1) {
        cu->enclosingUnit->localVars[directOuterLocalIndex].isUpvalue = true;
        return addUpvalue(cu, true, (uint32_t)directOuterLocalIndex);
    }
    // 向外层递归查找
    int directOuterUpvalueIndex = findUpvalue(cu->enclosingUnit, name, length);
    if (directOuterUpvalueIndex != -1) {
        return addUpvalue(cu, false, (uint32_t)directOuterUpvalueIndex);
    }
    return -1;
}
// c从局部变量或者upvalue查找符号
static Variable getVarFromLocalOrUpvalue(CompileUnit* cu, const char* name, uint32_t length) {
    Variable var;
    var.scopeType = VAR_SCOPE_INVALID;
    var.index = findLocal(cu, name, length);
    if (var.index != -1) {
        var.scopeType = VAR_SCOPE_LOCAL;
        return var;
    }

    var.index = findUpvalue(cu, name, length);
    if (var.index != -1) {
        var.scopeType = VAR_SCOPE_UPVALUE;
    }
    return var;
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

// 尝试编译setter
static bool trySetter(CompileUnit* cu, Signature* sign) {
    if (!matchToken(cu->curParser, TOKEN_ASSIGN)) {
        return false;
    }
    if (sign->type == SIGN_SUBSCRIPT) {
        sign->type = SIGN_SUBSCRIPT_SETTER;
    } else {
        sign->type = SIGN_SETTER;
    }
    // 读取=号右边的形参左边的(
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "expect '(' after '='!");
    // 读取形参
    consumeCurToken(cu->curParser, TOKEN_ID, "expect ID");
    // 声明形参
    declareVariable(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
    // 读取=号右边形参右边的(
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect '(' after argument list!");
    sign->argNum++;
    return true;
}

// 生成把变量加载到栈的指令
static void emitLoadVariable(CompileUnit* cu, Variable var) {
    switch(var.scopeType) {
        case VAR_SCOPE_LOCAL:
            writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, var.index);
            break;
        case VAR_SCOPE_UPVALUE:
            writeOpCodeByteOperand(cu, OPCODE_LOAD_UPVALUE, var.index);
            break;
        case VAR_SCOPE_MODULE:
            writeOpCodeShortOperand(cu, OPCODE_LOAD_MODULE_VAR, var.index);
            break;
        default:
            NOT_REACHED();
    }
}

// 生成变量存储指令
static void emitStoreVariable(CompileUnit* cu, Variable var) {
    switch (var.scopeType) {
    case VAR_SCOPE_LOCAL:
        writeOpCodeByteOperand(cu, OPCODE_STORE_LOCAL_VAR, var.index);
        break;
    case VAR_SCOPE_UPVALUE:
        writeOpCodeByteOperand(cu, OPCODE_STORE_LOCAL_VAR, var.index);
        break;
    case VAR_SCOPE_MODULE:
        writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, var.index);
        break;
    default:
        NOT_REACHED();
    }
}

// 生成加载或存储变量的指令
static void emitLoadOrStoreVariable(CompileUnit* cu, bool canAssign, Variable var) {
    if (canAssign && matchToken(cu->curParser, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST);  // 计算右边表达式的值
        emitStoreVariable(cu, var); // 为var生成赋值指令
    } else {
        emitLoadVariable(cu, var);
    }
}

// 生成加载this到栈的指令
static void emitLoadThis(CompileUnit* cu) {
    Variable var = getVarFromLocalOrUpvalue(cu, "this", 4);
    ASSERT(var.scopeType != VAR_SCOPE_INVALID, "get variable failed!");
    emitLoadVariable(cu, var);
}

// 编译代码块
static void compileBlock(CompileUnit* cu) {
    // 已经读入{
    while (!matchToken(cu->curParser, TOKEN_RIGHT_BRACE)) {
        if (PEEK_TOKEN(cu->curParser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->curParser, "expect '}' at the end of block");
        }
        compileProgram(cu);
    }
}

// 编译函数或方法体
static void compileBody(CompileUnit* cu, bool isConstruct) {
    // 已经读入{
    compileBlock(cu);
    if (isConstruct) {
        // 构造函数加载this作为OPCODE_RETURN的返回值
        writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, 0);
    } else {
        // 加载null占位
        writeOpCode(cu, OPCODE_PUSH_NULL);
    }
    writeOpCode(cu, OPCODE_RETURN);
}

// 结束cu的编译工作,并在外层为其创建闭包
#if DEBUG
static ObjFn* endCompileUnit(CompileUnit* cu, const char* debugName, uint32_t debugNameLen) {
    bindDebugFnName(cu->curparser->vm, cu->fu->debug, debugName, debugNameLen);
#else
static ObjFn* endCompileUnit(CompileUnit* cu) {
#endif
    writeOpCode(cu, OPCODE_END);
    if (cu->enclosingUnit != NULL) {
        // 把当前编译单元作为常量添加到父编译单元的常量表
        uint32_t index = addConstant(cu->enclosingUnit, OBJ_TO_VALUE(cu->fn));
        // 内层函数以闭包的形式在外层函数中存在
        // 在外层函数的指令流中添加"创建闭包"
        writeOpCodeShortOperand(cu->enclosingUnit, OPCODE_CREATE_CLOSURE, index);

        // 为每个upvalue生成参数
        index = 0;
        while (index < cu->fn->upvalueNum) {
            writeByte(cu->enclosingUnit, cu->upvalues[index].isEnclosingLocalVar ? 1 : 0);
            writeByte(cu->enclosingUnit, cu->upvalues[index].index);
            index++;
        }
    }
    // 下调本编译单元,使编译单元指向外层编译单元
    cu->curParser->curlCompileUnit = cu->enclosingUnit;
    return cu->fn;
}

// 生成getter或method调用指令
static void emitGetterMethodCall(CompileUnit* cu, Signature* sign, OpCode opCode) {
    Signature newSign;
    newSign.type = SIGN_GETTER;
    newSign.name = sign->name;
    newSign.length = sign->length;
    newSign.argNum = 0;

    if (matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
        newSign.type = SIGN_METHOD;
        // 如果后面不是)说明有参数列表
        if (!matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
            processArgList(cu, &newSign);
            consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after argument list");
        }
    }

    // 当method传入块参数时
    if (matchToken(cu->curParser, TOKEN_LEFT_BRACE)) {
        // 块参数当作实参处理,所以++
        newSign.argNum++;
        newSign.type = SIGN_METHOD;
        CompileUnit fnCU;
        initCompileUnit(cu->curParser, &fnCU, cu, false);
        Signature tmpFnSign = {SIGN_METHOD, "", 0, 0};  // 临时用于编译函数
        if (matchToken(cu->curParser, TOKEN_BIT_OR)) { // 若块参数也有参数
            processParaList(&fnCU, &tmpFnSign);
            consumeCurToken(cu->curParser, TOKEN_BIT_OR, "expect '|' after argument list!" );
        }
        fnCU.fn->argNum = tmpFnSign.argNum;
        // 编译函数体,将指令流写入自己的指令单元fnCu
        compileBody(&fnCU, false);
#if DEBUG
        // 以此函数被传给的方法来命名这个函数, 函数名=方法名+" block arg"
        char fnName[MAX_SIGN_LEN+10] = {'\0'}   // "block arg\0"
        uint32_t len = sign2String(&newSign, fnName);
        memove(fnName+len, " block arg", 10);
        endCompileUnit(&fnCU);
#else
        endCompileUnit(&fnCU);
#endif
    }
    // 如果在构造函数中调用了super则会执行到此,构造函数中调用的方法只能是super
    if (sign->type == SIGN_CONSTRUCT) {
        if (newSign.type != SIGN_METHOD) {
            COMPILE_ERROR(cu->curParser, "the form of supercall is super() of super(arguments)");
        }
        newSign.type = SIGN_CONSTRUCT;
    }
    // 根据签名生成调用指令
    emitCallBySignature(cu, &newSign, opCode);
}

// 生成方法调用指令,包括getter和setter
static void emitMethodCall(CompileUnit* cu, const char* name, uint32_t length, OpCode opCode, bool canAssign) {
    Signature sign;
    sign.type = SIGN_GETTER;
    sign.name = name;
    sign.length = length;
    if (matchToken(cu->curParser, TOKEN_ASSIGN) && canAssign) {
        sign.type = SIGN_SETTER;
        sign.argNum = 1; // setter只接受一个参数
        // 载入实参(即=右边的值)
        expression(cu, BP_LOWEST);
        emitCallBySignature(cu, &sign, opCode);
    } else {
        emitGetterMethodCall(cu, &sign, opCode);
    }
}

// 标识符的签名函数
static void idMethodSignature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_GETTER; // 刚识别,默认getter
    // new 方法为构造函数
    if (sign->length == 3 && memcmp(sign->name, "new", 3) == 0) {
        // 构造函数不能接=,不能成为setter
        if (matchToken(cu->curParser, TOKEN_ASSIGN)) {
            COMPILE_ERROR(cu->curParser, "constructor shouldn't be setter!");
        }
        // 构造函数必须是method,即new(_,....),new 后面必须接(
        if (!matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
            COMPILE_ERROR(cu->curParser, "constructor must be method!");
        }

        sign->type = SIGN_CONSTRUCT;

        // 无参数直接返回
        if (matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
            return;
        }
    } else { // 不是构造函数
        if (trySetter(cu, sign)) {
            return;
        }

        // setter
        if (!matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
            return;
        }

        sign->type = SIGN_METHOD;
        if (matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
            return;
        }
    }
    processParaList(cu, sign);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after parameter list");
}

// 判断局部比那量(以小写字母开头的为局部变量)
static bool isLocalName(const char* name) {
    return (name[0] >= 'a' && name[0] <= 'z');
}

// 标识符的nud方法(变量名或方法名)
static void id(CompileUnit* cu, bool canAssign) {
    // 备份变量名
    Token name = cu->curParser->preToken;
    ClassBookKeep* classBK = GetEnclosingClassBK(cu);
    // 标识符可以是任意符号
    // 处理顺序:函数调用->局部调用和upvalue->实例域->静态域->类getter方法调用->模块变量

    // 处理函数调用
    if (cu->enclosingUnit == NULL && matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
        char id[MAX_ID_LEN] = {'\0'};
        // 函数名加上Fn前缀,作为模块变量名
        memmove(id, "FN ", 3);
        memmove(id+3, name.start, name.length);
        Variable var;
        var.scopeType = VAR_SCOPE_MODULE;
        var.index = getIndexFromSymbolTable(&cu->curParser->curModule->moduleVarName, id, strlen(id));
        if (var.index == -1) {
            memmove(id, name.start, name.length);
            id[name.length] = '\0';
            COMPILE_ERROR(cu->curParser, "Undefined function: '%s!'", id);
        }
        // 1.把模块变量即函数闭包加载到栈
        emitLoadVariable(cu, var);
        Signature sign;
        // 函数调用和method类似(method有可选的块参数)
        sign.type = SIGN_METHOD;
        // 把函数调用编译成"闭包.call",故name为call
        sign.name = "call";
        sign.length = 4;
        sign.argNum = 0;
        if (matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
            processArgList(cu, &sign);
            consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after argument list!");
        }
        emitCallBySignature(cu, &sign, OPCODE_CALL0);
    } else { // 按照变量处理
        // 按照局部变量和upvalue处理
        Variable var = getVarFromLocalOrUpvalue(cu, name.start, name.length);
        if (var.index != -1) {
            emitLoadOrStoreVariable(cu, canAssign, var);
            return;
        }
        // 按照实例域来处理
        if (classBK != NULL) {
            int fieldIdx = getIndexFromSymbolTable(&classBK->fields,name.start, name.length);
            if (fieldIdx != -1) {
                bool isRead = true;
                if (canAssign && matchToken(cu->curParser, TOKEN_ASSIGN)) {
                    isRead = false;
                    expression(cu, BP_LOWEST);
                }
                // 如果正在编译类方法,则直接在该实例对象中加载field
                if (cu->enclosingUnit != NULL) {
                    writeOpCodeByteOperand(cu, isRead?OPCODE_LOAD_THIS_FIELD:OPCODE_STORE_THIS_FIELD, fieldIdx);
                } else {
                    emitLoadThis(cu);
                    writeOpCodeByteOperand(cu, isRead, OPCODE_LOAD_FIELD, fieldIdx);
                }
                return;
            }
        }

        // 按照静态域查找
        if (classBK != NULL) {
            char* staticFieldId = ALLOCATE_ARRAY(cu->curParser->vm, char, MAX_ID_LEN);
            memset(staticFieldId, 0, MAX_ID_LEN);
            uint32_t staticFieldLen;
            char* clsName = classBK->name->value.start;
            uint32_t clsLen = classBK->name->value.length;
            // 个类中静态域的名称以"Cls类名 静态域名"来命名
            memmove(staticFieldId, "Cls", 3);
            memmove(staticFieldId+3,clsName, clsLen);
            memmove(staticFieldId+3+clsLen, " ", 1);
            const char* tkName = name.start;
            uint32_t tkLen = name.length;
            memmove(staticFieldId+4+clsLen, tkName, tkLen);
            staticFieldLen = strlen(staticFieldId);
            var = getVarFromLocalOrUpvalue(cu, staticFieldId, staticFieldLen);
            DEALLOCATE_ARRAY(cu->curParser->vm, staticFieldId, MAX_ID_LEN);
            if (var.index != -1) {
                emitLoadOrStoreVariable(cu, canAssign, var);
                return;
            }
        }
        // 同类中的其他方法,方法规定以小写字符开头
        if (classBK != NULL && isLocalName(name.start)) {
            emitLoadThis(cu);   // 确保args[0]是this对象
            // 因为类可能未编译完,故留到运行时检查
            emitMethodCall(cu, name.start, name.length, OPCODE_CALL0, canAssign);
            return;
        }

        // 按照模块变量处理
        var.scopeType = VAR_SCOPE_MODULE;
        var.index = getIndexFromSymbolTable(&cu->curParser->curModule->moduleVarName, name.start, name.length);
        if (var.index == -1) {
            // 模块变量属于模块作用域,若当前引用处未定义该模块变量
            // 可能后面有定义,所以先声明,待模块统计完后再检查
            // 用关键字func定义的函数是以Fn后接函数名作为模块变量
            // 加上Fn前缀按照函数名重新查找
            char fnName[MAX_SIGN_LEN+4] = {'\0'};
            memmove(fnName, "Fn ", 3);
            memmove(fnName+3, name.start, name.length);
            var.index = getIndexFromSymbolTable(&cu->curParser->curModule->moduleVarName, fnName, strlen(fnName));

            // 若不是函数名,可能是该模块变量定义在引用处的后面
            // 先将行号作为变量值去声明
            if (var.index == -1) {
                var.index = declareModuleVar(cu->curParser->vm, cu->curParser->curModule, name.start,name.length,NUM_TO_VALUE(cu->curParser->curToken.lineNo));
            }
        }
        emitLoadOrStoreVariable(cu, canAssign, var);
    }
}

// 生成加载类的文件
static void emitLoadModuleVar(CompileUnit* cu, const char* name) {
    int index = getIndexFromSymbolTable(&cu->curParser->curModule->moduleVarName, name, strlen(name));
    ASSERT(index != -1, "symbol should have been defined");
    writeOpCodeShortOperand(cu, OPCODE_LOAD_MODULE_VAR, index);
}

// 内嵌表达式.nud()
static void stringInterpolation(CompileUnit* cu, bool canAssign UNUSED) {
    // a % (b+c) d %(e) f 会被编译为["a ", b+c, " d ", e, "f "].join()
    // 其中a和d是token_interpolation, b,c,e都是token_id, f是token_string
    // 创建一个list实例拆分字符串,将拆分出的个部分作为元素添加到list
    emitLoadModuleVar(cu, "List");
    emitCall(cu, 0, "new()", 5);

    // 每次处理字符串中的一个内嵌表达式,包括两部分,以a%(b+c)为例:
    // 1. 加载TOKEN_INTERPOLATION对应的字符串,如a,将其加载到list
    // 2. 解析内嵌表达式,如b+c,将结果添加到list
    do {
        // 1. 处理TOKEN_INTERPOLATION中对应的字符串,如a
        literal(cu, false);
        // 将字符串添加到list
        emitCall(cu, 1, "addCore_(_)", 11); // 以_结尾的方法名是内部使用
        // 2. 解析b+c
        expression(cu, BP_LOWEST);
        // 将结果添加到list
        emitCall(cu, 1, "addCore_(_)", 11);

    } while(matchToken(cu->curParser, TOKEN_INTERPOLATION)); // 处理下一个内嵌表达式,如a%(b+c) d %(e) f中的d %(e)

    // 读取最后的字符串f
    consumeCurToken(cu->curParser, TOKEN_STRING, "expect string at the end of interpolatation");
    // 加载最后的字符串
    literal(cu, false);
    // 将字符串添加到list
    emitCall(cu, 1, "addCore_(_)", 11);
    // 最后将以上list中的元素join为一个字符串
    emitCall(cu, 0, "join()", 6);
}

// 编译bool, true和false的nud方法
static void boolean(CompileUnit* cu, bool canAssign UNUSED) {
    OpCode opCode = cu->curParser->preToken.type == TOKEN_TRUE ? OPCODE_PUSH_TRUE : OPCODE_PUSH_FALSE;
    writeOpCode(cu, opCode);
}

// "null".nud()
static void null(CompileUnit* cu, bool canAssign UNUSED) {
    writeOpCode(cu, OPCODE_PUSH_NULL);
}
// "this".nud()
static void this(CompileUnit* cu, bool canAssign UNUSED) {
    if (GetEnclosingClassBK(cu) == NULL) {
        COMPILE_ERROR(cu->curParser, "this must inside a class method!");
    }
    emitLoadThis(cu);
}

// "super".nud()
static void super(CompileUnit* cu, bool canAssign) {
    ClassBookKeep* enClosingClassBK = GetEnclosingClassBK(cu);
    if (enClosingClassBK == NULL) {
        COMPILE_ERROR(cu->curParser, "can't invoke super outside a class method!");
    }
    // 加载this保证args[0]始终是this对象,对基类调用无效
    emitLoadThis(cu);

    if (matchToken(cu->curParser, TOKEN_DOT)) { // 形如super.methodName
        consumeCurToken(cu->curParser, TOKEN_ID, "expect name after '.'!");
        emitMethodCall(cu,cu->curParser->preToken.start, cu->curParser->preToken.length, OPCODE_SUPER0, canAssign);
    } else { // super()
        emitGetterMethodCall(cu, enClosingClassBK->signature, OPCODE_SUPER0);
    }
}


SymbolBindRule Rules[] = {
    UNUSED_RULE,    // 无效的token
    PREFIX_SYMBOL(literal),     // 数值token
    PREFIX_SYMBOL(literal),     // 字符串token
    {NULL, BP_NONE, id, NULL, idMethodSignature},   // token id
    PREFIX_SYMBOL(stringInterpolation),     // 内嵌表达式token
    UNUSED_RULE,    // TOKEN_VAR
    UNUSED_RULE,    // TOKEN_FUN
    UNUSED_RULE,    // TOKEN_IF
    UNUSED_RULE,    // TOKEN_ELSE
    PREFIX_SYMBOL(boolean), // TOKEN_TRUE
    PREFIX_SYMBOL(boolean), // TOKEN_FALSE
    UNUSED_RULE,    // TOKEN_WHILE
    UNUSED_RULE,    // TOKEN_FOR
    UNUSED_RULE,    // TOKEN_BREAK
    UNUSED_RULE,    // TOKEN_CONTINUE
    UNUSED_RULE,    // TOKEN_RETURN
    PREFIX_SYMBOL(null),    // TOKEN_NULL
    UNUSED_RULE,    // TOKEN_CLASS
};

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