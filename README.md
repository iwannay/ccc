# 这是一个功能健全的编译器

## 关于这个小语言

简单起见语言就叫 ccc, 主要用于学习之用. 整个语言实现基于郑刚的 "自制编程语言" 这本书. 感谢郑刚! 没有他的这本书就不会有 ccc. 向每一位知识的传递者致敬. ccc 以后也会继续更新, 用于实践自己的一些小的技术上的想法.

ccc 一切皆对象, 内部实现了: 线程切换 垃圾回收 闭包 模块等

## 基本数据类型的实现

ccc 对于大多数据存储采用大端字节序

## 设计架构

### 底层数据类型

```c
// 对象头用于保存对象的基本信息
// 整个ccc的所有基本数据类型的相关实现都会引用ObjHeader
typedef struct objHeader {
    ObjType type; // 对象类型
    bool isDark; // 对象是否可达
    Class* class; // 对象所属的类
    struct objHeader* next; // 用于链接所有已分配的对象
} ObjHeader; // 对象头，用于记录元信息和垃圾回收

// 通用值类型用于保存各种数据
typedef struct {
    ValueType type;
    union {
        double num;
        ObjHeader* objHeader;
    };
} Value; // 通用的值结构

// 声明buffer, 该宏定义用于声明所有的需要存储的数据类型
// 比如valueBuffer, stringBuffer, intBuffer...
#define DECLARE_BUFFER_TYPE(type)\
    typedef struct {\
        /* 数据缓冲区 */ \
        type* datas;\
        /* 缓冲区已使用的元素个数 */ \
        uint32_t count;\
        /* 缓冲区容量用 */\
        uint32_t capacity;\
    } type##Buffer;\
    void type##BufferInit(type##Buffer* buf);\
    void type##BufferFillWrite(VM* vm, type##Buffer* buf, type data, uint32_t fillCount); \
    void type##BufferAdd(VM* vm, type##Buffer* buf, type data); \
    void type##BufferClear(VM* vm, type##Buffer* buf);
```

### Map(字典)

用于 ccc 实现的 map 数据结构

```c
// 用于保存key:value数据对
typedef struct {
    Value key;
    Value value;
} Entry;

// Map数据对象
typedef struct {
    ObjHeader objHeader;
    uint32_t capacity; // Entry的容量，包括已使用和未使用的entry的数量
    uint32_t count; // map中使用的Entry的数量
    Entry* entries; // Entry 数组
} ObjMap;
```

哈希方法基于开放定址算法,基本思路是,首先对 key 取 hash 值, 然后除以 capacity.capacity 取余, 结果即为 ObjMap.Entries 中的数据槽位,如果槽位冲突槽位 取临近的下一个槽位,以此类推,直至找到空槽位保存 entry

### String(字符串)

```c
typedef struct {
    ObjHeader objHeader;
    uint32_t hashCode; // 字符串的hash
    CharValue value;
} ObjString;
```

ObjString 为整个 ccc 的字符串类型的底层实现

### List (列表)

```c
typedef struct {
    ObjHeader objHeader;
    ValueBuffer elements; // list 中的元素
} ObjList;
```

elements 是一个数组, 对 List 的相关操作基于 elements 实现

### Range (范围)

```c
typedef struct {
    ObjHeader objHeader;
    int from; // 范围起始
    int to; // 范围结束
} ObjRange;
```

range 用于实现范围操作

### Func (函数)

```c
typedef struct {
    ObjHeader objHeader;
    ByteBuffer instrStream; // 函数编译后的指令流
    ValueBuffer constants; // 函数中的常量表
    ObjModule* module; // 本函数所属的模块

    // 本函数最多需要的栈空间，是使用空间的峰值
    uint32_t maxStackSlotUsedNum;
    uint32_t upvalueNum; // 本函数所涵盖的upvalue数量
    uint8_t argNum; // 函数期望的参数个数
} ObjFn; // 函数对象
```

ccc 源码中的函数,对象方法,类方法,闭包,模块,类最终都会转化为函数对象 ObjFn,函数对象 ObjFn 并不单指函数,是一切可以调度的执行单元,在编译阶段函数的指令流会被写入 instrStream 中, 一个函数对象即是一个执行单元, 一个执行单元可以嵌套多个子执行单元.

constants 用于记录函数中的常量(字面量,函数闭包)

instrStream 用于记录指令+操作数,很多时候操作数实际上就是需要被加载的常量在 constants 中的索引

### Class (类)

```c
// 原生方法指针
typedef bool (*Primitive)(VM* vm, Value* args);

typedef struct method {
    MethodType type; // union 中的值由type的值决定
    union {
        // 指向脚本方法所关联的c实现
        Primitive primFn;
        // 指向脚本代码编译后的ObjClosure或ObjFn
        ObjClosure* obj;
    };
} Method;

DECLARE_BUFFER_TYPE(Method)

// 类是对象的模板
struct class {
    ObjHeader objHeader;
    struct class* superClass; // 父类
    uint32_t fieldNum; // 本体的字段数，包括基类的字段数
    MethodBuffer methods; // 本体的方法
    ObjString* name; // 类名
}; // 对象类
```

类是对象的模板, 每个对象都会关联到一个具体的类上, class.Methods 中记录了该类中所有的方法, 需要说明的是,class.Methods.datas 中的索引记录实际是 vm.allMethodNames 中的索引,vm 是 ccc 中使用的虚拟机

## vm (虚拟机)

虚拟机用于执行经过编译后生成的指令, 在虚拟机中保留了整个源代码中使用的符号表

```c
struct vm {
    Class* classOfClass;
    Class* objectClass; // objectClass是所有类的基类
    Class* stringClass; // 字符串类
    Class* mapClass;    // 字典类
    Class* rangeClass;  // 范围类
    Class* listClass;   // 切片类
    Class* nullClass;   // null类
    Class* boolClass;   // bool类
    Class* numberClass; // number类
    Class* fnClass;     // 函数类
    Class* threadClass; // 线程类
    uint32_t allocatedBytes; // 累计已分配的内存量
    Parser* curParser; // 当前词法分析器
    ObjHeader* allObjects; // 所有已分配的对象链表,用于垃圾回收
    SymbolTable allMethodNames; // 符号表中的存放的是所有类的所有方法
    ObjMap* allModules; // 记录源码中所有的模块
    ObjThread* curThread; // 当前正在执行的线程

    // 临时的根对象集合,存储被gc保留的对象,避免回收
    ObjHeader* tmpRoots[MAX_TEMP_ROOTS_NUM];
    uint32_t tmpRootNum;
    Gray grays; // gc时被标记为灰色的对象
    Configuration config; // gc配置
};

```

## 线程调度

```c
typedef struct objThread {
    ObjHeader objHeader;

    Value* stack; // 运行时栈的栈底
    Value* esp; // 运行时栈的栈顶
    uint32_t stackCapacity; // 栈容量

    Frame* frames; // 调用框架
    uint32_t usedFrameNum; // 已使用的frame数量
    uint32_t frameCapacity; // frame容量

    // "打开的upvalue" 的链表首节点
    ObjUpvalue* openUpvalues;

    // 当前thread的调用者
    struct objThread* caller;

    // 导致运行时错误的对象会放在此处，否则为空
    Value errorObj;
} ObjThread; // 线程对象
```

## 执行流程

### 赋值

形如 var a = "hello world" 编译器按如下步骤解析:

1. 词法分析生成 token(词素), 遍历 token,token.name = "var",此时 token.type == TOKEN_VAR 进入 compileVarDefinition 分支
2. token.name = a,此时 token.type = TOKEN_ID, 记录变量名 varName
3. token.name = "=" 此时调用 express() 计算右值
4. token.name = "hello world" 进入字符串的.nud()方法,即 literal()方法. 将字符串字面量"hello world" 写入常量表 fn->constants 中,并返回索引 index
5. 向 cu->fn->instrStream 依次写入操作码:OPCODE_LOAD_CONSTANT 操作数: index
6. 在模块变量名符号表 module->moduleVarName.datas 中写入 varName 记录索引 symbolIndex
7. 向 fn->instrStream 依次写入操作码:OPCODE_STORE_MODULE_VAR 操作数:symbolIndex, 操作码:OPCODE_POP

编译器生成对应字节码后,下一步需要虚拟机解析字节码并执行:

1. 执行 OPCODE_LOAD_CONSTANT 将 fn->constants[index] 压栈(thread->esp++ = fn->constants[index])
2. 执行 OPCODE_STORE_MODULE_VAR, fn->module->moduleVarValue.datas[symbolIndex] = thread->esp-1. 完毕

## 垃圾回收(待续)

## 启动

```sh
# debug模式编译（打印词素，操作码，操作数）
make -f makefile.debug r
# 普通模式编译
make r

# 交互式命令行模式运行
./ccc

# 文件模式运行
./ccc sample/family.ccc 
```

    
