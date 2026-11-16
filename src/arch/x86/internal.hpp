#ifndef JITVM_X86_INTERNAL_HPP
#define JITVM_X86_INTERNAL_HPP

#include "arch/x86/buf.hpp"

#include <jitvm/jitvm.h>

namespace jitvm {
namespace x86 {

Buf begin_code(uint32_t cap);
void finish_code(Buf& buf);

uintptr_t tail_for(uint32_t length);
uintptr_t dispatch_target(bool flow);
uintptr_t exit_callback_proc();
uintptr_t error_proc();
uintptr_t link_entry();
bool flow_enabled();

int branch_extras(const Decoded& inst, int* extra, int cap);
uintptr_t set_branch(const uint8_t* ip, uint32_t iclen, uintptr_t proc, const int* extra, int nextra);
uintptr_t translate(const uint8_t* ip);

}  // namespace x86
}  // namespace jitvm

#endif
