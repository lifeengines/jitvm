.intel_syntax noprefix

#if defined(__APPLE__) || (defined(_WIN32) && !defined(__x86_64__))
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(jitvm_run_native)
.globl CSYM(jitvm_run_vm)
.globl CSYM(jitvm_vm_resume)

CSYM(load_regs):
	mov r11, CSYM(g_active_regs)[rip]
	mov rax, [r11+0]
	mov rcx, [r11+8]
	mov rdx, [r11+16]
	mov rbx, [r11+24]
	mov rbp, [r11+40]
	mov rsi, [r11+48]
	mov rdi, [r11+56]
	mov r8,  [r11+64]
	mov r9,  [r11+72]
	mov r10, [r11+80]
	mov r12, [r11+96]
	mov r13, [r11+104]
	mov r14, [r11+112]
	mov r15, [r11+120]
	push qword ptr [r11+128]
	popfq
	mov r11, [r11+88]
	ret

CSYM(save_regs):
	pushfq
	push r10
	push r11
	mov r11, CSYM(g_active_regs)[rip]
	pop qword ptr [r11+88]
	pop qword ptr [r11+80]
	pop qword ptr [r11+128]
	mov [r11+0], rax
	mov [r11+8], rcx
	mov [r11+16], rdx
	mov [r11+24], rbx
	mov [r11+40], rbp
	mov [r11+48], rsi
	mov [r11+56], rdi
	mov [r11+64], r8
	mov [r11+72], r9
	mov [r11+96], r12
	mov [r11+104], r13
	mov [r11+112], r14
	mov [r11+120], r15
	ret

CSYM(jitvm_run_native):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	mov CSYM(g_saved_rsp)[rip], rsp
	mov rsp, CSYM(g_guest_stack_top)[rip]
	sub rsp, 0x28
	call CSYM(load_regs)
	call qword ptr CSYM(g_active_code)[rip]
	call CSYM(save_regs)
	mov rsp, CSYM(g_saved_rsp)[rip]
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret

CSYM(jitvm_run_vm):
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	mov CSYM(g_saved_rsp)[rip], rsp
	mov rsp, CSYM(g_guest_stack_top)[rip]
	sub rsp, 0x28
	lea rax, CSYM(jitvm_vm_resume)[rip]
	mov [rsp], rax
	call CSYM(load_regs)
	push qword ptr [rsp]
	jmp qword ptr CSYM(g_enter_proc)[rip]

CSYM(jitvm_vm_resume):
	call CSYM(save_regs)
	mov rsp, CSYM(g_saved_rsp)[rip]
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret
