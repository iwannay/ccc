#ifndef _VM_CORE_H
#define _VM_CORE_H
#include "vm.h"
extern char* rootDir;
char* readFile(const char* sourceFile);
int getIndexFromSymbolTable(SymbolTable* table, const char* symbol, uint32_t length);
int ensureSymbolExist(VM* vm, SymbolTable* table, const char* symbol, uint32_t length);
void bindSuperClass(VM* vm, Class* subClass, Class* superClass);
void bindMethod(VM* vm, Class* class, uint32_t index, Method method);
int addSymbol(VM* vm, SymbolTable* table, const char* symbol, uint32_t length);
VMResult executeModule(VM* vm, Value moduleName, const char* moduleCode);
void buildCore(VM* vm);
#endif