#ifndef JITVM_X86_CODEC_HPP
#define JITVM_X86_CODEC_HPP

#include <stdint.h>
#include <stddef.h>

#include <Zydis/Zydis.h>

namespace jitvm {
namespace x86 {

struct Decoded {
	ZydisDecodedInstruction dec;
	ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
	const uint8_t* bytes;
};

ZydisMachineMode machine_mode();
ZydisStackWidth stack_width();
bool decode_bytes(const uint8_t* bytes, uint32_t available, Decoded* out);
uint32_t opcode_length(const Decoded& inst);
bool refs_reg(const Decoded& inst, ZydisRegister reg);
ZydisRegister resize_reg(ZydisRegister reg, int bits);
ZydisRegister pick_temp(const Decoded* inst, ZydisRegister a = ZYDIS_REGISTER_NONE,
	ZydisRegister b = ZYDIS_REGISTER_NONE, ZydisRegister c = ZYDIS_REGISTER_NONE,
	ZydisRegister d = ZYDIS_REGISTER_NONE);
int explicit_index(const Decoded& inst, int nth);
bool encode_request(ZydisEncoderRequest* req, uint8_t* out, size_t cap, int* written);
bool encode_simple(ZydisMnemonic mnemonic, ZydisEncoderOperand* ops, uint8_t count, uint8_t* out, size_t cap, int* written);

}  // namespace x86
}  // namespace jitvm

#endif
