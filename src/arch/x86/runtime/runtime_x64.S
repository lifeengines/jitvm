.intel_syntax noprefix

#if defined(__APPLE__)
#define CSYM(name) _##name
#define SEG fs
#define CTX 0
#define STACK 40
#define TMP1 48
#elif defined(_WIN32)
#define CSYM(name) name
#define SEG gs
#define CTX 0xF88
#define STACK 0xFB0
#define TMP1 0xFB8
#define ERR 0x68
#define STATUS 0x1250
#else
#define CSYM(name) name
#define SEG gs
#define CTX 0
#define STACK 40
#define TMP1 48
#endif

.text
.globl CSYM(jitvm_link_entry)
.globl CSYM(jitvm_ensure_entry)

# rax = callee, rdx = rcx argument. Returns the callee's rax.
# Preserves x87/SSE/AVX across the call and restores rsp.
jitvm_invoke_saved:
	mov r10, rax
	mov r11, rdx
	mov eax, 1
	xor ecx, ecx
	cpuid
	bt ecx, 27
	jnc invoke_fx
	xor ecx, ecx
	xgetbv
	mov r8d, eax
	mov r9d, edx
	or eax, edx
	jz invoke_fx
	mov eax, 0xD
	xor ecx, ecx
	cpuid
	cmp ebx, 576
	jb invoke_fx
	cmp ebx, 8192
	ja invoke_fx
	mov r12d, ebx
	mov rax, rsp
	sub rsp, 40
	mov dword ptr [rsp], r8d
	mov dword ptr [rsp+4], r9d
	mov dword ptr [rsp+8], r12d
	mov qword ptr [rsp+16], rax
	mov qword ptr [rsp+24], r10
	mov qword ptr [rsp+32], r11
	mov rbx, rsp
	mov eax, r12d
	add rax, 128
	sub rsp, rax
	and rsp, -64
	# XSAVE does not clear header bytes 520-575. XRSTOR raises #GP if they are nonzero.
	xor eax, eax
	mov qword ptr [rsp+512], rax
	mov qword ptr [rsp+520], rax
	mov qword ptr [rsp+528], rax
	mov qword ptr [rsp+536], rax
	mov qword ptr [rsp+544], rax
	mov qword ptr [rsp+552], rax
	mov qword ptr [rsp+560], rax
	mov qword ptr [rsp+568], rax
	mov eax, dword ptr [rbx]
	mov edx, dword ptr [rbx+4]
	xsave64 [rsp]
	mov rcx, qword ptr [rbx+32]
	mov rdi, rcx
	sub rsp, 0x20
	call qword ptr [rbx+24]
	mov r10, rax
	add rsp, 0x20
	mov eax, dword ptr [rbx]
	mov edx, dword ptr [rbx+4]
	xrstor64 [rsp]
	mov rsp, qword ptr [rbx+16]
	mov rax, r10
	ret
invoke_fx:
	sub rsp, 0x220
	lea rax, [rsp+0x10]
	and rax, -16
	fxsave [rax]
	mov rcx, r11
	mov rdi, r11
	sub rsp, 0x28
	call r10
	mov r10, rax
	add rsp, 0x28
	lea rcx, [rsp+0x10]
	and rcx, -16
	fxrstor [rcx]
	add rsp, 0x220
	mov rax, r10
	ret

CSYM(jitvm_link_entry):
	mov SEG:[TMP1], rsp
	mov rsp, SEG:[STACK]
	push SEG:[TMP1]
	pushfq
	push rax
	push rcx
	push rdx
	push rbx
	push rbp
	push rsi
	push rdi
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	cld
#if defined(_WIN32)
	push rsi
	push rsi
	mov esi, dword ptr SEG:[ERR]
	mov dword ptr [rsp+0xC], esi
	mov esi, dword ptr SEG:[STATUS]
	mov dword ptr [rsp+0x8], esi
	pop rsi
#endif
	lea rax, [rip + CSYM(jitvm_op_link)]
	mov rdx, CTX
	call jitvm_invoke_saved
	mov SEG:[TMP1], rax
#if defined(_WIN32)
	xchg rsi, [rsp]
	mov dword ptr SEG:[STATUS], esi
	shr rsi, 32
	mov dword ptr SEG:[ERR], esi
	pop rsi
#endif
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	pop rdx
	pop rcx
	pop rax
	popfq
	pop rsp
	jmp SEG:[TMP1]

CSYM(jitvm_ensure_entry):
	pushfq
	push rax
	push rcx
	push rdx
	push rbx
	push rbp
	push rsi
	push rdi
	push r8
	push r9
	push r10
	push r11
	push r12
	push r13
	push r14
	push r15
	cld
#if defined(_WIN32)
	push rsi
	push rsi
	mov esi, dword ptr SEG:[ERR]
	mov dword ptr [rsp+0xC], esi
	mov esi, dword ptr SEG:[STATUS]
	mov dword ptr [rsp+0x8], esi
	pop rsi
#endif
	lea rax, [rip + CSYM(jitvm_ensure_stack)]
	mov rdx, CTX
	call jitvm_invoke_saved
#if defined(_WIN32)
	xchg rsi, [rsp]
	mov dword ptr SEG:[STATUS], esi
	shr rsi, 32
	mov dword ptr SEG:[ERR], esi
	pop rsi
#endif
	pop r15
	pop r14
	pop r13
	pop r12
	pop r11
	pop r10
	pop r9
	pop r8
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	pop rdx
	pop rcx
	pop rax
	popfq
	ret
