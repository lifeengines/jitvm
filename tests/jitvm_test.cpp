#include <jitvm/jitvm.h>
#include <jitvm/jitvm_x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <xmmintrin.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <setjmp.h>
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>
#endif

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#if defined(_M_IX86)
#define GUESTCALL __fastcall
#else
#define GUESTCALL
#endif
#else
#define NOINLINE __attribute__((noinline))
#define GUESTCALL
#endif

struct NmRegs {
	uintptr_t rax;
	uintptr_t rcx;
	uintptr_t rdx;
	uintptr_t rbx;
	uintptr_t rsp;
	uintptr_t rbp;
	uintptr_t rsi;
	uintptr_t rdi;
#if defined(_M_X64) || defined(__x86_64__)
	uintptr_t r8;
	uintptr_t r9;
	uintptr_t r10;
	uintptr_t r11;
	uintptr_t r12;
	uintptr_t r13;
	uintptr_t r14;
	uintptr_t r15;
#endif
	uintptr_t rflags;
};

extern "C" {
NmRegs* g_active_regs = nullptr;
void* g_active_code = nullptr;
void* g_enter_proc = nullptr;
uintptr_t g_guest_stack_top = 0;
uintptr_t g_saved_rsp = 0;
void jitvm_run_native();
void jitvm_run_vm();
void jitvm_vm_resume();
}

alignas(16) static uint8_t g_guest_stack[1 << 16];

static const uintptr_t kFlagMask = 0xED7;
static const uintptr_t kNoStop = ~(uintptr_t)0;
enum { kLogCap = 8192 };

struct InstRec { uintptr_t rip; };
struct FlowRec { uintptr_t insn; uintptr_t dest; uintptr_t type; };

static InstRec g_inst[kLogCap];
static int g_inst_n = 0;
static FlowRec g_flow[kLogCap];
static int g_flow_n = 0;
static int g_abort = 0;
static int g_exit_cb = 0;
static int g_inst_limit = 100000;
static uintptr_t g_stop_rip = 0;
static int g_passed = 0;
static int g_failed = 0;
static NmRegs g_last_vm;
static const char* g_test = "";
static const char* g_filter = nullptr;
static uint8_t* g_code = nullptr;
static jitvm_enter_fn g_enter = nullptr;

#if !defined(_WIN32)
static sigjmp_buf g_jump;
static struct sigaction g_old_trap;
static struct sigaction g_old_segv;
static struct sigaction g_old_ill;
static void on_signal(int) { siglongjmp(g_jump, 1); }
#endif

static void fail(int line, const char* msg) {
	printf("  FAIL %s(%d): %s\n", g_test, line, msg);
	g_failed++;
}

#define REQUIRE(cond) do { if (!(cond)) { fail(__LINE__, #cond); return; } } while (0)

static void note_stop(uintptr_t rip) {
	if (g_stop_rip != kNoStop && rip == g_stop_rip)
		jitvm_set_exit_flag();
	if (g_inst_n + g_flow_n > g_inst_limit) {
		g_abort = 1;
		jitvm_set_exit_flag();
	}
}

static int g_clobber_xmm;

static void on_inst(jitvm_guest_state* state) {
	if (g_clobber_xmm) {
		uint8_t src[64];
		uint8_t dst[64];
		memset(src, 0xA5, sizeof(src));
		memcpy(dst, src, sizeof(src));
		_mm_storeu_ps((float*)dst, _mm_setzero_ps());
	}
	uintptr_t rip = (uintptr_t)state->regs.x86->rip;
	if (g_inst_n < kLogCap)
		g_inst[g_inst_n++].rip = rip;
	note_stop(rip);
}

static void on_flow(jitvm_guest_state* state) {
	if (g_flow_n < kLogCap) {
		g_flow[g_flow_n].insn = jitvm_last_flow_ip();
		g_flow[g_flow_n].dest = (uintptr_t)state->regs.x86->rip;
		g_flow[g_flow_n].type = jitvm_last_flow_type();
		g_flow_n++;
	}
	note_stop((uintptr_t)state->regs.x86->rip);
}

static void on_exit(jitvm_guest_state* state) {
	g_exit_cb++;
	uintptr_t rip = (uintptr_t)state->regs.x86->rip;
	if (rip && *(uint8_t*)rip == 0xCC)
		state->regs.x86->rip = (intptr_t)(rip + 1);
}

static void fill_regs(NmRegs* r) {
	memset(r, 0, sizeof(*r));
	r->rax = 0x11111111;
	r->rcx = 0x22222222;
	r->rdx = 0x33333333;
	r->rbx = 0x44444444;
	r->rbp = 0x55555555;
	r->rsi = 0x66666666;
	r->rdi = 0x77777777;
#if defined(_M_X64) || defined(__x86_64__)
	r->r8 = 0x88888888;
	r->r9 = 0x99999999;
	r->r10 = 0xAAAAAAAA;
	r->r11 = 0xBBBBBBBB;
	r->r12 = 0xCCCCCCCC;
	r->r13 = 0xDDDDDDDD;
	r->r14 = 0xEEEEEEEE;
	r->r15 = 0xFFFFFFFF;
#endif
	r->rflags = 0x202;
}

static void flush_code(void* p, size_t n) {
#if defined(_WIN32)
	FlushInstructionCache(GetCurrentProcess(), p, n);
#else
	__builtin___clear_cache((char*)p, (char*)p + n);
#endif
}

static void* publish(const uint8_t* bytes, size_t n) {
	if (!g_code) {
#if defined(_WIN32)
		g_code = (uint8_t*)VirtualAlloc(nullptr, 0x10000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
		g_code = (uint8_t*)mmap(nullptr, 0x10000, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (g_code == MAP_FAILED)
			g_code = nullptr;
#endif
		if (!g_code)
			return nullptr;
	}
	memset(g_code, 0xCC, 64);
	memcpy(g_code, bytes, n);
	flush_code(g_code, n + 16);
	return g_code;
}

static int exec_call(void (*fn)()) {
#if defined(_WIN32)
	__try {
		fn();
		return 0;
	} __except (EXCEPTION_EXECUTE_HANDLER) {
		return (int)GetExceptionCode();
	}
#else
	struct sigaction act;
	memset(&act, 0, sizeof(act));
	act.sa_handler = on_signal;
	sigaction(SIGTRAP, &act, &g_old_trap);
	sigaction(SIGSEGV, &act, &g_old_segv);
	sigaction(SIGILL, &act, &g_old_ill);
	int rc = 0;
	if (sigsetjmp(g_jump, 1) == 0)
		fn();
	else
		rc = 0x80000003;
	sigaction(SIGTRAP, &g_old_trap, nullptr);
	sigaction(SIGSEGV, &g_old_segv, nullptr);
	sigaction(SIGILL, &g_old_ill, nullptr);
	return rc;
#endif
}

static int exec_native(void* code, NmRegs* regs) {
	g_active_regs = regs;
	g_active_code = code;
	return exec_call(jitvm_run_native);
}

static int exec_vm(void* code, NmRegs* regs, uintptr_t stop) {
	g_active_regs = regs;
	g_active_code = code;
	g_stop_rip = stop ? stop : (uintptr_t)jitvm_vm_resume;
	g_inst_n = 0;
	g_flow_n = 0;
	g_abort = 0;
	g_exit_cb = 0;
	jitvm_set_guest_ip((uintptr_t)code);
	jitvm_clear_exit_flag();
	return exec_call(jitvm_run_vm);
}

struct RegName { const char* name; size_t offset; };
static const RegName kRegNames[] = {
	{"rax", offsetof(NmRegs, rax)},
	{"rcx", offsetof(NmRegs, rcx)},
	{"rdx", offsetof(NmRegs, rdx)},
	{"rbx", offsetof(NmRegs, rbx)},
	{"rbp", offsetof(NmRegs, rbp)},
	{"rsi", offsetof(NmRegs, rsi)},
	{"rdi", offsetof(NmRegs, rdi)},
#if defined(_M_X64) || defined(__x86_64__)
	{"r8", offsetof(NmRegs, r8)},
	{"r9", offsetof(NmRegs, r9)},
	{"r10", offsetof(NmRegs, r10)},
	{"r11", offsetof(NmRegs, r11)},
	{"r12", offsetof(NmRegs, r12)},
	{"r13", offsetof(NmRegs, r13)},
	{"r14", offsetof(NmRegs, r14)},
	{"r15", offsetof(NmRegs, r15)},
#endif
};

static bool same_regs(const NmRegs& native, const NmRegs& vm) {
	bool ok = true;
	for (size_t i = 0; i < sizeof(kRegNames) / sizeof(kRegNames[0]); i++) {
		uintptr_t a = *(uintptr_t*)((const uint8_t*)&native + kRegNames[i].offset);
		uintptr_t b = *(uintptr_t*)((const uint8_t*)&vm + kRegNames[i].offset);
		if (a != b) {
			printf("  reg %s native=%p vm=%p\n", kRegNames[i].name, (void*)a, (void*)b);
			ok = false;
		}
	}
	if ((native.rflags & kFlagMask) != (vm.rflags & kFlagMask)) {
		printf("  rflags native=%p vm=%p\n", (void*)native.rflags, (void*)vm.rflags);
		ok = false;
	}
	return ok;
}

static void dump_trace() {
	printf("  inst %d flow %d exit %d\n", g_inst_n, g_flow_n, g_exit_cb);
	int n = g_inst_n < 8 ? g_inst_n : 8;
	for (int i = 0; i < n; i++)
		printf("  inst %p\n", (void*)g_inst[i].rip);
	n = g_flow_n < 8 ? g_flow_n : 8;
	for (int i = 0; i < n; i++)
		printf("  flow type %u %p -> %p\n", (unsigned)g_flow[i].type, (void*)g_flow[i].insn, (void*)g_flow[i].dest);
}

static bool diff_code(const uint8_t* bytes, size_t n, const NmRegs& seed, const void* mem, size_t mem_n, uint8_t* native_mem, uint8_t* vm_mem) {
	void* code = publish(bytes, n);
	if (!code) {
		fail(__LINE__, "publish");
		return false;
	}
	NmRegs native = seed;
	NmRegs vm = seed;
	uintptr_t native_base = 0;
	uintptr_t vm_base = 0;
	if (mem && mem_n) {
		memcpy(native_mem, mem, mem_n);
		memcpy(vm_mem, mem, mem_n);
		native_base = (uintptr_t)native_mem;
		vm_base = (uintptr_t)vm_mem;
		native.rcx = native_base;
		vm.rcx = vm_base;
	}
	int cn = exec_native(code, &native);
	if (cn) {
		printf("  native exception %08X\n", cn);
		fail(__LINE__, "native exec");
		return false;
	}
	int cv = exec_vm(code, &vm, 0);
	if (cv) {
		printf("  vm exception %08X\n", cv);
		dump_trace();
		fail(__LINE__, "vm exec");
		return false;
	}
	if (g_abort) {
		dump_trace();
		fail(__LINE__, "vm abort");
		return false;
	}
	if (mem && mem_n) {
		if ((native.rcx - native_base) != (vm.rcx - vm_base)) {
			fail(__LINE__, "rcx drift");
			return false;
		}
		native.rcx = 0;
		vm.rcx = 0;
	}
	if (!same_regs(native, vm)) {
		dump_trace();
		fail(__LINE__, "regs differ");
		return false;
	}
	if (mem && mem_n && memcmp(native_mem, vm_mem, mem_n) != 0) {
		fail(__LINE__, "memory differs");
		return false;
	}
	g_last_vm = vm;
	return true;
}

static bool flow_is(const uintptr_t* types, int n) {
	if (g_flow_n != n)
		return false;
	for (int i = 0; i < n; i++) {
		if (g_flow[i].type != types[i])
			return false;
	}
	return true;
}

static bool bytes_contain(const uint8_t* p, int n, const void* needle, int ns) {
	if (!p || n < ns)
		return false;
	for (int i = 0; i + ns <= n; i++) {
		if (memcmp(p + i, needle, ns) == 0)
			return true;
	}
	return false;
}

static void restore_monitor() {
	jitvm_set_inst_monitor(JITVM_MONITOR_SKIP_FILTER);
	jitvm_set_flow_monitor(JITVM_MONITOR_SKIP_FILTER);
	jitvm_set_filter(jitvm_default_filter());
	jitvm_clear_exit_flag();
}

#if defined(_MSC_VER)
#pragma optimize("", off)
#endif
NOINLINE int GUESTCALL guest_add(int a, int b) {
	volatile int x = a;
	x += b;
	return x;
}
NOINLINE int GUESTCALL guest_max(int a, int b) {
	volatile int va = a;
	volatile int vb = b;
	if (va >= vb)
		return va;
	return vb;
}
NOINLINE int GUESTCALL guest_leaf(int x) {
	volatile int v = x;
	return v + 3;
}
NOINLINE int GUESTCALL guest_nest(int x) {
	volatile int v = guest_leaf(x);
	return v * 5;
}
NOINLINE int GUESTCALL guest_sum(int* p, int n) {
	volatile int s = 0;
	volatile int i = 0;
	volatile int nn = n;
	while (i < nn) {
		s += p[i];
		i += 1;
	}
	p[0] = s;
	return s;
}
#if defined(_MSC_VER)
#pragma optimize("", on)
#endif

static void test_context_slots() {
	uintptr_t ip = jitvm_get_guest_ip();
	uintptr_t rsi = jitvm_x86_get_saved_rsi();
	uintptr_t data = jitvm_get_data();
	uintptr_t stk = jitvm_get_vstack();
	uintptr_t filt = jitvm_get_filter();
	uintptr_t flags = jitvm_get_flags();
	jitvm_set_guest_ip(0x12345000);
	jitvm_x86_set_saved_rsi(0x23456000);
	jitvm_set_data(0x34567000);
	jitvm_set_vstack(0x45678000);
	jitvm_set_filter(0x56789000);
	REQUIRE(jitvm_get_guest_ip() == 0x12345000);
	REQUIRE(jitvm_x86_get_saved_rsi() == 0x23456000);
	REQUIRE(jitvm_get_data() == 0x34567000);
	REQUIRE(jitvm_get_vstack() == 0x45678000);
	REQUIRE(jitvm_get_filter() == 0x56789000);
	jitvm_clear_flags(flags | JITVM_FLAG_EXIT | JITVM_FLAG_MATCH);
	jitvm_set_exit_flag();
	REQUIRE((jitvm_get_flags() & JITVM_FLAG_EXIT) != 0);
	jitvm_clear_exit_flag();
	REQUIRE((jitvm_get_flags() & JITVM_FLAG_EXIT) == 0);
	jitvm_set_guest_ip(ip);
	jitvm_x86_set_saved_rsi(rsi);
	jitvm_set_data(data);
	jitvm_set_vstack(stk);
	jitvm_set_filter(filt);
	jitvm_clear_flags(~flags);
}

static void test_monitor_state() {
	REQUIRE(jitvm_inst_monitor() == JITVM_MONITOR_SKIP_FILTER);
	REQUIRE(jitvm_flow_monitor() == JITVM_MONITOR_SKIP_FILTER);
	jitvm_set_inst_monitor(JITVM_MONITOR_OFF);
	jitvm_set_flow_monitor(JITVM_MONITOR_OFF);
	REQUIRE(jitvm_inst_monitor() == JITVM_MONITOR_OFF);
	REQUIRE(jitvm_flow_monitor() == JITVM_MONITOR_OFF);
	jitvm_set_inst_monitor(JITVM_MONITOR_ALL);
	jitvm_set_flow_monitor(JITVM_MONITOR_ALL);
	REQUIRE(jitvm_inst_monitor() == JITVM_MONITOR_ALL);
	REQUIRE(jitvm_flow_monitor() == JITVM_MONITOR_ALL);
	restore_monitor();
}

static void test_code_space() {
	void* p = jitvm_alloc_code(64);
	REQUIRE(p != nullptr);
	REQUIRE(jitvm_ip_in_code((uintptr_t)p) == 1);
	REQUIRE(jitvm_ip_in_code(1) == 0);
	REQUIRE(jitvm_grow_code(p, 32) == 32);
	REQUIRE(jitvm_grow_code(p, 16) == 0);
}

static void expect_iclen(const uint8_t* bytes, uint32_t n, uint32_t expect) {
	uint32_t got = jitvm_x86_opcode_length(bytes, n);
	if (got != expect) {
		printf("  iclen %u expect %u\n", got, expect);
		fail(__LINE__, "icode");
	}
}

static void test_icode_len() {
	static const uint8_t kNop[] = {0x90};
	static const uint8_t kRet[] = {0xC3};
	static const uint8_t kAddRR[] = {0x01, 0xD8};
	static const uint8_t kMovImm[] = {0xB8, 0x78, 0x56, 0x34, 0x12};
	static const uint8_t kAddImm[] = {0x83, 0xC0, 0x07};
	static const uint8_t kJmp[] = {0xE9, 0x00, 0x00, 0x00, 0x00};
	static const uint8_t kJz[] = {0x74, 0x00};
	static const uint8_t kShl[] = {0xC1, 0xE0, 0x03};
	static const uint8_t kShr1[] = {0xD1, 0xE8};
	static const uint8_t kMovDisp8[] = {0x89, 0x41, 0x04};
	static const uint8_t kMovDisp32[] = {0x89, 0x81, 0x78, 0x56, 0x34, 0x12};
	expect_iclen(kNop, 1, 1);
	expect_iclen(kRet, 1, 1);
	expect_iclen(kAddRR, 2, 2);
	expect_iclen(kMovImm, 5, 1);
	expect_iclen(kAddImm, 3, 2);
	expect_iclen(kJmp, 5, 1);
	expect_iclen(kJz, 2, 1);
	expect_iclen(kShl, 3, 2);
	expect_iclen(kShr1, 2, 2);
	expect_iclen(kMovDisp8, 3, 2);
	expect_iclen(kMovDisp32, 6, 2);
#if defined(_M_X64) || defined(__x86_64__)
	static const uint8_t kMov64[] = {0x48, 0xB8, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
	expect_iclen(kMov64, 10, 2);
#endif
}

static void test_vm_proc_before_use() {
	static const uint8_t kNot[] = {0xF6, 0xD0};
	REQUIRE(jitvm_x86_translated_proc(kNot, sizeof(kNot)) == nullptr);
}

static void test_filter_encoding() {
	const uintptr_t range_s = 0x13572468;
	const uintptr_t range_e = 0x24681357;
	jitvm_x86_range_filter* in_range = jitvm_x86_range_filter_create(0, 0, 0);
	jitvm_x86_range_filter* out_range = jitvm_x86_range_filter_create(0x11111111, 0x22222222, 1);
	REQUIRE(jitvm_x86_range_filter_proc(in_range) != 0);
	REQUIRE(jitvm_x86_range_filter_proc(out_range) != 0);
	REQUIRE(jitvm_x86_range_filter_start(out_range) == 0x11111111);
	REQUIRE(jitvm_x86_range_filter_end(out_range) == 0x22222222);
	jitvm_x86_range_filter_set_range(in_range, range_s, range_e);
	jitvm_x86_range_filter_set_range(out_range, range_s, range_e);
	REQUIRE(jitvm_x86_range_filter_start(out_range) == range_s);
	REQUIRE(jitvm_x86_range_filter_end(out_range) == range_e);
	const uint8_t* in_bytes = (const uint8_t*)jitvm_x86_range_filter_proc(in_range);
	const uint8_t* out_bytes = (const uint8_t*)jitvm_x86_range_filter_proc(out_range);
	REQUIRE(bytes_contain(in_bytes, 160, &range_s, sizeof(range_s)));
	REQUIRE(bytes_contain(in_bytes, 160, &range_e, sizeof(range_e)));
	REQUIRE(bytes_contain(out_bytes, 160, &range_s, sizeof(range_s)));
	REQUIRE(bytes_contain(out_bytes, 160, &range_e, sizeof(range_e)));
	uintptr_t match = jitvm_nofilter_exit();
	uintptr_t unmatch = jitvm_nofilter();
	REQUIRE(match != 0);
	REQUIRE(unmatch != 0);
	REQUIRE(match != unmatch);
	jitvm_x86_range_filter_set_targets(out_range, match, unmatch);
#if defined(_M_X64) || defined(__x86_64__)
	REQUIRE(bytes_contain(out_bytes, 200, &match, sizeof(match)));
	REQUIRE(bytes_contain(out_bytes, 200, &unmatch, sizeof(unmatch)));
#endif
	REQUIRE(*(uint8_t*)jitvm_x86_range_filter_proc(out_range) == 0x9C);
	jitvm_x86_range_filter_destroy(in_range);
	jitvm_x86_range_filter_destroy(out_range);
}

static void test_alu_regs() {
	static const uint8_t kCode[] = {
		0xB8, 0x10, 0x00, 0x00, 0x00,
		0x83, 0xC0, 0x05,
		0x83, 0xE8, 0x01,
		0x31, 0xD8,
		0xF7, 0xD0,
		0xFF, 0xC0,
		0xC3
	};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
}

static void test_known_imul_and_shift() {
	static const uint8_t kImul[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0x69, 0xC0, 0x06, 0x00, 0x00, 0x00, 0xC3};
	static const uint8_t kShl[] = {0xB8, 0x03, 0x00, 0x00, 0x00, 0xC1, 0xE0, 0x03, 0xD1, 0xE8, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kImul, sizeof(kImul), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 42);
	REQUIRE(diff_code(kShl, sizeof(kShl), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == ((3 << 3) >> 1));
}

static void test_flags_sub_zero() {
	static const uint8_t kCode[] = {0xB8, 0x05, 0x00, 0x00, 0x00, 0x83, 0xE8, 0x05, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((g_last_vm.rflags & 0x40) != 0);
}

static void test_push_pop_and_lea() {
	static const uint8_t kPush[] = {0xB8, 0x42, 0x00, 0x00, 0x00, 0x50, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x58, 0xC3};
	static const uint8_t kLea[] = {0x8D, 0x44, 0x51, 0x01, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kPush, sizeof(kPush), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 0x42);
	seed.rcx = 10;
	seed.rdx = 7;
	REQUIRE(diff_code(kLea, sizeof(kLea), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 25);
}

static void test_movzx() {
	static const uint8_t kCode[] = {0xB1, 0x80, 0x0F, 0xB6, 0xC1, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 0x80);
}

static void test_memory_ops() {
	static const uint8_t kAdd[] = {0x83, 0x01, 0x05, 0x8B, 0x01, 0xC3};
	static const uint8_t kShl[] = {0xC1, 0x21, 0x03, 0x8B, 0x01, 0xC3};
	uint8_t mem[8], native_mem[8], vm_mem[8];
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 10;
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kAdd, sizeof(kAdd), seed, mem, sizeof(mem), native_mem, vm_mem));
	REQUIRE(*(uint32_t*)vm_mem == 15);
	REQUIRE((uint32_t)g_last_vm.rax == 15);
	*(uint32_t*)mem = 3;
	REQUIRE(diff_code(kShl, sizeof(kShl), seed, mem, sizeof(mem), native_mem, vm_mem));
	REQUIRE(*(uint32_t*)vm_mem == 24);
}

static void test_rsi_is_a_real_register() {
#if defined(_M_X64) || defined(__x86_64__)
	static const uint8_t kCode[] = {0x48, 0x31, 0xF6, 0x48, 0x81, 0xC6, 0x34, 0x12, 0x00, 0x00, 0x48, 0x89, 0xF0, 0xC3};
#else
	static const uint8_t kCode[] = {0x31, 0xF6, 0x81, 0xC6, 0x34, 0x12, 0x00, 0x00, 0x89, 0xF0, 0xC3};
#endif
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE(g_last_vm.rax == 0x1234);
	REQUIRE(g_last_vm.rsi == 0x1234);
}

static void test_control_flow() {
	static const uint8_t kJmp[] = {0xB8, 0x04, 0x00, 0x00, 0x00, 0xEB, 0x05, 0xB8, 0x05, 0x00, 0x00, 0x00, 0xC3};
	static const uint8_t kJzTaken[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x01, 0x74, 0x05, 0xB8, 0x00, 0x00, 0x00, 0x00, 0xC3};
	static const uint8_t kJzSkip[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x02, 0x74, 0x05, 0xB8, 0x09, 0x00, 0x00, 0x00, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kJmp, sizeof(kJmp), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 4);
	const uintptr_t jmp_flow[] = {JITVM_FLOW_JUMP_IMM, JITVM_FLOW_RETURN};
	if (!flow_is(jmp_flow, 2)) {
		dump_trace();
		fail(__LINE__, "jmp flow sequence");
		return;
	}
	REQUIRE(diff_code(kJzTaken, sizeof(kJzTaken), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 1);
	const uintptr_t jcc_flow[] = {JITVM_FLOW_COND, JITVM_FLOW_RETURN};
	if (!flow_is(jcc_flow, 2)) {
		dump_trace();
		fail(__LINE__, "jcc taken flow sequence");
		return;
	}
	REQUIRE(diff_code(kJzSkip, sizeof(kJzSkip), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 9);
	if (!flow_is(jcc_flow, 2)) {
		dump_trace();
		fail(__LINE__, "jcc skip flow sequence");
	}
}

static void test_call_ret() {
	static const uint8_t kCode[] = {0xB8, 0x07, 0x00, 0x00, 0x00, 0xE8, 0x01, 0x00, 0x00, 0x00, 0xC3, 0x83, 0xC0, 0x03, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	REQUIRE((uint32_t)g_last_vm.rax == 10);
	const uintptr_t expect[] = {JITVM_FLOW_CALL_IMM, JITVM_FLOW_RETURN, JITVM_FLOW_RETURN};
	if (!flow_is(expect, 3)) {
		dump_trace();
		fail(__LINE__, "call/ret flow sequence");
	}
}

static void test_call_reg_and_jmp_reg() {
	uint8_t code[64];
	memset(code, 0xCC, sizeof(code));
	REQUIRE(publish(code, sizeof(code)) != nullptr);
	uint8_t* p = g_code;
	p[0] = 0xB8;
	*(uint32_t*)(p + 1) = 5;
	int call_at = 5;
#if defined(_M_X64) || defined(__x86_64__)
	p[call_at] = 0x48;
	p[call_at + 1] = 0xB9;
	int imm_at = call_at + 2;
	int call_op = call_at + 10;
#else
	p[call_at] = 0xB9;
	int imm_at = call_at + 1;
	int call_op = call_at + 5;
#endif
	p[call_op] = 0xFF;
	p[call_op + 1] = 0xD1;
	p[call_op + 2] = 0xC3;
	int func = call_op + 3;
	p[func] = 0x83;
	p[func + 1] = 0xC0;
	p[func + 2] = 0x03;
	p[func + 3] = 0xC3;
#if defined(_M_X64) || defined(__x86_64__)
	*(uint64_t*)(p + imm_at) = (uint64_t)(p + func);
#else
	*(uint32_t*)(p + imm_at) = (uint32_t)(p + func);
#endif
	flush_code(p, 64);
	NmRegs seed;
	fill_regs(&seed);
	NmRegs native = seed;
	NmRegs vm = seed;
	REQUIRE(exec_native(p, &native) == 0);
	REQUIRE(exec_vm(p, &vm, 0) == 0);
	REQUIRE(!g_abort);
	REQUIRE(same_regs(native, vm));
	REQUIRE((uint32_t)vm.rax == 8);
	bool saw_call_reg = false;
	bool saw_ret = false;
	for (int i = 0; i < g_flow_n; i++) {
		if (g_flow[i].type == JITVM_FLOW_CALL_REG)
			saw_call_reg = true;
		if (g_flow[i].type == JITVM_FLOW_RETURN)
			saw_ret = true;
	}
	if (!saw_call_reg || !saw_ret) {
		dump_trace();
		fail(__LINE__, "call reg flow");
		return;
	}
	memset(p, 0xCC, 64);
	p[0] = 0xB8;
	*(uint32_t*)(p + 1) = 1;
#if defined(_M_X64) || defined(__x86_64__)
	p[5] = 0x48;
	p[6] = 0xB9;
	*(uint64_t*)(p + 7) = (uint64_t)(p + 22);
	p[15] = 0xFF;
	p[16] = 0xE1;
	p[17] = 0xB8;
	*(uint32_t*)(p + 18) = 2;
	p[22] = 0xC3;
#else
	p[5] = 0xB9;
	*(uint32_t*)(p + 6) = (uint32_t)(p + 17);
	p[10] = 0xFF;
	p[11] = 0xE1;
	p[12] = 0xB8;
	*(uint32_t*)(p + 13) = 2;
	p[17] = 0xC3;
#endif
	flush_code(p, 64);
	native = seed;
	vm = seed;
	REQUIRE(exec_native(p, &native) == 0);
	REQUIRE(exec_vm(p, &vm, 0) == 0);
	REQUIRE(same_regs(native, vm));
	REQUIRE((uint32_t)vm.rax == 1);
	bool saw_jmp_reg = false;
	for (int i = 0; i < g_flow_n; i++) {
		if (g_flow[i].type == JITVM_FLOW_JUMP_REG)
			saw_jmp_reg = true;
	}
	if (!saw_jmp_reg) {
		dump_trace();
		fail(__LINE__, "jmp reg flow");
	}
}

static void test_vm_proc_cached() {
	static const uint8_t kCode[] = {0xB8, 0x78, 0x56, 0x34, 0x12, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
	void* proc1 = jitvm_x86_translated_proc(g_code, 16);
	void* proc2 = jitvm_x86_translated_proc(g_code, 16);
	REQUIRE(proc1 != nullptr);
	REQUIRE(proc1 == proc2);
	REQUIRE(jitvm_ip_in_code((uintptr_t)proc1) == 1);
}

static void test_exit_flag_stops_vm() {
	static const uint8_t kCode[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0x90, 0xB8, 0x02, 0x00, 0x00, 0x00, 0xC3};
	void* code = publish(kCode, sizeof(kCode));
	REQUIRE(code != nullptr);
	NmRegs vm;
	fill_regs(&vm);
	uintptr_t nop = (uintptr_t)code + 5;
	uintptr_t second = (uintptr_t)code + 6;
	REQUIRE(exec_vm(code, &vm, nop) == 0);
	REQUIRE(!g_abort);
	REQUIRE((uint32_t)vm.rax == 2);
	bool saw_nop = false;
	bool saw_second = false;
	for (int i = 0; i < g_inst_n; i++) {
		if (g_inst[i].rip == nop)
			saw_nop = true;
		if (g_inst[i].rip == second)
			saw_second = true;
	}
	REQUIRE(saw_nop);
	REQUIRE(!saw_second);
	REQUIRE(g_exit_cb == 0);
}

static void test_int3_exits_vm() {
	static const uint8_t kCode[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xCC, 0xB8, 0x22, 0x00, 0x00, 0x00, 0xC3};
	void* code = publish(kCode, sizeof(kCode));
	REQUIRE(code != nullptr);
	REQUIRE(jitvm_x86_opcode_length((uint8_t*)code + 5, 1) == 1);
	NmRegs vm;
	fill_regs(&vm);
	int cv = exec_vm(code, &vm, 0);
	if (cv) {
		printf("  vm exception %08X\n", cv);
		dump_trace();
		fail(__LINE__, "int3 should be redirected by exit callback");
		return;
	}
	REQUIRE(g_exit_cb == 1);
	REQUIRE((uint32_t)vm.rax == 0x22);
	bool saw_int3 = false;
	bool saw_second = false;
	uintptr_t int3_ip = (uintptr_t)code + 5;
	uintptr_t second = (uintptr_t)code + 6;
	for (int i = 0; i < g_inst_n; i++) {
		if (g_inst[i].rip == int3_ip)
			saw_int3 = true;
		if (g_inst[i].rip == second)
			saw_second = true;
	}
	REQUIRE(saw_int3);
	REQUIRE(!saw_second);
}

static void test_last_error_preserved() {
	static const uint8_t kCode[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3};
	NmRegs seed;
	fill_regs(&seed);
#if defined(_WIN32)
	SetLastError(0x2345);
#else
	errno = 0x2345;
#endif
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, nullptr, 0, nullptr, nullptr));
#if defined(_WIN32)
	REQUIRE(GetLastError() == 0x2345);
#else
	REQUIRE(errno == 0x2345);
#endif
}

static bool diff_fn(void* fn, const NmRegs& seed) {
	NmRegs native = seed;
	NmRegs vm = seed;
	int cn = exec_native(fn, &native);
	if (cn) {
		fail(__LINE__, "native fn");
		return false;
	}
	int cv = exec_vm(fn, &vm, 0);
	if (cv) {
		printf("  vm exception %08X\n", cv);
		dump_trace();
		fail(__LINE__, "vm fn");
		return false;
	}
	if (g_abort || !same_regs(native, vm)) {
		dump_trace();
		fail(__LINE__, "fn regs differ");
		return false;
	}
	g_last_vm = vm;
	return true;
}

static void test_compiled_guests() {
	NmRegs seed;
	fill_regs(&seed);
#if defined(_WIN32) || !defined(__x86_64__)
	seed.rcx = 20;
	seed.rdx = 22;
#else
	seed.rdi = 20;
	seed.rsi = 22;
#endif
	REQUIRE(diff_fn((void*)guest_add, seed));
	REQUIRE((int)g_last_vm.rax == 42);
	seed.rcx = 3;
	seed.rdx = 7;
#if !defined(_WIN32) && defined(__x86_64__)
	seed.rdi = 3;
	seed.rsi = 7;
#endif
	REQUIRE(diff_fn((void*)guest_max, seed));
	REQUIRE((int)g_last_vm.rax == 7);
	seed.rcx = 9;
	seed.rdx = 2;
#if !defined(_WIN32) && defined(__x86_64__)
	seed.rdi = 9;
	seed.rsi = 2;
#endif
	REQUIRE(diff_fn((void*)guest_max, seed));
	REQUIRE((int)g_last_vm.rax == 9);
	seed.rcx = 4;
#if !defined(_WIN32) && defined(__x86_64__)
	seed.rdi = 4;
#endif
	REQUIRE(diff_fn((void*)guest_nest, seed));
	REQUIRE((int)g_last_vm.rax == 35);
	int native_buf[4] = {1, 2, 3, 4};
	int vm_buf[4] = {1, 2, 3, 4};
	NmRegs native = seed;
	NmRegs vm = seed;
#if defined(_WIN32) || !defined(__x86_64__)
	native.rcx = (uintptr_t)native_buf;
	native.rdx = 4;
	vm.rcx = (uintptr_t)vm_buf;
	vm.rdx = 4;
#else
	native.rdi = (uintptr_t)native_buf;
	native.rsi = 4;
	vm.rdi = (uintptr_t)vm_buf;
	vm.rsi = 4;
#endif
	REQUIRE(exec_native((void*)guest_sum, &native) == 0);
	REQUIRE(exec_vm((void*)guest_sum, &vm, 0) == 0);
	REQUIRE(!g_abort);
	REQUIRE(native.rax == vm.rax);
	REQUIRE((int)vm.rax == 10);
	REQUIRE(vm_buf[0] == 10);
}

static bool diff_regs(const uint8_t* code, size_t n, NmRegs seed) {
	return diff_code(code, n, seed, nullptr, 0, nullptr, nullptr);
}

static bool diff_mem(const uint8_t* code, size_t n, NmRegs seed, const void* mem, size_t mem_n) {
	uint8_t native_mem[64];
	uint8_t vm_mem[64];
	if (mem_n > sizeof(native_mem))
		return false;
	return diff_code(code, n, seed, mem, mem_n, native_mem, vm_mem);
}

static void test_relocated_operands() {
	NmRegs seed;
	fill_regs(&seed);
	static const uint8_t kMovImm[] = {
		0xC7, 0x41, 0x00, 0x11, 0x22, 0x33, 0x44,
		0xC7, 0x41, 0x04, 0x55, 0x66, 0x77, 0x88,
		0xC3
	};
	uint8_t mem[16];
	memset(mem, 0, sizeof(mem));
	REQUIRE(diff_mem(kMovImm, sizeof(kMovImm), seed, mem, sizeof(mem)));
	static const uint8_t kInc[] = {0xFF, 0x41, 0x00, 0xFF, 0x41, 0x04, 0xC3};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 1;
	*(uint32_t*)(mem + 4) = 2;
	REQUIRE(diff_mem(kInc, sizeof(kInc), seed, mem, sizeof(mem)));
	static const uint8_t kShl[] = {0xC1, 0x41, 0x00, 0x03, 0xC1, 0x41, 0x04, 0x01, 0xC3};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 1;
	*(uint32_t*)(mem + 4) = 1;
	REQUIRE(diff_mem(kShl, sizeof(kShl), seed, mem, sizeof(mem)));
	static const uint8_t kCmp[] = {
		0x83, 0x79, 0x00, 0x05, 0x0F, 0x94, 0xC0,
		0x83, 0x79, 0x04, 0x01, 0x0F, 0x94, 0xC3,
		0xC3
	};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 5;
	*(uint32_t*)(mem + 4) = 2;
	REQUIRE(diff_mem(kCmp, sizeof(kCmp), seed, mem, sizeof(mem)));
	static const uint8_t kBt[] = {
		0x0F, 0xBA, 0x61, 0x00, 0x00, 0x0F, 0x92, 0xC0,
		0x0F, 0xBA, 0x61, 0x04, 0x01, 0x0F, 0x92, 0xC3,
		0x0F, 0xBA, 0x61, 0x00, 0x20, 0x0F, 0x92, 0xC2,
		0xC3
	};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 1;
	*(uint32_t*)(mem + 4) = 2;
	REQUIRE(diff_mem(kBt, sizeof(kBt), seed, mem, sizeof(mem)));
	static const uint8_t kNeg[] = {0xF7, 0x59, 0x00, 0xF7, 0x59, 0x04, 0xC3};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 3;
	*(uint32_t*)(mem + 4) = 4;
	REQUIRE(diff_mem(kNeg, sizeof(kNeg), seed, mem, sizeof(mem)));
	static const uint8_t kSign[] = {0xB8, 0x05, 0x00, 0x00, 0x00, 0x83, 0xC0, 0xFF, 0xC3};
	REQUIRE(diff_regs(kSign, sizeof(kSign), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 4);
	static const uint8_t kSign64[] = {0x48, 0x83, 0xC0, 0xFF, 0xC3};
	seed.rax = 0x11111111;
	REQUIRE(diff_regs(kSign64, sizeof(kSign64), seed));
	REQUIRE(g_last_vm.rax == 0x11111110);
	static const uint8_t kPush[] = {
		0x6A, 0xFF, 0x58,
		0x68, 0xFF, 0xFF, 0xFF, 0x7F, 0x5B,
		0x68, 0x78, 0x56, 0x34, 0x12, 0x59,
		0x68, 0x11, 0x22, 0x33, 0x44, 0x5A,
		0xC3
	};
	fill_regs(&seed);
	REQUIRE(diff_regs(kPush, sizeof(kPush), seed));
	static const uint8_t kShift2[] = {0xB8, 0x01, 0x00, 0x00, 0x00, 0xC1, 0xE0, 0x04, 0xC3};
	REQUIRE(diff_regs(kShift2, sizeof(kShift2), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 16);
	static const uint8_t kImulA[] = {0x69, 0xC0, 0x05, 0x00, 0x00, 0x00, 0xC3};
	static const uint8_t kImulB[] = {0x69, 0xC0, 0x07, 0x00, 0x00, 0x00, 0xC3};
	seed.rax = 3;
	REQUIRE(diff_regs(kImulA, sizeof(kImulA), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 15);
	REQUIRE(diff_regs(kImulB, sizeof(kImulB), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 21);
	static const uint8_t kRip[] = {
		0x31, 0xC0,
		0x03, 0x05, 0x07, 0x00, 0x00, 0x00,
		0x03, 0x05, 0x05, 0x00, 0x00, 0x00,
		0xC3,
		0x0A, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x00, 0x00
	};
	fill_regs(&seed);
	REQUIRE(diff_regs(kRip, sizeof(kRip), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 13);
	static const uint8_t kLeaRip[] = {0x48, 0x8D, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC3};
	REQUIRE(diff_regs(kLeaRip, sizeof(kLeaRip), seed));
#if defined(_M_X64) || defined(__x86_64__)
	static const uint8_t kGs[] = {0x65, 0x48, 0x8B, 0x04, 0x25, 0x30, 0x00, 0x00, 0x00, 0xC3};
	REQUIRE(diff_regs(kGs, sizeof(kGs), seed));
	static const uint8_t kEnterA[] = {0xC8, 0x10, 0x00, 0x00, 0x48, 0x89, 0xE0, 0xC9, 0xC3};
	static const uint8_t kEnterB[] = {0xC8, 0x20, 0x00, 0x00, 0x48, 0x89, 0xE0, 0xC9, 0xC3};
	REQUIRE(diff_regs(kEnterA, sizeof(kEnterA), seed));
	uintptr_t frame_a = g_last_vm.rax;
	REQUIRE(diff_regs(kEnterB, sizeof(kEnterB), seed));
	REQUIRE(g_last_vm.rax + 16 == frame_a);
#endif
}

static void test_rsi_base_and_call_mem() {
	static const uint8_t kRsi[] = {0x31, 0xC0, 0x03, 0x46, 0x04, 0x03, 0x46, 0x08, 0xC3};
	uint8_t raw[16];
	memset(raw, 0, sizeof(raw));
	*(uint32_t*)(raw + 4) = 5;
	*(uint32_t*)(raw + 8) = 7;
	uint8_t native_buf[16];
	uint8_t vm_buf[16];
	memcpy(native_buf, raw, sizeof(raw));
	memcpy(vm_buf, raw, sizeof(raw));
	void* code = publish(kRsi, sizeof(kRsi));
	REQUIRE(code != nullptr);
	NmRegs seed;
	fill_regs(&seed);
	NmRegs native = seed;
	NmRegs vm = seed;
	native.rsi = (uintptr_t)native_buf;
	vm.rsi = (uintptr_t)vm_buf;
	REQUIRE(exec_native(code, &native) == 0);
	REQUIRE(exec_vm(code, &vm, 0) == 0);
	REQUIRE(!g_abort);
	native.rsi = 0;
	vm.rsi = 0;
	REQUIRE(same_regs(native, vm));
	REQUIRE((uint32_t)vm.rax == 12);
	static const uint8_t kCall[] = {0xFF, 0x11, 0xC3, 0x83, 0xC0, 0x03, 0xC3};
	code = publish(kCall, sizeof(kCall));
	REQUIRE(code != nullptr);
	uint8_t slot_n[16];
	uint8_t slot_v[16];
	memset(slot_n, 0, sizeof(slot_n));
	memset(slot_v, 0, sizeof(slot_v));
#if defined(_M_X64) || defined(__x86_64__)
	*(uint64_t*)slot_n = (uint64_t)((uint8_t*)code + 3);
	*(uint64_t*)slot_v = (uint64_t)((uint8_t*)code + 3);
#else
	*(uint32_t*)slot_n = (uint32_t)((uint8_t*)code + 3);
	*(uint32_t*)slot_v = (uint32_t)((uint8_t*)code + 3);
#endif
	fill_regs(&seed);
	native = seed;
	vm = seed;
	native.rcx = (uintptr_t)slot_n;
	vm.rcx = (uintptr_t)slot_v;
	REQUIRE(exec_native(code, &native) == 0);
	REQUIRE(exec_vm(code, &vm, 0) == 0);
	REQUIRE(!g_abort);
	native.rcx = 0;
	vm.rcx = 0;
	REQUIRE(same_regs(native, vm));
	REQUIRE((uint32_t)vm.rax == (uint32_t)seed.rax + 3);
}

static void test_ret_imm_and_far() {
	static const uint8_t kRet[] = {
		0x68, 0x10, 0x00, 0x00, 0x00,
		0x68, 0x20, 0x00, 0x00, 0x00,
		0xE8, 0x17, 0x00, 0x00, 0x00,
		0x5B,
		0x68, 0x30, 0x00, 0x00, 0x00,
		0x68, 0x40, 0x00, 0x00, 0x00,
		0x68, 0x50, 0x00, 0x00, 0x00,
		0xE8, 0x05, 0x00, 0x00, 0x00,
		0x58,
		0xC3,
		0xC2, 0x08, 0x00,
		0xC2, 0x10, 0x00
	};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_regs(kRet, sizeof(kRet), seed));
	REQUIRE((uint32_t)g_last_vm.rbx == 0x10);
	REQUIRE((uint32_t)g_last_vm.rax == 0x30);
#if defined(_M_X64) || defined(__x86_64__)
	uint8_t far_bytes[32];
	memset(far_bytes, 0x90, sizeof(far_bytes));
	far_bytes[0] = 0xFF;
	far_bytes[1] = 0x1D;
	*(int32_t*)(far_bytes + 2) = 10;
	far_bytes[6] = 0xC3;
	far_bytes[7] = 0xCB;
	void* code = publish(far_bytes, sizeof(far_bytes));
	REQUIRE(code != nullptr);
	uint8_t* p = (uint8_t*)code;
	*(uint64_t*)(p + 16) = (uint64_t)(p + 7);
	*(uint16_t*)(p + 24) = 0x33;
	flush_code(p, sizeof(far_bytes));
	fill_regs(&seed);
	NmRegs vm = seed;
	REQUIRE(exec_vm(code, &vm, 0) == 0);
	REQUIRE(!g_abort);
	REQUIRE(vm.rax == seed.rax);
	REQUIRE(vm.rcx == seed.rcx);
	REQUIRE(vm.rdx == seed.rdx);
	REQUIRE(vm.rbx == seed.rbx);
	REQUIRE(vm.rsi == seed.rsi);
	REQUIRE(vm.rdi == seed.rdi);
	REQUIRE(vm.rbp == seed.rbp);
	REQUIRE((vm.rflags & kFlagMask) == (seed.rflags & kFlagMask));
#endif
}

static void test_more_ops() {
	NmRegs seed;
	fill_regs(&seed);
	static const uint8_t kDiv[] = {
		0xB8, 0x0A, 0x00, 0x00, 0x00, 0x31, 0xD2, 0xB9, 0x03, 0x00, 0x00, 0x00, 0xF7, 0xF1, 0xC3
	};
	REQUIRE(diff_regs(kDiv, sizeof(kDiv), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 3);
	REQUIRE((uint32_t)g_last_vm.rdx == 1);
	static const uint8_t kIdiv[] = {
		0xB8, 0xFE, 0xFF, 0xFF, 0xFF, 0x99, 0xB9, 0x02, 0x00, 0x00, 0x00, 0xF7, 0xF9, 0xC3
	};
	REQUIRE(diff_regs(kIdiv, sizeof(kIdiv), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 0xFFFFFFFF);
	static const uint8_t kMul[] = {
		0xB8, 0x04, 0x00, 0x00, 0x00, 0xB9, 0x05, 0x00, 0x00, 0x00, 0xF7, 0xE1, 0xC3
	};
	REQUIRE(diff_regs(kMul, sizeof(kMul), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 20);
	static const uint8_t kCmov[] = {
		0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x01, 0x0F, 0x44, 0xD8, 0xC3
	};
	REQUIRE(diff_regs(kCmov, sizeof(kCmov), seed));
	REQUIRE((uint32_t)g_last_vm.rbx == 1);
	static const uint8_t kMovsx[] = {0xB0, 0xFF, 0x0F, 0xBE, 0xC0, 0xC3};
	REQUIRE(diff_regs(kMovsx, sizeof(kMovsx), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 0xFFFFFFFF);
	static const uint8_t kBswap[] = {0xB8, 0x78, 0x56, 0x34, 0x12, 0x0F, 0xC8, 0xC3};
	REQUIRE(diff_regs(kBswap, sizeof(kBswap), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 0x78563412);
	static const uint8_t kShld[] = {
		0xB8, 0x01, 0x00, 0x00, 0x00, 0xBB, 0x80, 0x00, 0x00, 0x00,
		0x0F, 0xA4, 0xD8, 0x01, 0x0F, 0xA4, 0xD8, 0x04, 0xC3
	};
	REQUIRE(diff_regs(kShld, sizeof(kShld), seed));
	static const uint8_t kLoop[] = {
		0x31, 0xC0, 0xB9, 0x03, 0x00, 0x00, 0x00, 0xFF, 0xC0, 0xE2, 0xFC, 0xC3
	};
	REQUIRE(diff_regs(kLoop, sizeof(kLoop), seed));
	REQUIRE((uint32_t)g_last_vm.rax == 3);
	REQUIRE((uint32_t)g_last_vm.rcx == 0);
	static const uint8_t kXadd[] = {0xB8, 0x03, 0x00, 0x00, 0x00, 0x0F, 0xC1, 0x01, 0xC3};
	uint8_t mem[16];
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 10;
	REQUIRE(diff_mem(kXadd, sizeof(kXadd), seed, mem, sizeof(mem)));
	REQUIRE((uint32_t)g_last_vm.rax == 10);
	static const uint8_t kCmpxchg[] = {
		0xB8, 0x01, 0x00, 0x00, 0x00, 0xBA, 0x02, 0x00, 0x00, 0x00, 0x0F, 0xB1, 0x11, 0xC3
	};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 1;
	REQUIRE(diff_mem(kCmpxchg, sizeof(kCmpxchg), seed, mem, sizeof(mem)));
	static const uint8_t kImulMem[] = {
		0x69, 0x41, 0x00, 0x03, 0x00, 0x00, 0x00,
		0x69, 0x41, 0x04, 0x04, 0x00, 0x00, 0x00,
		0xC3
	};
	memset(mem, 0, sizeof(mem));
	*(uint32_t*)mem = 5;
	*(uint32_t*)(mem + 4) = 6;
	REQUIRE(diff_mem(kImulMem, sizeof(kImulMem), seed, mem, sizeof(mem)));
	REQUIRE((uint32_t)g_last_vm.rax == 24);
	static const uint8_t kPushPop[] = {0xFF, 0x31, 0x8F, 0x41, 0x08, 0xC3};
	memset(mem, 0, sizeof(mem));
	*(uint64_t*)mem = 0x1234;
	REQUIRE(diff_mem(kPushPop, sizeof(kPushPop), seed, mem, sizeof(mem)));
	static const uint8_t kSet[] = {
		0xB8, 0x01, 0x00, 0x00, 0x00, 0x83, 0xF8, 0x01, 0x0F, 0x94, 0x41, 0x00,
		0x83, 0xF8, 0x02, 0x0F, 0x94, 0x41, 0x01, 0xC3
	};
	memset(mem, 0, sizeof(mem));
	REQUIRE(diff_mem(kSet, sizeof(kSet), seed, mem, sizeof(mem)));
	static const uint8_t kNegNot[] = {0xB8, 0x03, 0x00, 0x00, 0x00, 0xF7, 0xD8, 0xF7, 0xD0, 0xC3};
	REQUIRE(diff_regs(kNegNot, sizeof(kNegNot), seed));
}

static void test_call_rsp_disp() {
	static const uint8_t kCode[] = {
		0x48, 0x83, 0xEC, 0x80,
		0x48, 0x8D, 0x05, 0x0E, 0x00, 0x00, 0x00,
		0x48, 0x89, 0x44, 0x24, 0x70,
		0xFF, 0x54, 0x24, 0x70,
		0x48, 0x83, 0xC4, 0x80,
		0xC3,
		0xB8, 0x09, 0x00, 0x00, 0x00,
		0xC3,
	};
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_regs(kCode, sizeof(kCode), seed));
}

static bool exec_pair(void* code, const NmRegs& seed) {
	NmRegs native = seed;
	NmRegs vm = seed;
	int cn = exec_native(code, &native);
	if (cn) {
		printf("  native exception %08X\n", cn);
		fail(__LINE__, "native exec");
		return false;
	}
	int cv = exec_vm(code, &vm, 0);
	if (cv) {
		printf("  vm exception %08X\n", cv);
		dump_trace();
		fail(__LINE__, "vm exec");
		return false;
	}
	if (g_abort) {
		dump_trace();
		fail(__LINE__, "vm abort");
		return false;
	}
	if (!same_regs(native, vm)) {
		dump_trace();
		fail(__LINE__, "regs differ");
		return false;
	}
	g_last_vm = vm;
	return true;
}

static void test_sib_rsi_r12() {
#if defined(_M_X64) || defined(__x86_64__)
	// user32+0x5D046: lea r12,[rip-0x5D04D] then movzx r14d,[rsi+r12+0xC8AE4]
	const size_t span = 0x100000;
	uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, span, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	REQUIRE(mem != nullptr);
	memset(mem, 0xA5, span);
	static const uint8_t kUser[] = {
		0x8B, 0xF7,
		0x4C, 0x8D, 0x25, 0xB3, 0x2F, 0xFA, 0xFF,
		0x83, 0xE6, 0x0F,
		0x8B, 0xC7,
		0xC1, 0xE8, 0x0E,
		0x83, 0xE0, 0x01,
		0x46, 0x0F, 0xB6, 0xB4, 0x26, 0xE4, 0x8A, 0x0C, 0x00,
		0x44, 0x03, 0xF0,
		0x49, 0x8B, 0xC6,
		0xC3,
	};
	const uint32_t code_off = 0x5D044;
	memcpy(mem + code_off, kUser, sizeof(kUser));
	mem[0xC8AE4 + 6] = 0x3C;
	mem[0xC8AE4 + 0xA] = 0x11;
	flush_code(mem + code_off, sizeof(kUser));
	NmRegs seed;
	fill_regs(&seed);
	seed.rdi = 6;
	seed.rsi = ~(uintptr_t)0;
	if (!exec_pair(mem + code_off, seed)) {
		VirtualFree(mem, 0, MEM_RELEASE);
		return;
	}
	REQUIRE(g_last_vm.rax == 0x3C);
	seed.rdi = 0x800000000000400AULL;
	if (!exec_pair(mem + code_off, seed)) {
		VirtualFree(mem, 0, MEM_RELEASE);
		return;
	}
	REQUIRE(g_last_vm.rax == 0x12);
	VirtualFree(mem, 0, MEM_RELEASE);

	static const uint8_t kZx[] = {
		0x46, 0x0F, 0xB6, 0xB4, 0x26, 0x20, 0x00, 0x00, 0x00,
		0x49, 0x8B, 0xC6,
		0xC3,
	};
	uint8_t slot[64];
	memset(slot, 0xA5, sizeof(slot));
	slot[0x20 + 3] = 0x5A;
	void* code = publish(kZx, sizeof(kZx));
	REQUIRE(code != nullptr);
	fill_regs(&seed);
	seed.rsi = 3;
	seed.r12 = (uintptr_t)slot;
	REQUIRE(exec_pair(code, seed));
	REQUIRE(g_last_vm.rax == 0x5A);

	static const uint8_t kScale[] = {
		0x41, 0x8B, 0x84, 0x84, 0x10, 0x00, 0x00, 0x00,
		0xC3,
	};
	uint8_t wide[64];
	memset(wide, 0, sizeof(wide));
	*(uint32_t*)(wide + 0x10 + 8) = 0x11223344;
	code = publish(kScale, sizeof(kScale));
	REQUIRE(code != nullptr);
	fill_regs(&seed);
	seed.rax = 2;
	seed.r12 = (uintptr_t)wide;
	REQUIRE(exec_pair(code, seed));
	REQUIRE((uint32_t)g_last_vm.rax == 0x11223344);
#else
	(void)0;
#endif
}

static void test_rex_mem() {
	static const uint8_t kCode[] = {
		0x44, 0x8B, 0x01,
		0x8B, 0x41, 0x04,
		0x44, 0x3B, 0xC0,
		0xC3,
	};
	uint8_t mem[8];
	uint8_t native_mem[8];
	uint8_t vm_mem[8];
	*(uint32_t*)mem = 3;
	*(uint32_t*)(mem + 4) = 5;
	NmRegs seed;
	fill_regs(&seed);
	REQUIRE(diff_code(kCode, sizeof(kCode), seed, mem, sizeof(mem), native_mem, vm_mem));
}

static void test_ymm_across_hook() {
	static const uint8_t kCode[] = {
		0xC5, 0xFE, 0x6F, 0x01,
		0xC5, 0xFE, 0x7F, 0x01,
		0xC3,
	};
	uint8_t mem[32];
	for (int i = 0; i < 32; i++)
		mem[i] = (uint8_t)(0x40 + i);
	NmRegs seed;
	fill_regs(&seed);
	g_clobber_xmm = 1;
	bool ok = diff_mem(kCode, sizeof(kCode), seed, mem, sizeof(mem));
	g_clobber_xmm = 0;
	REQUIRE(ok);
}

static void test_sse_across_hook() {
	static const uint8_t kCode[] = {0x0F, 0x10, 0x01, 0x0F, 0x11, 0x41, 0x10, 0xC3};
	uint8_t mem[32];
	for (int i = 0; i < 16; i++)
		mem[i] = (uint8_t)(0x10 + i);
	memset(mem + 16, 0, 16);
	NmRegs seed;
	fill_regs(&seed);
	g_clobber_xmm = 1;
	bool ok = diff_mem(kCode, sizeof(kCode), seed, mem, sizeof(mem));
	g_clobber_xmm = 0;
	REQUIRE(ok);
}

static void test_filter_live() {
	static const uint8_t kCode[] = {0xB8, 0x55, 0x00, 0x00, 0x00, 0xC3};
	void* code = publish(kCode, sizeof(kCode));
	REQUIRE(code != nullptr);
	jitvm_x86_range_filter* filt = jitvm_x86_range_filter_create(0, 0, 1);
	jitvm_x86_range_filter_set_range(filt, (uintptr_t)code, (uintptr_t)code + sizeof(kCode));
	jitvm_x86_range_filter_set_targets(filt, jitvm_nofilter_exit(), jitvm_default_filter());
	jitvm_set_filter(jitvm_x86_range_filter_proc(filt));
	jitvm_set_inst_monitor(JITVM_MONITOR_ALL);
	jitvm_set_flow_monitor(JITVM_MONITOR_ALL);
	NmRegs vm;
	fill_regs(&vm);
	int cv = exec_vm(code, &vm, kNoStop);
	restore_monitor();
	jitvm_x86_range_filter_destroy(filt);
	if (cv || g_abort || (uint32_t)vm.rax != 0x55) {
		printf("  filter live exception=%08X abort=%d rax=%p\n", cv, g_abort, (void*)vm.rax);
		dump_trace();
		fail(__LINE__, "filter should exit when ip leaves the guest range");
	}
}

static void run_test(const char* name, void (*fn)()) {
	if (g_filter && !strstr(name, g_filter))
		return;
	g_test = name;
	int before = g_failed;
	printf("[RUN] %s\n", name);
	fflush(stdout);
	fn();
	if (g_failed == before) {
		g_passed++;
		printf("[OK]  %s\n", name);
	} else {
		printf("[FAIL] %s\n", name);
	}
	fflush(stdout);
}

int main(int argc, char** argv) {
	g_guest_stack_top = ((uintptr_t)g_guest_stack + sizeof(g_guest_stack)) & ~(uintptr_t)15;
	g_guest_stack_top -= sizeof(void*);
	if (argc > 1)
		g_filter = argv[1];
	jitvm_config bad = {};
	bad.machine = JITVM_MACHINE_AARCH64;
	if (jitvm_init(&bad) != nullptr || jitvm_init_status() != JITVM_ERR_UNSUPPORTED_MACHINE) {
		printf("aarch64 init should be unsupported\n");
		return 1;
	}

	jitvm_config cfg = {};
#if defined(JITVM_HOST_X64)
	cfg.machine = JITVM_MACHINE_X64;
#elif defined(JITVM_HOST_X86)
	cfg.machine = JITVM_MACHINE_X86;
#else
	printf("host cannot execute\n");
	return 1;
#endif
	cfg.on_inst = on_inst;
	cfg.on_flow = on_flow;
	cfg.on_exit = on_exit;
	g_enter = jitvm_init(&cfg);
	if (!g_enter) {
		printf("jitvm_init failed %d\n", jitvm_init_status());
		return 1;
	}
	g_enter_proc = (void*)g_enter;
	restore_monitor();

	run_test("context_slots", test_context_slots);
	run_test("monitor_state", test_monitor_state);
	run_test("code_space", test_code_space);
	run_test("icode_len", test_icode_len);
	run_test("vm_proc_before_use", test_vm_proc_before_use);
	run_test("filter_encoding", test_filter_encoding);
	run_test("alu_regs", test_alu_regs);
	run_test("imul_shift", test_known_imul_and_shift);
	run_test("flags_sub_zero", test_flags_sub_zero);
	run_test("push_pop_lea", test_push_pop_and_lea);
	run_test("movzx", test_movzx);
	run_test("memory_ops", test_memory_ops);
	run_test("rsi_register", test_rsi_is_a_real_register);
	run_test("control_flow", test_control_flow);
	run_test("call_ret", test_call_ret);
	run_test("call_jmp_reg", test_call_reg_and_jmp_reg);
	run_test("vm_proc_cached", test_vm_proc_cached);
	run_test("exit_flag", test_exit_flag_stops_vm);
	run_test("int3_exit", test_int3_exits_vm);
	run_test("last_error", test_last_error_preserved);
	run_test("compiled_guests", test_compiled_guests);
	run_test("relocated_operands", test_relocated_operands);
	run_test("rsi_base_call_mem", test_rsi_base_and_call_mem);
	run_test("ret_imm_far", test_ret_imm_and_far);
	run_test("more_ops", test_more_ops);
	run_test("call_rsp_disp", test_call_rsp_disp);
	run_test("sib_rsi_r12", test_sib_rsi_r12);
	run_test("rex_mem", test_rex_mem);
	run_test("ymm_across_hook", test_ymm_across_hook);
	run_test("sse_across_hook", test_sse_across_hook);
	run_test("filter_live", test_filter_live);

	printf("\njitvm tests: %d passed, %d failed\n", g_passed, g_failed);
	return g_failed ? 1 : 0;
}
