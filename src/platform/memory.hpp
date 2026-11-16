#ifndef JITVM_PLATFORM_MEMORY_HPP
#define JITVM_PLATFORM_MEMORY_HPP

#include <stddef.h>
#include <stdint.h>

namespace jitvm {

struct CodeBlock {
	uint8_t* base;
	size_t reserved;
	size_t committed;
	size_t used;
};

CodeBlock* code_reserve();
bool code_commit(CodeBlock* block, size_t need);
uint8_t* code_alloc(uint32_t size);
int32_t code_grow(void* code, uint32_t size);
int code_contains(uintptr_t pc);
void code_flush(void* p, size_t n);

void* mem_alloc(size_t size);
void mem_free(void* p);
void* stack_alloc(size_t size);

}  // namespace jitvm

#endif
