#include "cli.h"
#include <stdio.h>
#include <string.h>
#include "parser.h"
#include "vm.h"
#include "core.h"

static void runFile(const char* path) {
    const char* lastSlash = strrchr(path, '/');
    if (lastSlash != NULL) {
        char* root = (char*)malloc(lastSlash - path + 2);
        memcpy(root, path, lastSlash - path + 1);
        root[lastSlash - path + 1] = '\0';
        rootDir = root;
    }

    VM* vm = newVM();
    const char* sourceCode = readFile(path);

    struct parser parser;
    initParser(vm, &parser, path, sourceCode, NULL); // 临时用NULL

    #include "token.list"
    while (parser.curToken.type != TOKEN_EOF) {    
        getNextToken(&parser);
        // printf("--%d---%s-----%c\n",parser.curToken.type, tokenArray[parser.curToken.type], parser.curChar);
        printf("%dL: %s [", parser.curToken.lineNo, tokenArray[parser.curToken.type]);
        uint32_t idx = 0;
        while (idx < parser.curToken.length) {
            printf("%c", *(parser.curToken.start+idx++));
        }
        printf("]\n");
    }
}

int main(int argc, const char** argv) {
    if (argc == 1) {
        ;
    } else {
        runFile(argv[1]);
    }

    return 0;
}