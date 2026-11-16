#ifndef JITVM_X86_BUF_HPP
#define JITVM_X86_BUF_HPP

#include "arch/x86/codec.hpp"
#include "arch/x86/layout.hpp"

#include <string.h>

namespace jitvm {
namespace x86 {

struct Buf {
	uint8_t* start;
	uint8_t* p;
	uint8_t* end;

	void b(uint8_t v) { *p++ = v; }
	void w(uint16_t v) { memcpy(p, &v, 2); p += 2; }
	void d(uint32_t v) { memcpy(p, &v, 4); p += 4; }
	void q(uint64_t v) { memcpy(p, &v, 8); p += 8; }
	void raw(const void* s, size_t n) { memcpy(p, s, n); p += n; }
	size_t used() const { return (size_t)(p - start); }
	void seg() { b(seg_prefix()); }
};

inline ZydisEncoderOperand reg_op(ZydisRegister reg) {
	ZydisEncoderOperand op;
	memset(&op, 0, sizeof(op));
	op.type = ZYDIS_OPERAND_TYPE_REGISTER;
	op.reg.value = reg;
	return op;
}

inline ZydisEncoderOperand imm_op(int64_t value) {
	ZydisEncoderOperand op;
	memset(&op, 0, sizeof(op));
	op.type = ZYDIS_OPERAND_TYPE_IMMEDIATE;
	op.imm.s = value;
	return op;
}

inline ZydisEncoderOperand mem_op(ZydisRegister base, ZydisRegister index, uint8_t scale, int64_t disp, uint16_t size_bytes) {
	ZydisEncoderOperand op;
	memset(&op, 0, sizeof(op));
	op.type = ZYDIS_OPERAND_TYPE_MEMORY;
	op.mem.base = base;
	op.mem.index = index;
	op.mem.scale = scale;
	op.mem.displacement = disp;
	op.mem.size = size_bytes;
	return op;
}

inline int reg_id(ZydisRegister reg) {
	ZydisRegister large = ZydisRegisterGetLargestEnclosing(machine_mode(), reg);
	return (int)ZydisRegisterGetId(large);
}

inline void emit_abs_mem(Buf& buf, ZydisRegister reg, uint32_t disp, bool store) {
	int id = reg_id(reg);
	buf.seg();
	if (host64()) {
		uint8_t rex = 0x48;
		if (id >= 8)
			rex = (uint8_t)(rex | 0x04);
		buf.b(rex);
		buf.b(store ? 0x89 : 0x8B);
		buf.b((uint8_t)(0x04 | ((id & 7) << 3)));
		buf.b(0x25);
		buf.d(disp);
	} else {
		buf.b(store ? 0x89 : 0x8B);
		buf.b((uint8_t)(0x05 | ((id & 7) << 3)));
		buf.d(disp);
	}
}

inline void emit_mov_reg_ctx(Buf& buf, ZydisRegister reg, int slot) {
	emit_abs_mem(buf, resize_reg(reg, host64() ? 64 : 32), ctx_off(slot), false);
}

inline void emit_mov_ctx_reg(Buf& buf, int slot, ZydisRegister reg) {
	emit_abs_mem(buf, resize_reg(reg, host64() ? 64 : 32), ctx_off(slot), true);
}

inline void emit_mov_ctx_imm32(Buf& buf, int slot, uint32_t value) {
	buf.seg();
	if (host64()) {
		buf.b(0x48);
		buf.b(0xC7);
		buf.b(0x04);
		buf.b(0x25);
		buf.d(ctx_off(slot));
		buf.d(value);
	} else {
		buf.b(0xC7);
		buf.b(0x05);
		buf.d(ctx_off(slot));
		buf.d(value);
	}
}

inline void emit_pop_ctx(Buf& buf, int slot) {
	buf.seg();
	if (host64()) {
		buf.b(0x8F);
		buf.b(0x04);
		buf.b(0x25);
		buf.d(ctx_off(slot));
	} else {
		buf.b(0x8F);
		buf.b(0x05);
		buf.d(ctx_off(slot));
	}
}

inline void emit_jmp_rel(Buf& buf, uintptr_t target) {
	buf.b(0xE9);
	uint8_t* at = buf.p;
	buf.d(0);
	int32_t rel = (int32_t)((intptr_t)target - (intptr_t)(at + 4));
	memcpy(at, &rel, 4);
}

inline void emit_lea_rsi_imm8(Buf& buf, uint8_t imm) {
	if (host64())
		buf.b(0x48);
	buf.b(0x8D);
	buf.b(0x76);
	buf.b(imm);
}

inline void emit_movzx_rsi_byte(Buf& buf, int disp) {
	if (host64())
		buf.b(0x48);
	buf.b(0x0F);
	buf.b(0xB6);
	if (disp == 0) {
		buf.b(0x36);
	} else {
		buf.b(0x76);
		buf.b((uint8_t)disp);
	}
}

inline bool emit_encoded(Buf& buf, ZydisEncoderRequest* req) {
	int n = 0;
	if (!encode_request(req, buf.p, (size_t)(buf.end - buf.p), &n))
		return false;
	buf.p += n;
	return true;
}

inline bool emit_ops(Buf& buf, ZydisMnemonic mnemonic, ZydisEncoderOperand* ops, uint8_t count) {
	int n = 0;
	if (!encode_simple(mnemonic, ops, count, buf.p, (size_t)(buf.end - buf.p), &n))
		return false;
	buf.p += n;
	return true;
}

inline bool emit_load_sx(Buf& buf, ZydisRegister dst, int disp, int bits) {
	ZydisRegister cursor = host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI;
	if (bits < 0) {
		ZydisEncoderOperand ops[2] = {reg_op(resize_reg(dst, host64() ? 64 : 32)), imm_op(0)};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
	}
	int width = bits == 0 ? (host64() ? 64 : 32) : bits;
	ZydisMnemonic mn = ZYDIS_MNEMONIC_MOVSX;
	ZydisRegister to = resize_reg(dst, host64() ? 64 : 32);
	if (width == 32 && !host64())
		mn = ZYDIS_MNEMONIC_MOV;
	if (width == 32 && host64())
		mn = ZYDIS_MNEMONIC_MOVSXD;
	if (width >= 64) {
		mn = ZYDIS_MNEMONIC_MOV;
		to = resize_reg(dst, 64);
		width = 64;
	}
	ZydisEncoderOperand ops[2] = {
		reg_op(width == 32 && !host64() ? resize_reg(dst, 32) : to),
		mem_op(cursor, ZYDIS_REGISTER_NONE, 0, disp, (uint16_t)(width / 8)),
	};
	if (width == 32 && host64())
		ops[0] = reg_op(resize_reg(dst, 64));
	return emit_ops(buf, mn, ops, 2);
}

inline bool emit_load_zx(Buf& buf, ZydisRegister dst, int disp, int bits) {
	ZydisRegister cursor = host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI;
	if (bits < 0) {
		ZydisEncoderOperand ops[2] = {reg_op(resize_reg(dst, host64() ? 64 : 32)), imm_op(1)};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
	}
	int width = bits == 0 ? (host64() ? 64 : 32) : bits;
	if ((width == 8 || width == 16)) {
		ZydisEncoderOperand ops[2] = {
			reg_op(resize_reg(dst, host64() ? 64 : 32)),
			mem_op(cursor, ZYDIS_REGISTER_NONE, 0, disp, (uint16_t)(width / 8)),
		};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOVZX, ops, 2);
	}
	if (width == 32 && host64()) {
		ZydisEncoderOperand zero[2] = {reg_op(resize_reg(dst, 64)), imm_op(0)};
		if (!emit_ops(buf, ZYDIS_MNEMONIC_MOV, zero, 2))
			return false;
		ZydisEncoderOperand ops[2] = {
			reg_op(resize_reg(dst, 32)),
			mem_op(cursor, ZYDIS_REGISTER_NONE, 0, disp, 4),
		};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
	}
	ZydisEncoderOperand ops[2] = {
		reg_op(resize_reg(dst, width >= 64 ? 64 : 32)),
		mem_op(cursor, ZYDIS_REGISTER_NONE, 0, disp, (uint16_t)((width >= 64 ? 64 : width) / 8)),
	};
	return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
}

}  // namespace x86
}  // namespace jitvm

#endif
