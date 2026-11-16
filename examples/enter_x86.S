.intel_syntax noprefix

#if defined(__APPLE__) || defined(_WIN32)
#define CSYM(name) _##name
#else
#define CSYM(name) name
#endif

.text
.globl CSYM(demo_run_vm)
.globl CSYM(demo_vm_resume)

CSYM(demo_load_regs):
	mov edx, CSYM(demo_regs_ptr)
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

CSYM(demo_save_regs):
	pushfd
	push edx
	mov edx, CSYM(demo_regs_ptr)
	pop dword ptr [edx+8]
	pop dword ptr [edx+32]
	mov [edx+0], eax
	mov [edx+4], ecx
	mov [edx+12], ebx
	mov [edx+20], ebp
	mov [edx+24], esi
	mov [edx+28], edi
	ret

CSYM(demo_run_vm):
	push ebx
	push ebp
	push esi
	push edi
	mov CSYM(demo_saved_rsp), esp
	mov esp, CSYM(demo_stack_top)
	push offset CSYM(demo_vm_resume)
	call CSYM(demo_load_regs)
	jmp dword ptr [CSYM(demo_enter_proc)]

CSYM(demo_vm_resume):
	call CSYM(demo_save_regs)
	mov esp, CSYM(demo_saved_rsp)
	pop edi
	pop esi
	pop ebp
	pop ebx
	ret
