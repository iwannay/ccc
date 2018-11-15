#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include "common.h"
#include "utils.h"
#include "unicodeUtf8.h"
#include <string.h>
#include <ctype.h>

struct keywordToken {
    char* keyword;
    uint8_t length;
    TokenType token;
}; // 关键字保留字结构

// 关键字查找表
struct keywordToken keywordsToken[] = {
    {"var", 3, TOKEN_VAR},
    {"fun", 3, TOKEN_FUN},
    {"if", 2, TOKEN_IF},
    {"else", 4, TOKEN_ELSE},
    {"true", 4, TOKEN_TRUE},
    {"false", 5, TOKEN_FALSE},
    {"while", 5, TOKEN_WHILE},
    {"for", 3, TOKEN_FOR},
    {"break", 5, TOKEN_BREAK},
    {"continue", 8, TOKEN_CONTINUE},
    {"return", 6, TOKEN_RETURN},
    {"null", 4, TOKNE_NULL},
    {"class", 5, TOKEN_CLASS},
    {"is", 2, TOKEN_IS},
    {"static", 6, TOKEN_STATIC},
    {"this", 4, TOKEN_THIS},
    {"super", 5, TOKEN_SUPER},
    {"import", 6, TOKEN_IMPORT},
    {"null", 0, TOKEN_UNKNOWN}
};

// 判断start是否为关键字并返回相应的token
static TokenType idOrKeyword(const char* start, uint32_t length) {
    uint32_t idx = 0;
    while (keywordsToken[idx].keyword != NULL) {
        if (keywordsToken[idx].length == length && \
        memcmp(keywordsToken[idx].keyword, start, length) == 0) {
            return keywordsToken[idx].token;
        }
        idx++;
    }
    return TOKEN_ID;
}

// 向前看一个字符
char lookAheadChar(Parser* parser) {
    return *parser->nextCharPtr;
}

// 获取下一个字符
static void getNextChar(Parser* parser) {
    parser->curChar = *parser->nextCharPtr++;
}

// 查看下一个字符是否为期望的
static bool matchNextChar(Parser* parser, char expectedChar) {
    if (lookAheadChar(parser) == expectedChar) {
        getNextChar(parser);
        return true;
    }
    return false;
}

// 跳过连续的空白字符
static void skipBlanks(Parser* parser) {
    while(isspace(parser->curChar)) {
        if (parser->curChar == '\n') {
            parser->curToken.lineNo++;
        }
        getNextChar(parser);
    }
}

static void parseId(Parser* parser, TokenType type) {
    while (isalnum(parser->curChar) || parser->curChar == '_') {
        getNextChar(parser);
    }

    // nextCharPtr 会指向第1个不合法字符的下一个字符，因此-1
    uint32_t length = (uint32_t) (parser->nextCharPtr - parser->curToken.start - 1);
    if (type != TOKEN_UNKNOWN) {
        parser->curToken.type = type;
    } else {
        parser->curToken.type = idOrKeyword(parser->curToken.start, length);
    }
    parser->curToken.length = length;
}

// 解析unicode 码点
static void parseUincodeCodePoint(Parser* parser, ByteBuffer* buf) {
    uint32_t idx = 0;
    int value = 0;
    uint8_t digit = 0;

    // 获取数值，u后面跟着4位十六进制数字
    while(idx++ < 4) {
        getNextChar(parser);
        if (parser->curChar == '\0') {
            LEX_ERROR(parser, "unterminated unicode!");
        }

        if (parser->curChar >= '0' && parser->curChar <= '9') {
            digit = parser->curChar - '0';
        } else if (parser->curChar >= 'a' && parser->curChar <= 'f') {
            digit = parser->curChar - 'a' + 10;
        } else {
            LEX_ERROR(parser, "invalid unicode!");
        }
        value = value * 16 | digit;
    }

    uint32_t byteNum = getByteNumOfEncodeUtf8(value);
    ASSERT(byteNum != 0, "utf8 encode bytes should be between 1 and 4!");

    // 为代码通用，下面会直接写buf->datas, 在此先写入byteNum个0, 以保证事先有byteNum个空间
    ByteBufferFillWrite(parser->vim, buf, 0, byteNum);

    // 把value 编码为UTF-8后写入缓冲区buf
    encodeUtf8(buf->datas + buf->count - byteNum, value);
}

// 解析字符串
static void parseString(Parser* parser) {
    ByteBuffer str;
    ByteBufferInit(&str);
    while (true) {
        getNextChar(parser);

        if (parser->curChar == '\0') {
            LEX_ERROR(parser, "unterminated string");
        }

        if (parser->curChar == '"') {
            parser->curToken.type = TOKEN_STRING;
            break;
        }

        if (parser->curChar == '%') {
            if (!matchNextChar(parser, '(')) {
                LEX_ERROR(parser, "'%' should followd by '('!");
            }
            if (parser->interpolationExpectRightParenNum > 0) {
                COMPILE_ERROR(parser, "sorry, i don't support next interpolate expression!");
            }
            parser->interpolationExpectRightParenNum = 1;
            parser->curToken.type = TOKEN_INTERPOLATION;
            break;
        }

        // 处理转义字符
        if (parser->curChar == '\\') {
            getNextChar(parser);
            switch (parser->curChar) {
                case '0':
                    ByteBufferAdd(parser->vm, $str, '\0');
                    break;
                case 'a':
                    ByteBufferAdd(parser->vm, &str, '\a');
                    break;
                case 'b':
                    ByteBufferAdd(parser->vm, &str, '\b');
                    break;
                case 'f':
                    ByteBufferAdd(parser->vm, &str, '\f');
                    break;
                case 'n':
                    ByteBufferAdd(parser->vm, &str, '\n');
                    break;
                case 'r':
                    ByteBufferAdd(parser->vm, &str, '\r');
                    break;
                case 't':
                    ByteBufferAdd(parser->vm, &str, '\t');
                    break;
                case 'u':
                    ByteBufferAdd(parser->vm, &str, '\u');
                    break;
                case '"':
                    ByteBufferAdd(parser->vm, &str, '"');
                    break;
                case '\\':
                    ByteBufferAdd(parser->vm, &str, '\\');
                    break;
                default:
                    LEX_ERROR(parser, "unsupport escape \\%c", parser->curChar);
                    break;
            }
        } else { // 普通字符串
            ByteBufferAdd(parser->vm, &str, parser->curChar);
        }
    }
    ByteBufferClear(parser->vm, &str, parser->curChar);
}