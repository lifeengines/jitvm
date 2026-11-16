#ifndef JITVM_JITVM_H
#define JITVM_JITVM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum jitvm_machine {
	JITVM_MACHINE_X86 = 1,
	JITVM_MACHINE_X64 = 2,
	JITVM_MACHINE_AARCH64 = 3
} jitvm_machine;

typedef enum jitvm_status {
	JITVM_OK = 0,
	JITVM_ERR_UNSUPPORTED_MACHINE = 1,
	JITVM_ERR_FAILED = 2
} jitvm_status;

typedef enum jitvm_monitor {
	JITVM_MONITOR_OFF = 0,
	JITVM_MONITOR_SKIP_FILTER = 1,
	JITVM_MONITOR_ALL = 2
} jitvm_monitor;

typedef enum jitvm_flow {
	JITVM_FLOW_JUMP_IMM = 0,
	JITVM_FLOW_JUMP_REG = 1,
	JITVM_FLOW_JUMP_MEM = 2,
	JITVM_FLOW_COND = 3,
	JITVM_FLOW_CALL_IMM = 4,
	JITVM_FLOW_CALL_REG = 5,
	JITVM_FLOW_CALL_MEM = 6,
	JITVM_FLOW_RETURN = 7
} jitvm_flow;

typedef enum jitvm_flag {
	JITVM_FLAG_NONE = 0,
	JITVM_FLAG_EXIT = 1,
	JITVM_FLAG_MATCH = 2
} jitvm_flag;

typedef struct jitvm_regs_x86 jitvm_regs_x86;

typedef struct jitvm_guest_state {
	jitvm_machine machine;
	uint32_t reserved;
	union {
		jitvm_regs_x86* x86;
	} regs;
} jitvm_guest_state;

typedef void (*jitvm_hook)(jitvm_guest_state* state);
typedef void (*jitvm_enter_fn)(void);

typedef struct jitvm_config {
	jitvm_machine machine;
	jitvm_hook on_inst;
	jitvm_hook on_flow;
	jitvm_hook on_exit;
} jitvm_config;

jitvm_machine jitvm_host_machine(void);
jitvm_enter_fn jitvm_init(const jitvm_config* cfg);
jitvm_status jitvm_init_status(void);

int jitvm_bind_thread(void);

uintptr_t jitvm_get_guest_ip(void);
void jitvm_set_guest_ip(uintptr_t ip);

uintptr_t jitvm_get_flags(void);
void jitvm_set_exit_flag(void);
void jitvm_clear_exit_flag(void);
void jitvm_clear_flags(uintptr_t mask);

uintptr_t jitvm_get_data(void);
void jitvm_set_data(uintptr_t value);

uintptr_t jitvm_get_vstack(void);
void jitvm_set_vstack(uintptr_t stack_top);

uintptr_t jitvm_get_filter(void);
void jitvm_set_filter(uintptr_t proc);
uintptr_t jitvm_default_filter(void);
uintptr_t jitvm_nofilter(void);
uintptr_t jitvm_nofilter_exit(void);

void jitvm_set_inst_monitor(jitvm_monitor state);
void jitvm_set_flow_monitor(jitvm_monitor state);
jitvm_monitor jitvm_inst_monitor(void);
jitvm_monitor jitvm_flow_monitor(void);

uint32_t jitvm_last_flow_type(void);
uintptr_t jitvm_last_flow_ip(void);

void* jitvm_alloc_code(uint32_t size);
int32_t jitvm_grow_code(void* code, uint32_t size);
int jitvm_ip_in_code(uintptr_t pc);

#ifdef __cplusplus
}
#endif

#endif
