OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

EXTERN g_active_regs:QWORD
EXTERN g_active_code:QWORD
EXTERN g_enter_proc:QWORD
EXTERN g_guest_stack_top:QWORD
EXTERN g_saved_rsp:QWORD

PUBLIC jitvm_run_native
PUBLIC jitvm_run_vm
PUBLIC jitvm_vm_resume

.code

load_regs PROC
	mov r11, g_active_regs
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
load_regs ENDP

save_regs PROC
	pushfq
	push r10
	push r11
	mov r11, g_active_regs
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
save_regs ENDP

jitvm_run_native PROC
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	mov g_saved_rsp, rsp
	mov rsp, g_guest_stack_top
	sub rsp, 28h
	call load_regs
	call qword ptr [g_active_code]
	call save_regs
	mov rsp, g_saved_rsp
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret
jitvm_run_native ENDP

jitvm_run_vm PROC
	push rbx
	push rbp
	push rsi
	push rdi
	push r12
	push r13
	push r14
	push r15
	mov g_saved_rsp, rsp
	mov rsp, g_guest_stack_top
	sub rsp, 28h
	lea rax, jitvm_vm_resume
	mov [rsp], rax
	call load_regs
	push qword ptr [rsp]
	jmp qword ptr [g_enter_proc]
jitvm_vm_resume::
	call save_regs
	mov rsp, g_saved_rsp
	pop r15
	pop r14
	pop r13
	pop r12
	pop rdi
	pop rsi
	pop rbp
	pop rbx
	ret
jitvm_run_vm ENDP

END
