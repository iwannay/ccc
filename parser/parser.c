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