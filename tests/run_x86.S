.intel_syntax noprefix

#if defined(__APPLE__) || defined(_WIN32)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(jitvm_run_native)
.globl CSYM(jitvm_run_vm)
.globl CSYM(jitvm_vm_resume)

CSYM(load_regs):
	mov edx, CSYM(g_active_regs)
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

CSYM(save_regs):
	pushfd
	push edx
	mov edx, CSYM(g_active_regs)
	pop dword ptr [edx+8]
	pop dword ptr [edx+32]
	mov [edx+0], eax
	mov [edx+4], ecx
	mov [edx+12], ebx
	mov [edx+20], ebp
	mov [edx+24], esi
	mov [edx+28], edi
	ret

CSYM(jitvm_run_native):
	push ebx
	push ebp
	push esi
	push edi
	mov CSYM(g_saved_rsp), esp
	mov esp, CSYM(g_guest_stack_top)
	call CSYM(load_regs)
	call dword ptr CSYM(g_active_code)
	call CSYM(save_regs)
	mov esp, CSYM(g_saved_rsp)
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret

CSYM(jitvm_run_vm):
	push ebx
	push ebp
	push esi
	push edi
	mov CSYM(g_saved_rsp), esp
	mov esp, CSYM(g_guest_stack_top)
	push CSYM(jitvm_vm_resume)
	call CSYM(load_regs)
	jmp dword ptr CSYM(g_enter_proc)

CSYM(jitvm_vm_resume):
	call CSYM(save_regs)
	mov esp, CSYM(g_saved_rsp)
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret
