#ifndef JITVM_X86_LAYOUT_HPP
#define JITVM_X86_LAYOUT_HPP

#include <stdint.h>

namespace jitvm {
namespace x86 {

enum Slot {
	SLOT_FLAGS = 0,
	SLOT_DATA = 1,
	SLOT_FILTER = 2,
	SLOT_IP = 3,
	SLOT_RSI = 4,
	SLOT_STACK = 5,
	SLOT_TMP1 = 6,
	SLOT_TMP2 = 7,
	SLOT_FLOWTYPE = 8,
	SLOT_FLOWIP = 9
};

inline uint32_t ctx_base() {
#if defined(_WIN32) && (defined(JITVM_HOST_X64) || defined(_M_X64) || defined(__x86_64__))
	return 0xF88u;
#elif defined(_WIN32)
	return 0x250u;
#else
	return 0u;
#endif
}

inline uint32_t ctx_off(int slot) {
	return ctx_base() + (uint32_t)slot * (uint32_t)sizeof(uintptr_t);
}

inline uint8_t seg_prefix() {
#if defined(__APPLE__)
	return 0x64;
#elif defined(JITVM_HOST_X64) || defined(_M_X64) || defined(__x86_64__)
	return 0x65;
#else
	return 0x64;
#endif
}

inline bool host64() {
#if defined(JITVM_HOST_X64) || defined(_M_X64) || defined(__x86_64__)
	return true;
#else
	return false;
#endif
}

}  // namespace x86
}  // namespace jitvm

#endif
