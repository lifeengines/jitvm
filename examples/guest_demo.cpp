#include <jitvm/jitvm.h>
#include <jitvm/jitvm_x86.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define NOINLINE __declspec(noinline)
#if defined(_M_IX86)
#define GUESTCALL __fastcall
#else
#define GUESTCALL
#endif
#include <intrin.h>
#pragma optimize("", off)
#else
#define NOINLINE __attribute__((noinline))
#define GUESTCALL
#endif

enum { kItems = 8 };

// 关优化编译，让循环、调用、乘除、移位和内存访问都留下真实指令。
// main 不直接依赖它的内部实现，先原生跑一遍，再交给模拟器跑同一份机器码。
extern "C" NOINLINE int GUESTCALL guest_leaf(int x) {
	volatile unsigned u = (unsigned)x;
	volatile unsigned hi = u >> 5;
	volatile unsigned lo = u << 3;
	u = hi | lo;
	u ^= u >> 4;
	u *= 0x45u;
	if ((int)u < 0)
		u = ~u;
	return (int)(u & 0x7fffffffu);
}

extern "C" NOINLINE int GUESTCALL guest_work(int seed, int* data) {
	volatile int acc = seed;
	volatile int i = 0;
	volatile int n = kItems;
	volatile unsigned char bytes[4];
	volatile short words[2];

	while (i < n) {
		volatile int v = data[i];
		v += acc;
		v ^= (i + 1) * 13;
		if ((v & 3) == 0)
			v = guest_leaf(v);
		else if (v < 0)
			v = -v + 1;
		else
			v = (v % 7) + (v / 3);

		bytes[0] = (unsigned char)v;
		bytes[1] = (unsigned char)(v >> 8);
		bytes[2] = (unsigned char)(v >> 16);
		bytes[3] = (unsigned char)(v >> 24);
		words[0] = (short)(bytes[0] | (bytes[1] << 8));
		words[1] = (short)(bytes[2] | (bytes[3] << 8));
		v = (int)(unsigned short)words[0] + ((int)words[1] << 16);

		data[i] = v;
		acc += v;
		if (acc & 1) {
#if defined(_MSC_VER)
			acc = (int)_rotl((unsigned)acc, 1);
#else
			unsigned uu = (unsigned)acc;
			acc = (int)((uu << 1) | (uu >> 31));
#endif
		}
		i = i + 1;
	}

	{
		unsigned long bit = 0;
		unsigned probe = (unsigned)acc | 1u;
#if defined(_MSC_VER)
		_BitScanForward(&bit, probe);
		acc ^= (int)bit;
		acc = (int)_byteswap_ulong((unsigned)acc);
#else
		bit = (unsigned long)__builtin_ctz(probe);
		acc ^= (int)bit;
		acc = (int)__builtin_bswap32((unsigned)acc);
#endif
	}

	switch (acc & 3) {
	case 0:
		acc += 11;
		break;
	case 1:
		acc -= 7;
		break;
	case 2:
		acc ^= 0x5a;
		break;
	default:
		acc = ~acc;
		break;
	}
	return acc;
}

extern "C" NOINLINE int GUESTCALL guest_box(void) {
	return MessageBoxW(nullptr, L"这是在模拟执行里调用的 MessageBox", L"jitvm", MB_OK);
}

#if defined(_MSC_VER)
#pragma optimize("", on)
#endif

struct DemoRegs {
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
DemoRegs* demo_regs_ptr = nullptr;
void* demo_enter_proc = nullptr;
uintptr_t demo_stack_top = 0;
uintptr_t demo_saved_rsp = 0;
void demo_run_vm();
void demo_vm_resume();
}

static uint8_t* g_stack_mem;
static const size_t g_stack_size = 8u << 20;
static DemoRegs g_regs;
static int g_steps;
static uintptr_t g_last_rip;
static uintptr_t g_last_base;
static char g_last_mod[64];

struct Trail {
	uintptr_t rip;
	uint8_t bytes[8];
};
static Trail g_trail[24];
static int g_trail_n;

static void note_trail(uintptr_t rip) {
	Trail* t = &g_trail[g_trail_n++ % 24];
	t->rip = rip;
	memcpy(t->bytes, (void*)rip, sizeof(t->bytes));
}

struct SeenMod {
	uintptr_t base;
	uintptr_t size;
	int step;
	char name[64];
};

static SeenMod g_mods[48];
static int g_mod_n;

static bool module_of(uintptr_t rip, char* name, size_t name_n, uintptr_t* base, uintptr_t* size) {
	uint8_t* teb = (uint8_t*)NtCurrentTeb();
#if defined(_M_X64) || defined(__x86_64__)
	uint8_t* peb = *(uint8_t**)(teb + 0x60);
	uint8_t* ldr = *(uint8_t**)(peb + 0x18);
	auto* head = (LIST_ENTRY*)(ldr + 0x20);
	const int links = 0x10;
	const int dll_base = 0x30;
	const int image_size = 0x40;
	const int base_name = 0x58;
#else
	uint8_t* peb = *(uint8_t**)(teb + 0x30);
	uint8_t* ldr = *(uint8_t**)(peb + 0x0C);
	auto* head = (LIST_ENTRY*)(ldr + 0x14);
	const int links = 0x08;
	const int dll_base = 0x18;
	const int image_size = 0x20;
	const int base_name = 0x2C;
#endif
	for (LIST_ENTRY* link = head->Flink; link && link != head; link = link->Flink) {
		uint8_t* entry = (uint8_t*)link - links;
		uintptr_t dll = *(uintptr_t*)(entry + dll_base);
		uintptr_t bytes = *(uint32_t*)(entry + image_size);
		if (rip < dll || rip >= dll + bytes)
			continue;
		uint16_t nchars = *(uint16_t*)(entry + base_name) / 2;
		wchar_t* wide = *(wchar_t**)(entry + base_name + sizeof(void*));
		WideCharToMultiByte(CP_UTF8, 0, wide, nchars, name, (int)name_n - 1, nullptr, nullptr);
		name[name_n - 1] = 0;
		*base = dll;
		*size = bytes;
		return true;
	}
	return false;
}

static void remember(uintptr_t rip) {
	g_last_rip = rip;
	for (int i = 0; i < g_mod_n; i++) {
		if (rip >= g_mods[i].base && rip < g_mods[i].base + g_mods[i].size) {
			g_last_base = g_mods[i].base;
			memcpy(g_last_mod, g_mods[i].name, sizeof(g_last_mod));
			return;
		}
	}
	char name[64] = {};
	uintptr_t base = 0;
	uintptr_t size = 0;
	if (!module_of(rip, name, sizeof(name), &base, &size)) {
		snprintf(name, sizeof(name), "%p", (void*)rip);
		base = rip;
		size = 1;
	}
	g_last_base = base;
	memcpy(g_last_mod, name, sizeof(g_last_mod));
	if (g_mod_n < (int)(sizeof(g_mods) / sizeof(g_mods[0]))) {
		g_mods[g_mod_n].base = base;
		g_mods[g_mod_n].size = size;
		g_mods[g_mod_n].step = g_steps;
		memcpy(g_mods[g_mod_n].name, name, sizeof(g_mods[g_mod_n].name));
		g_mod_n++;
	}
	fprintf(stderr, "第 %d 步进入 %s +0x%llX\n", g_steps, name, (unsigned long long)(rip - base));
}

static void on_guest(jitvm_guest_state* state) {
	uintptr_t rip = (uintptr_t)state->regs.x86->rip;
	if (rip == (uintptr_t)demo_vm_resume) {
		jitvm_set_exit_flag();
		return;
	}
	g_steps++;
	remember(rip);
	note_trail(rip);
	if ((g_steps % 500000) == 0) {
		fprintf(stderr, "已模拟 %d 步，当前 %s +0x%llX\n", g_steps, g_last_mod,
			(unsigned long long)(rip - g_last_base));
	}
}

static void on_flow(jitvm_guest_state* state) {
	uintptr_t rip = (uintptr_t)state->regs.x86->rip;
	if (rip == (uintptr_t)demo_vm_resume) {
		jitvm_set_exit_flag();
		return;
	}
	g_steps++;
	remember(rip);
	note_trail(rip);
}

static void dump_bytes(const char* label, uintptr_t addr) {
	MEMORY_BASIC_INFORMATION mi;
	if (!VirtualQuery((void*)addr, &mi, sizeof(mi)) || mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD) ||
		(mi.Protect & PAGE_NOACCESS)) {
		fprintf(stderr, "  %s %p 不可读\n", label, (void*)addr);
		return;
	}
	uint8_t b[16];
	memcpy(b, (void*)addr, sizeof(b));
	fprintf(stderr, "  %s %p :", label, (void*)addr);
	for (int i = 0; i < 16; i++)
		fprintf(stderr, " %02X", b[i]);
	fprintf(stderr, "\n");
}

static LONG CALLBACK on_fault(EXCEPTION_POINTERS* info) {
	unsigned code = info->ExceptionRecord->ExceptionCode;
	if (code != 0xC0000005 && code != 0xC000001D && code != 0xC0000374 && code != 0xC0000409)
		return EXCEPTION_CONTINUE_SEARCH;
	uintptr_t at = (uintptr_t)info->ExceptionRecord->ExceptionAddress;
	char where[64] = {};
	uintptr_t base = 0;
	uintptr_t size = 0;
	const char* mod = module_of(at, where, sizeof(where), &base, &size) ? where : "?";
	fprintf(stderr, "异常 %08X，位置 %s +0x%llX，客人最后指令 %s +0x%llX（%p），已模拟 %d 步\n", code, mod,
		(unsigned long long)(base ? at - base : 0), g_last_mod, (unsigned long long)(g_last_rip - g_last_base),
		(void*)g_last_rip, g_steps);
	dump_bytes("客人指令", g_last_rip);
	dump_bytes("异常位置", at);
	if (info->ExceptionRecord->NumberParameters >= 2) {
		fprintf(stderr, "  访问 %s %p\n", info->ExceptionRecord->ExceptionInformation[0] ? "写" : "读",
			(void*)info->ExceptionRecord->ExceptionInformation[1]);
	}
	int shown = g_trail_n < 24 ? g_trail_n : 24;
	int start = g_trail_n - shown;
	HANDLE log = CreateFileA("jitvm_fault.txt", GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
		FILE_ATTRIBUTE_NORMAL, nullptr);
	if (log != INVALID_HANDLE_VALUE) {
		char line[160];
		int n = snprintf(line, sizeof(line), "code %08X rip %p steps %d\r\n", code, (void*)g_last_rip, g_steps);
		DWORD wrote = 0;
		WriteFile(log, line, n, &wrote, nullptr);
		for (int i = 0; i < shown; i++) {
			Trail* t = &g_trail[(start + i) % 24];
			n = snprintf(line, sizeof(line), "%p %02X %02X %02X %02X %02X %02X %02X %02X\r\n", (void*)t->rip,
				t->bytes[0], t->bytes[1], t->bytes[2], t->bytes[3], t->bytes[4], t->bytes[5], t->bytes[6],
				t->bytes[7]);
			WriteFile(log, line, n, &wrote, nullptr);
		}
		CloseHandle(log);
	}
	CONTEXT* ctx = info->ContextRecord;
#if defined(_M_X64) || defined(__x86_64__)
	fprintf(stderr,
		"  rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX\n"
		"  r8=%llX r9=%llX r10=%llX r11=%llX rsp=%llX rip=%llX\n",
		(unsigned long long)ctx->Rax, (unsigned long long)ctx->Rbx, (unsigned long long)ctx->Rcx,
		(unsigned long long)ctx->Rdx, (unsigned long long)ctx->Rsi, (unsigned long long)ctx->Rdi,
		(unsigned long long)ctx->R8, (unsigned long long)ctx->R9, (unsigned long long)ctx->R10,
		(unsigned long long)ctx->R11, (unsigned long long)ctx->Rsp, (unsigned long long)ctx->Rip);
#else
	fprintf(stderr,
		"  eax=%08X ebx=%08X ecx=%08X edx=%08X esi=%08X edi=%08X\n"
		"  ebp=%08X esp=%08X eip=%08X\n",
		(unsigned)ctx->Eax, (unsigned)ctx->Ebx, (unsigned)ctx->Ecx, (unsigned)ctx->Edx,
		(unsigned)ctx->Esi, (unsigned)ctx->Edi, (unsigned)ctx->Ebp, (unsigned)ctx->Esp,
		(unsigned)ctx->Eip);
#endif
	fprintf(stderr, "  最近指令\n");
	for (int i = 0; i < shown; i++) {
		Trail* t = &g_trail[(start + i) % 24];
		char name[64] = {};
		uintptr_t base = 0;
		uintptr_t size = 0;
		const char* mod = module_of(t->rip, name, sizeof(name), &base, &size) ? name : "?";
		fprintf(stderr, "  %s +%llX", mod, (unsigned long long)(base ? t->rip - base : 0));
		for (int b = 0; b < 8; b++)
			fprintf(stderr, " %02X", t->bytes[b]);
		fprintf(stderr, "\n");
	}
	fflush(stderr);
	ExitProcess(2);
}

int main() {
	SetConsoleOutputCP(65001);
	g_stack_mem = (uint8_t*)VirtualAlloc(nullptr, g_stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
	if (!g_stack_mem) {
		printf("客人栈分配失败\n");
		return 1;
	}
	demo_stack_top = ((uintptr_t)g_stack_mem + g_stack_size) & ~(uintptr_t)15;
	demo_stack_top -= sizeof(void*);

	jitvm_config cfg = {};
#if defined(JITVM_HOST_X64)
	cfg.machine = JITVM_MACHINE_X64;
#elif defined(JITVM_HOST_X86)
	cfg.machine = JITVM_MACHINE_X86;
#else
	printf("当前主机不能跑 x86 模拟\n");
	return 1;
#endif
	cfg.on_inst = on_guest;
	cfg.on_flow = on_flow;
	jitvm_enter_fn enter = jitvm_init(&cfg);
	if (!enter) {
		printf("jitvm_init 失败 %d\n", jitvm_init_status());
		return 1;
	}
	jitvm_clear_exit_flag();
	AddVectoredExceptionHandler(1, on_fault);

	memset(&g_regs, 0, sizeof(g_regs));
	g_regs.rflags = 0x202;
	demo_regs_ptr = &g_regs;
	demo_enter_proc = (void*)enter;
	jitvm_set_guest_ip((uintptr_t)guest_box);

	NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
	void* old_base = tib->StackBase;
	void* old_limit = tib->StackLimit;
	// 客人栈和指令回调栈都不是这条线程原来的栈。栈检查会拿 RSP 和这两个字段比较，
	// 范围过窄时，还没执行到第一条客人指令就会访问违规。
#if defined(_M_X64) || defined(__x86_64__)
	tib->StackBase = (void*)(((uintptr_t)1 << 47) - 1);
#else
	tib->StackBase = (void*)0x7FFFFFFF;
#endif
	tib->StackLimit = (void*)0x10000;

	uint8_t* teb = (uint8_t*)NtCurrentTeb();
	printf("初始化完成，TEB+78 = %p，跳进模拟器执行 guest_box\n", *(void**)(teb + 0x78));
	fflush(stdout);
	demo_run_vm();

	tib->StackBase = old_base;
	tib->StackLimit = old_limit;

	printf("模拟返回 rax = %d，GetLastError = %lu，共 %d 步，经过 %d 个模块\n", (int)g_regs.rax,
		GetLastError(), g_steps, g_mod_n);
	for (int i = 0; i < g_mod_n; i++)
		printf("  [%02d] 第 %d 步 %s\n", i, g_mods[i].step, g_mods[i].name);
	return g_regs.rax == IDOK ? 0 : 1;
}
