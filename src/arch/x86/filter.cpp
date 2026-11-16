#include "arch/x86/internal.hpp"
#include "arch/backend.h"

#include "platform/lock.hpp"
#include "platform/memory.hpp"

#include <jitvm/jitvm_x86.h>

#include <string.h>

namespace jitvm {
namespace x86 {
namespace {

void patch_rel(uint8_t* at, uint8_t* target) {
	int32_t rel = (int32_t)(target - (at + 4));
	memcpy(at, &rel, 4);
}

void emit_bts_match(Buf& buf) {
	buf.seg();
	if (host64()) {
		buf.raw("\x48\x0F\xBA\x2C\x25", 5);
		buf.d(ctx_off(SLOT_FLAGS));
		buf.b(1);
	} else {
		buf.raw("\x0F\xBA\x2D", 3);
		buf.d(ctx_off(SLOT_FLAGS));
		buf.b(1);
	}
}

void emit_abs_rdi(Buf& buf, uintptr_t value, uint32_t* off) {
	if (host64()) {
		buf.raw("\x48\xBF", 2);
		*off = (uint32_t)(buf.p - buf.start);
		buf.q(value);
	} else {
		buf.raw("\x81\xFE", 2);
		*off = (uint32_t)(buf.p - buf.start);
		buf.d((uint32_t)value);
	}
}

}

struct FilterCode {
	uint8_t* proc;
	uint32_t start_off;
	uint32_t end_off;
	uint32_t match_off;
	uint32_t unmatch_off;
};

FilterCode* emit_filter(uintptr_t start, uintptr_t end, int outside, uintptr_t match, uintptr_t unmatch) {
	Buf buf = begin_code(160);
	if (!buf.start)
		return nullptr;
	FilterCode local;
	memset(&local, 0, sizeof(local));
	local.proc = buf.start;
	ZydisRegister cursor = host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI;
	buf.b(0x9C);
	emit_mov_reg_ctx(buf, cursor, SLOT_IP);
	if (host64()) {
		buf.b(0x57);
		emit_abs_rdi(buf, start, &local.start_off);
		buf.raw("\x48\x3B\xF7", 3);
		buf.raw(outside ? "\x0F\x82" : "\x0F\x86", 2);
		uint8_t* j1 = buf.p;
		buf.d(0);
		emit_abs_rdi(buf, end, &local.end_off);
		buf.raw("\x48\x3B\xF7", 3);
		buf.raw("\x0F\x83", 2);
		uint8_t* j2 = buf.p;
		buf.d(0);
		if (outside) {
			buf.raw("\x48\xBF", 2);
			local.unmatch_off = (uint32_t)(buf.p - buf.start);
			buf.q(unmatch);
			buf.b(0xE9);
			uint8_t* skip = buf.p;
			buf.d(0);
			uint8_t* match_at = buf.p;
			patch_rel(j1, match_at);
			patch_rel(j2, match_at);
			emit_bts_match(buf);
			buf.raw("\x48\xBF", 2);
			local.match_off = (uint32_t)(buf.p - buf.start);
			buf.q(match);
			patch_rel(skip, buf.p);
		} else {
			emit_bts_match(buf);
			buf.raw("\x48\xBF", 2);
			local.match_off = (uint32_t)(buf.p - buf.start);
			buf.q(match);
			buf.b(0xE9);
			uint8_t* to_join = buf.p;
			buf.d(0);
			uint8_t* unmatch_at = buf.p;
			patch_rel(j1, unmatch_at);
			patch_rel(j2, unmatch_at);
			buf.raw("\x48\xBF", 2);
			local.unmatch_off = (uint32_t)(buf.p - buf.start);
			buf.q(unmatch);
			patch_rel(to_join, buf.p);
		}
		buf.raw("\x48\x87\x7C\x24\x08", 5);
		buf.raw("\x48\x87\x3C\x24", 4);
		buf.b(0x9D);
		buf.b(0xC3);
	} else {
		emit_abs_rdi(buf, start, &local.start_off);
		buf.raw(outside ? "\x0F\x82" : "\x0F\x86", 2);
		uint8_t* j1 = buf.p;
		buf.d(0);
		buf.raw("\x81\xFE", 2);
		local.end_off = (uint32_t)(buf.p - buf.start);
		buf.d((uint32_t)end);
		buf.raw("\x0F\x83", 2);
		uint8_t* j2 = buf.p;
		buf.d(0);
		if (!outside) {
			emit_bts_match(buf);
			buf.b(0x9D);
			buf.b(0xE9);
			local.match_off = (uint32_t)(buf.p - buf.start);
			buf.d(0);
			patch_rel(buf.p - 4, (uint8_t*)match);
			uint8_t* unmatch_at = buf.p;
			patch_rel(j1, unmatch_at);
			patch_rel(j2, unmatch_at);
			buf.b(0x9D);
			buf.b(0xE9);
			local.unmatch_off = (uint32_t)(buf.p - buf.start);
			buf.d(0);
			patch_rel(buf.p - 4, (uint8_t*)unmatch);
		} else {
			buf.b(0x9D);
			buf.b(0xE9);
			local.unmatch_off = (uint32_t)(buf.p - buf.start);
			buf.d(0);
			patch_rel(buf.p - 4, (uint8_t*)unmatch);
			uint8_t* match_at = buf.p;
			patch_rel(j1, match_at);
			patch_rel(j2, match_at);
			emit_bts_match(buf);
			buf.b(0x9D);
			buf.b(0xE9);
			local.match_off = (uint32_t)(buf.p - buf.start);
			buf.d(0);
			patch_rel(buf.p - 4, (uint8_t*)match);
		}
	}
	finish_code(buf);
	FilterCode* out = (FilterCode*)mem_alloc(sizeof(FilterCode));
	if (!out)
		return nullptr;
	*out = local;
	return out;
}

}  // namespace x86
}  // namespace jitvm

struct jitvm_x86_range_filter {
	jitvm::x86::FilterCode* code;
	uintptr_t start;
	uintptr_t end;
	int absolute;
};

extern "C" jitvm_x86_range_filter* jitvm_x86_range_filter_create(uintptr_t start, uintptr_t end, int outside) {
	jitvm::LockGuard guard(jitvm::code_lock());
	uintptr_t match = jitvm::active_backend()->default_filter();
	uintptr_t unmatch = match;
	auto* code = jitvm::x86::emit_filter(start, end, outside, match, unmatch);
	if (!code)
		return nullptr;
	jitvm_x86_range_filter* filter = (jitvm_x86_range_filter*)jitvm::mem_alloc(sizeof(jitvm_x86_range_filter));
	if (!filter)
		return nullptr;
	filter->code = code;
	filter->start = start;
	filter->end = end;
#if defined(JITVM_HOST_X64)
	filter->absolute = 1;
#else
	filter->absolute = 0;
#endif
	(void)outside;
	return filter;
}

extern "C" void jitvm_x86_range_filter_destroy(jitvm_x86_range_filter* filter) {
	if (!filter)
		return;
	jitvm::mem_free(filter->code);
	jitvm::mem_free(filter);
}

extern "C" uintptr_t jitvm_x86_range_filter_proc(const jitvm_x86_range_filter* filter) {
	if (!filter || !filter->code)
		return 0;
	return (uintptr_t)filter->code->proc;
}

extern "C" void jitvm_x86_range_filter_set_range(jitvm_x86_range_filter* filter, uintptr_t start, uintptr_t end) {
	if (!filter || !filter->code)
		return;
	filter->start = start;
	filter->end = end;
	memcpy(filter->code->proc + filter->code->start_off, &start, sizeof(start));
	memcpy(filter->code->proc + filter->code->end_off, &end, sizeof(end));
	jitvm::code_flush(filter->code->proc, 160);
}

extern "C" void jitvm_x86_range_filter_set_targets(jitvm_x86_range_filter* filter, uintptr_t match, uintptr_t unmatch) {
	if (!filter || !filter->code)
		return;
	uint8_t* proc = filter->code->proc;
	if (filter->absolute) {
		memcpy(proc + filter->code->match_off, &match, sizeof(match));
		memcpy(proc + filter->code->unmatch_off, &unmatch, sizeof(unmatch));
	} else {
		int32_t mrel = (int32_t)(match - (uintptr_t)(proc + filter->code->match_off) - 4);
		int32_t urel = (int32_t)(unmatch - (uintptr_t)(proc + filter->code->unmatch_off) - 4);
		memcpy(proc + filter->code->match_off, &mrel, 4);
		memcpy(proc + filter->code->unmatch_off, &urel, 4);
	}
	jitvm::code_flush(proc, 160);
}

extern "C" uintptr_t jitvm_x86_range_filter_start(const jitvm_x86_range_filter* filter) {
	return filter ? filter->start : 0;
}

extern "C" uintptr_t jitvm_x86_range_filter_end(const jitvm_x86_range_filter* filter) {
	return filter ? filter->end : 0;
}
