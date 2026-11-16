#include "platform/thread_ctx.hpp"

#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#else
#include <errno.h>
#include <stdlib.h>
#if defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#if defined(__x86_64__)
#include <asm/prctl.h>
#endif
#if defined(__i386__)
#include <asm/ldt.h>
#endif
#endif
#endif

namespace jitvm {
namespace {

uint32_t ctx_base() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return 0xF88;
#elif defined(_WIN32)
	return 0x250;
#else
	return 0;
#endif
}

uint32_t status_off() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return 0x1250;
#elif defined(_WIN32)
	return 0xBF4;
#else
	return 0;
#endif
}

#if !defined(_WIN32)
thread_local uint8_t* g_block = nullptr;

uint8_t* ensure_block() {
	if (!g_block) {
		g_block = (uint8_t*)malloc(4096);
		if (g_block)
			memset(g_block, 0, 4096);
	}
	return g_block;
}
#endif

}  // namespace

int thread_bind() {
#if defined(_WIN32)
	static thread_local int ready = 0;
	if (ready)
		return 1;
	uint32_t base = ctx_base();
	uint32_t step = (uint32_t)sizeof(void*);
	for (uint32_t i = 0; i < 10; i++)
		thread_store(base + i * step, 0);
	ready = 1;
	return 1;
#elif defined(__x86_64__)
	uint8_t* base = ensure_block();
	if (!base)
		return 0;
#if defined(__linux__)
	if (syscall(SYS_arch_prctl, ARCH_SET_GS, base) != 0)
		return 0;
	return 1;
#elif defined(__APPLE__)
	__asm__ volatile("wrfsbase %0" :: "r"(base));
	return 1;
#else
	(void)base;
	return 0;
#endif
#elif defined(__i386__) && defined(__linux__)
	uint8_t* base = ensure_block();
	if (!base)
		return 0;
	struct user_desc desc;
	memset(&desc, 0, sizeof(desc));
	desc.entry_number = (unsigned int)-1;
	desc.base_addr = (unsigned int)(uintptr_t)base;
	desc.limit = 1;
	desc.seg_32bit = 1;
	desc.contents = 0;
	desc.read_exec_only = 0;
	desc.limit_in_pages = 1;
	desc.seg_not_present = 0;
	desc.useable = 1;
	if (syscall(SYS_set_thread_area, &desc) != 0)
		return 0;
	unsigned short sel = (unsigned short)((desc.entry_number << 3) | 3);
	__asm__ volatile("mov %0, %%fs" :: "r"(sel));
	return 1;
#else
	return 0;
#endif
}

uintptr_t thread_load(uint32_t offset) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return (uintptr_t)__readgsqword(offset);
#elif defined(_WIN32)
	return (uintptr_t)__readfsdword(offset);
#elif defined(__x86_64__) && defined(__APPLE__)
	uintptr_t value = 0;
	uintptr_t addr = offset;
	__asm__ volatile("movq %%fs:(%1), %0" : "=r"(value) : "r"(addr));
	return value;
#elif defined(__x86_64__)
	uintptr_t value = 0;
	uintptr_t addr = offset;
	__asm__ volatile("movq %%gs:(%1), %0" : "=r"(value) : "r"(addr));
	return value;
#elif defined(__i386__)
	uintptr_t value = 0;
	uintptr_t addr = offset;
	__asm__ volatile("movl %%fs:(%1), %0" : "=r"(value) : "r"(addr));
	return value;
#else
	(void)offset;
	return 0;
#endif
}

void thread_store(uint32_t offset, uintptr_t value) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	__writegsqword(offset, (unsigned __int64)value);
#elif defined(_WIN32)
	__writefsdword(offset, (unsigned long)value);
#elif defined(__x86_64__) && defined(__APPLE__)
	uintptr_t addr = offset;
	__asm__ volatile("movq %0, %%fs:(%1)" :: "r"(value), "r"(addr));
#elif defined(__x86_64__)
	uintptr_t addr = offset;
	__asm__ volatile("movq %0, %%gs:(%1)" :: "r"(value), "r"(addr));
#elif defined(__i386__)
	uintptr_t addr = offset;
	__asm__ volatile("movl %0, %%fs:(%1)" :: "r"(value), "r"(addr));
#else
	(void)offset;
	(void)value;
#endif
}

void thread_or_flags(uintptr_t flags) {
	uint32_t off = ctx_base();
	thread_store(off, thread_load(off) | flags);
}

void thread_and_flags(uintptr_t mask) {
	uint32_t off = ctx_base();
	thread_store(off, thread_load(off) & mask);
}

uint32_t host_error() {
#if defined(_WIN32)
	return GetLastError();
#else
	return (uint32_t)errno;
#endif
}

void host_set_error(uint32_t value) {
#if defined(_WIN32)
	SetLastError(value);
#else
	errno = (int)value;
#endif
}

uint32_t host_status() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	return __readgsdword(status_off());
#elif defined(_WIN32)
	return __readfsdword(status_off());
#else
	return 0;
#endif
}

void host_set_status(uint32_t value) {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
	__writegsdword(status_off(), value);
#elif defined(_WIN32)
	__writefsdword(status_off(), value);
#else
	(void)value;
#endif
}

}  // namespace jitvm
