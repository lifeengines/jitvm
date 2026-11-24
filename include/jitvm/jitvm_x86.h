#ifndef JITVM_JITVM_X86_H
#define JITVM_JITVM_X86_H

#include "jitvm.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct jitvm_regs_x86 {
#if defined(_M_X64) || defined(__x86_64__)
	intptr_t r15;
	intptr_t r14;
	intptr_t r13;
	intptr_t r12;
	intptr_t r11;
	intptr_t r10;
	intptr_t r9;
	intptr_t r8;
#endif
	intptr_t rdi;
	intptr_t rip;
	intptr_t rbp;
	intptr_t rsp_slot;
	intptr_t rbx;
	intptr_t rdx;
	intptr_t rcx;
	intptr_t rax;
	intptr_t rsi;
	uint32_t last_status;
	uint32_t last_error;
	intptr_t eflags;
	intptr_t rsp;
} jitvm_regs_x86;
#pragma pack(pop)

uintptr_t jitvm_x86_get_saved_rsi(void);
void jitvm_x86_set_saved_rsi(uintptr_t value);

uint32_t jitvm_x86_opcode_length(const uint8_t* bytes, uint32_t available);
void* jitvm_x86_translated_proc(const uint8_t* bytes, uint32_t available);
uint32_t jitvm_x86_last_fault(uintptr_t* ip, uint32_t* blocks, uint32_t* used, uint32_t* reserved);
void jitvm_x86_code_stats(uint32_t* blocks, uint32_t* used, uint32_t* reserved);

typedef struct jitvm_x86_range_filter jitvm_x86_range_filter;

jitvm_x86_range_filter* jitvm_x86_range_filter_create(uintptr_t start, uintptr_t end, int outside);
void jitvm_x86_range_filter_destroy(jitvm_x86_range_filter* filter);
uintptr_t jitvm_x86_range_filter_proc(const jitvm_x86_range_filter* filter);
void jitvm_x86_range_filter_set_range(jitvm_x86_range_filter* filter, uintptr_t start, uintptr_t end);
void jitvm_x86_range_filter_set_targets(jitvm_x86_range_filter* filter, uintptr_t match, uintptr_t unmatch);
uintptr_t jitvm_x86_range_filter_start(const jitvm_x86_range_filter* filter);
uintptr_t jitvm_x86_range_filter_end(const jitvm_x86_range_filter* filter);

#ifdef __cplusplus
}
#endif

#endif
