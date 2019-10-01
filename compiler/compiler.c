#include "compiler.h"
#include "parser.h"
#include "core.h"
#include "utils.h"
#include <string.h>
#include "gc.h"

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

    // 当前正在编译类的编译信息
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
    // 根据scopeType的值,index可能指向局部变量,upvalue,模块变量
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

static uint32_t addConstant(CompileUnit* cu, Value constant);
static void expression(CompileUnit* cu, BindPower rbp);
static void compileProgram(CompileUnit* cu);
static void infixOperator(CompileUnit* cu, bool canAssign UNUSED);
static void unaryOperator(CompileUnit* cu, bool canAssign UNUSED);
static void compileStatment(CompileUnit* cu);

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
            buf[pos++] = '(';
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

// 不关注左操作数的符号称为前缀符号
// 用于入字面量,变量名,前缀符号等非运算符
// SymbolBindRule
#define PREFIX_SYMBOL(nud) {NULL, BP_NONE, nud, NULL, NULL}

// 前缀运算符,如!
#define PREFIX_OPERATOR(id) {id, BP_NONE, unaryOperator, NULL, unaryMethodSignature}

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
    parser->curCompileUnit = cu;
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
        IntBufferAdd(cu->curParser->vm, &cu->fn->debug->lineNo, cu->curParser->preToken.lineNo);
    #endif
    ByteBufferAdd(cu->curParser->vm, &cu->fn->instrStream, (uint8_t)byte);
    // 从0开始,实际长度需要-1
    return cu->fn->instrStream.count - 1;
}

// 写入操作码并累加运行时栈
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
    writeByte(cu, operand & 0xff);
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
        COMPILE_ERROR(cu->curParser, "the max length of local variable of one scope is %d", MAX_LOCAL_VAR_NUM);
    }
    // 判断当前作用域中变量是否已经存在
    int idx = (int)cu->localVarNum-1;
    while (idx >= 0) {
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
    return declareLocalVar(cu, name, length);
}

// 为中缀运算符创建签名
static void infixMethodSignature(CompileUnit* cu, Signature* sign) {
    // 在类中的运算符都是方法,类型为SIGN_METHOD
    sign->type = SIGN_METHOD;
    // 中缀运算符只有一个参数,故初始为1
    sign->argNum = 1;
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "expect '(' after infix operator!");
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


// 为单运算符生成签名
static void unaryMethodSignature(CompileUnit* cu UNUSED, Signature* sign UNUSED) {
    sign->type = SIGN_GETTER;
}

// 丢掉作用域scopeDepth之内的局部局部变量
static uint32_t discardLocalVar(CompileUnit* cu, int scopeDepth) {
    ASSERT(cu->scopeDepth>-1, "upmost scope can't exit!");
    int localIdx = cu->localVarNum - 1;
    while (localIdx >= 0 && cu->localVars[localIdx].scopeDepth >= scopeDepth) {
        if (cu->localVars[localIdx].isUpvalue) {
            // 如果局部变量是内层的upvalue就关闭
            writeByte(cu, OPCODE_CLOSE_UPVALUE);
        } else {
            // 否则就弹出该变量回收空间
            writeByte(cu, OPCODE_POP);
        }
        localIdx--;
    }
    // 返回丢掉的局部变量的个数
    return cu->localVarNum-1-localIdx;
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
        if (cu->localVars[index].length == length && memcmp(cu->localVars[index].name, name, length) == 0) {
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


// sign尝试编译setter
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
        writeOpCodeByteOperand(cu, OPCODE_STORE_UPVALUE, var.index);
        break;
    case VAR_SCOPE_MODULE:
        writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, var.index);
        break;
    default:
        NOT_REACHED();
    }
}

// 定义变量为其赋值
static void defineVariable(CompileUnit* cu, uint32_t index) {
   // 局部变量已存储到栈中,无须处理.
   // 模块变量并不存储到栈中,因此将其写回相应位置
   if (cu->scopeDepth == -1) {
      //把栈顶数据存入参数index指定的全局模块变量
      writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, index);
      writeOpCode(cu, OPCODE_POP);  // 弹出栈顶数据,因为上面OPCODE_STORE_MODULE_VAR已经将其存储了
   }
}

//从局部变量,upvalue和模块中查找变量name
static Variable findVariable(CompileUnit* cu, const char* name, uint32_t length) {

   //先从局部变量和upvalue中查找
   Variable var = getVarFromLocalOrUpvalue(cu, name, length);
   if (var.index != -1) return var;
  
   //若未找到再从模块变量中查找
   var.index = getIndexFromSymbolTable(
	 &cu->curParser->curModule->moduleVarName, name, length);
   if (var.index != -1) {
      var.scopeType = VAR_SCOPE_MODULE;
   }
   return var;
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
    bindDebugFnName(cu->curParser->vm, cu->fn->debug, debugName, debugNameLen);
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
    cu->curParser->curCompileUnit = cu->enclosingUnit;
    return cu->fn;
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
        char fnName[MAX_SIGN_LEN+10] = {'\0'};   // "block arg\0"
        uint32_t len = sign2String(&newSign, fnName);
        memmove(fnName+len, " block arg", 10);
        endCompileUnit(&fnCU, fnName, len + 10);
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
        if (!matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
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

    // 处理函数调用, id此时为函数名
    if (cu->enclosingUnit == NULL && matchToken(cu->curParser, TOKEN_LEFT_PAREN)) {
        char id[MAX_ID_LEN] = {'\0'};
        // 函数名加上Fn前缀,作为模块变量名
        memmove(id, "Fn ", 3);
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
        if (!matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
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
                    writeOpCodeByteOperand(cu, isRead ? OPCODE_LOAD_THIS_FIELD : OPCODE_STORE_THIS_FIELD, fieldIdx);
                } else {
                    emitLoadThis(cu);
                    writeOpCodeByteOperand(cu, OPCODE_LOAD_FIELD, fieldIdx);
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
            // 用关键字fun定义的函数是以Fn后接函数名作为模块变量
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



// 占位符号作为参数设置指令
static uint32_t emitInstrWithPlaceholder(CompileUnit* cu, OpCode opCode) {
    writeOpCode(cu, opCode);
    writeByte(cu, 0xff);
    return writeByte(cu, 0xff)-1; // -1后返回高位地址,用于回填
}

// 用当前字节的码结束地址的偏移量去替换占位符oxffff
static void patchPlaceholder(CompileUnit* cu, uint32_t absIndex) {
    uint32_t offset = cu->fn->instrStream.count - absIndex - 2;
    // 先回填高8位
    cu->fn->instrStream.datas[absIndex] = (offset >> 8) & 0xff;
    // 低8位
    cu->fn->instrStream.datas[absIndex+1] = offset & 0xff;
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

// "(".nud()
static void parentheses(CompileUnit* cu, bool canAssign UNUSED) {
    // curToken 为"("
    expression(cu, BP_LOWEST);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after expression!");
}
// '['.nud() 用于处理字面量形式的列表
static void listLiteral(CompileUnit* cu, bool canAssign UNUSED) {
    // curToken = "["
    emitLoadModuleVar(cu, "List");
    emitCall(cu, 0, "new()", 5);
    do {
        // 支持字面量形式定义的空列表
        if (PEEK_TOKEN(cu->curParser) == TOKEN_RIGHT_BRACKET) {
            break;
        }
        expression(cu, BP_LOWEST);
        emitCall(cu, 1, "addCore_(_)", 11);
    } while(matchToken(cu->curParser, TOKEN_COMMA));
    consumeCurToken(cu->curParser, TOKEN_RIGHT_BRACKET, "expect ']' after list element!");

}

// '['.led() 用于索引list元素,如list[x]
static void subscript(CompileUnit* cu, bool canAssign) {
    if (matchToken(cu->curParser, TOKEN_RIGHT_BRACKET)) {
        COMPILE_ERROR(cu->curParser, "need argument in the '[]'");
    }
    // 默认[_],getter
    Signature sign = {SIGN_SUBSCRIPT, "", 0, 0};
    processArgList(cu, &sign);
    consumeCurToken(cu->curParser,TOKEN_RIGHT_BRACKET,"expect ']' after argument list!");
    // [_]=(_) 则为setter
    if (canAssign && matchToken(cu->curParser, TOKEN_ASSIGN)) {
        sign.type = SIGN_SUBSCRIPT_SETTER;
        // = 右边的值也算一个参数,签名是[args[1]]=(args[2])
        if (++sign.argNum > MAX_ARG_NUM) {
            COMPILE_ERROR(cu->curParser, "the max number of argument is %d", MAX_ARG_NUM);
        }
        // 获取=右边的表达式
        expression(cu, BP_LOWEST);
    }
    emitCallBySignature(cu, &sign, OPCODE_CALL0);
}

// 为下标操作符[编译签名
static void subscriptMethodSignature(CompileUnit* cu, Signature* sign) {
    sign->type = SIGN_SUBSCRIPT;
    sign->length = 0;
    processParaList(cu, sign);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_BRACKET, "expect ']' after index list!");
    trySetter(cu, sign);
}

// '.'.led() 方法入口,编译所有调用入口
static void callEntry(CompileUnit* cu, bool canAssign) {
    // curToken= TOKEN_ID
    consumeCurToken(cu->curParser, TOKEN_ID, "expect method name after '.'!");
    emitMethodCall(cu, cu->curParser->preToken.start, cu->curParser->preToken.length, OPCODE_CALL0, canAssign);
}

// '{'.nud map对象字面量
static void mapLiteral(CompileUnit* cu, bool canAssign UNUSED) {
    // curToken = key
    // 1.加载类
    emitLoadModuleVar(cu, "Map");
    // 2.加载调用方法,方法从类中获取
    emitCall(cu, 0, "new()", 5);
    do {
        // 允许空map
        if (PEEK_TOKEN(cu->curParser) == TOKEN_RIGHT_BRACE) {
            break;
        }
        // 读取key
        expression(cu, BP_UNARY);
        consumeCurToken(cu->curParser, TOKEN_COLON, "expect ':' after key!");
        // 读取value
        expression(cu, BP_LOWEST);
        // 添加指令
        emitCall(cu, 2, "addCore_(_,_)", 13);
    } while(matchToken(cu->curParser, TOKEN_COMMA));
}

// '||'.led()
static void logicOr(CompileUnit* cu, bool canAssign UNUSED) {
    // 此时栈顶为||的左操作数
    uint32_t placeholderIndex = emitInstrWithPlaceholder(cu, OPCODE_OR);
    // 生成右操作数的指令
    expression(cu, BP_LOGIC_OR);
    patchPlaceholder(cu, placeholderIndex);
}

// '&&'.led()
static void logicAdd(CompileUnit* cu, bool canAssign UNUSED) {
    // 此时栈顶是&&的左操作数
    uint32_t placeHolderIndex = emitInstrWithPlaceholder(cu, OPCODE_AND);
    expression(cu, BP_LOGIC_AND);
    patchPlaceholder(cu, placeHolderIndex);
}
// '?:'.led()
static void condition(CompileUnit* cu, bool canAssign UNUSED) {
    uint32_t falseBranchStart = emitInstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);
    // 编译true分支
    expression(cu, BP_LOWEST);
    consumeCurToken(cu->curParser, TOKEN_COLON, "expect ':' after true branch!");
    uint32_t falseBranchEnd = emitInstrWithPlaceholder(cu, OPCODE_JUMP);
    patchPlaceholder(cu, falseBranchStart);
    expression(cu, BP_LOWEST);
    patchPlaceholder(cu, falseBranchEnd);
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
    PREFIX_SYMBOL(this),    // TOKEN_THIS
    UNUSED_RULE,    // TOKEN_STATIC
    INFIX_OPERATOR("is", BP_IS),    // TOKEN_IS
    PREFIX_SYMBOL(super),   // TOKEN_SUPER
    UNUSED_RULE,    //  TOKEN_IMPORT
    UNUSED_RULE,    // TOKEN_COMMA
    UNUSED_RULE,    // TOKEN_COMMA
    PREFIX_SYMBOL(parentheses), // TOKEN_LEFT_PAREN
    UNUSED_RULE,    // TOKEN_RIGHT_PAREN
    {NULL, BP_CALL, listLiteral, subscript, subscriptMethodSignature},   // TOKEN_LEFT_BRACKET
    UNUSED_RULE,    // TOKEN_RIGHT_BRACKET
    PREFIX_SYMBOL(mapLiteral),  // TOKEN_LEFT_BRACE
    UNUSED_RULE,    // TOKEN_RIGHT_BRACE
    INFIX_SYMBOL(BP_CALL, callEntry),   // TOKEN_DOT
    INFIX_OPERATOR("..", BP_RANGE), // TOKEN_DOT_DOT
    INFIX_OPERATOR("+", BP_TERM),   // TOKEN_ADD
    MIX_OPERATOR("-"),  // TOKEN_SUB
    INFIX_OPERATOR("*", BP_FACTORE),    // TOKEN_MUL
    INFIX_OPERATOR("/", BP_FACTORE),    // TOKEN_DIV
    INFIX_OPERATOR("%", BP_FACTORE),    // TOKEN_MOD
    UNUSED_RULE,    // TOKEN_ASSIGN
    INFIX_OPERATOR("&", BP_BIT_AND),    // TOKEN_BIT_AND
    INFIX_OPERATOR("|", BP_BIT_OR), // TOKEN_BIT_OR
    PREFIX_OPERATOR("~"),   // TOKEN_BIT_NOT
    INFIX_OPERATOR(">>", BP_BIT_SHIFT), // TOKEN_BIT_SHIFT_RIGHT
    INFIX_OPERATOR("<<", BP_BIT_SHIFT), // TOKEN_BIT_SHIFT_LEFT
    INFIX_SYMBOL(BP_LOGIC_AND, logicAdd),   // TOKEN_LOGIC_AND
    INFIX_SYMBOL(BP_LOGIC_OR, logicOr), // TOKEN_LOGIC_OR
    PREFIX_OPERATOR("!"),   // TOKEN_LOGIC_NOT
    INFIX_OPERATOR("==",BP_EQUAL),  // TOKEN_EQUAL
    INFIX_OPERATOR("!=", BP_EQUAL), // TOKEN_NOT_EQUAL
    INFIX_OPERATOR(">", BP_CMP),    // TOKEN_GREATER
    INFIX_OPERATOR(">=", BP_CMP),   // TOKEN_GREATER_EQUAL
    INFIX_OPERATOR("<", BP_CMP),    // TOKEN_LESS
    INFIX_OPERATOR("<=", BP_CMP),   // TOKEN_LESS_EQUAL
    INFIX_SYMBOL(BP_ASSIGN, condition), // TOKEN_QUESTION
    UNUSED_RULE, // TOKEN_EOF
};

static void expression(CompileUnit* cu, BindPower rbp) {
    DenotationFn nud = Rules[cu->curParser->curToken.type].nud;
    // 表达式开头的要么是操作数,要么是前缀运算符,必然存在nud方法
    if (nud == NULL) {
        printf("nud is NULL!");
    }
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

// 编译变量定义
static void compileVarDefinition(CompileUnit* cu, bool isStatic) {
    consumeCurToken(cu->curParser, TOKEN_ID, "missing variable name!");
    Token name = cu->curParser->preToken;
    // 只支持一次定义单个变量
    if (cu->curParser->curToken.type == TOKEN_COMMA) {
        COMPILE_ERROR(cu->curParser, "var only support declaring a variable.");
    }

    // 判断是否是类中的静态域,并确保cu是模块cu.
    // 模块cu的enclosing为null
    if (cu->enclosingUnit == NULL && cu->enclosingClassBK != NULL) {
        if (isStatic) { // 静态域
            char* staticFieldId = ALLOCATE_ARRAY(cu->curParser->vm, char, MAX_ID_LEN);
            memset(staticFieldId, 0, MAX_ID_LEN);
            uint32_t staticFieldIdLen;
            char* clsName = cu->enclosingClassBK->name->value.start;
            uint32_t clsLen = cu->enclosingClassBK->name->value.length;
            // 用前缀 'Cls '+类名+变量名,作为静态域在模块编译单元中的局部变量
            memmove(staticFieldId, "Cls", 3);
            memmove(staticFieldId+3, clsName, clsLen);
            memmove(staticFieldId+3+clsLen, " ", 1);
            const char* tkName = name.start;
            uint32_t tkLen = name.length;
            memmove(staticFieldId+4+clsLen, tkName, tkLen);
            staticFieldIdLen = strlen(staticFieldId);
            if (findLocal(cu, staticFieldId, staticFieldIdLen) == -1) {
                int index = declareLocalVar(cu, staticFieldId, staticFieldIdLen);
                // 默认值null
                writeOpCode(cu, OPCODE_PUSH_NULL);
                ASSERT(cu->scopeDepth == 0, "should in class scope!");
                defineVariable(cu, index);
                // 静态域可初始化
                Variable var = findVariable(cu, staticFieldId, staticFieldIdLen);
                if (matchToken(cu->curParser, TOKEN_ASSIGN)) {
                    expression(cu, BP_LOWEST);
                    emitStoreVariable(cu, var);
                }
            } else {
                COMPILE_ERROR(cu->curParser, "static field '%s' redefinition!", strchr(staticFieldId, ' ')+1);
            }
        } else { // 实例域
            ClassBookKeep* classBK = GetEnclosingClassBK(cu);
            int fieldIndex = getIndexFromSymbolTable(&classBK->fields, name.start, name.length);
            if (fieldIndex == -1) {
                fieldIndex = addSymbol(cu->curParser->vm, &classBK->fields, name.start, name.length);
            } else {
                if (fieldIndex > MAX_FIELD_NUM) {
                    COMPILE_ERROR(cu->curParser, "the max number of instance field is %d!", MAX_FIELD_NUM);
                } else {
                    char id[MAX_ID_LEN] = {'\0'};
                    memcpy(id, name.start, name.length);
                    COMPILE_ERROR(cu->curParser, "instance field '%s' redefinition!", id);
                }
                if (matchToken(cu->curParser, TOKEN_ASSIGN)) {
                    COMPILE_ERROR(cu->curParser, "instance field isn't allowed initalization");
                }
            }

        }
        return;
    }

    // 若不是类中的域定义,则按照一般定义
    if (matchToken(cu->curParser, TOKEN_ASSIGN)) {
        expression(cu, BP_LOWEST);
    } else {
        writeOpCode(cu, OPCODE_PUSH_NULL);
    }
    uint32_t index = declareVariable(cu, name.start, name.length);
    defineVariable(cu, index);
}

static void compileIfStatment(CompileUnit* cu) {
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "missing '(' after if!");
    expression(cu, BP_LOWEST);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "missing ')' before '}' in if!");
    uint32_t falseBranchStart = emitInstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);
    // 编译then分支
    // 代码前后的{}由compileStatment读取
    compileStatment(cu);
    if (matchToken(cu->curParser, TOKEN_ELSE)) {
        uint32_t falseBranchEnd = emitInstrWithPlaceholder(cu, OPCODE_JUMP);
        patchPlaceholder(cu, falseBranchStart);
        compileStatment(cu);
        patchPlaceholder(cu, falseBranchEnd);
    } else {
        patchPlaceholder(cu, falseBranchStart);
    }
}

// 编译continue
inline static void compileContinue(CompileUnit* cu) {
    if (cu->curLoop == NULL) {
        COMPILE_ERROR(cu->curParser, "continue should be used inside a loop!");
    }
    // 不能在cu->localVars中删除,否则在continue语句后面若引用前面的变量则提示找不到
    discardLocalVar(cu, cu->curLoop->scopeDepth+1);
    int loopBackOffset = cu->fn->instrStream.count - cu->curLoop->condStartIndex+2;
    // 向回跳转的
    writeOpCodeShortOperand(cu, OPCODE_LOOP, loopBackOffset);
}

// 获得ip所指向的操作码的操作数占用的字节
uint32_t getBytesOfOperands(Byte* instrStream, Value* constants, int ip) {
    switch ((OpCode)instrStream[ip]) {
    case OPCODE_CONSTRUCT:
    case OPCODE_RETURN:
    case OPCODE_CLOSE_UPVALUE:
    case OPCODE_PUSH_NULL:
    case OPCODE_PUSH_FALSE:
    case OPCODE_PUSH_TRUE:
    case OPCODE_POP:
        return 0;
    case OPCODE_CREATE_CLASS:
    case OPCODE_LOAD_THIS_FIELD:
    case OPCODE_STORE_THIS_FIELD:
    case OPCODE_LOAD_FIELD:
    case OPCODE_STORE_FIELD:
    case OPCODE_LOAD_LOCAL_VAR:
    case OPCODE_STORE_LOCAL_VAR:
    case OPCODE_LOAD_UPVALUE:
    case OPCODE_STORE_UPVALUE:
        return 1;
    case OPCODE_CALL0:
    case OPCODE_CALL1:
    case OPCODE_CALL2:
    case OPCODE_CALL3:
    case OPCODE_CALL4:
    case OPCODE_CALL5:
    case OPCODE_CALL6:
    case OPCODE_CALL7:
    case OPCODE_CALL8:
    case OPCODE_CALL9:
    case OPCODE_CALL10:
    case OPCODE_CALL11:
    case OPCODE_CALL12:
    case OPCODE_CALL13:
    case OPCODE_CALL14:
    case OPCODE_CALL15:
    case OPCODE_CALL16:
    case OPCODE_LOAD_CONSTANT:
    case OPCODE_LOAD_MODULE_VAR:
    case OPCODE_STORE_MODULE_VAR:
    case OPCODE_LOOP:
    case OPCODE_JUMP:
    case OPCODE_JUMP_IF_FALSE:
    case OPCODE_AND:
    case OPCODE_OR:
    case OPCODE_INSTANCE_METHOD:
    case OPCODE_STATIC_METHOD:
        return 2;
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
    case OPCODE_SUPER16:
        // OPCODE_SUPERX的操作数分别由writeOpCodeShortOperand
        // 和writeShortOperand写入的,共1个操作码和4个字节的操作数
        return 4;
    case OPCODE_CREATE_CLOSURE: {
        // 获得操作码OPCODE_CLOSURE操作数,2字节
        // 该操作数是待创建闭包的函数在常量表中的索引
        uint32_t index = (instrStream[ip+1]<<8) | instrStream[ip+2];
        // 2是值index在指令流中占用的字节
        // 每个upvalue有一对参数
        return 2 + (VALUE_TO_OBJFN(constants[index]))->upvalueNum * 2;
    }
    default:
        NOT_REACHED()
    }
}

// 进入循环体时的相关设置
static void enterLoopSetting(CompileUnit* cu, Loop* loop) {
    loop->condStartIndex = cu->fn->instrStream.count - 1;
    loop->scopeDepth = cu->scopeDepth;
    // 在当前循环中嵌套新的循环层
    loop->enclosingLoop = cu->curLoop;
    cu->curLoop = loop;
}

// 离开循环体时的相关设置
static void leaveLoopPatch(CompileUnit* cu) {
    // 2是指两个字节的操作数
    int loopBackOffset = cu->fn->instrStream.count - cu->curLoop->condStartIndex + 2;
    // 生成向回跳转的CODE_LOOP指令,即使ip -= loopBackOffset
    writeOpCodeShortOperand(cu, OPCODE_LOOP, loopBackOffset);
    // 回填循环体的结束地址
    patchPlaceholder(cu, cu->curLoop->exitIndex);
    // 下面在循环体中回填break的占位符OPCODE_END
    // 循环体开始地址
    uint32_t idx = cu->curLoop->bodyStartIndex;
    // 循环体结束地址
    uint32_t loopEndIndex = cu->fn->instrStream.count;
    while (idx < loopEndIndex) {
        // 回填循环体中所有可能的break语句
        if (OPCODE_END == cu->fn->instrStream.datas[idx]) {
            cu->fn->instrStream.datas[idx] = OPCODE_JUMP;
            // 回填OPCODE_JUMP的操作数,即跳转偏移量
            patchPlaceholder(cu, idx+1);
            // 使idx指向指令流中下一个操作码
            idx += 3;
        } else {
            idx += 1 + getBytesOfOperands(cu->fn->instrStream.datas, cu->fn->constants.datas, idx);
        }
    }
    // 退出当前循环体
    cu->curLoop = cu->curLoop->enclosingLoop;
}

// 进入作用域
static void enterScope(CompileUnit* cu) {
    // 进入内嵌作用域
    cu->scopeDepth++;
}

// 退出作用域
static void leaveScope(CompileUnit* cu) {
    if (cu->enclosingUnit != NULL) {
        uint32_t discardNum = discardLocalVar(cu, cu->scopeDepth);
        cu->localVarNum -= discardNum;
        cu->stackSlotNum -= discardNum;
    }
    // 回到上一层作用域
    cu->scopeDepth--;
}

// 编译循环体
static void compileLoopBody(CompileUnit* cu) {
    cu->curLoop->bodyStartIndex = cu->fn->instrStream.count;
    compileStatment(cu);
}

// 比那以while循环,如while(a<b){循环体}
static void compileWhileStatement(CompileUnit* cu) {
    Loop loop;
    // 设置循环体起始地址
    enterLoopSetting(cu, &loop);
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "expect '(' befor condition!");
    expression(cu, BP_LOWEST);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after condition!");
    // 先把条件失败时跳转的目标地址占位
    loop.exitIndex = emitInstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);
    compileLoopBody(cu);
    // 设置循环体结束等等
    leaveLoopPatch(cu);
}

// 编译for循环,如 for i (sequence) {循环体}
static void compileForStatment(CompileUnit* cu) {
    // for i (sequence) {
    //      System.Print(i)
    // }
    // 内部展开为:
    // var seq = sequence
    // var iter
    // while iter = seq.iterate(iter) {
    //      var i = seq.iteratorValue(iter)
    //      system.Print(i)
    // }
    
    // 为局部变量seq和iter创建作用域
    enterScope(cu);
    // 读取循环变量的名字,如for i (sequence) 中 i
    consumeCurToken(cu->curParser, TOKEN_ID, "expect variable after for!");
    const char* loopVarName = cu->curParser->preToken.start;
    uint32_t loopVarLen = cu->curParser->preToken.length;
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "expect '(' befor sequence!");

    // 编译迭代序列
    expression(cu, BP_LOWEST);
    consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after sequence");
    // 申请一个局部变量seq来存储序列对象
    // 其值就是expression存储到栈中的结果
    uint32_t seqSlot = addLocalVar(cu, "seq ", 4);
    writeOpCode(cu, OPCODE_PUSH_NULL);
    uint32_t iterSlot = addLocalVar(cu, "iter ", 5);
    Loop loop;
    enterLoopSetting(cu, &loop);
    // 为调用seq.iterate(iter)做准备
    // 1. 先压如序列对象seq
    writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, seqSlot);
    // 2. 在压入参数iter
    writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, iterSlot);
    // 3. 调用seq.iterate(iter)
    emitCall(cu, 1, "iterate(_)", 10);
    // seq.iterate(iter) 把结果(下一个迭代器) 存储到args[0](即栈顶),现在将其同步到iter
    writeOpCodeByteOperand(cu, OPCODE_STORE_LOCAL_VAR, iterSlot);
    // 如果条件失败跳出循环
    // 写入占位符
    loop.exitIndex = emitInstrWithPlaceholder(cu, OPCODE_JUMP_IF_FALSE);
    
    // 调用seq.iteratorValue(iter)以获取值
    writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, seqSlot);
    writeOpCodeByteOperand(cu, OPCODE_LOAD_LOCAL_VAR, iterSlot);
    emitCall(cu, 1, "iteratorValue(_)", 16);

    // 为循环变量i创建作用域
    enterScope(cu);
    addLocalVar(cu, loopVarName, loopVarLen);
    // 编译循环体
    compileLoopBody(cu);
    leaveScope(cu); // 离开循环变量i的作用域
    leaveLoopPatch(cu);
    leaveScope(cu); // 离开变量seq和iter的作用域
}

// 编译return
inline static void compileReturn(CompileUnit* cu) {
    if (PEEK_TOKEN(cu->curParser) == TOKEN_RIGHT_BRACE) {
        writeOpCode(cu, OPCODE_PUSH_NULL);
    } else {
        expression(cu, BP_LOWEST);
    }
    writeOpCode(cu, OPCODE_RETURN); // 返回栈顶的值
}

// 编译break
inline static void compileBreak(CompileUnit* cu) {
    if (cu->curLoop == NULL) {
        COMPILE_ERROR(cu->curParser, "break should be used inside a loop!");
    }
    // 退出循环体之前丢弃其局部变量
    discardLocalVar(cu, cu->curLoop->scopeDepth+1);
    // 由于用OPCODE_END表示break占位,此时无需记录占位符的返回地址
    emitInstrWithPlaceholder(cu, OPCODE_END);
}


// 编译语句(与声明,定义无关的,表示动作的代码)
static void compileStatment(CompileUnit* cu) {
    if (matchToken(cu->curParser, TOKEN_IF)) {
        compileIfStatment(cu);
    } else if (matchToken(cu->curParser, TOKEN_WHILE)) {
        compileWhileStatement(cu);
    } else if (matchToken(cu->curParser, TOKEN_FOR)) {
        compileForStatment(cu);
    } else if (matchToken(cu->curParser, TOKEN_RETURN)) {
        compileReturn(cu);
    } else if (matchToken(cu->curParser, TOKEN_BREAK)) {
        compileBreak(cu);
    } else if (matchToken(cu->curParser, TOKEN_CONTINUE)) {
        compileContinue(cu);
    } else if (matchToken(cu->curParser, TOKEN_LEFT_BRACE)) {
        // 代码块有单独的作用域
        enterScope(cu);
        compileBlock(cu);
        leaveScope(cu);
    } else {
        expression(cu, BP_LOWEST);
        // 弹出栈顶数据
        writeOpCode(cu, OPCODE_POP);
    }
}

// 生成模块变量存储指令
static void emitStoreModuleVar(CompileUnit* cu, int index) {
    // 把栈顶数据存储到moduleVarIndex
    writeOpCodeShortOperand(cu, OPCODE_STORE_MODULE_VAR, index);
    writeOpCode(cu, OPCODE_POP);
}

// 声明方法
static int declareMethod(CompileUnit* cu, char* signStr, uint32_t length) {
    // 确保方法被录入到vm->allMethodNames
    int index = ensureSymbolExist(cu->curParser->vm, &cu->curParser->vm->allMethodNames, signStr, length);
    IntBuffer* methods = cu->enclosingClassBK->inStatic ? &cu->enclosingClassBK->staticMethods:
    &cu->enclosingClassBK->instantMethods;
    uint32_t idx = 0;
    while (idx < methods->count) {
        if (methods->datas[idx] == index) {
            COMPILE_ERROR(cu->curParser, "repeat define method %s in class %s!", signStr, cu->enclosingClassBK->name->value.start);
        }
        idx++;
    }
    IntBufferAdd(cu->curParser->vm, methods, index);
    return index;
}

// 将方法methodIndex指代的方法装入classVar指代的class.methods中
static void defineMethod(CompileUnit* cu, Variable classVar, bool isStatic, int methodIndex) {
    // 1. 待绑定的方法已经在栈顶
    // 2. 将方法所属的类加载到栈顶
    emitLoadVariable(cu, classVar);
    // 3. 在运行时绑定
    OpCode opCode = isStatic ? OPCODE_STATIC_METHOD : OPCODE_INSTANCE_METHOD;
    writeOpCodeShortOperand(cu, opCode, methodIndex);
}

// 创建实例,constructorIndex是构造函数的索引
static void emitCreateInstance(CompileUnit* cu, Signature* sign, uint32_t constructIndex) {
    CompileUnit methodCU;
    initCompileUnit(cu->curParser, &methodCU, cu, true);
    // 1. 生成OPCODE_CONSTRUCE指令,该指令生成新实例存储到stack[0]中
    writeOpCode(&methodCU, OPCODE_CONSTRUCT);
    // 2. 生成OPCODE_CALLx指令,该指令调用新实例的构造函数
    writeOpCodeShortOperand(&methodCU, (OpCode)(OPCODE_CALL0+sign->argNum), constructIndex);
    // 生成return指令,将栈顶中的实例返回
    writeOpCode(&methodCU, OPCODE_RETURN);

#if DEBUG
    endCompileUnit(&methodCU, "", 0);
#else
    endCompileUnit(&methodCU);
#endif
}

// 编译方法定义
static void compileMethod(CompileUnit* cu, Variable classVar, bool isStatic) {
    // curToken是方法名
    cu->enclosingClassBK->inStatic = isStatic;
    methodSignatureFn methodSign = Rules[cu->curParser->curToken.type].methodSign;
    if (methodSign == NULL) {
        COMPILE_ERROR(cu->curParser, "method need signature function!");
    }
    Signature sign;
    sign.name = cu->curParser->curToken.start;
    sign.length = cu->curParser->curToken.length;
    sign.argNum = 0;
    cu->enclosingClassBK->signature = &sign;
    getNextToken(cu->curParser);

    // 为了单独存储函数方法自己的指令流和局部变量
    // 每个函数都有自己的CompileUnit
    CompileUnit methodCU;
    initCompileUnit(cu->curParser, &methodCU, cu, true);
    methodSign(&methodCU, &sign);
    consumeCurToken(cu->curParser, TOKEN_LEFT_BRACE, "expect '{' at the beginning of method body");
    if (cu->enclosingClassBK->inStatic && sign.type == SIGN_CONSTRUCT) {
        COMPILE_ERROR(cu->curParser, "constructor is not allowed to be static!");
    }
    char signatureString[MAX_SIGN_LEN] = {'\0'};
    uint32_t signLen = sign2String(&sign, signatureString);
    uint32_t methodIndex = declareMethod(cu, signatureString, signLen);
    compileBody(&methodCU, sign.type == SIGN_CONSTRUCT);

#if DEBUG
endCompileUnit(&methodCU, signatureString, signLen);
#else
endCompileUnit(&methodCU);
#endif
    // 定义方法:将上面创建的方法闭包绑定到类
    defineMethod(cu, classVar, cu->enclosingClassBK->inStatic, methodIndex);
    if (sign.type == SIGN_CONSTRUCT) {
        sign.type = SIGN_METHOD;
        char signatureString[MAX_SIGN_LEN] = {'\0'};
        uint32_t signLen = sign2String(&sign, signatureString);
        uint32_t constructIndex = ensureSymbolExist(cu->curParser->vm, &cu->curParser->vm->allMethodNames, signatureString, signLen);
        emitCreateInstance(cu, &sign, methodIndex);
        // 构造函数是静态方法,即类方法
        defineMethod(cu, classVar, true, constructIndex);
    }
}

// 编译类体
static void compileClassBody(CompileUnit* cu, Variable classVar) {
    if (matchToken(cu->curParser, TOKEN_STATIC)) { 
        if (matchToken(cu->curParser, TOKEN_VAR)) { // 处理静态域static var id
            compileVarDefinition(cu, true);
        } else {    // 静态方法
            compileMethod(cu, classVar, true);
        } 
    } else if (matchToken(cu->curParser, TOKEN_VAR)) {  // 实例域
        compileVarDefinition(cu, false);
    } else { // 类的方法
        compileMethod(cu, classVar, false);
    }
}

// 编译类定义
static void compileClassDefinition(CompileUnit* cu) {
    Variable classVar;
    if (cu->scopeDepth != -1) {
        COMPILE_ERROR(cu->curParser, "class definition must be in the module scope!");
    }

    classVar.scopeType = VAR_SCOPE_MODULE;
    consumeCurToken(cu->curParser, TOKEN_ID, "keyword class should follow by class name!"); // 读入类名
    classVar.index = declareVariable(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
    // 生成类名,用于创建类
    ObjString* className = newObjString(cu->curParser->vm, cu->curParser->preToken.start, cu->curParser->preToken.length);
    // 生成加载类名的指令
    emitLoadConstant(cu, OBJ_TO_VALUE(className));
    if (matchToken(cu->curParser, TOKEN_LESS)) { // 类继承
        expression(cu, BP_CALL); // 把类名加载到栈顶
    } else {    // 默认加载object类为基类
        emitLoadModuleVar(cu, "object");
    }
    // 创建类时不知道域的个数,先暂时写作255,以后回填
    int fieldNumIndex = writeOpCodeByteOperand(cu, OPCODE_CREATE_CLASS, 0xff);
    // 虚拟机执行完OPCODE_CREATE_CLASS后, 栈顶留下了创建好的类
    // 故现在可以用该类为之前声明的类名className赋值
    if (cu->scopeDepth == -1) {
        emitStoreModuleVar(cu, classVar.index);
    }
    ClassBookKeep classBK;
    classBK.name = className;
    classBK.inStatic = false;
    StringBufferInit(&classBK.fields);
    IntBufferInit(&classBK.instantMethods);
    IntBufferInit(&classBK.staticMethods);
    cu->enclosingClassBK = &classBK;

    consumeCurToken(cu->curParser, TOKEN_LEFT_BRACE, "expect '{' after class name in the class declaration!");

    // 进入类体
    enterScope(cu);

    while(!matchToken(cu->curParser, TOKEN_RIGHT_BRACE)) {
        compileClassBody(cu, classVar);
        if (PEEK_TOKEN(cu->curParser) == TOKEN_EOF) {
            COMPILE_ERROR(cu->curParser, "expect '}' after at the end of class declaration!");
        }
    }

    // 回填域定义数量
    // 域定义数量在compileVarDefinition统计
    cu->fn->instrStream.datas[fieldNumIndex] = classBK.fields.count;

    symbolTableClear(cu->curParser->vm, &classBK.fields);
    IntBufferClear(cu->curParser->vm, &classBK.instantMethods);
    IntBufferClear(cu->curParser->vm, &classBK.staticMethods);

    cu->enclosingClassBK = NULL;
    leaveScope(cu);
}

static void compileFunctionDefinition(CompileUnit* cu) {
    if (cu->enclosingUnit != NULL) {
        COMPILE_ERROR(cu->curParser, "'fun' should be in module scope");
    }
    consumeCurToken(cu->curParser, TOKEN_ID, "missing function name");
    // 函数名加上Fn作为模块变量存储
    char fnName[MAX_SIGN_LEN+4] = {'\0'}; // "fn xxx\0"
    memmove(fnName, "Fn ", 3);
    memmove(fnName+3, cu->curParser->preToken.start, cu->curParser->preToken.length);

    uint32_t fnNameIndex = declareVariable(cu, fnName, strlen(fnName));
    // 生成fnCU,专用于存储函数指令流
    CompileUnit fnCU;
    initCompileUnit(cu->curParser, &fnCU, cu, false);
    Signature tmpFnSign = {SIGN_METHOD, "", 0, 0};
    consumeCurToken(cu->curParser, TOKEN_LEFT_PAREN, "expect '(' after function name!");

    // 将形参声明为局部变量
    if (!matchToken(cu->curParser, TOKEN_RIGHT_PAREN)) {
        processParaList(&fnCU, &tmpFnSign);
        consumeCurToken(cu->curParser, TOKEN_RIGHT_PAREN, "expect ')' after parametre list!");
    }
    fnCU.fn->argNum = tmpFnSign.argNum;
    consumeCurToken(cu->curParser, TOKEN_LEFT_BRACE, "expect '{' at the beginning of method body.");
    // 编译函数体,将指令流写进该函数自己的指令单元fnCU
    compileBody(&fnCU, false);

#if DEBUG
    endCompileUnit(&fnCU, fnName, strlen(fnName));
#else
    endCompileUnit(&fnCU);
#endif
    defineVariable(cu, fnNameIndex);
}

// 编译import导入
static void compileImport(CompileUnit* cu) {
    // import "foo"
    // 转化为
    // System.importModule("foo")
    // import foo for bar1, bar2
    // 转化为
    // bar1 = System.getModuleVariable("foo", "bar1")
    // bar2 = System.getModuleVariable("foo", "bar2")
    consumeCurToken(cu->curParser, TOKEN_ID, "expect module name after export!");
    Token moduleNameToken = cu->curParser->preToken;
    // 过滤扩展名
    if (cu->curParser->preToken.start[cu->curParser->preToken.length] == '.') {
        printf("\nwarning!!! the imported module needn't extension!, compiler try to ignor it!");
        getNextToken(cu->curParser);    // 跳过.
        getNextToken(cu->curParser);    // 跳过extension
    }
    ObjString* moduleName = newObjString(cu->curParser->vm, moduleNameToken.start, moduleNameToken.length);
    uint32_t constModIdx = addConstant(cu, OBJ_TO_VALUE(moduleName));
    // 为调用System.importModule("foo")压入参数system
    emitLoadModuleVar(cu, "System");
    // 压入参数foo
    writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, constModIdx);
    // 调用
    emitCall(cu, 1, "importModule(_)", 15);
    // 回收返回值args[0]的空间
    writeOpCode(cu, OPCODE_POP);
    
    if (!matchToken(cu->curParser, TOKEN_FOR)) {
        return;
    }
    do {
        consumeCurToken(cu->curParser, TOKEN_ID, "expect variable name after 'for' in import!"); 
        uint32_t varIdx = declareVariable(cu, cu->curParser->preToken.start, cu->curParser->preToken.length);
        ObjString* constVarName = newObjString(cu->curParser->vm, cu->curParser->preToken.start, cu->curParser->preToken.length);
        uint32_t constVarIdx = addConstant(cu, OBJ_TO_VALUE(constVarName));
        
        // 为调用System.getModuleVariable("foo", "bar1")压入system
        emitLoadModuleVar(cu, "System");
        // 压入foo
        writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, constModIdx);
        // 压入bar1
        writeOpCodeShortOperand(cu, OPCODE_LOAD_CONSTANT, constVarIdx);
        // 调用
        emitCall(cu, 2, "getModuleVariable(_,_)", 22);
        defineVariable(cu, varIdx);
    } while(matchToken(cu->curParser, TOKEN_COMMA));
}

// 编译程序
static void compileProgram(CompileUnit* cu) {
    if (matchToken(cu->curParser, TOKEN_CLASS)) {
        compileClassDefinition(cu);
    } else if (matchToken(cu->curParser, TOKEN_FUN)) {
        compileFunctionDefinition(cu);
    } else if (matchToken(cu->curParser, TOKEN_VAR)) {
        compileVarDefinition(cu, cu->curParser->preToken.type == TOKEN_STATIC);
    } else if (matchToken(cu->curParser, TOKEN_IMPORT)) {
        compileImport(cu);
    } else {
        compileStatment(cu);
    }
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
    // 记录现在模块变量的数量, 后面检查预定义模块变量时可减少遍历
    uint32_t moduleVarNumBefor = objModule->moduleVarValue.count;
    // 初始的parser->curToken.type为TOKEN_UNKNOWN,下面使其指向第一个合法的token
    getNextToken(&parser);
    
    while (!matchToken(&parser, TOKEN_EOF)) {
        compileProgram(&moduleCU);
    }
    
    // 编译模块完成， 生成return null 返回，避免执行下面endCompileUnit中添加的OPCODE_END
    writeOpCode(&moduleCU, OPCODE_PUSH_NULL);
    writeOpCode(&moduleCU, OPCODE_RETURN);
    
    uint32_t idx = moduleVarNumBefor;
    while (idx < objModule->moduleVarValue.count) {
        if (VALUE_IS_NUM(objModule->moduleVarValue.datas[idx])) {
            char* str = objModule->moduleVarName.datas[idx].str;
            ASSERT(str[objModule->moduleVarName.datas[idx].length] == '\0', "module var name is not closed!");
            uint32_t lineNo = VALUE_TO_NUM(objModule->moduleVarValue.datas[idx]);
            COMPILE_ERROR(&parser, "line:%d, variable '%s' not defined!", lineNo, str);
        }
        idx++;
    }
    // 编译完成后置空
    vm->curParser->curCompileUnit = NULL;
    vm->curParser = vm->curParser->parent;

#if DEBUG
    return endCompileUnit(&moduleCU, "(script)", 8);
#else
    return endCompileUnit(&moduleCU);
#endif
}

// 标识compileUnit使用的所有堆分配的对象(及其所有父对象)可达,以使它们不被GC收集
void grayCompileUnit(VM* vm, CompileUnit* cu) {
   grayValue(vm, vm->curParser->curToken.value);
   grayValue(vm, vm->curParser->preToken.value);

   // 向上遍历父编译器外层链 使其fn可到达
   // 编译结束后,vm->curParser会在endCompileUnit中置为NULL,
   // 本函数是在编译过程中调用的,即vm->curParser肯定不为NULL,
   ASSERT(vm->curParser != NULL, "only called while compiling!");
   do {
      grayObject(vm, (ObjHeader*)cu->fn);
      cu = cu->enclosingUnit;
   } while (cu != NULL);
}