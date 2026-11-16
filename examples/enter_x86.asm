.model flat, C
OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

EXTERN demo_regs_ptr:DWORD
EXTERN demo_enter_proc:DWORD
EXTERN demo_stack_top:DWORD
EXTERN demo_saved_rsp:DWORD

PUBLIC demo_run_vm
PUBLIC demo_vm_resume

.code

load_regs PROC
	mov edx, demo_regs_ptr
	mov eax, [edx+0]
	mov ecx, [edx+4]
	push dword ptr [edx+8]
	mov ebx, [edx+12]
	mov ebp, [edx+20]
	mov esi, [edx+24]
	mov edi, [edx+28]
	push dword ptr [edx+32]
	popfd
	pop edx
	ret
load_regs ENDP

save_regs PROC
	pushfd
	push edx
	mov edx, demo_regs_ptr
	pop dword ptr [edx+8]
	pop dword ptr [edx+32]
	mov [edx+0], eax
	mov [edx+4], ecx
	mov [edx+12], ebx
	mov [edx+20], ebp
	mov [edx+24], esi
	mov [edx+28], edi
	ret
save_regs ENDP

demo_run_vm PROC
	push ebx
	push ebp
	push esi
	push edi
	mov demo_saved_rsp, esp
	mov esp, demo_stack_top
	push offset demo_vm_resume
	call load_regs
	jmp dword ptr [demo_enter_proc]
demo_vm_resume::
	call save_regs
	mov esp, demo_saved_rsp
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret
demo_run_vm ENDP

END
