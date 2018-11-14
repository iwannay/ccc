#ifndef _PARSER_PARSER_H
#define _PARSER_PARSER_H
#include "common.h"
#include "vm.h"

typedef enum {
    TOKEN_UNKNOWN,
    // 数据类型
    TOKEN_NUM,
    TOKEN_STRING,
    TOKEN_ID,
    TOKEN_INTERPOLATION, // 内嵌表达式
    
    // 关键字
    TOKEN_VAR,
    TOKEN_FUN,
    TOKEN_IF,
    TOKEN_ELSE,
    TOKEN_TRUE,
    TOKEN_FALSE,
    TOKEN_WHILE,
    TOKEN_FOR,
    TOKEN_BREAK,
    TOKEN_CONTINUE,
    TOKEN_RETURN,
    TOKNE_NULL,

    // 关键字和模块导入的token
    TOKEN_CLASS,
    TOKEN_THIS,
    TOKEN_STATIC,
    TOKEN_IS,
    TOKEN_SUPER,
    TOKEN_IMPORT,

    // 分隔符
    TOKEN_COMMA,
    TOKEN_COLON,
    TOKEN_LEFT_PAREN,
    TOKEN_RIGHT_PAREN,
    TOKEN_LEFT_BRACKET,
    TOKEN_RIGHT_BRACKET,
    TOKEN_LEFT_BRACE,
    TOKEN_RIGHT_BRACE,
    TOKEN_DOT,
    TOKEN_DOT_DOT,

    // 简单双目运算符
    TOKEN_ADD,
    TOKEN_SUB,
    TOKEN_MUL,
    TOKEN_DIV,
    TOKEN_MOD,

    // 赋值运算符
    TOKEN_ASSIGN,

    // 位运算符
    TOKEN_BIT_AND,
    TOKEN_BIT_OR,
    TOKEN_BIT_NOT,
    TOKEN_BIT_SHIFT_RIGHT,
    TOKEN_BIT_SHIFT_LEFT,

    // 逻辑运算符
    TOKEN_LOGIC_AND,
    TOKEN_LOGIC_OR,
    TOKEN_LOGIC_NOT,

    // 关系操作符
    TOKEN_EQUAL,
    TOKEN_NOT_EQUAL,
    TOKEN_GREATE,
    TOKEN_GREATE_EQUAL,
    TOKEN_LESS,
    TOKEN_LESS_EQUAL,

    TOKEN_QUESTION,

    TOKEN_EOF,
} TokenType;

typedef struct {
    TokenType type;
    const char start;
    uint32_t length;
    uint32_t lineNo;
} Token;

struct parser {
    const char* file;
    const char* sourceCode;
    const char* nextCharPtr;
    char curChar;
    Token curToken;
    Token preToken;

    // 处于内嵌表达式中期望的右括号数量，用于跟踪小括号对的嵌套
    int interpolationExpectRightParenNum;
    VM* vim;
};

#define PEEK_TOKEN(parserPtr) parserPtr->curToken.type

char lookAheadChar(Parser* parser);
void getNextToken(Parser* parser);
bool matchtoken(Parser* parser, TokenType expected);
void consumeCurToken(Parser* parser, TokenType expected, const char* errMsg);
void consumeNextToken(Parser* parser, TokenType expected, const char* errMsg);
uint32_t getByteNumOfEncodeUtf8(int value);
uint8_t encodeUtf8(uint8_t* buf, int value);
void initParser(VM* vm, Parser* parser, const char* file, const char* sourceCode);
#endif