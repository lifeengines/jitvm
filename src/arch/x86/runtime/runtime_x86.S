.intel_syntax noprefix

#if defined(__APPLE__)
#define CSYM(name) _##name
#elif defined(_WIN32)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

#if defined(_WIN32)
#define CTX_STACK 0x264
#define CTX_TMP1 0x268
#define ERR 0x34
#define STATUS 0xBF4
#else
#define CTX_STACK 20
#define CTX_TMP1 24
#endif

.text
.globl CSYM(jitvm_link_entry)
.globl CSYM(jitvm_ensure_entry)

CSYM(jitvm_link_entry):
	mov fs:[CTX_TMP1], esp
	mov esp, fs:[CTX_STACK]
	push fs:[CTX_TMP1]
	pushfd
	pushad
	cld
#if defined(_WIN32)
	push dword ptr fs:[ERR]
	push dword ptr fs:[STATUS]
#endif
	sub esp, 0x220
	lea eax, [esp+0x10]
	and eax, -16
	fxsave [eax]
#if defined(_WIN32)
	push 0
	call CSYM(jitvm_op_link)
	add esp, 4
#else
	push CTX_TMP1
	call CSYM(jitvm_op_link)
	add esp, 4
#endif
	mov fs:[CTX_TMP1], eax
	lea ecx, [esp+0x10]
	and ecx, -16
	fxrstor [ecx]
	add esp, 0x220
#if defined(_WIN32)
	pop dword ptr fs:[STATUS]
	pop dword ptr fs:[ERR]
#endif
	popad
	popfd
	pop esp
	jmp fs:[CTX_TMP1]

CSYM(jitvm_ensure_entry):
	pushfd
	pushad
	cld
#if defined(_WIN32)
	push dword ptr fs:[ERR]
	push dword ptr fs:[STATUS]
#endif
	sub esp, 0x220
	lea eax, [esp+0x10]
	and eax, -16
	fxsave [eax]
	call CSYM(jitvm_ensure_stack)
	lea ecx, [esp+0x10]
	and ecx, -16
	fxrstor [ecx]
	add esp, 0x220
#if defined(_WIN32)
	pop dword ptr fs:[STATUS]
	pop dword ptr fs:[ERR]
#endif
	popad
	popfd
	ret
