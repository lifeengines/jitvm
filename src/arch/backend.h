#ifndef JITVM_ARCH_BACKEND_H
#define JITVM_ARCH_BACKEND_H

#include <jitvm/jitvm.h>

namespace jitvm {

enum Role {
	ROLE_FLAGS = 0,
	ROLE_DATA = 1,
	ROLE_FILTER = 2,
	ROLE_IP = 3,
	ROLE_SAVED_RSI = 4,
	ROLE_STACK = 5,
	ROLE_FLOWTYPE = 6,
	ROLE_FLOWIP = 7
};

struct Backend {
	jitvm_enter_fn (*prepare)(const jitvm_config* cfg);
	int (*bind)();
	uintptr_t (*load)(int role);
	void (*store)(int role, uintptr_t value);
	void (*or_flags)(uintptr_t flags);
	void (*and_flags)(uintptr_t mask);
	void (*set_inst_monitor)(jitvm_monitor state);
	void (*set_flow_monitor)(jitvm_monitor state);
	jitvm_monitor (*inst_monitor)();
	jitvm_monitor (*flow_monitor)();
	uintptr_t (*default_filter)();
	uintptr_t (*nofilter)();
	uintptr_t (*nofilter_exit)();
};

const Backend* active_backend();

}  // namespace jitvm

#endif
