#include "arch/x86/internal.hpp"
#include "arch/backend.h"

#include "platform/lock.hpp"
#include "platform/memory.hpp"
#include "platform/thread_ctx.hpp"

#include <jitvm/jitvm_x86.h>

#include <string.h>
#include <vector>

#if defined(_MSC_VER)
#include <intrin.h>
#endif

extern "C" void jitvm_link_entry();
extern "C" void jitvm_ensure_entry();
extern "C" void jitvm_x86_hook(jitvm_regs_x86* image, int kind);

namespace jitvm {
namespace x86 {
uintptr_t translate_insn(const uint8_t* ip);
}
}

namespace jitvm {
namespace x86 {
namespace {

struct Node {
	uint8_t* code;
	uintptr_t* table;
};

std::vector<Node> g_nodes;
uintptr_t* g_root = nullptr;
uintptr_t g_inst_entry = 0;
uintptr_t g_flow_entry = 0;
uintptr_t g_monitor_body = 0;
uintptr_t g_flow_body = 0;
uintptr_t g_nofilter = 0;
uintptr_t g_flow_nofilter = 0;
uintptr_t g_clean_exit = 0;
uintptr_t g_exit_cb = 0;
uintptr_t g_error = 0;
uintptr_t g_tails[16] = {};
uint8_t* g_inst_rel = nullptr;
uint8_t* g_flow_rel = nullptr;
int g_skip_filter = 0;
int g_skip_all = 0;
int g_flow_skip_filter = 0;
int g_flow_skip_all = 0;
jitvm_hook g_on_inst = nullptr;
uint32_t g_fp_size = 512;
uint32_t g_fp_alloc = 640;
uint32_t g_fp_mask_lo = 0;
uint32_t g_fp_mask_hi = 0;
bool g_fp_xsave = false;
jitvm_hook g_on_flow = nullptr;
jitvm_hook g_on_exit = nullptr;
jitvm_enter_fn g_enter = nullptr;

ZydisRegister cursor_reg() {
	return host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI;
}

ZydisRegister scratch_di() {
	return host64() ? ZYDIS_REGISTER_RDI : ZYDIS_REGISTER_EDI;
}

void emit_push_ctx(Buf& buf, int slot) {
	buf.seg();
	if (host64()) {
		buf.b(0xFF);
		buf.b(0x34);
		buf.b(0x25);
		buf.d(ctx_off(slot));
	} else {
		buf.b(0xFF);
		buf.b(0x35);
		buf.d(ctx_off(slot));
	}
}

void emit_jmp_ctx(Buf& buf, int slot) {
	buf.seg();
	if (host64()) {
		buf.b(0xFF);
		buf.b(0x24);
		buf.b(0x25);
		buf.d(ctx_off(slot));
	} else {
		buf.b(0xFF);
		buf.b(0x25);
		buf.d(ctx_off(slot));
	}
}

void emit_cmp_ctx_zero(Buf& buf, int slot) {
	buf.seg();
	if (host64()) {
		buf.raw("\x48\x83\x3C\x25", 4);
		buf.d(ctx_off(slot));
		buf.b(0);
	} else {
		buf.raw("\x83\x3D", 2);
		buf.d(ctx_off(slot));
		buf.b(0);
	}
}

void emit_btr_flag(Buf& buf) {
	buf.seg();
	if (host64()) {
		buf.raw("\x48\x0F\xBA\x34\x25", 5);
		buf.d(ctx_off(SLOT_FLAGS));
		buf.b(0);
	} else {
		buf.raw("\x0F\xBA\x35", 3);
		buf.d(ctx_off(SLOT_FLAGS));
		buf.b(0);
	}
}

void emit_xchg_rsi_ctx(Buf& buf) {
	buf.seg();
	if (host64()) {
		buf.raw("\x48\x87\x34\x25", 4);
		buf.d(ctx_off(SLOT_RSI));
	} else {
		buf.b(0x87);
		buf.b(0x35);
		buf.d(ctx_off(SLOT_RSI));
	}
}

void emit_pusha(Buf& buf) {
	if (!host64()) {
		buf.b(0x60);
		return;
	}
	static const uint8_t kPush[] = {
		0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
		0x41, 0x50, 0x41, 0x51, 0x41, 0x52, 0x41, 0x53,
		0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57,
	};
	buf.raw(kPush, sizeof(kPush));
}

void emit_popa(Buf& buf) {
	if (!host64()) {
		buf.b(0x61);
		return;
	}
	static const uint8_t kPop[] = {
		0x41, 0x5F, 0x41, 0x5E, 0x41, 0x5D, 0x41, 0x5C,
		0x41, 0x5B, 0x41, 0x5A, 0x41, 0x59, 0x41, 0x58,
		0x5F, 0x5E, 0x5D, 0x5B, 0x5B, 0x5A, 0x59, 0x58,
	};
	buf.raw(kPop, sizeof(kPop));
}

void emit_call_hook(Buf& buf, int kind) {
	if (host64()) {
#if defined(_WIN32)
		buf.raw("\x48\x8B\x8C\x24", 4);
		buf.d(g_fp_size);
		buf.raw("\x48\x83\xEC\x20", 4);
		buf.b(0xBA);
		buf.d((uint32_t)kind);
#else
		buf.raw("\x48\x8B\xBC\x24", 4);
		buf.d(g_fp_size);
		buf.b(0xBE);
		buf.d((uint32_t)kind);
#endif
		buf.raw("\x48\xB8", 2);
		buf.q((uint64_t)&jitvm_x86_hook);
		buf.raw("\xFF\xD0", 2);
#if defined(_WIN32)
		buf.raw("\x48\x83\xC4\x20", 4);
#endif
	} else {
		buf.raw("\x8B\x8C\x24", 3);
		buf.d(g_fp_size);
		buf.b(0xBA);
		buf.d((uint32_t)kind);
		buf.b(0xB8);
		buf.d((uint32_t)(uintptr_t)&jitvm_x86_hook);
		buf.raw("\xFF\xD0", 2);
	}
}

void emit_restore_host_error(Buf& buf) {
#if defined(_WIN32)
	if (host64()) {
		buf.b(0x50);
		buf.raw("\x8B\x44\x24\x08", 4);
		buf.seg();
		buf.raw("\x89\x04\x25", 3);
		buf.d(0x1250);
		buf.raw("\x8B\x44\x24\x0C", 4);
		buf.seg();
		buf.raw("\x89\x04\x25", 3);
		buf.d(0x68);
		buf.b(0x58);
		buf.raw("\x48\x83\xC4\x08", 4);
	} else {
		buf.seg();
		buf.b(0x8F);
		buf.b(0x05);
		buf.d(0xBF4);
		buf.seg();
		buf.b(0x8F);
		buf.b(0x05);
		buf.d(0x34);
	}
#else
	if (host64())
		buf.raw("\x48\x83\xC4\x08", 4);
	else
		buf.raw("\x83\xC4\x08", 3);
#endif
}

void emit_capture_error(Buf& buf) {
#if defined(_WIN32)
	if (host64()) {
		buf.b(0x56);
		buf.b(0x56);
		buf.seg();
		buf.raw("\x8B\x34\x25", 3);
		buf.d(0x68);
		buf.raw("\x89\x74\x24\x0C", 4);
		buf.seg();
		buf.raw("\x8B\x34\x25", 3);
		buf.d(0x1250);
		buf.raw("\x89\x74\x24\x08", 4);
		buf.b(0x5E);
	} else {
		buf.seg();
		buf.b(0xFF);
		buf.b(0x35);
		buf.d(0x34);
		buf.seg();
		buf.b(0xFF);
		buf.b(0x35);
		buf.d(0xBF4);
	}
#else
	if (host64())
		buf.raw("\x6A\x00", 2);
	else
		buf.raw("\x6A\x00\x6A\x00", 4);
#endif
}

void emit_table_jump(Buf& buf, uintptr_t* table, int level) {
	emit_mov_reg_ctx(buf, cursor_reg(), SLOT_IP);
	emit_movzx_rsi_byte(buf, level);
	if (host64()) {
		emit_mov_ctx_reg(buf, SLOT_TMP1, ZYDIS_REGISTER_R13);
		buf.raw("\x4C\x8D\x2D", 3);
		uint8_t* rel = buf.p;
		buf.d(0);
		int32_t disp = (int32_t)((intptr_t)table - (intptr_t)(rel + 4));
		memcpy(rel, &disp, 4);
		buf.raw("\x49\x8D\x74\xF5\x00", 5);
		emit_mov_reg_ctx(buf, ZYDIS_REGISTER_R13, SLOT_TMP1);
		buf.raw("\xFF\x26", 2);
	} else {
		buf.raw("\xFF\x24\xB5", 3);
		buf.d((uint32_t)(uintptr_t)table);
	}
}

uintptr_t* new_table() {
	Buf buf = begin_code((uint32_t)(sizeof(uintptr_t) * 256));
	uintptr_t* table = (uintptr_t*)buf.p;
	uintptr_t link = (uintptr_t)&jitvm_link_entry;
	for (int i = 0; i < 256; i++)
		table[i] = link;
	buf.p += sizeof(uintptr_t) * 256;
	finish_code(buf);
	return table;
}

void register_node(uint8_t* code, uintptr_t* table) {
	g_nodes.push_back(Node{code, table});
}

uintptr_t* table_of(uintptr_t code) {
	for (size_t i = 0; i < g_nodes.size(); i++) {
		if ((uintptr_t)g_nodes[i].code == code)
			return g_nodes[i].table;
	}
	return nullptr;
}

void emit_leave_native(Buf& buf) {
	buf.b(0x9D);
	buf.b(0x5C);
	emit_xchg_rsi_ctx(buf);
	emit_jmp_ctx(buf, SLOT_RSI);
}

void init_fp_save() {
	int info[4] = {};
#if defined(_MSC_VER)
	__cpuid(info, 1);
#else
	__asm__ volatile("cpuid" : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3]) : "a"(1), "c"(0));
#endif
	bool osxsave = (info[2] & (1 << 27)) != 0;
	g_fp_size = 512;
	g_fp_alloc = 640;
	g_fp_mask_lo = 0;
	g_fp_mask_hi = 0;
	g_fp_xsave = false;
	if (!osxsave)
		return;
	uint64_t mask = 0;
#if defined(_MSC_VER)
	mask = _xgetbv(0);
#else
	uint32_t lo = 0;
	uint32_t hi = 0;
	__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
	mask = ((uint64_t)hi << 32) | lo;
#endif
#if defined(_MSC_VER)
	__cpuidex(info, 0xD, 0);
#else
	__asm__ volatile("cpuid" : "=a"(info[0]), "=b"(info[1]), "=c"(info[2]), "=d"(info[3]) : "a"(0xD), "c"(0));
#endif
	uint32_t bytes = (uint32_t)info[1];
	// 576 covers the 512-byte legacy region and the 64-byte header. The saved RSP is stored at this offset.
	if (bytes < 576 || bytes > 8192 || mask == 0)
		return;
	g_fp_size = bytes;
	g_fp_alloc = bytes + 128;
	g_fp_mask_lo = (uint32_t)mask;
	g_fp_mask_hi = (uint32_t)(mask >> 32);
	g_fp_xsave = true;
}

void emit_mov_eax_imm(Buf& buf, uint32_t value) {
	buf.b(0xB8);
	buf.d(value);
}

void emit_mov_edx_imm(Buf& buf, uint32_t value) {
	buf.b(0xBA);
	buf.d(value);
}

void emit_zero_xsave_header(Buf& buf) {
	// XSAVE does not clear header bytes 520-575. XRSTOR raises #GP if they are nonzero.
	if (host64()) {
		buf.raw("\x31\xC0", 2);
		for (uint32_t off = 512; off < 576; off += 8) {
			buf.raw("\x48\x89\x84\x24", 4);
			buf.d(off);
		}
	} else {
		buf.raw("\x31\xC0", 2);
		for (uint32_t off = 512; off < 576; off += 4) {
			buf.raw("\x89\x84\x24", 3);
			buf.d(off);
		}
	}
}

void emit_fp_save(Buf& buf) {
	if (host64()) {
		buf.raw("\x48\x89\xE0", 3);
		buf.raw("\x48\x81\xEC", 3);
		buf.d(g_fp_alloc);
		buf.raw("\x48\x83\xE4\xC0", 4);
		buf.raw("\x48\x89\x84\x24", 4);
		buf.d(g_fp_size);
	} else {
		buf.raw("\x89\xE0", 2);
		buf.raw("\x81\xEC", 2);
		buf.d(g_fp_alloc);
		buf.raw("\x83\xE4\xC0", 3);
		buf.raw("\x89\x84\x24", 3);
		buf.d(g_fp_size);
	}
	if (g_fp_xsave) {
		emit_zero_xsave_header(buf);
		emit_mov_eax_imm(buf, g_fp_mask_lo);
		emit_mov_edx_imm(buf, g_fp_mask_hi);
		if (host64())
			buf.raw("\x48\x0F\xAE\x24\x24", 5);
		else
			buf.raw("\x0F\xAE\x24\x24", 4);
	} else if (host64()) {
		buf.raw("\x48\x0F\xAE\x04\x24", 5);
	} else {
		buf.raw("\x0F\xAE\x04\x24", 4);
	}
}

void emit_fp_restore(Buf& buf) {
	if (g_fp_xsave) {
		emit_mov_eax_imm(buf, g_fp_mask_lo);
		emit_mov_edx_imm(buf, g_fp_mask_hi);
		if (host64())
			buf.raw("\x48\x0F\xAE\x2C\x24", 5);
		else
			buf.raw("\x0F\xAE\x2C\x24", 4);
	} else if (host64()) {
		buf.raw("\x48\x0F\xAE\x0C\x24", 5);
	} else {
		buf.raw("\x0F\xAE\x0C\x24", 4);
	}
	if (host64()) {
		buf.raw("\x48\x8B\x84\x24", 4);
		buf.d(g_fp_size);
		buf.raw("\x48\x89\xC4", 3);
	} else {
		buf.raw("\x8B\x84\x24", 3);
		buf.d(g_fp_size);
		buf.raw("\x89\xC4", 2);
	}
}

void emit_framed_hook(Buf& buf, int kind, bool always_leave) {
	emit_mov_ctx_reg(buf, SLOT_TMP1, host64() ? ZYDIS_REGISTER_RSP : ZYDIS_REGISTER_ESP);
	emit_mov_reg_ctx(buf, host64() ? ZYDIS_REGISTER_RSP : ZYDIS_REGISTER_ESP, SLOT_STACK);
	emit_push_ctx(buf, SLOT_TMP1);
	buf.b(0x9C);
	emit_capture_error(buf);
	emit_push_ctx(buf, SLOT_RSI);
	buf.b(0xFC);
	emit_pusha(buf);
	emit_fp_save(buf);
	emit_call_hook(buf, kind);
	emit_fp_restore(buf);
	emit_popa(buf);
	emit_pop_ctx(buf, SLOT_RSI);
	emit_restore_host_error(buf);
	if (always_leave) {
		emit_leave_native(buf);
		return;
	}
	emit_btr_flag(buf);
	buf.b(0x72);
	uint8_t* jb = buf.p;
	buf.b(0);
	buf.b(0x9D);
	buf.b(0x5C);
	emit_mov_ctx_reg(buf, SLOT_IP, cursor_reg());
	buf.b(0xE9);
	uint8_t* rel = buf.p;
	buf.d(0);
	*jb = (uint8_t)(buf.p - (jb + 1));
	emit_leave_native(buf);
	int32_t skip = (int32_t)(buf.p - (rel + 4));
	memcpy(rel, &skip, 4);
}

uintptr_t emit_level_dispatch(int level, int kind) {
	Buf buf = begin_code(level == 0 ? 1280 : 64);
	uint8_t* entry = buf.start;
	uintptr_t* table = nullptr;
	if (level == 0) {
		if (!g_root)
			g_root = new_table();
		table = g_root;
		buf.b(0xE9);
		uint8_t* rel = buf.p;
		buf.d(0);
		uint8_t* filter_at = buf.p;
		emit_jmp_ctx(buf, SLOT_FILTER);
		int skip_filter = (int)(buf.p - (rel + 4));
		uintptr_t body = (uintptr_t)buf.p;
		emit_mov_reg_ctx(buf, cursor_reg(), SLOT_IP);
		emit_framed_hook(buf, kind, false);
		uintptr_t nofilter = (uintptr_t)buf.p;
		emit_table_jump(buf, table, 0);
		int skip_all = (int)((uint8_t*)nofilter - (rel + 4));
		int32_t initial = skip_filter;
		memcpy(rel, &initial, 4);
		(void)filter_at;
		if (kind == 0) {
			g_inst_rel = rel;
			g_skip_filter = skip_filter;
			g_skip_all = skip_all;
			g_monitor_body = body;
			g_nofilter = nofilter;
		} else {
			g_flow_rel = rel;
			g_flow_skip_filter = skip_filter;
			g_flow_skip_all = skip_all;
			g_flow_body = body;
			g_flow_nofilter = nofilter;
		}
	} else {
		table = new_table();
		emit_table_jump(buf, table, level);
	}
	finish_code(buf);
	register_node(entry, table);
	code_flush(entry, buf.used());
	return (uintptr_t)entry;
}

void patch_monitor(uint8_t* rel, int skip_filter, int skip_all, jitvm_monitor state) {
	if (!rel)
		return;
	int32_t value = 0;
	if (state == JITVM_MONITOR_OFF)
		value = skip_all;
	else if (state == JITVM_MONITOR_SKIP_FILTER)
		value = skip_filter;
	memcpy(rel, &value, 4);
	code_flush(rel - 1, 8);
}

}  // namespace

Buf begin_code(uint32_t cap) {
	uint8_t* p = code_alloc(cap);
	if (p)
		code_grow(p, cap);
	Buf buf;
	buf.start = p;
	buf.p = p;
	buf.end = p ? p + cap : p;
	return buf;
}

void finish_code(Buf& buf) {
	if (buf.start)
		code_flush(buf.start, buf.used());
}

uintptr_t tail_for(uint32_t length) {
	if (length == 0 || length > 16)
		return error_proc();
	return g_tails[length - 1];
}

uintptr_t dispatch_target(bool flow) {
	if (flow && g_flow_entry)
		return g_flow_entry;
	return g_inst_entry;
}

uintptr_t exit_callback_proc() {
	return g_exit_cb;
}

uintptr_t error_proc() {
	return g_error;
}

uintptr_t link_entry() {
	return (uintptr_t)&jitvm_link_entry;
}

bool flow_enabled() {
	return g_on_flow != nullptr && g_flow_entry != 0;
}

uintptr_t set_branch(const uint8_t* ip, uint32_t iclen, uintptr_t proc, const int* extra, int nextra) {
	if (!ip || iclen == 0 || !g_root)
		return proc;
	uintptr_t* table = g_root;
	uintptr_t link = link_entry();
	for (uint32_t i = 0; i < iclen; i++) {
		uint8_t idx = ip[i];
		bool last = i + 1 == iclen;
		if (!last) {
			if (table[idx] == link)
				table[idx] = emit_level_dispatch((int)i + 1, -1);
			uintptr_t* next = table_of(table[idx]);
			if (!next)
				return proc;
			table = next;
			continue;
		}
		uintptr_t* cur = table;
		uint8_t cur_idx = idx;
		for (int e = 0; e < nextra; e++) {
			int off = extra ? extra[e] : -1;
			if (off < 0)
				return proc;
			if (cur[cur_idx] == link)
				cur[cur_idx] = emit_level_dispatch(off, -1);
			uintptr_t* next = table_of(cur[cur_idx]);
			if (!next)
				return proc;
			cur = next;
			cur_idx = ip[off];
		}
		cur[cur_idx] = proc;
		return proc;
	}
	return proc;
}

extern "C" void jitvm_x86_hook(jitvm_regs_x86* image, int kind) {
	if (!image)
		return;
	jitvm_guest_state state;
	memset(&state, 0, sizeof(state));
#if defined(JITVM_HOST_X64)
	state.machine = JITVM_MACHINE_X64;
#else
	state.machine = JITVM_MACHINE_X86;
#endif
	state.regs.x86 = image;
#if !defined(_WIN32)
	image->last_error = host_error();
	image->last_status = 0;
#endif
	jitvm_hook cb = g_on_inst;
	if (kind == 1)
		cb = g_on_flow;
	else if (kind == 2)
		cb = g_on_exit;
	if (cb)
		cb(&state);
#if !defined(_WIN32)
	host_set_error(image->last_error);
#endif
}

extern "C" void jitvm_ensure_stack() {
	uint32_t err = host_error();
	uint32_t status = host_status();
	if (thread_load(ctx_off(SLOT_STACK))) {
		host_set_status(status);
		host_set_error(err);
		return;
	}
	size_t bytes = host64() ? 0x40000 : 0x10000;
	uint8_t* block = (uint8_t*)stack_alloc(bytes);
	if (block) {
		uintptr_t top = (uintptr_t)block + bytes;
		top &= ~(uintptr_t)15;
		top -= 0x40;
		top &= ~(uintptr_t)15;
		thread_store(ctx_off(SLOT_STACK), top);
	}
	host_set_status(status);
	host_set_error(err);
}

extern "C" uintptr_t jitvm_op_link(uintptr_t) {
	uint32_t err = host_error();
	uint32_t status = host_status();
	uintptr_t ip = thread_load(ctx_off(SLOT_IP));
	uintptr_t proc = 0;
	{
		LockGuard guard(code_lock());
		proc = translate((const uint8_t*)ip);
	}
	host_set_status(status);
	host_set_error(err);
	return proc;
}

namespace {

uintptr_t make_clean_exit() {
	Buf buf = begin_code(32);
	emit_push_ctx(buf, SLOT_IP);
	emit_mov_reg_ctx(buf, cursor_reg(), SLOT_RSI);
	buf.b(0xC3);
	finish_code(buf);
	return (uintptr_t)buf.start;
}

uintptr_t make_error() {
	Buf buf = begin_code(8);
	buf.raw("\x0F\x0B", 2);
	finish_code(buf);
	return (uintptr_t)buf.start;
}

uintptr_t make_exit_callback() {
	Buf buf = begin_code(800);
	emit_mov_reg_ctx(buf, cursor_reg(), SLOT_IP);
	emit_framed_hook(buf, 2, true);
	finish_code(buf);
	return (uintptr_t)buf.start;
}

uintptr_t make_tail(uint32_t length) {
	Buf buf = begin_code(40);
	emit_mov_reg_ctx(buf, cursor_reg(), SLOT_IP);
	emit_lea_rsi_imm8(buf, (uint8_t)length);
	emit_mov_ctx_reg(buf, SLOT_IP, cursor_reg());
	emit_jmp_rel(buf, g_inst_entry);
	finish_code(buf);
	return (uintptr_t)buf.start;
}

uintptr_t make_enter() {
	Buf buf = begin_code(256);
	emit_mov_ctx_reg(buf, SLOT_RSI, cursor_reg());
	buf.b(0x9C);
	emit_cmp_ctx_zero(buf, SLOT_STACK);
	buf.b(0x75);
	uint8_t* j1 = buf.p;
	buf.b(0);
	if (host64()) {
		buf.b(0x50);
		buf.raw("\x48\xB8", 2);
		buf.q((uint64_t)&jitvm_ensure_entry);
		buf.raw("\xFF\xD0", 2);
		buf.b(0x58);
	} else {
		buf.b(0xE8);
		uint8_t* at = buf.p;
		buf.d(0);
		int32_t rel = (int32_t)((intptr_t)&jitvm_ensure_entry - (intptr_t)(at + 4));
		memcpy(at, &rel, 4);
	}
	*j1 = (uint8_t)(buf.p - (j1 + 1));
	emit_cmp_ctx_zero(buf, SLOT_FILTER);
	buf.b(0x75);
	uint8_t* j2 = buf.p;
	buf.b(0);
	if (host64()) {
		buf.b(0x50);
		buf.raw("\x48\xB8", 2);
		buf.q(g_monitor_body);
		emit_mov_ctx_reg(buf, SLOT_FILTER, ZYDIS_REGISTER_RAX);
		buf.b(0x58);
	} else {
		buf.b(0xB8);
		buf.d((uint32_t)g_monitor_body);
		emit_mov_ctx_reg(buf, SLOT_FILTER, ZYDIS_REGISTER_EAX);
	}
	*j2 = (uint8_t)(buf.p - (j2 + 1));
	buf.b(0x9D);
	emit_jmp_rel(buf, g_inst_entry);
	finish_code(buf);
	return (uintptr_t)buf.start;
}

}  // namespace

jitvm_enter_fn prepare(const jitvm_config* cfg) {
	LockGuard guard(code_lock());
	if (g_enter)
		return g_enter;
	if (!thread_bind())
		return nullptr;
	init_fp_save();
	g_on_inst = cfg->on_inst;
	g_on_flow = cfg->on_flow;
	g_on_exit = cfg->on_exit;
	g_error = make_error();
	g_clean_exit = make_clean_exit();
	g_exit_cb = make_exit_callback();
	g_inst_entry = emit_level_dispatch(0, 0);
	if (g_on_flow)
		g_flow_entry = emit_level_dispatch(0, 1);
	for (uint32_t i = 1; i <= 16; i++)
		g_tails[i - 1] = make_tail(i);
	thread_store(ctx_off(SLOT_FILTER), g_monitor_body);
	patch_monitor(g_inst_rel, g_skip_filter, g_skip_all, JITVM_MONITOR_SKIP_FILTER);
	patch_monitor(g_flow_rel, g_flow_skip_filter, g_flow_skip_all, JITVM_MONITOR_SKIP_FILTER);
	g_enter = (jitvm_enter_fn)make_enter();
	return g_enter;
}

uintptr_t translate(const uint8_t* ip) {
	return translate_insn(ip);
}

void set_inst_monitor(jitvm_monitor state) {
	patch_monitor(g_inst_rel, g_skip_filter, g_skip_all, state);
}

void set_flow_monitor(jitvm_monitor state) {
	patch_monitor(g_flow_rel, g_flow_skip_filter, g_flow_skip_all, state);
}

jitvm_monitor read_monitor(uint8_t* rel, int skip_filter, int skip_all) {
	if (!rel)
		return JITVM_MONITOR_OFF;
	int32_t value = 0;
	memcpy(&value, rel, 4);
	if (value == 0)
		return JITVM_MONITOR_ALL;
	if (value == skip_filter)
		return JITVM_MONITOR_SKIP_FILTER;
	if (value == skip_all)
		return JITVM_MONITOR_OFF;
	return JITVM_MONITOR_OFF;
}

jitvm_monitor inst_monitor() {
	return read_monitor(g_inst_rel, g_skip_filter, g_skip_all);
}

jitvm_monitor flow_monitor() {
	return read_monitor(g_flow_rel, g_flow_skip_filter, g_flow_skip_all);
}

uintptr_t monitor_body() { return g_monitor_body; }
uintptr_t nofilter_body() { return g_nofilter; }
uintptr_t clean_exit() { return g_clean_exit; }

void* lookup_proc(const uint8_t* bytes, uint32_t available) {
	if (!bytes || !g_root || available == 0)
		return nullptr;
	Decoded inst;
	if (!decode_bytes(bytes, available, &inst))
		return nullptr;
	uint32_t iclen = opcode_length(inst);
	int extra[8];
	int nextra = branch_extras(inst, extra, 8);
	if (nextra < 0 || iclen == 0 || iclen > available)
		return nullptr;
	uintptr_t* table = g_root;
	uintptr_t link = link_entry();
	for (uint32_t i = 0; i < iclen; i++) {
		uintptr_t slot = table[bytes[i]];
		bool last = i + 1 == iclen;
		if (!last) {
			uintptr_t* next = table_of(slot);
			if (!next)
				return nullptr;
			table = next;
			continue;
		}
		for (int e = 0; e < nextra; e++) {
			uintptr_t* next = table_of(slot);
			if (!next || extra[e] < 0 || (uint32_t)extra[e] >= available)
				return nullptr;
			slot = next[bytes[extra[e]]];
		}
		if (slot == 0 || slot == link)
			return nullptr;
		return (void*)slot;
	}
	return nullptr;
}

namespace {

int role_slot(int role) {
	switch (role) {
	case ROLE_FLAGS: return SLOT_FLAGS;
	case ROLE_DATA: return SLOT_DATA;
	case ROLE_FILTER: return SLOT_FILTER;
	case ROLE_IP: return SLOT_IP;
	case ROLE_SAVED_RSI: return SLOT_RSI;
	case ROLE_STACK: return SLOT_STACK;
	case ROLE_FLOWTYPE: return SLOT_FLOWTYPE;
	case ROLE_FLOWIP: return SLOT_FLOWIP;
	default: return SLOT_FLAGS;
	}
}

uintptr_t role_load(int role) {
	thread_bind();
	return thread_load(ctx_off(role_slot(role)));
}

void role_store(int role, uintptr_t value) {
	thread_bind();
	thread_store(ctx_off(role_slot(role)), value);
}

void role_or(uintptr_t flags) {
	thread_bind();
	thread_or_flags(flags);
}

void role_and(uintptr_t mask) {
	thread_bind();
	thread_and_flags(mask);
}

const Backend g_backend = {
	prepare,
	thread_bind,
	role_load,
	role_store,
	role_or,
	role_and,
	set_inst_monitor,
	set_flow_monitor,
	inst_monitor,
	flow_monitor,
	monitor_body,
	nofilter_body,
	clean_exit,
};

}  // namespace

const Backend* backend() {
	return &g_backend;
}

uintptr_t saved_rsi() {
	return thread_load(ctx_off(SLOT_RSI));
}

void set_saved_rsi(uintptr_t value) {
	thread_store(ctx_off(SLOT_RSI), value);
}

}  // namespace x86

const Backend* active_backend() {
	return x86::backend();
}

}  // namespace jitvm

extern "C" uintptr_t jitvm_x86_get_saved_rsi(void) {
	return jitvm::x86::saved_rsi();
}

extern "C" void jitvm_x86_set_saved_rsi(uintptr_t value) {
	jitvm::x86::set_saved_rsi(value);
}

extern "C" uint32_t jitvm_x86_opcode_length(const uint8_t* bytes, uint32_t available) {
	jitvm::x86::Decoded inst;
	if (!jitvm::x86::decode_bytes(bytes, available, &inst))
		return 0;
	return jitvm::x86::opcode_length(inst);
}

extern "C" void* jitvm_x86_translated_proc(const uint8_t* bytes, uint32_t available) {
	return jitvm::x86::lookup_proc(bytes, available);
}
