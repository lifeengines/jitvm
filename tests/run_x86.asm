.model flat, C
OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

EXTERN g_active_regs:DWORD
EXTERN g_active_code:DWORD
EXTERN g_enter_proc:DWORD
EXTERN g_guest_stack_top:DWORD
EXTERN g_saved_rsp:DWORD

PUBLIC jitvm_run_native
PUBLIC jitvm_run_vm
PUBLIC jitvm_vm_resume

.code

load_regs PROC
	mov edx, g_active_regs
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
	mov edx, g_active_regs
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

jitvm_run_native PROC
	push ebx
	push ebp
	push esi
	push edi
	mov g_saved_rsp, esp
	mov esp, g_guest_stack_top
	call load_regs
	call dword ptr [g_active_code]
	call save_regs
	mov esp, g_saved_rsp
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret
jitvm_run_native ENDP

jitvm_run_vm PROC
	push ebx
	push ebp
	push esi
	push edi
	mov g_saved_rsp, esp
	mov esp, g_guest_stack_top
	push offset jitvm_vm_resume
	call load_regs
	jmp dword ptr [g_enter_proc]
jitvm_vm_resume::
	call save_regs
	mov esp, g_saved_rsp
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret
jitvm_run_vm ENDP

END
