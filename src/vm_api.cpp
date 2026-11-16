#include "arch/backend.h"

#include "platform/lock.hpp"
#include "platform/memory.hpp"

static jitvm_status g_status = JITVM_OK;

static bool host_supports(jitvm_machine machine) {
#if defined(JITVM_HOST_X64)
	return machine == JITVM_MACHINE_X64;
#elif defined(JITVM_HOST_X86)
	return machine == JITVM_MACHINE_X86;
#else
	(void)machine;
	return false;
#endif
}

extern "C" jitvm_machine jitvm_host_machine(void) {
#if defined(JITVM_HOST_X64)
	return JITVM_MACHINE_X64;
#elif defined(JITVM_HOST_X86)
	return JITVM_MACHINE_X86;
#else
	return JITVM_MACHINE_AARCH64;
#endif
}

extern "C" jitvm_enter_fn jitvm_init(const jitvm_config* cfg) {
	if (!cfg || !host_supports(cfg->machine)) {
		g_status = JITVM_ERR_UNSUPPORTED_MACHINE;
		return nullptr;
	}
	const jitvm::Backend* backend = jitvm::active_backend();
	jitvm_enter_fn enter = backend->prepare(cfg);
	if (!enter) {
		g_status = JITVM_ERR_FAILED;
		return nullptr;
	}
	g_status = JITVM_OK;
	return enter;
}

extern "C" jitvm_status jitvm_init_status(void) {
	return g_status;
}

extern "C" int jitvm_bind_thread(void) {
	const jitvm::Backend* backend = jitvm::active_backend();
	return backend->bind();
}

static const jitvm::Backend* bound() {
	const jitvm::Backend* backend = jitvm::active_backend();
	backend->bind();
	return backend;
}

extern "C" uintptr_t jitvm_get_guest_ip(void) {
	return bound()->load(jitvm::ROLE_IP);
}

extern "C" void jitvm_set_guest_ip(uintptr_t ip) {
	bound()->store(jitvm::ROLE_IP, ip);
}

extern "C" uintptr_t jitvm_get_flags(void) {
	return bound()->load(jitvm::ROLE_FLAGS);
}

extern "C" void jitvm_set_exit_flag(void) {
	bound()->or_flags(JITVM_FLAG_EXIT);
}

extern "C" void jitvm_clear_exit_flag(void) {
	bound()->and_flags(~(uintptr_t)JITVM_FLAG_EXIT);
}

extern "C" void jitvm_clear_flags(uintptr_t mask) {
	bound()->and_flags(~mask);
}

extern "C" uintptr_t jitvm_get_data(void) {
	return bound()->load(jitvm::ROLE_DATA);
}

extern "C" void jitvm_set_data(uintptr_t value) {
	bound()->store(jitvm::ROLE_DATA, value);
}

extern "C" uintptr_t jitvm_get_vstack(void) {
	return bound()->load(jitvm::ROLE_STACK);
}

extern "C" void jitvm_set_vstack(uintptr_t stack_top) {
	bound()->store(jitvm::ROLE_STACK, stack_top);
}

extern "C" uintptr_t jitvm_get_filter(void) {
	return bound()->load(jitvm::ROLE_FILTER);
}

extern "C" void jitvm_set_filter(uintptr_t proc) {
	bound()->store(jitvm::ROLE_FILTER, proc);
}

extern "C" uintptr_t jitvm_default_filter(void) {
	return bound()->default_filter();
}

extern "C" uintptr_t jitvm_nofilter(void) {
	return bound()->nofilter();
}

extern "C" uintptr_t jitvm_nofilter_exit(void) {
	return bound()->nofilter_exit();
}

extern "C" void jitvm_set_inst_monitor(jitvm_monitor state) {
	bound()->set_inst_monitor(state);
}

extern "C" void jitvm_set_flow_monitor(jitvm_monitor state) {
	bound()->set_flow_monitor(state);
}

extern "C" jitvm_monitor jitvm_inst_monitor(void) {
	return bound()->inst_monitor();
}

extern "C" jitvm_monitor jitvm_flow_monitor(void) {
	return bound()->flow_monitor();
}

extern "C" uint32_t jitvm_last_flow_type(void) {
	return (uint32_t)bound()->load(jitvm::ROLE_FLOWTYPE);
}

extern "C" uintptr_t jitvm_last_flow_ip(void) {
	return bound()->load(jitvm::ROLE_FLOWIP);
}

extern "C" void* jitvm_alloc_code(uint32_t size) {
	jitvm::LockGuard guard(jitvm::code_lock());
	return jitvm::code_alloc(size);
}

extern "C" int32_t jitvm_grow_code(void* code, uint32_t size) {
	jitvm::LockGuard guard(jitvm::code_lock());
	return jitvm::code_grow(code, size);
}

extern "C" int jitvm_ip_in_code(uintptr_t pc) {
	return jitvm::code_contains(pc);
}
