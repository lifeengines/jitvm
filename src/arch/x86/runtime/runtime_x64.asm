OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

EXTERN jitvm_op_link:PROC
EXTERN jitvm_ensure_stack:PROC

PUBLIC jitvm_link_entry
PUBLIC jitvm_ensure_entry
PUBLIC jitvm_host_fcw_save
PUBLIC jitvm_host_fcw_load

.code

; rax = callee, rdx = rcx argument. Returns the callee's rax.
; Saves x87/SSE/AVX state around the call. rsp is unchanged on return.
jitvm_invoke_saved PROC
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
	mov eax, 0Dh
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
	; XSAVE does not clear header bytes 520-575. XRSTOR raises #GP if they are nonzero.
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
	xsave64 qword ptr [rsp]
	mov rcx, qword ptr [rbx+32]
	mov rdi, rcx
	sub rsp, 20h
	call qword ptr [rbx+24]
	mov r10, rax
	add rsp, 20h
	mov eax, dword ptr [rbx]
	mov edx, dword ptr [rbx+4]
	xrstor64 qword ptr [rsp]
	mov rsp, qword ptr [rbx+16]
	mov rax, r10
	ret
invoke_fx:
	sub rsp, 220h
	lea rax, [rsp+10h]
	and rax, -16
	fxsave [rax]
	mov rcx, r11
	mov rdi, r11
	sub rsp, 28h
	call r10
	mov r10, rax
	add rsp, 28h
	lea rcx, [rsp+10h]
	and rcx, -16
	fxrstor [rcx]
	add rsp, 220h
	mov rax, r10
	ret
jitvm_invoke_saved ENDP

jitvm_link_entry PROC
	mov qword ptr gs:[0FB8h], rsp
	mov rsp, qword ptr gs:[0FB0h]
	push qword ptr gs:[0FB8h]
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
	push rsi
	push rsi
	mov esi, dword ptr gs:[068h]
	mov dword ptr [rsp+0Ch], esi
	mov esi, dword ptr gs:[01250h]
	mov dword ptr [rsp+08h], esi
	pop rsi
	lea rax, jitvm_op_link
	mov rdx, 0F88h
	call jitvm_invoke_saved
	mov qword ptr gs:[0FB8h], rax
	xchg rsi, [rsp]
	mov dword ptr gs:[01250h], esi
	shr rsi, 32
	mov dword ptr gs:[068h], esi
	pop rsi
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
	jmp qword ptr gs:[0FB8h]
jitvm_link_entry ENDP

jitvm_ensure_entry PROC
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
	push rsi
	push rsi
	mov esi, dword ptr gs:[068h]
	mov dword ptr [rsp+0Ch], esi
	mov esi, dword ptr gs:[01250h]
	mov dword ptr [rsp+08h], esi
	pop rsi
	lea rax, jitvm_ensure_stack
	xor edx, edx
	call jitvm_invoke_saved
	xchg rsi, [rsp]
	mov dword ptr gs:[01250h], esi
	shr rsi, 32
	mov dword ptr gs:[068h], esi
	pop rsi
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
jitvm_ensure_entry ENDP

; void jitvm_host_fcw_save(unsigned short* out)
jitvm_host_fcw_save PROC
	fnstcw word ptr [rcx]
	ret
jitvm_host_fcw_save ENDP

; void jitvm_host_fcw_load(const unsigned short* in)
jitvm_host_fcw_load PROC
	fldcw word ptr [rcx]
	ret
jitvm_host_fcw_load ENDP

END
