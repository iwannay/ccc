#include "cli.h"
#include <stdio.h>
#include <string.h>
#include "parser.h"
#include "utils.h"
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
    printf("There is something to do...\n");exit(0);
    VM* vm = newVM(); 
    const char* sourceCode = readFile(path);
    executeModule(vm, OBJ_TO_VALUE(newObjString(vm, path, strlen(path))), sourceCode);
}

static void runCli(void) {
    VM* vm = newVM();
    char sourceLine[MAX_LINE_LEN];
    char source[MAX_SOURCE_CODE_LEN];
    char endStr = '\n';
    printf("\033[36mccc version: 0.1\033[0m\n");
    
    while (true) {
        if (endStr == '\\') {
            printf("\033[32m...\033[0m ");
        } else {
            printf("\033[34m>>>\033[0m ");
        }
        
        if (!fgets(sourceLine, MAX_LINE_LEN, stdin) || memcmp(sourceLine, "quit", 4) == 0) {
            break;
        }
        int l = strlen(sourceLine);
        endStr = sourceLine[l-2]; 
        strcat(source, sourceLine);
        if (strlen(source) >= MAX_SOURCE_CODE_LEN) {
            IO_ERROR("source code len exceeded %d", MAX_SOURCE_CODE_LEN);
        } else if (endStr != '\\') {
            executeModule(vm, OBJ_TO_VALUE(newObjString(vm, "cli", 3)), source);
        } else {
            source[strlen(source)-2] = ' ';
        }
        
    }
    freeVM(vm);
}

int main(int argc, const char** argv) {
    if (argc == 1) {
        runCli();
    } else {
        runFile(argv[1]);
    }

    return 0;
}