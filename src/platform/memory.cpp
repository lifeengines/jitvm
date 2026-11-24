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

constexpr size_t kPage = 0x1000;
constexpr size_t kGranule = 0x10000;
constexpr size_t kCommitQuantum = 0x10000;
constexpr int kMaxBlocks = 16;
// Stubs reach each other with rel32, so every block must sit inside this span.
constexpr uintptr_t kReach = 0x7F000000;
constexpr size_t kPreferred[] = {0x10000000, 0x4000000, 0x1000000, 0x100000, 0x10000};

size_t page_align(size_t n) {
	return (n + kPage - 1) & ~(kPage - 1);
}

#if defined(_WIN32)
bool range_writable(uint8_t* p, size_t n) {
	uint8_t* end = p + n;
	while (p < end) {
		MEMORY_BASIC_INFORMATION info;
		if (!VirtualQuery(p, &info, sizeof(info)))
			return false;
		if (info.State != MEM_COMMIT || (info.Protect & PAGE_GUARD))
			return false;
		DWORD prot = info.Protect & 0xFF;
		bool write = prot == PAGE_READWRITE || prot == PAGE_EXECUTE_READWRITE ||
			prot == PAGE_WRITECOPY || prot == PAGE_EXECUTE_WRITECOPY;
		if (!write)
			return false;
		uint8_t* next = (uint8_t*)info.BaseAddress + info.RegionSize;
		if (next <= p)
			return false;
		p = next;
	}
	return true;
}

// The runtime heap hook can report a fixed-address MEM_COMMIT as successful
// while leaving the page reserved or executable-only. new_table then faults
// on the first store. Ask for read/write/execute and refuse the pointer
// unless the pages are actually writable.
bool ensure_rwx(uint8_t* p, size_t n) {
	if (!p || !n)
		return false;
	DWORD old = 0;
	VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &old);
	return range_writable(p, n);
}
#endif

CodeBlock* g_blocks[kMaxBlocks];
int g_block_count = 0;

uintptr_t granule_up(uintptr_t n) {
	return (n + (kGranule - 1)) & ~(uintptr_t)(kGranule - 1);
}

bool span_ok(uintptr_t base, size_t bytes) {
	if (base < 0x10000 || bytes == 0 || base + bytes < base)
		return false;
	uintptr_t lo = base;
	uintptr_t hi = base + bytes;
	for (int i = 0; i < g_block_count; i++) {
		uintptr_t b = (uintptr_t)g_blocks[i]->base;
		uintptr_t e = b + g_blocks[i]->reserved;
		if (b < lo)
			lo = b;
		if (e > hi)
			hi = e;
	}
	return hi - lo <= kReach;
}

void release_region(uint8_t* base, size_t bytes) {
	if (!base)
		return;
#if defined(_WIN32)
	(void)bytes;
	VirtualFree(base, 0, MEM_RELEASE);
#else
	munmap(base, bytes);
#endif
}

uint8_t* reserve_at(void* addr, size_t bytes) {
#if defined(_WIN32)
	return (uint8_t*)VirtualAlloc(addr, bytes, MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;
#if defined(MAP_JIT)
	flags |= MAP_JIT;
#endif
	void* mapped = mmap(addr, bytes, PROT_NONE, flags, -1, 0);
	if (mapped == MAP_FAILED)
		return nullptr;
	if (addr && mapped != addr) {
		munmap(mapped, bytes);
		return nullptr;
	}
	return (uint8_t*)mapped;
#endif
}

uint8_t* reserve_near(size_t bytes) {
	if (g_block_count == 0)
		return reserve_at(nullptr, bytes);
	uintptr_t anchor = (uintptr_t)g_blocks[0]->base;
	uintptr_t up = (uintptr_t)g_blocks[g_block_count - 1]->base + g_blocks[g_block_count - 1]->reserved;
	uintptr_t addr = granule_up(up);
	uintptr_t hi_limit = anchor + kReach;
	while (addr < hi_limit && addr + bytes >= addr && span_ok(addr, bytes)) {
#if defined(_WIN32)
		MEMORY_BASIC_INFORMATION info;
		if (!VirtualQuery((void*)addr, &info, sizeof(info)))
			break;
		uintptr_t region = (uintptr_t)info.BaseAddress;
		uintptr_t region_end = region + info.RegionSize;
		if (info.State == MEM_FREE) {
			uintptr_t slot = granule_up(addr > region ? addr : region);
			if (slot >= region && region_end >= slot + bytes && span_ok(slot, bytes)) {
				uint8_t* got = reserve_at((void*)slot, bytes);
				if (got)
					return got;
			}
		}
		if (region_end <= addr)
			break;
		addr = granule_up(region_end);
#else
		uint8_t* got = reserve_at((void*)addr, bytes);
		if (got)
			return got;
		break;
#endif
	}
	uintptr_t down = (uintptr_t)g_blocks[0]->base;
	while (down > 0x10000) {
#if defined(_WIN32)
		MEMORY_BASIC_INFORMATION info;
		if (!VirtualQuery((void*)(down - 1), &info, sizeof(info)))
			break;
		uintptr_t region = (uintptr_t)info.BaseAddress;
		uintptr_t region_end = region + info.RegionSize;
		if (info.State == MEM_FREE && region_end > region) {
			uintptr_t slot_end = region_end < down ? region_end : down;
			if (slot_end >= bytes) {
				uintptr_t slot = (slot_end - bytes) & ~(uintptr_t)(kGranule - 1);
				if (slot >= region && slot + bytes <= slot_end && span_ok(slot, bytes)) {
					uint8_t* got = reserve_at((void*)slot, bytes);
					if (got)
						return got;
				}
			}
		}
		if (region == 0 || region >= down)
			break;
		down = region;
#else
		if (down < bytes)
			break;
		uintptr_t slot = (down - bytes) & ~(uintptr_t)(kGranule - 1);
		if (!span_ok(slot, bytes))
			break;
		uint8_t* got = reserve_at((void*)slot, bytes);
		if (got)
			return got;
		break;
#endif
	}
	return nullptr;
}

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

CodeBlock* code_reserve(size_t min_bytes) {
	if (g_block_count >= kMaxBlocks)
		return nullptr;
	size_t need = page_align(min_bytes < kPage ? kPage : min_bytes);
	size_t choices[8];
	int nchoices = 0;
	for (size_t i = 0; i < sizeof(kPreferred) / sizeof(kPreferred[0]); i++) {
		if (kPreferred[i] >= need)
			choices[nchoices++] = kPreferred[i];
	}
	if (nchoices == 0)
		choices[nchoices++] = page_align(need);
	size_t bytes = 0;
	uint8_t* base = nullptr;
	for (int i = 0; i < nchoices && !base; i++) {
		bytes = choices[i];
		base = reserve_near(bytes);
		if (base && !span_ok((uintptr_t)base, bytes)) {
			release_region(base, bytes);
			base = nullptr;
		}
	}
	if (!base)
		return nullptr;
	CodeBlock* block = (CodeBlock*)mem_alloc(sizeof(CodeBlock));
	if (!block) {
		release_region(base, bytes);
		return nullptr;
	}
	block->base = base;
	block->reserved = bytes;
	block->committed = 0;
	block->used = 0;
	block->commit_failed = 0;
	g_blocks[g_block_count++] = block;
	return block;
}

bool code_commit(CodeBlock* block, size_t need) {
	if (!block)
		return false;
	need += 16;
	if (block->used > block->committed || block->committed > block->reserved)
		return false;
	if (block->committed - block->used >= need)
		return true;
	if (block->commit_failed || block->committed >= block->reserved)
		return false;
	size_t end = page_align(block->used + need);
	if (block->committed + kCommitQuantum > end && block->committed + kCommitQuantum <= block->reserved)
		end = block->committed + kCommitQuantum;
	if (end > block->reserved)
		end = block->reserved;
	end = page_align(end);
	if (end > block->reserved || end <= block->committed) {
		block->commit_failed = 1;
		return false;
	}
	size_t extra = end - block->committed;
#if defined(_WIN32)
	uint8_t* page = (uint8_t*)VirtualAlloc(block->base + block->committed, extra, MEM_COMMIT, PAGE_EXECUTE_READWRITE);
	if (page != block->base + block->committed || !ensure_rwx(page, extra)) {
		block->commit_failed = 1;
		return false;
	}
#else
	if (mprotect(block->base + block->committed, extra, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
		block->commit_failed = 1;
		return false;
	}
#endif
	block->committed = end;
	return true;
}

uint8_t* code_alloc(uint32_t size) {
	if (size == 0)
		return nullptr;
	for (int pass = 0; pass < 2; pass++) {
		for (int i = 0; i < g_block_count; i++) {
			CodeBlock* block = g_blocks[i];
			if (!code_commit(block, size))
				continue;
			if (block->used > block->committed)
				continue;
			size_t avail = block->committed - block->used;
			size_t align = sizeof(void*);
			size_t touch = size + align;
			if (touch > avail)
				touch = avail;
#if defined(_WIN32)
			if (!ensure_rwx(block->base + block->used, touch))
				continue;
#endif
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
		if (pass == 0 && !code_reserve((size_t)size + 16))
			return nullptr;
	}
	return nullptr;
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

void code_rewind(void* code, uint32_t cap, uint32_t keep) {
	if (!code || keep > cap)
		return;
	uint8_t* p = (uint8_t*)code;
	for (int i = 0; i < g_block_count; i++) {
		CodeBlock* block = g_blocks[i];
		if (p < block->base || p >= block->base + block->committed)
			continue;
		if (block->base + block->used != p + cap)
			return;
		block->used = (size_t)(p - block->base) + keep;
		return;
	}
}

void code_account(uint32_t* blocks, uint32_t* used, uint32_t* reserved) {
	uint32_t n = 0;
	uint32_t u = 0;
	uint32_t r = 0;
	for (int i = 0; i < g_block_count; i++) {
		n++;
		u += (uint32_t)g_blocks[i]->used;
		r += (uint32_t)g_blocks[i]->reserved;
	}
	if (blocks)
		*blocks = n;
	if (used)
		*used = u;
	if (reserved)
		*reserved = r;
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
