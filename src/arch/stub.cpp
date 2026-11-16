#include "arch/backend.h"

#include <jitvm/jitvm_x86.h>

namespace jitvm {
namespace {

jitvm_enter_fn prepare(const jitvm_config*) { return nullptr; }
int bind() { return 0; }
uintptr_t load(int) { return 0; }
void store(int, uintptr_t) {}
void or_flags(uintptr_t) {}
void and_flags(uintptr_t) {}
void set_inst(jitvm_monitor) {}
void set_flow(jitvm_monitor) {}
jitvm_monitor inst_monitor() { return JITVM_MONITOR_OFF; }
jitvm_monitor flow_monitor() { return JITVM_MONITOR_OFF; }
uintptr_t zero() { return 0; }

const Backend g_backend = {
	prepare,
	bind,
	load,
	store,
	or_flags,
	and_flags,
	set_inst,
	set_flow,
	inst_monitor,
	flow_monitor,
	zero,
	zero,
	zero,
};

}  // namespace

const Backend* active_backend() {
	return &g_backend;
}

}  // namespace jitvm

extern "C" uintptr_t jitvm_x86_get_saved_rsi(void) { return 0; }
extern "C" void jitvm_x86_set_saved_rsi(uintptr_t) {}
extern "C" uint32_t jitvm_x86_opcode_length(const uint8_t*, uint32_t) { return 0; }
extern "C" void* jitvm_x86_translated_proc(const uint8_t*, uint32_t) { return nullptr; }
extern "C" jitvm_x86_range_filter* jitvm_x86_range_filter_create(uintptr_t, uintptr_t, int) { return nullptr; }
extern "C" void jitvm_x86_range_filter_destroy(jitvm_x86_range_filter*) {}
extern "C" uintptr_t jitvm_x86_range_filter_proc(const jitvm_x86_range_filter*) { return 0; }
extern "C" void jitvm_x86_range_filter_set_range(jitvm_x86_range_filter*, uintptr_t, uintptr_t) {}
extern "C" void jitvm_x86_range_filter_set_targets(jitvm_x86_range_filter*, uintptr_t, uintptr_t) {}
extern "C" uintptr_t jitvm_x86_range_filter_start(const jitvm_x86_range_filter*) { return 0; }
extern "C" uintptr_t jitvm_x86_range_filter_end(const jitvm_x86_range_filter*) { return 0; }
