#include "platform/memory.hpp"

#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace jitvm {
namespace {

constexpr size_t kReserve = 0x1000000;
constexpr size_t kPage = 0x1000;

size_t page_align(size_t n) {
	return (n + kPage - 1) & ~(kPage - 1);
}

CodeBlock* g_blocks[8];
int g_block_count = 0;

}  // namespace

void* mem_alloc(size_t size) {
	void* p = malloc(size);
	if (p)
		memset(p, 0, size);
	return p;
}

void mem_free(void* p) {
	free(p);
}

CodeBlock* code_reserve() {
	if (g_block_count >= 8)
		return nullptr;
	CodeBlock* block = (CodeBlock*)mem_alloc(sizeof(CodeBlock));
	if (!block)
		return nullptr;
#if defined(_WIN32)
	block->base = (uint8_t*)VirtualAlloc(nullptr, kReserve, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_JIT)
	flags |= MAP_JIT;
#endif
	void* mapped = mmap(nullptr, kReserve, PROT_NONE, flags, -1, 0);
	block->base = mapped == MAP_FAILED ? nullptr : (uint8_t*)mapped;
#endif
	if (!block->base) {
		mem_free(block);
		return nullptr;
	}
	block->reserved = kReserve;
	block->committed = 0;
	block->used = 0;
	g_blocks[g_block_count++] = block;
	return block;
}

bool code_commit(CodeBlock* block, size_t need) {
	if (!block)
		return false;
	need += 16;
	if (block->committed - block->used >= need)
		return true;
	size_t end = page_align(block->used + need);
	if (end > block->reserved)
		return false;
	size_t extra = end - block->committed;
	if (!extra)
		return true;
#if defined(_WIN32)
	uint8_t* page = (uint8_t*)VirtualAlloc(block->base + block->committed, extra, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (page != block->base + block->committed)
		return false;
#else
	if (mprotect(block->base + block->committed, extra, PROT_READ | PROT_WRITE | PROT_EXEC) != 0)
		return false;
#endif
	block->committed = end;
	return true;
}

uint8_t* code_alloc(uint32_t size) {
	for (int i = 0; i < g_block_count; i++) {
		CodeBlock* block = g_blocks[i];
		if (!code_commit(block, size))
			continue;
		size_t align = sizeof(void*);
		if (block->used % align) {
			size_t pad = align - (block->used % align);
			if (block->committed - block->used < pad)
				pad = block->committed - block->used;
			for (size_t n = 0; n < pad; n++)
				block->base[block->used++] = 0x90;
		}
		if (block->committed - block->used < size)
			continue;
		return block->base + block->used;
	}
	if (!code_reserve())
		return nullptr;
	return code_alloc(size);
}

int32_t code_grow(void* code, uint32_t size) {
	uint8_t* p = (uint8_t*)code;
	for (int i = 0; i < g_block_count; i++) {
		CodeBlock* block = g_blocks[i];
		if (p < block->base || p >= block->base + block->committed)
			continue;
		if (block->base + block->used != p)
			return 0;
		if (block->committed - block->used < size)
			return 0;
		block->used += size;
		return (int32_t)size;
	}
	return 0;
}

int code_contains(uintptr_t pc) {
	for (int i = 0; i < g_block_count; i++) {
		CodeBlock* block = g_blocks[i];
		if (pc >= (uintptr_t)block->base && pc < (uintptr_t)block->base + block->committed)
			return 1;
	}
	return 0;
}

void* stack_alloc(size_t size) {
#if defined(_WIN32)
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
#else
	void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
#endif
}

void code_flush(void* p, size_t n) {
	if (!p || !n)
		return;
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), p, n);
#else
	__builtin___clear_cache((char*)p, (char*)p + n);
#endif
}

}  // namespace jitvm
