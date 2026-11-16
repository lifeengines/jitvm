#ifndef JITVM_PLATFORM_THREAD_CTX_HPP
#define JITVM_PLATFORM_THREAD_CTX_HPP

#include <stdint.h>

namespace jitvm {

int thread_bind();
uintptr_t thread_load(uint32_t offset);
void thread_store(uint32_t offset, uintptr_t value);
void thread_or_flags(uintptr_t flags);
void thread_and_flags(uintptr_t mask);
uint32_t host_error();
void host_set_error(uint32_t value);
uint32_t host_status();
void host_set_status(uint32_t value);

}  // namespace jitvm

#endif
