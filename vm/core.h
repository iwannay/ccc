#ifndef _VM_CORE_H
#define _VM_CORE_H
extern char* rootDir;
char* readFile(const char* sourceFile);
int getIndexFromSymbolTable(SymbolTable* table, const char* symbol, uint32_t length);
#endif