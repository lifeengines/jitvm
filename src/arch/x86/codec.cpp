#include "arch/x86/codec.hpp"

#include "arch/x86/layout.hpp"

#include <string.h>

namespace jitvm {
namespace x86 {
namespace {

ZydisDecoder g_decoder;
bool g_ready = false;

void ready() {
	if (g_ready)
		return;
	ZydisDecoderInit(&g_decoder, machine_mode(), stack_width());
	g_ready = true;
}

bool same_reg(ZydisRegister a, ZydisRegister b) {
	if (a == ZYDIS_REGISTER_NONE || b == ZYDIS_REGISTER_NONE)
		return false;
	ZydisRegister la = ZydisRegisterGetLargestEnclosing(machine_mode(), a);
	ZydisRegister lb = ZydisRegisterGetLargestEnclosing(machine_mode(), b);
	return la == lb;
}

}  // namespace

ZydisMachineMode machine_mode() {
	return host64() ? ZYDIS_MACHINE_MODE_LONG_64 : ZYDIS_MACHINE_MODE_LONG_COMPAT_32;
}

ZydisStackWidth stack_width() {
	return host64() ? ZYDIS_STACK_WIDTH_64 : ZYDIS_STACK_WIDTH_32;
}

bool decode_bytes(const uint8_t* bytes, uint32_t available, Decoded* out) {
	if (!bytes || !out || available == 0)
		return false;
	ready();
	memset(&out->dec, 0, sizeof(out->dec));
	memset(out->ops, 0, sizeof(out->ops));
	if (ZYAN_FAILED(ZydisDecoderDecodeFull(&g_decoder, bytes, available, &out->dec, out->ops)))
		return false;
	out->bytes = bytes;
	return out->dec.length > 0 && out->dec.length <= available;
}

uint32_t opcode_length(const Decoded& inst) {
	uint32_t length = inst.dec.length;
	bool moved = true;
	while (moved) {
		moved = false;
		const ZyanU8 fields[3][2] = {
			{inst.dec.raw.disp.offset, inst.dec.raw.disp.size},
			{inst.dec.raw.imm[0].offset, inst.dec.raw.imm[0].size},
			{inst.dec.raw.imm[1].offset, inst.dec.raw.imm[1].size},
		};
		for (int i = 0; i < 3; i++) {
			uint32_t off = fields[i][0];
			uint32_t bits = fields[i][1];
			uint32_t sz = bits / 8;
			if (sz && off + sz == length && off < length) {
				length = off;
				moved = true;
			}
		}
	}
	if (length == 0)
		length = inst.dec.length;
	return length;
}

bool refs_reg(const Decoded& inst, ZydisRegister reg) {
	for (ZyanU8 i = 0; i < inst.dec.operand_count; i++) {
		const ZydisDecodedOperand& op = inst.ops[i];
		if (op.type == ZYDIS_OPERAND_TYPE_REGISTER && same_reg(op.reg.value, reg))
			return true;
		if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
			if (same_reg(op.mem.base, reg) || same_reg(op.mem.index, reg))
				return true;
		}
	}
	return false;
}

ZydisRegister resize_reg(ZydisRegister reg, int bits) {
	if (reg == ZYDIS_REGISTER_NONE)
		return reg;
	ZydisRegister large = ZydisRegisterGetLargestEnclosing(machine_mode(), reg);
	ZyanU8 id = ZydisRegisterGetId(large);
	ZydisRegisterClass cls = ZYDIS_REGCLASS_GPR64;
	if (!host64() || bits == 32)
		cls = ZYDIS_REGCLASS_GPR32;
	if (bits == 16)
		cls = ZYDIS_REGCLASS_GPR16;
	if (bits == 8)
		cls = ZYDIS_REGCLASS_GPR8;
	if (bits >= 64 && host64())
		cls = ZYDIS_REGCLASS_GPR64;
	return ZydisRegisterEncode(cls, id);
}

ZydisRegister pick_temp(const Decoded* inst, ZydisRegister a, ZydisRegister b, ZydisRegister c, ZydisRegister d) {
	const ZydisRegister pool[] = {
		host64() ? ZYDIS_REGISTER_RAX : ZYDIS_REGISTER_EAX,
		host64() ? ZYDIS_REGISTER_RCX : ZYDIS_REGISTER_ECX,
		host64() ? ZYDIS_REGISTER_RDX : ZYDIS_REGISTER_EDX,
		host64() ? ZYDIS_REGISTER_RBX : ZYDIS_REGISTER_EBX,
		host64() ? ZYDIS_REGISTER_RDI : ZYDIS_REGISTER_EDI,
		host64() ? ZYDIS_REGISTER_R11 : ZYDIS_REGISTER_NONE,
	};
	for (ZydisRegister cand : pool) {
		if (cand == ZYDIS_REGISTER_NONE)
			continue;
		if (same_reg(cand, a) || same_reg(cand, b) || same_reg(cand, c) || same_reg(cand, d))
			continue;
		if (same_reg(cand, host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI))
			continue;
		if (inst && refs_reg(*inst, cand))
			continue;
		return cand;
	}
	return host64() ? ZYDIS_REGISTER_R11 : ZYDIS_REGISTER_EAX;
}

int explicit_index(const Decoded& inst, int nth) {
	int seen = 0;
	for (ZyanU8 i = 0; i < inst.dec.operand_count; i++) {
		if (inst.ops[i].visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
			continue;
		if (seen == nth)
			return (int)i;
		seen++;
	}
	return -1;
}

bool encode_request(ZydisEncoderRequest* req, uint8_t* out, size_t cap, int* written) {
	ZyanUSize len = cap;
	if (ZYAN_FAILED(ZydisEncoderEncodeInstruction(req, out, &len)) || len == 0 || len > cap)
		return false;
	*written = (int)len;
	return true;
}

bool encode_simple(ZydisMnemonic mnemonic, ZydisEncoderOperand* ops, uint8_t count, uint8_t* out, size_t cap, int* written) {
	ZydisEncoderRequest req;
	memset(&req, 0, sizeof(req));
	req.machine_mode = machine_mode();
	req.mnemonic = mnemonic;
	req.operand_count = count;
	req.allowed_encodings = ZYDIS_ENCODABLE_ENCODING_LEGACY;
	for (uint8_t i = 0; i < count; i++)
		req.operands[i] = ops[i];
	return encode_request(&req, out, cap, written);
}

}  // namespace x86
}  // namespace jitvm
