.model flat, C
OPTION PROLOGUE:NONE
OPTION EPILOGUE:NONE

.code

ASSUME fs:NOTHING

EXTERN jitvm_op_link:PROC
EXTERN jitvm_ensure_stack:PROC

PUBLIC jitvm_link_entry
PUBLIC jitvm_ensure_entry

jitvm_link_entry PROC
	mov dword ptr fs:[0268h], esp
	mov esp, dword ptr fs:[0264h]
	push dword ptr fs:[0268h]
	pushfd
	pushad
	cld
	push dword ptr fs:[034h]
	push dword ptr fs:[0BF4h]
	sub esp, 220h
	lea eax, [esp+10h]
	and eax, -16
	fxsave [eax]
	push 0
	call jitvm_op_link
	add esp, 4
	mov dword ptr fs:[0268h], eax
	lea ecx, [esp+10h]
	and ecx, -16
	fxrstor [ecx]
	add esp, 220h
	pop dword ptr fs:[0BF4h]
	pop dword ptr fs:[034h]
	popad
	popfd
	pop esp
	jmp dword ptr fs:[0268h]
jitvm_link_entry ENDP

jitvm_ensure_entry PROC
	pushfd
	pushad
	cld
	push dword ptr fs:[034h]
	push dword ptr fs:[0BF4h]
	sub esp, 220h
	lea eax, [esp+10h]
	and eax, -16
	fxsave [eax]
	call jitvm_ensure_stack
	lea ecx, [esp+10h]
	and ecx, -16
	fxrstor [ecx]
	add esp, 220h
	pop dword ptr fs:[0BF4h]
	pop dword ptr fs:[034h]
	popad
	popfd
	ret
jitvm_ensure_entry ENDP

END
