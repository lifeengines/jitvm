#include "arch/x86/internal.hpp"

#include "platform/lock.hpp"

#include <string.h>

namespace jitvm {
namespace x86 {
namespace {

enum PlanKind {
	PLAN_COPY = 0,
	PLAN_REENCODE = 1,
	PLAN_ERROR = 2
};

struct Plan {
	int kind;
	bool fold;
	bool substitute;
	bool vsib;
	int extra[8];
	int nextra;
	ZydisRegister ea;
	ZydisRegister imm[2];
	int nimm;
};

uintptr_t install_at(const Decoded& inst, uint32_t iclen, uintptr_t proc, const int* extra, int nextra) {
	return set_branch(inst.bytes, iclen, proc, extra, nextra);
}

uintptr_t install_error(const Decoded& inst, uint32_t iclen, const int* extra, int nextra) {
	return install_at(inst, iclen, error_proc(), extra, nextra);
}

bool buf_ok(const Buf& buf) {
	return buf.start != nullptr;
}

ZydisRegister rsi_reg() {
	return host64() ? ZYDIS_REGISTER_RSI : ZYDIS_REGISTER_ESI;
}

ZydisRegister rdi_reg() {
	return host64() ? ZYDIS_REGISTER_RDI : ZYDIS_REGISTER_EDI;
}

ZydisRegister rax_reg() {
	return host64() ? ZYDIS_REGISTER_RAX : ZYDIS_REGISTER_EAX;
}

ZydisRegister rcx_reg() {
	return host64() ? ZYDIS_REGISTER_RCX : ZYDIS_REGISTER_ECX;
}

bool same_reg(ZydisRegister a, ZydisRegister b) {
	if (a == ZYDIS_REGISTER_NONE || b == ZYDIS_REGISTER_NONE)
		return false;
	ZydisRegister la = ZydisRegisterGetLargestEnclosing(machine_mode(), a);
	ZydisRegister lb = ZydisRegisterGetLargestEnclosing(machine_mode(), b);
	return la == lb;
}

bool rip_relative(const ZydisDecodedOperand& op) {
	return op.type == ZYDIS_OPERAND_TYPE_MEMORY &&
		(op.mem.base == ZYDIS_REGISTER_RIP || op.mem.base == ZYDIS_REGISTER_EIP);
}

ZydisRegister mem_base(const ZydisDecodedOperand& op) {
	if (op.mem.base == ZYDIS_REGISTER_RIP || op.mem.base == ZYDIS_REGISTER_EIP)
		return ZYDIS_REGISTER_NONE;
	return op.mem.base;
}

bool is_vsib(const ZydisDecodedOperand& op) {
	return op.type == ZYDIS_OPERAND_TYPE_MEMORY && op.mem.type == ZYDIS_MEMOP_TYPE_VSIB;
}

int native_bits() {
	return host64() ? 64 : 32;
}

ZydisRegister native_reg(ZydisRegister reg) {
	return resize_reg(reg, native_bits());
}

void emit_finish_tail(Buf& buf, uint32_t length) {
	emit_jmp_rel(buf, tail_for(length));
}

void emit_finish_flow(Buf& buf, bool flow, uint32_t flow_type) {
	if (flow && flow_enabled())
		emit_mov_ctx_imm32(buf, SLOT_FLOWTYPE, flow_type);
	emit_jmp_rel(buf, dispatch_target(flow && flow_enabled()));
}

bool pick_free(const Decoded* inst, ZydisRegister a, ZydisRegister b, ZydisRegister c, ZydisRegister* out) {
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
		if (same_reg(cand, a) || same_reg(cand, b) || same_reg(cand, c) || same_reg(cand, rsi_reg()))
			continue;
		if (inst && refs_reg(*inst, cand))
			continue;
		*out = cand;
		return true;
	}
	return false;
}

ZyanU64 seg_attr(ZydisRegister seg) {
	switch (seg) {
	case ZYDIS_REGISTER_CS: return ZYDIS_ATTRIB_HAS_SEGMENT_CS;
	case ZYDIS_REGISTER_SS: return ZYDIS_ATTRIB_HAS_SEGMENT_SS;
	case ZYDIS_REGISTER_DS: return ZYDIS_ATTRIB_HAS_SEGMENT_DS;
	case ZYDIS_REGISTER_ES: return ZYDIS_ATTRIB_HAS_SEGMENT_ES;
	case ZYDIS_REGISTER_FS: return ZYDIS_ATTRIB_HAS_SEGMENT_FS;
	case ZYDIS_REGISTER_GS: return ZYDIS_ATTRIB_HAS_SEGMENT_GS;
	default: return 0;
	}
}

bool flow_fixed(const Decoded& inst) {
	switch (inst.dec.meta.category) {
	case ZYDIS_CATEGORY_CALL:
	case ZYDIS_CATEGORY_RET:
	case ZYDIS_CATEGORY_UNCOND_BR:
	case ZYDIS_CATEGORY_COND_BR:
	case ZYDIS_CATEGORY_INTERRUPT:
	case ZYDIS_CATEGORY_SYSCALL:
	case ZYDIS_CATEGORY_SYSRET:
		return true;
	default:
		break;
	}
	switch (inst.dec.mnemonic) {
	case ZYDIS_MNEMONIC_XBEGIN:
	case ZYDIS_MNEMONIC_SYSCALL:
	case ZYDIS_MNEMONIC_SYSENTER:
	case ZYDIS_MNEMONIC_SYSEXIT:
	case ZYDIS_MNEMONIC_SYSRET:
	case ZYDIS_MNEMONIC_INT:
	case ZYDIS_MNEMONIC_INT1:
	case ZYDIS_MNEMONIC_INT3:
	case ZYDIS_MNEMONIC_INTO:
	case ZYDIS_MNEMONIC_UD2:
		return true;
	default:
		return false;
	}
}

bool bt_mem_imm(const Decoded& inst) {
	switch (inst.dec.mnemonic) {
	case ZYDIS_MNEMONIC_BT:
	case ZYDIS_MNEMONIC_BTC:
	case ZYDIS_MNEMONIC_BTS:
	case ZYDIS_MNEMONIC_BTR:
		break;
	default:
		return false;
	}
	for (ZyanU8 i = 0; i < inst.dec.operand_count_visible; i++) {
		if (inst.ops[i].visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT &&
			inst.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
			return true;
	}
	return false;
}

int extend_bits(const Decoded& inst, const ZydisDecodedOperand& imm) {
	int isz = (int)imm.size;
	int osz = (int)inst.dec.operand_width;
	if (isz <= 0)
		return 0;
	if (osz <= 0 || osz > 64)
		return isz <= 64 ? isz : 0;
	if (isz >= osz)
		return isz <= 64 ? isz : 0;
	return osz;
}

int append_span(int* extra, int n, int cap, int off, int bytes) {
	if (bytes <= 0 || off < 0)
		return n;
	if (n + bytes > cap)
		return -1;
	for (int i = 0; i < bytes; i++)
		extra[n++] = off + i;
	return n;
}

int append_imm_bytes(const Decoded& inst, int* extra, int n, int cap) {
	for (int k = 0; k < 2; k++) {
		int bytes = (int)inst.dec.raw.imm[k].size / 8;
		if (!bytes)
			continue;
		n = append_span(extra, n, cap, (int)inst.dec.raw.imm[k].offset, bytes);
		if (n < 0)
			return -1;
	}
	return n;
}

int append_disp_bytes(const Decoded& inst, int* extra, int n, int cap) {
	int bytes = (int)inst.dec.raw.disp.size / 8;
	if (!bytes)
		return n;
	return append_span(extra, n, cap, (int)inst.dec.raw.disp.offset, bytes);
}

bool rewrite_request(const Decoded& inst, bool fold, bool substitute, ZydisRegister ea,
	ZydisRegister imm_a, ZydisRegister imm_b, ZydisEncoderRequest* req) {
	memset(req, 0, sizeof(*req));
	if (ZYAN_FAILED(ZydisEncoderDecodedInstructionToEncoderRequest(
			&inst.dec, inst.ops, inst.dec.operand_count_visible, req)))
		return false;
	ZyanU8 n = inst.dec.operand_count_visible;
	if (n > ZYDIS_ENCODER_MAX_OPERANDS)
		return false;
	int imm_n = 0;
	int mem_n = 0;
	for (ZyanU8 i = 0; i < n; i++) {
		const ZydisDecodedOperand& op = inst.ops[i];
		if (op.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
			continue;
		if (substitute && op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
			int bits = extend_bits(inst, op);
			if (bits != 8 && bits != 16 && bits != 32 && bits != 64)
				return false;
			if (imm_n >= 2)
				return false;
			ZydisRegister tmp = imm_n == 0 ? imm_a : imm_b;
			req->operands[i] = reg_op(resize_reg(tmp, bits));
			imm_n++;
			continue;
		}
		if (fold && mem_n == 0 && op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
			if (req->operands[i].type != ZYDIS_OPERAND_TYPE_MEMORY)
				return false;
			bool vsib = is_vsib(op);
			bool agen = op.mem.type == ZYDIS_MEMOP_TYPE_AGEN || inst.dec.mnemonic == ZYDIS_MNEMONIC_LEA;
			ZydisEncoderOperand& mem = req->operands[i];
			mem.mem.base = native_reg(ea);
			mem.mem.displacement = 0;
			if (!vsib) {
				mem.mem.index = ZYDIS_REGISTER_NONE;
				mem.mem.scale = 0;
				req->address_size_hint = host64() ? ZYDIS_ADDRESS_SIZE_HINT_64 : ZYDIS_ADDRESS_SIZE_HINT_32;
			}
			req->prefixes &= ~ZYDIS_ATTRIB_HAS_SEGMENT;
			if (!agen) {
				ZyanU64 seg = seg_attr(op.mem.segment);
				if (op.mem.segment != ZYDIS_REGISTER_DS && op.mem.segment != ZYDIS_REGISTER_NONE && seg)
					req->prefixes |= seg;
			}
			mem_n++;
		}
	}
	if (fold && mem_n != 1)
		return false;
	if (substitute && imm_n == 0)
		return false;
	return true;
}

bool encode_ok(const Decoded& inst, bool fold, bool substitute, ZydisRegister ea,
	ZydisRegister imm_a, ZydisRegister imm_b) {
	ZydisEncoderRequest req;
	if (!rewrite_request(inst, fold, substitute, ea, imm_a, imm_b, &req))
		return false;
	uint8_t tmp[64];
	int n = 0;
	return encode_request(&req, tmp, sizeof(tmp), &n);
}

bool any_rip(const Decoded& inst) {
	for (ZyanU8 i = 0; i < inst.dec.operand_count_visible; i++) {
		if (inst.ops[i].visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT && rip_relative(inst.ops[i]))
			return true;
	}
	return false;
}

void make_plan(const Decoded& inst, Plan* plan) {
	memset(plan, 0, sizeof(*plan));
	plan->kind = PLAN_COPY;
	plan->ea = ZYDIS_REGISTER_NONE;
	plan->imm[0] = ZYDIS_REGISTER_NONE;
	plan->imm[1] = ZYDIS_REGISTER_NONE;
	if (flow_fixed(inst))
		return;

	uint32_t iclen = opcode_length(inst);
	bool has_disp = inst.dec.raw.disp.size != 0;
	bool rip = any_rip(inst);
	bool vsib = false;
	int explicit_imms = 0;
	bool blocked = bt_mem_imm(inst);
	bool relative = false;
	int mems = 0;
	for (ZyanU8 i = 0; i < inst.dec.operand_count_visible; i++) {
		const ZydisDecodedOperand& op = inst.ops[i];
		if (op.visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
			continue;
		if (op.type == ZYDIS_OPERAND_TYPE_MEMORY) {
			mems++;
			if (is_vsib(op))
				vsib = true;
		} else if (op.type == ZYDIS_OPERAND_TYPE_IMMEDIATE) {
			explicit_imms++;
			if (op.imm.is_relative)
				relative = true;
			if (op.encoding == ZYDIS_OPERAND_ENCODING_IS4)
				blocked = true;
		}
	}
	bool fold = mems > 0 && (has_disp || rip);
	plan->fold = fold;
	plan->vsib = vsib;
	plan->nimm = explicit_imms;

	if (!fold && iclen >= inst.dec.length)
		return;
	if (relative) {
		plan->kind = PLAN_ERROR;
		return;
	}

	bool want_sub = explicit_imms > 0 && explicit_imms <= 2 && !blocked && !(fold && explicit_imms > 1);
	ZydisRegister ea = ZYDIS_REGISTER_NONE;
	ZydisRegister i0 = ZYDIS_REGISTER_NONE;
	ZydisRegister i1 = ZYDIS_REGISTER_NONE;
	bool temps_ok = true;
	if (fold && !pick_free(&inst, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, &ea))
		temps_ok = false;
	if (want_sub) {
		if (!pick_free(&inst, ea, ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, &i0))
			temps_ok = false;
		if (explicit_imms > 1 && !pick_free(&inst, ea, i0, ZYDIS_REGISTER_NONE, &i1))
			temps_ok = false;
	}

	if (want_sub && temps_ok && encode_ok(inst, fold, true, fold ? ea : rdi_reg(), i0, i1)) {
		plan->kind = PLAN_REENCODE;
		plan->substitute = true;
		plan->ea = fold ? ea : ZYDIS_REGISTER_NONE;
		plan->imm[0] = i0;
		plan->imm[1] = i1;
		plan->nextra = 0;
		return;
	}

	if (fold && temps_ok && encode_ok(inst, true, false, ea, rdi_reg(), rdi_reg())) {
		int n = append_imm_bytes(inst, plan->extra, 0, 8);
		if (n >= 0) {
			plan->kind = PLAN_REENCODE;
			plan->substitute = false;
			plan->ea = ea;
			plan->nextra = n;
			return;
		}
	}

	if (!rip) {
		int n = 0;
		if (has_disp)
			n = append_disp_bytes(inst, plan->extra, 0, 8);
		if (n >= 0)
			n = append_imm_bytes(inst, plan->extra, n, 8);
		if (n < 0 || (iclen < inst.dec.length && n == 0)) {
			plan->kind = PLAN_ERROR;
			return;
		}
		plan->kind = PLAN_COPY;
		plan->fold = false;
		plan->substitute = false;
		plan->nextra = n;
		return;
	}
	plan->kind = PLAN_ERROR;
}

bool emit_add_reg(Buf& buf, ZydisRegister acc, ZydisRegister add, uint8_t scale, int disp) {
	ZydisRegister a = native_reg(acc);
	ZydisRegister b = native_reg(add);
	ZydisRegister base = a;
	ZydisRegister index = b;
	uint8_t sc = scale == 0 ? 1 : scale;
	if (reg_id(b) == 4) {
		if (sc != 1)
			return false;
		base = b;
		index = a;
	}
	ZydisEncoderOperand ops[2] = {
		reg_op(a),
		mem_op(base, index, sc, disp, (uint16_t)(native_bits() / 8)),
	};
	return emit_ops(buf, ZYDIS_MNEMONIC_LEA, ops, 2);
}

bool emit_fit_address(Buf& buf, ZydisRegister dst, int width) {
	if (width == 32 && host64()) {
		ZydisEncoderOperand ops[2] = {reg_op(resize_reg(dst, 32)), reg_op(resize_reg(dst, 32))};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
	}
	if (width == 16) {
		ZydisEncoderOperand ops[2] = {
			reg_op(resize_reg(dst, 32)),
			reg_op(resize_reg(dst, 16)),
		};
		return emit_ops(buf, ZYDIS_MNEMONIC_MOVZX, ops, 2);
	}
	return true;
}

bool emit_ea(Buf& buf, const Decoded& inst, const ZydisDecodedOperand& mem, ZydisRegister dst, bool partial, bool* rsi_guest) {
	int dbits = inst.dec.raw.disp.size ? (int)inst.dec.raw.disp.size : -1;
	int doff = dbits > 0 ? (int)inst.dec.raw.disp.offset : -1;
	if (!emit_load_sx(buf, dst, doff, dbits))
		return false;
	if (rip_relative(mem)) {
		if (!emit_add_reg(buf, dst, rsi_reg(), 1, (int)inst.dec.length))
			return false;
	}
	ZydisRegister base = mem_base(mem);
	ZydisRegister index = ZYDIS_REGISTER_NONE;
	uint8_t scale = 1;
	if (!partial && !is_vsib(mem) && mem.mem.index != ZYDIS_REGISTER_NONE) {
		index = mem.mem.index;
		scale = mem.mem.scale ? mem.mem.scale : 1;
	}
	if (same_reg(base, rsi_reg()) || same_reg(index, rsi_reg())) {
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_RSI);
		*rsi_guest = true;
	}
	if (base != ZYDIS_REGISTER_NONE) {
		if (!emit_add_reg(buf, dst, base, 1, 0))
			return false;
	}
	if (index != ZYDIS_REGISTER_NONE) {
		if (!emit_add_reg(buf, dst, index, scale, 0))
			return false;
	}
	int aw = (int)inst.dec.address_width;
	if (partial || aw == 16 || (aw == 32 && host64())) {
		if (!emit_fit_address(buf, dst, aw == 16 ? 16 : (aw == 32 ? 32 : native_bits())))
			return false;
	}
	return true;
}

bool emit_load_imm(Buf& buf, ZydisRegister dst, const Decoded& inst, int operand_index) {
	int raw = 0;
	for (int i = 0; i < operand_index; i++) {
		if (inst.ops[i].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			raw++;
	}
	if (raw > 1)
		return false;
	const ZydisDecodedOperand& op = inst.ops[operand_index];
	int isz = (int)inst.dec.raw.imm[raw].size;
	if (isz <= 0)
		isz = (int)op.size;
	int bits = extend_bits(inst, op);
	int off = (int)inst.dec.raw.imm[raw].offset;
	if (op.imm.is_signed && isz < bits && isz > 0)
		return emit_load_sx(buf, dst, off, isz);
	return emit_load_zx(buf, dst, off, isz);
}

const ZydisDecodedOperand* explicit_mem(const Decoded& inst) {
	for (ZyanU8 i = 0; i < inst.dec.operand_count_visible; i++) {
		if (inst.ops[i].visibility == ZYDIS_OPERAND_VISIBILITY_EXPLICIT &&
			inst.ops[i].type == ZYDIS_OPERAND_TYPE_MEMORY)
			return &inst.ops[i];
	}
	return nullptr;
}

uintptr_t emit_copy(const Decoded& inst, uint32_t iclen, bool force_rsi, const int* extra, int nextra) {
	Buf buf = begin_code(96);
	if (!buf_ok(buf))
		return install_error(inst, iclen, extra, nextra);
	bool rsi = force_rsi || refs_reg(inst, rsi_reg());
	if (rsi)
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_RSI);
	buf.raw(inst.bytes, inst.dec.length);
	if (rsi)
		emit_mov_ctx_reg(buf, SLOT_RSI, rsi_reg());
	emit_finish_tail(buf, inst.dec.length);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, extra, nextra);
}

uintptr_t emit_reencode(const Decoded& inst, uint32_t iclen, const Plan& plan) {
	Buf buf = begin_code(640);
	if (!buf_ok(buf))
		return install_error(inst, iclen, plan.extra, plan.nextra);
	bool rsi_guest = false;
	bool uses_rsi = refs_reg(inst, rsi_reg());
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	ZydisRegister saved[3];
	int ns = 0;
	if (plan.fold)
		saved[ns++] = plan.ea;
	if (plan.substitute) {
		saved[ns++] = plan.imm[0];
		if (plan.nimm > 1)
			saved[ns++] = plan.imm[1];
	}
	int slot = SLOT_TMP1;
	for (int i = 0; i < ns; i++) {
		emit_mov_ctx_reg(buf, slot, native_reg(saved[i]));
		slot = SLOT_TMP2;
		if (i == 2)
			break;
	}
	if (plan.substitute) {
		int seen = 0;
		for (ZyanU8 i = 0; i < inst.dec.operand_count_visible; i++) {
			if (inst.ops[i].visibility != ZYDIS_OPERAND_VISIBILITY_EXPLICIT)
				continue;
			if (inst.ops[i].type != ZYDIS_OPERAND_TYPE_IMMEDIATE)
				continue;
			ZydisRegister dst = seen == 0 ? plan.imm[0] : plan.imm[1];
			if (!emit_load_imm(buf, dst, inst, (int)i))
				return install_error(inst, iclen, plan.extra, plan.nextra);
			seen++;
			if (seen >= 2)
				break;
		}
	}
	if (plan.fold) {
		const ZydisDecodedOperand* mem = explicit_mem(inst);
		if (!mem || !emit_ea(buf, inst, *mem, plan.ea, plan.vsib, &rsi_guest))
			return install_error(inst, iclen, plan.extra, plan.nextra);
	}
	if (uses_rsi && !rsi_guest)
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_RSI);
	ZydisEncoderRequest req;
	ZydisRegister ea = plan.fold ? plan.ea : rdi_reg();
	ZydisRegister i0 = plan.substitute ? plan.imm[0] : rdi_reg();
	ZydisRegister i1 = plan.substitute && plan.nimm > 1 ? plan.imm[1] : rdi_reg();
	if (!rewrite_request(inst, plan.fold, plan.substitute, ea, i0, i1, &req))
		return install_error(inst, iclen, plan.extra, plan.nextra);
	if (!emit_encoded(buf, &req))
		return install_error(inst, iclen, plan.extra, plan.nextra);
	if (uses_rsi)
		emit_mov_ctx_reg(buf, SLOT_RSI, rsi_reg());
	for (int i = ns - 1; i >= 0; i--) {
		int which = i == 0 ? SLOT_TMP1 : SLOT_TMP2;
		emit_mov_reg_ctx(buf, native_reg(saved[i]), which);
	}
	emit_finish_tail(buf, inst.dec.length);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, plan.extra, plan.nextra);
}

int ptr_bits(const Decoded& inst, const ZydisDecodedOperand& mem, bool far) {
	if (far) {
		if (host64())
			return 64;
		return inst.dec.operand_width == 16 ? 16 : 32;
	}
	int w = (int)inst.dec.operand_width;
	if (w == 16 || w == 32 || w == 64)
		return w;
	int bytes = (int)mem.size / 8;
	if (bytes == 8 || bytes == 4 || bytes == 2 || bytes == 1)
		return bytes * 8;
	return native_bits();
}

int stack_bits(const Decoded& inst, bool far) {
	if (host64() && far)
		return 64;
	int w = (int)inst.dec.operand_width;
	if (w == 16 || w == 32 || w == 64)
		return w;
	return native_bits();
}

bool emit_zero_reg(Buf& buf, ZydisRegister reg) {
	ZydisEncoderOperand ops[2] = {reg_op(resize_reg(reg, 32)), imm_op(0)};
	return emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2);
}

bool emit_load_abs(Buf& buf, ZydisRegister dst, ZydisRegister base, int bits, ZydisRegister seg) {
	if (bits == 16 && !emit_zero_reg(buf, dst))
		return false;
	ZydisEncoderRequest req;
	memset(&req, 0, sizeof(req));
	req.machine_mode = machine_mode();
	req.mnemonic = ZYDIS_MNEMONIC_MOV;
	req.operand_count = 2;
	req.allowed_encodings = ZYDIS_ENCODABLE_ENCODING_LEGACY;
	int rb = bits == 64 ? 64 : bits == 16 ? 16 : 32;
	req.operands[0] = reg_op(resize_reg(dst, rb));
	req.operands[1] = mem_op(native_reg(base), ZYDIS_REGISTER_NONE, 0, 0, (uint16_t)(bits / 8));
	req.address_size_hint = host64() ? ZYDIS_ADDRESS_SIZE_HINT_64 : ZYDIS_ADDRESS_SIZE_HINT_32;
	if (seg == ZYDIS_REGISTER_FS || seg == ZYDIS_REGISTER_GS)
		req.prefixes |= seg_attr(seg);
	return emit_encoded(buf, &req);
}

bool emit_push_reg(Buf& buf, ZydisRegister reg, int bits) {
	ZydisEncoderOperand op = reg_op(resize_reg(reg, bits == 64 ? 64 : bits == 16 ? 16 : 32));
	return emit_ops(buf, ZYDIS_MNEMONIC_PUSH, &op, 1);
}

bool emit_pop_ip(Buf& buf, int bits) {
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (bits == 16) {
		if (!emit_zero_reg(buf, rdi_reg()))
			return false;
		ZydisEncoderOperand op = reg_op(resize_reg(rdi_reg(), 16));
		if (!emit_ops(buf, ZYDIS_MNEMONIC_POP, &op, 1))
			return false;
	} else if (bits == 32 && host64()) {
		if (!emit_zero_reg(buf, rdi_reg()))
			return false;
		ZydisEncoderOperand op = reg_op(resize_reg(rdi_reg(), 32));
		if (!emit_ops(buf, ZYDIS_MNEMONIC_POP, &op, 1))
			return false;
	} else {
		ZydisEncoderOperand op = reg_op(native_reg(rdi_reg()));
		if (!emit_ops(buf, ZYDIS_MNEMONIC_POP, &op, 1))
			return false;
	}
	emit_mov_ctx_reg(buf, SLOT_IP, native_reg(rdi_reg()));
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	return true;
}

bool emit_pop_discard(Buf& buf, int bits) {
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	ZydisEncoderOperand op = reg_op(resize_reg(rdi_reg(), bits == 16 ? 16 : native_bits()));
	if (bits == 64)
		op = reg_op(native_reg(rdi_reg()));
	if (!emit_ops(buf, ZYDIS_MNEMONIC_POP, &op, 1))
		return false;
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	return true;
}

uintptr_t emit_jmp_imm(const Decoded& inst, uint32_t iclen) {
	Buf buf = begin_code(128);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	int off = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].offset : (int)iclen;
	int bits = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].size : 32;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (!emit_load_sx(buf, rdi_reg(), off, bits))
		return install_error(inst, iclen, nullptr, 0);
	if (!emit_add_reg(buf, rsi_reg(), rdi_reg(), 1, (int)inst.dec.length))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, JITVM_FLOW_JUMP_IMM);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_far_ptr(const Decoded& inst, uint32_t iclen, bool call) {
	Buf buf = begin_code(192);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	int off = (int)inst.dec.raw.imm[0].offset;
	int bits = (int)inst.dec.raw.imm[0].size;
	if (bits != 16 && bits != 32)
		bits = 32;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (!emit_load_zx(buf, rdi_reg(), off, bits))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_ctx_reg(buf, SLOT_TMP2, native_reg(rdi_reg()));
	if (call) {
		buf.raw("\x8C\xCF", 2);
		if (!emit_push_reg(buf, rdi_reg(), stack_bits(inst, true)))
			return install_error(inst, iclen, nullptr, 0);
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
		emit_lea_rsi_imm8(buf, (uint8_t)inst.dec.length);
		if (!emit_push_reg(buf, rsi_reg(), stack_bits(inst, true)))
			return install_error(inst, iclen, nullptr, 0);
	}
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP2);
	emit_mov_ctx_reg(buf, SLOT_IP, native_reg(rdi_reg()));
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, call ? JITVM_FLOW_CALL_IMM : JITVM_FLOW_JUMP_IMM);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_jmp_reg(const Decoded& inst, uint32_t iclen) {
	int index = explicit_index(inst, 0);
	if (index < 0)
		return install_error(inst, iclen, nullptr, 0);
	ZydisRegister target = inst.ops[index].reg.value;
	bool rsi = refs_reg(inst, rsi_reg());
	Buf buf = begin_code(96);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	if (rsi)
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_RSI);
	int bits = (int)inst.ops[index].size;
	if (bits == 16) {
		emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
		if (!emit_zero_reg(buf, rdi_reg()))
			return install_error(inst, iclen, nullptr, 0);
		ZydisEncoderOperand ops[2] = {reg_op(resize_reg(rdi_reg(), 16)), reg_op(resize_reg(target, 16))};
		if (!emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2))
			return install_error(inst, iclen, nullptr, 0);
		emit_mov_ctx_reg(buf, SLOT_IP, native_reg(rdi_reg()));
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	} else if (bits == 32 && host64()) {
		emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
		if (!emit_zero_reg(buf, rdi_reg()))
			return install_error(inst, iclen, nullptr, 0);
		ZydisEncoderOperand mov[2] = {reg_op(resize_reg(rdi_reg(), 32)), reg_op(resize_reg(target, 32))};
		if (!emit_ops(buf, ZYDIS_MNEMONIC_MOV, mov, 2))
			return install_error(inst, iclen, nullptr, 0);
		emit_mov_ctx_reg(buf, SLOT_IP, native_reg(rdi_reg()));
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	} else {
		emit_mov_ctx_reg(buf, SLOT_IP, native_reg(target));
	}
	if (rsi)
		emit_mov_ctx_reg(buf, SLOT_RSI, rsi_reg());
	emit_finish_flow(buf, true, JITVM_FLOW_JUMP_REG);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_jmp_mem(const Decoded& inst, uint32_t iclen, bool call) {
	int index = explicit_index(inst, 0);
	if (index < 0)
		return install_error(inst, iclen, nullptr, 0);
	const ZydisDecodedOperand& mem = inst.ops[index];
	bool far = inst.dec.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR;
	ZydisRegister tmp = ZYDIS_REGISTER_NONE;
	if (!pick_free(&inst, rdi_reg(), ZYDIS_REGISTER_NONE, ZYDIS_REGISTER_NONE, &tmp))
		tmp = host64() ? ZYDIS_REGISTER_R11 : ZYDIS_REGISTER_EBX;
	Buf buf = begin_code(320);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	bool rsi_guest = false;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(tmp));
	if (far)
		emit_mov_ctx_reg(buf, SLOT_TMP2, native_reg(rdi_reg()));
	if (!emit_ea(buf, inst, mem, tmp, false, &rsi_guest))
		return install_error(inst, iclen, nullptr, 0);
	int bits = ptr_bits(inst, mem, far);
	if (!emit_load_abs(buf, tmp, tmp, bits, mem.mem.segment))
		return install_error(inst, iclen, nullptr, 0);
	if (call) {
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
		emit_lea_rsi_imm8(buf, (uint8_t)inst.dec.length);
		if (far) {
			buf.raw("\x8C\xCF", 2);
			if (!emit_push_reg(buf, rdi_reg(), stack_bits(inst, true)))
				return install_error(inst, iclen, nullptr, 0);
		}
		if (!emit_push_reg(buf, rsi_reg(), stack_bits(inst, far)))
			return install_error(inst, iclen, nullptr, 0);
	}
	emit_mov_ctx_reg(buf, SLOT_IP, native_reg(tmp));
	emit_mov_reg_ctx(buf, native_reg(tmp), SLOT_TMP1);
	if (far)
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP2);
	emit_finish_flow(buf, true, call ? JITVM_FLOW_CALL_MEM : JITVM_FLOW_JUMP_MEM);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_jcc(const Decoded& inst, uint32_t iclen) {
	Buf buf = begin_code(320);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	int rbits = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].size : 8;
	int roff = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].offset : (int)iclen;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	buf.raw(inst.bytes, iclen);
	uint8_t* skip_disp = nullptr;
	int skip_width = 0;
	if (rbits <= 8) {
		buf.b(2);
		buf.b(0xEB);
		skip_disp = buf.p;
		buf.b(0);
		skip_width = 1;
	} else if (rbits == 16) {
		buf.w(5);
		buf.b(0xE9);
		skip_disp = buf.p;
		buf.d(0);
		skip_width = 4;
	} else {
		skip_disp = buf.p;
		buf.d(0);
		skip_width = 4;
	}
	uint8_t* fall_or_taken = buf.p;
	if (rbits >= 32) {
		emit_lea_rsi_imm8(buf, (uint8_t)inst.dec.length);
		emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
		if (flow_enabled())
			emit_mov_ctx_imm32(buf, SLOT_FLOWTYPE, JITVM_FLOW_COND);
		emit_jmp_rel(buf, dispatch_target(flow_enabled()));
		int32_t gap = (int32_t)(buf.p - (skip_disp + 4));
		memcpy(skip_disp, &gap, 4);
		(void)fall_or_taken;
	}
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (!emit_load_sx(buf, rdi_reg(), roff, rbits))
		return install_error(inst, iclen, nullptr, 0);
	if (!emit_add_reg(buf, rsi_reg(), rdi_reg(), 1, (int)inst.dec.length))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, JITVM_FLOW_COND);
	if (rbits < 32) {
		uint8_t* fall = buf.p;
		if (skip_width == 1) {
			int rel = (int)(fall - (skip_disp + 1));
			if (rel < -128 || rel > 127)
				return install_error(inst, iclen, nullptr, 0);
			*skip_disp = (uint8_t)rel;
		} else {
			int32_t rel = (int32_t)(fall - (skip_disp + 4));
			memcpy(skip_disp, &rel, 4);
		}
		emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
		emit_lea_rsi_imm8(buf, (uint8_t)inst.dec.length);
		emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
		if (flow_enabled())
			emit_mov_ctx_imm32(buf, SLOT_FLOWTYPE, JITVM_FLOW_COND);
		emit_jmp_rel(buf, dispatch_target(flow_enabled()));
	}
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_call_imm(const Decoded& inst, uint32_t iclen) {
	Buf buf = begin_code(128);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	int off = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].offset : (int)iclen;
	int bits = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].size : 32;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	int push_bits = stack_bits(inst, false);
	if (push_bits == native_bits()) {
		buf.b(0x57);
		if (host64())
			buf.b(0x48);
		buf.b(0x8D);
		buf.b(0x7E);
		buf.b((uint8_t)inst.dec.length);
		if (host64())
			buf.b(0x48);
		buf.raw("\x87\x3C\x24", 3);
	} else {
		emit_mov_ctx_reg(buf, SLOT_TMP2, native_reg(rdi_reg()));
		ZydisEncoderOperand lea[2] = {
			reg_op(native_reg(rdi_reg())),
			mem_op(native_reg(rsi_reg()), ZYDIS_REGISTER_NONE, 0, (int)inst.dec.length, (uint16_t)(native_bits() / 8)),
		};
		if (!emit_ops(buf, ZYDIS_MNEMONIC_LEA, lea, 2))
			return install_error(inst, iclen, nullptr, 0);
		if (!emit_push_reg(buf, rdi_reg(), push_bits))
			return install_error(inst, iclen, nullptr, 0);
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP2);
	}
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (!emit_load_sx(buf, rdi_reg(), off, bits))
		return install_error(inst, iclen, nullptr, 0);
	if (!emit_add_reg(buf, rsi_reg(), rdi_reg(), 1, (int)inst.dec.length))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, JITVM_FLOW_CALL_IMM);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_call_reg(const Decoded& inst, uint32_t iclen) {
	int index = explicit_index(inst, 0);
	if (index < 0)
		return install_error(inst, iclen, nullptr, 0);
	ZydisRegister target = inst.ops[index].reg.value;
	Buf buf = begin_code(160);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (same_reg(target, rsi_reg()))
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_RSI);
	else if (same_reg(target, rdi_reg()))
		emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	else {
		ZydisEncoderOperand ops[2] = {reg_op(native_reg(rdi_reg())), reg_op(native_reg(target))};
		if ((int)inst.ops[index].size == 32 && host64()) {
			if (!emit_zero_reg(buf, rdi_reg()))
				return install_error(inst, iclen, nullptr, 0);
			ops[0] = reg_op(resize_reg(rdi_reg(), 32));
			ops[1] = reg_op(resize_reg(target, 32));
		}
		if (!emit_ops(buf, ZYDIS_MNEMONIC_MOV, ops, 2))
			return install_error(inst, iclen, nullptr, 0);
	}
	emit_mov_ctx_reg(buf, SLOT_TMP2, native_reg(rdi_reg()));
	emit_lea_rsi_imm8(buf, (uint8_t)inst.dec.length);
	if (!emit_push_reg(buf, rsi_reg(), stack_bits(inst, false)))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP2);
	emit_mov_ctx_reg(buf, SLOT_IP, native_reg(rdi_reg()));
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, JITVM_FLOW_CALL_REG);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_ret(const Decoded& inst, uint32_t iclen) {
	bool iret = inst.dec.mnemonic == ZYDIS_MNEMONIC_IRET || inst.dec.mnemonic == ZYDIS_MNEMONIC_IRETD ||
		inst.dec.mnemonic == ZYDIS_MNEMONIC_IRETQ;
	if (iret)
		return emit_copy(inst, iclen, false, nullptr, 0);
	Buf buf = begin_code(192);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	bool far = inst.dec.meta.branch_type == ZYDIS_BRANCH_TYPE_FAR;
	int bits = stack_bits(inst, far);
	int imm = explicit_index(inst, 0);
	bool has_imm = imm >= 0 && inst.ops[imm].type == ZYDIS_OPERAND_TYPE_IMMEDIATE;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	if (flow_enabled())
		emit_mov_ctx_reg(buf, SLOT_FLOWIP, rsi_reg());
	if (has_imm) {
		int off = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].offset : (int)iclen;
		int ib = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].size : 16;
		emit_mov_ctx_reg(buf, SLOT_TMP2, native_reg(rax_reg()));
		if (!emit_load_zx(buf, rax_reg(), off, ib))
			return install_error(inst, iclen, nullptr, 0);
	}
	if (!emit_pop_ip(buf, bits))
		return install_error(inst, iclen, nullptr, 0);
	if (far && !emit_pop_discard(buf, bits))
		return install_error(inst, iclen, nullptr, 0);
	if (has_imm) {
		if (!emit_add_reg(buf, host64() ? ZYDIS_REGISTER_RSP : ZYDIS_REGISTER_ESP, rax_reg(), 1, 0))
			return install_error(inst, iclen, nullptr, 0);
		emit_mov_reg_ctx(buf, native_reg(rax_reg()), SLOT_TMP2);
	}
	emit_finish_flow(buf, true, JITVM_FLOW_RETURN);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_xbegin(const Decoded& inst, uint32_t iclen) {
	Buf buf = begin_code(192);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	int roff = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].offset : (int)iclen;
	int rbits = inst.dec.raw.imm[0].size ? (int)inst.dec.raw.imm[0].size : 32;
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	buf.raw(inst.bytes, iclen);
	uint8_t* disp = buf.p;
	if (rbits <= 8)
		buf.b(0);
	else if (rbits == 16)
		buf.w(0);
	else
		buf.d(0);
	emit_finish_tail(buf, inst.dec.length);
	uint8_t* abort_at = buf.p;
	if (rbits <= 8) {
		int rel = (int)(abort_at - (disp + 1));
		if (rel < -128 || rel > 127)
			return install_error(inst, iclen, nullptr, 0);
		*disp = (uint8_t)rel;
	} else if (rbits == 16) {
		int16_t rel = (int16_t)(abort_at - (disp + 2));
		memcpy(disp, &rel, 2);
	} else {
		int32_t rel = (int32_t)(abort_at - (disp + 4));
		memcpy(disp, &rel, 4);
	}
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	emit_mov_ctx_reg(buf, SLOT_TMP1, native_reg(rdi_reg()));
	if (!emit_load_sx(buf, rdi_reg(), roff, rbits))
		return install_error(inst, iclen, nullptr, 0);
	if (!emit_add_reg(buf, rsi_reg(), rdi_reg(), 1, (int)inst.dec.length))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_ctx_reg(buf, SLOT_IP, rsi_reg());
	emit_mov_reg_ctx(buf, native_reg(rdi_reg()), SLOT_TMP1);
	emit_finish_flow(buf, true, JITVM_FLOW_COND);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_syscall(const Decoded& inst, uint32_t iclen) {
	Buf buf = begin_code(96);
	if (!buf_ok(buf))
		return install_error(inst, iclen, nullptr, 0);
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_RSI);
	buf.raw(inst.bytes, inst.dec.length);
	emit_mov_ctx_reg(buf, SLOT_RSI, rsi_reg());
	emit_mov_reg_ctx(buf, rsi_reg(), SLOT_IP);
	ZydisEncoderOperand ops[2] = {
		reg_op(native_reg(rcx_reg())),
		mem_op(native_reg(rsi_reg()), ZYDIS_REGISTER_NONE, 0, (int)inst.dec.length, (uint16_t)(native_bits() / 8)),
	};
	if (!emit_ops(buf, ZYDIS_MNEMONIC_LEA, ops, 2))
		return install_error(inst, iclen, nullptr, 0);
	emit_finish_tail(buf, inst.dec.length);
	finish_code(buf);
	return install_at(inst, iclen, (uintptr_t)buf.start, nullptr, 0);
}

uintptr_t emit_exit(const Decoded& inst, uint32_t iclen) {
	return install_at(inst, iclen, exit_callback_proc(), nullptr, 0);
}

}  // namespace

int branch_extras(const Decoded& inst, int* extra, int cap) {
	Plan plan;
	make_plan(inst, &plan);
	if (plan.kind == PLAN_ERROR)
		return 0;
	int n = plan.nextra;
	if (n > cap)
		return -1;
	if (extra && n > 0)
		memcpy(extra, plan.extra, (size_t)n * sizeof(int));
	return n;
}

uintptr_t translate_insn(const uint8_t* ip) {
	Decoded inst;
	if (!decode_bytes(ip, 15, &inst))
		return error_proc();
	uint32_t iclen = opcode_length(inst);
	ZydisMnemonic mn = inst.dec.mnemonic;
	if (mn == ZYDIS_MNEMONIC_INT || mn == ZYDIS_MNEMONIC_INT1 || mn == ZYDIS_MNEMONIC_INT3 ||
		mn == ZYDIS_MNEMONIC_INTO || mn == ZYDIS_MNEMONIC_UD2)
		return emit_exit(inst, iclen);
	if (mn == ZYDIS_MNEMONIC_XBEGIN)
		return emit_xbegin(inst, iclen);
	switch (inst.dec.meta.category) {
	case ZYDIS_CATEGORY_CALL: {
		int op = explicit_index(inst, 0);
		if (op < 0)
			break;
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			return emit_call_imm(inst, iclen);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_REGISTER)
			return emit_call_reg(inst, iclen);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_MEMORY)
			return emit_jmp_mem(inst, iclen, true);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_POINTER)
			return emit_far_ptr(inst, iclen, true);
		break;
	}
	case ZYDIS_CATEGORY_RET:
		return emit_ret(inst, iclen);
	case ZYDIS_CATEGORY_SYSCALL:
		if (mn == ZYDIS_MNEMONIC_SYSCALL)
			return emit_syscall(inst, iclen);
		return emit_copy(inst, iclen, true, nullptr, 0);
	case ZYDIS_CATEGORY_SYSRET:
		return emit_copy(inst, iclen, refs_reg(inst, rsi_reg()), nullptr, 0);
	case ZYDIS_CATEGORY_UNCOND_BR: {
		int op = explicit_index(inst, 0);
		if (op < 0)
			break;
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_IMMEDIATE)
			return emit_jmp_imm(inst, iclen);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_REGISTER)
			return emit_jmp_reg(inst, iclen);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_MEMORY)
			return emit_jmp_mem(inst, iclen, false);
		if (inst.ops[op].type == ZYDIS_OPERAND_TYPE_POINTER)
			return emit_far_ptr(inst, iclen, false);
		break;
	}
	case ZYDIS_CATEGORY_COND_BR:
		return emit_jcc(inst, iclen);
	case ZYDIS_CATEGORY_INTERRUPT:
		return emit_exit(inst, iclen);
	default:
		break;
	}
	Plan plan;
	make_plan(inst, &plan);
	if (plan.kind == PLAN_ERROR)
		return install_error(inst, iclen, nullptr, 0);
	if (plan.kind == PLAN_COPY)
		return emit_copy(inst, iclen, false, plan.extra, plan.nextra);
	return emit_reencode(inst, iclen, plan);
}

}  // namespace x86
}  // namespace jitvm
