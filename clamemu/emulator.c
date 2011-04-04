/*
 *  ClamAV PE emulator
 *
 *  Copyright (C) 2010 - 2011, Sourcefire, Inc.
 *
 *  Authors: Török Edvin
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */
#include "emulator.h"
#include "vmm.h"
#include "others.h"
#include "disasm-common.h"
#include "disasm.h"
#include "pe.h"
#include "flags.h"
#include <string.h>


#define MAXREG (sizeof(reg_masks) / sizeof(reg_masks[0]))

struct DIS_mem_arg {
    enum X86REGS scale_reg;/**< register used as scale */
    enum X86REGS add_reg;/**< register used as displacemenet */
    uint8_t scale;/**< scale as immediate number */
    int32_t displacement;/**< displacement as immediate number */
};

enum operand {
    OPERAND_CALC, /* just calculate, i.e. lea not mov */
    OPERAND_READ, /* calculate and read from memory */
    OPERAND_WRITEREG,
    OPERAND_WRITEMEM
};

#define DEFINE_MEM(first, last, bits) \
    [first ... last] = {~0u >> (32 - bits), 0, 0, bits, bits - 1}
static const desc_t mem_desc [] = {
    DEFINE_MEM(SIZED, SIZED, 32),
    DEFINE_MEM(SIZEW, SIZEW, 16),
    DEFINE_MEM(SIZEB, SIZEB,  8)
};

int mem_push(cli_emu_t *state, unsigned size, uint32_t value)
{
    int32_t esp;

    esp = state->reg_val[REG_ESP];
    esp -= size;
    state->reg_val[REG_ESP] = esp;
    cli_dbgmsg("push %x -> %08x\n", value, esp);
    switch (size) {
	case 2:
	    return cli_emu_vmm_write16(state->mem, esp, value);
	case 4:
	    return cli_emu_vmm_write32(state->mem, esp, value);
	default:
	    return -1;
    }
}

cli_emu_t* cli_emulator_new(emu_vmm_t *v, struct cli_pe_hook_data *pedata)
{
    uint32_t stack, stackend, stacksize;
    cli_emu_t *emu = cli_malloc(sizeof(*emu));
    if (!emu)
	return NULL;
    emu->mem = v;
    emu->eip = cli_emu_vmm_rva2va(v, pedata->opt32.AddressOfEntryPoint);
    memset(emu->cached_disasm, 0, sizeof(emu->cached_disasm));

    stacksize = pedata->opt32.SizeOfStackReserve;
    cli_emu_vmm_alloc(v, stacksize, &stack);
    stackend = (stack + stacksize + 4095) &~ 4095;/* align */
    cli_dbgmsg("Mapped stack: %08x - %08x\n", stack, stackend);
    emu->reg_val[REG_ESP] = stackend;

    mem_push(emu, 4, MAPPING_END);
    /* TODO: init registers */
    return emu;
}

void cli_emulator_free(cli_emu_t *emu)
{
    free(emu);
}

#define UNIMPLEMENTED_REG do { cli_dbgmsg("Unimplemented register access\n"); return -1; } while(0)
#define INVALID_SIZE do { printf("Invalid access size\n"); return -1; } while(0)
static always_inline int get_reg(desc_t *desc, enum X86REGS reg)
{
    if (reg >= MAXREG) {
	if (reg != REG_INVALID)
	    UNIMPLEMENTED_REG;
	desc->idx = REGIDX_INVALID;
	return 0;
    }
    desc->mask = reg_masks[reg].rw_mask;
    desc->shift = reg_masks[reg].rw_shift;
    desc->idx = reg - reg_masks[reg].sub;
    desc->carry_bit = reg_masks[reg].carry_bit;
    desc->sign_bit = reg_masks[reg].carry_bit;
    return 0;
}

/** Disassembles one X86 instruction starting at the specified offset.
  \group_disasm
 * @param[out] result disassembly result
 * @param[in] offset start disassembling from this offset, in the current file
 * @param[in] len max amount of bytes to disassemble
 * @return offset where disassembly ended*/
static uint32_t
DisassembleAt(emu_vmm_t *v, struct dis_instr* result, uint32_t offset)
{
    struct DISASM_RESULT res;
    unsigned i;
    uint8_t dis[32];
    const uint8_t *next;
    uint8_t operation_size, address_size;
    uint8_t segment;
    enum X86OPS x86_opcode;
    int rc;

    rc = cli_emu_vmm_read_x(v, offset, dis, sizeof(dis));
    if (rc < 0)
	return rc;

    next = cli_disasm_one(dis, sizeof(dis), &res, 1);
    result->operation_size = res.opsize;
    result->address_size = res.adsize;
    result->segment = res.segment;
    result->opcode = (enum X86OPS) cli_readint16(&res.real_op);
    for (i=0;i<3;i++) {
	enum DIS_SIZE size = (enum DIS_SIZE) res.arg[i][1];/* not valid for REG */
	struct dis_arg *arg = &result->arg[i];
	arg->access_size = SIZE_INVALID;
	switch ((enum DIS_ACCESS)res.arg[i][0]) {
	    case ACCESS_MEM:
		get_reg(&arg->scale_reg, (enum X86REGS)res.arg[i][2]);
		get_reg(&arg->add_reg, (enum X86REGS)res.arg[i][3]);
		arg->scale = res.arg[i][4];
		if (arg->scale == 1 && res.arg[i][3] == REG_INVALID) {
		    memcpy(&arg->add_reg, &arg->scale_reg, sizeof(arg->scale_reg));
		    arg->scale_reg.idx = REGIDX_INVALID;
		}
		if (arg->scale == 0)
		    arg->scale_reg.idx = REGIDX_INVALID;
		arg->displacement = cli_readint32((const uint32_t*)&res.arg[i][6]);
		arg->access_size = size; /* not valid for REG */
		break;
	    case ACCESS_REG:
		get_reg(&arg->add_reg, (enum X86REGS)res.arg[i][1]);
		arg->scale_reg.idx = REGIDX_INVALID;
		arg->displacement = 0;
		arg->access_size = SIZE_INVALID;
		break;
	    case ACCESS_REL:
		arg->access_size = SIZE_REL;
		/* fall-through */
	    default: {
		uint32_t c = cli_readint32((const uint32_t*)&res.arg[i][6]);
		if (c && c != 0xffffffff)
		    cli_dbgmsg("truncating 64-bit immediate\n");
		arg->scale_reg.idx = REGIDX_INVALID;
		arg->add_reg.idx = REGIDX_INVALID;
		arg->scale = 0;
		switch (size) {
		    case SIZEB:
			arg->displacement = *(const int8_t*)&res.arg[i][2];
			break;
		    case SIZEW:
			arg->displacement = cli_readint16((const int16_t*)&res.arg[i][2]);
			break;
		    case SIZED:
		    default:
			arg->displacement = cli_readint32((const int32_t*)&res.arg[i][2]);
			break;
		}
		break;
	    }
	}
    }
    return offset + next - dis;
}

static always_inline uint32_t hash32shift(uint32_t key)
{
  key = ~key + (key << 15);
  key = key ^ (key >> 12);
  key = key + (key << 2);
  key = key ^ (key >> 4);
  key = (key + (key << 3)) + (key << 11);
  key = key ^ (key >> 16);
  return key;
}

static always_inline struct dis_instr* disasm(cli_emu_t *emu)
{
    int ret;
    struct dis_instr *instr;
    uint32_t idx = hash32shift(emu->eip) & (DISASM_CACHE_SIZE-1);
    instr = &emu->cached_disasm[idx];
//    if (instr->opcode == OP_INVALID) {
    if ((ret = DisassembleAt(emu->mem, instr, emu->eip)) < 0)
	return NULL;
	instr->len = ret - emu->eip;
	/* TODO discard cache when writing to this page! */
//    }
    return instr;
}

static always_inline uint32_t readreg(const cli_emu_t *emu,
				       const desc_t *reg)
{
    /* TODO: we could read directly at the right offset instead of shifting */
    return reg->idx != REGIDX_INVALID ? (emu->reg_val[reg->idx] & reg->mask) >> reg->shift : 0;
    /* TODO: make invalid reg a real offset that just returns 0 */
}

static always_inline int read_reg(const cli_emu_t *emu, enum X86REGS reg, uint32_t *value)
{
    desc_t desc;
    get_reg(&desc, reg);
    if (desc.idx == REGIDX_INVALID)
	return -1;
    *value = readreg(emu, &desc);
    return 0;
}

static always_inline int writereg(cli_emu_t *emu, const desc_t *reg, uint32_t value)
{
    if (reg->idx == REGIDX_INVALID)
	return -1;
    emu->reg_val[reg->idx] = (emu->reg_val[reg->idx] & (~reg->mask)) |
	((value << reg->shift) & reg->mask);
    return 0;
}

static always_inline int write_reg(cli_emu_t *emu, enum X86REGS reg, uint32_t value)
{
    desc_t desc;
    get_reg(&desc, reg);
    writereg(emu, &desc, value);
    return 0;
}

static always_inline uint32_t calcreg(const cli_emu_t *emu, const struct dis_arg *arg)
{
    uint32_t value = arg->displacement + readreg(emu, &arg->add_reg);
    if (arg->scale_reg.idx != REGIDX_INVALID)
	value += arg->scale * readreg(emu, &arg->scale_reg);
    return value;
}

static always_inline int mem_read(const cli_emu_t *emu, uint32_t addr, enum DIS_SIZE size, uint32_t *value)
{
    switch (size) {
	case SIZE_INVALID:
	    return 0;
	case SIZEB:
	    return cli_emu_vmm_read8(emu->mem, addr, value);
	case SIZEW:
	    return cli_emu_vmm_read16(emu->mem, addr, value);
	case SIZED:
	default:
	    return cli_emu_vmm_read32(emu->mem, addr, value);
    }
}

static always_inline int read_operand(const cli_emu_t *emu,
				      const struct dis_arg *arg, uint32_t *value)
{
    *value = calcreg(emu, arg);
    return mem_read(emu, *value, arg->access_size, value);
}

static always_inline int mem_write(cli_emu_t *emu, uint32_t addr, enum DIS_SIZE size, uint32_t value)
{
    switch (size) {
	case SIZEB:
	    return cli_emu_vmm_write8(emu->mem, addr, value);
	case SIZEW:
	    return cli_emu_vmm_write16(emu->mem, addr, value);
	case SIZED:
	default:
	    return cli_emu_vmm_write32(emu->mem, addr, value);
    }
}

static always_inline int write_operand(cli_emu_t *emu,
				       const struct dis_arg *arg, uint32_t value)
{
    if (arg->access_size == SIZE_INVALID) {
	return writereg(emu, &arg->add_reg, value);
    } else {
	/* TODO: check for FS segment */
	uint32_t addr = calcreg(emu, arg);
	return mem_write(emu, addr, arg->access_size, value);
    }
}

#define READ_OPERAND(value, op) do { \
    if (read_operand(state, &instr->arg[(op)], &(value)) < 0) {\
	fprintf(stderr, "operand read failed\n");\
	return -1;\
    }\
} while (0)

#define WRITE_RESULT(op, value) do {\
    if (write_operand(state, &instr->arg[(op)], (value)) < 0) {\
	fprintf(stderr, "operand write failed\n");\
	return -1;\
    }\
} while (0)

#define NOSTACK do { printf("Stack overflowed\n"); return -1;} while(0)

static int emu_mov(cli_emu_t *state, instr_t *instr)
{
    //TODO: FS segment support, the rest of segments are equal anyway on win32
    int32_t reg;
    READ_OPERAND(reg, 1);
    WRITE_RESULT(0, reg);
    return 0;
}

#define MEM_PUSH(val) do { if (mem_push(state, instr->operation_size ? 2 : 4, (val)) < 0) {\
    fprintf(stderr,"push failed\n");return -1;}} while(0)

#define MEM_POP(val) do { if (mem_pop(state, instr->operation_size ? 2: 4, (val)) < 0) {\
    fprintf(stderr,"pop failed\n");return -1;}} while(0)

static int emu_push(cli_emu_t *state, instr_t *instr)
{
    int32_t value;

    READ_OPERAND(value, 0);
    MEM_PUSH(value);
    return 0;
}

int mem_pop(cli_emu_t *state, int size, int32_t *value)
{
    uint32_t esp;

    esp = state->reg_val[REG_ESP];
    switch (size) {
	case 2:
	    if (cli_emu_vmm_read16(state->mem, esp, value) < 0)
		return -1;
	    break;
	case 4:
	    if (cli_emu_vmm_read32(state->mem, esp, value) < 0)
		return -1;
	    break;
    }

    esp += size;
    state->reg_val[REG_ESP] = esp;
    return 0;
}

static int emu_pop(cli_emu_t *state, struct dis_instr *instr)
{
    int32_t value;
    MEM_POP(&value);
    WRITE_RESULT(0, value);
    return 0;
}

static int emu_cld(cli_emu_t *state, struct dis_instr *instr)
{
    state->eflags &= ~(1 << bit_df);
    state->eflags_def |= 1 << bit_df;
    return 0;
}

static int emu_std(cli_emu_t *state, instr_t *instr)
{
    state->eflags |= 1 << bit_df;
    state->eflags_def |= 1 << bit_df;
    return 0;
}

static int emu_inc(cli_emu_t *state, instr_t *instr)
{
    int32_t reg;
    READ_OPERAND(reg, 0);
    WRITE_RESULT(0, ++reg);
    /* FIXME: inc byte ptr [addr] */
    calc_flags_inc(state, reg, &instr->arg[0].add_reg);
    return 0;
}

static int emu_dec(cli_emu_t *state, instr_t *instr)
{
    int32_t reg;
    READ_OPERAND(reg, 0);
    WRITE_RESULT(0, --reg);
    /* FIXME: inc byte ptr [addr] */
    calc_flags_dec(state, reg, &instr->arg[0].add_reg);
    return 0;
}
// returns 1 if loop should not be entered
static int emu_prefix_pre(cli_emu_t *state, int8_t ad16, int8_t repe_is_rep)
{
    if (state->prefix_repe || state->prefix_repne) {
	int32_t cnt;
	read_reg(state, ad16 ? REG_CX : REG_ECX, &cnt);
	if (!cnt)
	    return 1;
    }
    return 0;
}

static int emu_prefix_post(cli_emu_t *state, int8_t ad16, int8_t repe_is_rep)
{
    if (state->prefix_repe || state->prefix_repne) {
	int32_t cnt;
	read_reg(state, ad16 ? REG_CX : REG_ECX, &cnt);
	cnt--;
	write_reg(state, ad16 ? REG_CX : REG_ECX, cnt);
	if (!cnt)
	    return 0;
	if (state->prefix_repe && !repe_is_rep &&
	    !(state->eflags & (1 << bit_zf)))
	    return 0;
	if (state->prefix_repne &&
	    (state->eflags & (1 << bit_zf)))
	    return 0;
	return 1;
    }
    return 0;
}

static int emu_lodsx(cli_emu_t *state, instr_t *instr, enum DIS_SIZE size, enum X86REGS reg, uint32_t add)
{
    int32_t esi;
    uint32_t val;

    if (emu_prefix_pre(state, instr->address_size, 1))
	return 0;
    //TODO:address size
    do {
	esi = state->reg_val[REG_ESI];
	if (read_reg(state, REG_ESI, &esi) == -1 ||
	    mem_read(state, esi, size, &val) == -1 ||
	    write_reg(state, reg, val) == -1)
	    return -1;
	if (state->eflags & (1 << bit_df)) {
	    esi -= add;
	} else {
	    esi += add;
	}
	if (write_reg(state, REG_ESI, esi) == -1)
	    return -1;
    } while (emu_prefix_post(state, instr->address_size, 1));
    return 0;
}

static int emu_stosx(cli_emu_t *state, instr_t *instr, enum DIS_SIZE size, enum X86REGS reg, uint32_t add)
{
    int32_t edi;
    uint32_t val;

    if (emu_prefix_pre(state, instr->address_size, 1))
	return 0;
    //TODO:address size
    do {
	if (read_reg(state, REG_EDI, &edi) == -1 ||
	    read_reg(state, reg, &val) == -1 ||
	    mem_write(state, edi, size, val) == -1)
	    return -1;
	if (state->eflags & (1 << bit_df)) {
	    edi -= add;
	} else {
	    edi += add;
	}
	if (write_reg(state, REG_EDI, edi) == -1)
	    return -1;
    } while (emu_prefix_post(state, instr->address_size, 1));
    return 0;
}

static int emu_xor(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    reg1 ^= reg2;
    /* TODO: only calculate flags on demand */
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_test(state, reg1, &instr->arg[0].add_reg);
    else
	calc_flags_test(state, reg1, &mem_desc[instr->arg[0].access_size]);
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_shl(cli_emu_t *state, instr_t *instr)
{
    uint8_t largeshift;
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);

    const desc_t *desc =
	(instr->arg[0].access_size == SIZE_INVALID) ?
	&instr->arg[0].add_reg :
	&mem_desc[instr->arg[0].access_size];
    largeshift = reg2 >= desc->carry_bit;
    reg2 &= 0x1f;

    if (!reg2)
	return 0;
    uint64_t result = (uint64_t)reg1 << (uint8_t)reg2;
    uint8_t cf = (result >> desc->carry_bit) & 1;
    reg1 = result;
    if (reg2 == 1) {
	uint8_t of = ((result >> desc->sign_bit) & 1) ^ cf;
	state->eflags = (state->eflags & ~((1<< bit_cf) | (1 << bit_of))) |
			 (cf << bit_cf) |
			 (of << bit_of);
	state->eflags_def |= (1<<bit_cf) | (1<<bit_of);
    } else {
	state->eflags = (state->eflags & ~(1<< bit_cf)) |
			 (cf << bit_cf);
	state->eflags_def |= (1<<bit_cf);
	//OF undefined for shift > 1
	state->eflags_def &= ~(1<<bit_of);
    }
    if (largeshift)
	state->eflags_def &= ~(1<<bit_cf);
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_shr(cli_emu_t *state, instr_t *instr)
{
    uint8_t largeshift;
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);

    const desc_t *desc =
	(instr->arg[0].access_size == SIZE_INVALID) ?
	&instr->arg[0].add_reg :
	&mem_desc[instr->arg[0].access_size];
    largeshift = reg2 >= desc->carry_bit;
    reg2 &= 0x1f;

    if (!reg2)
	return 0;
    uint32_t result = reg1;
    result >>= (uint8_t)(reg2 - 1);
    uint8_t cf = (result & 1);
    reg1 = result >> 1;
    if (reg2 == 1) {
	uint8_t of = ((result >> desc->sign_bit) & 1);
	state->eflags = (state->eflags & ~((1<< bit_cf) | (1 << bit_of))) |
			 (cf << bit_cf) |
			 (of << bit_of);
	state->eflags_def |= (1<<bit_cf) | (1<<bit_of);
    } else {
	state->eflags = (state->eflags & ~(1<< bit_cf)) |
			 (cf << bit_cf);
	state->eflags_def |= (1<<bit_cf);
	//OF undefined for shift > 1
	state->eflags_def &= ~(1<<bit_of);
    }
    if (largeshift)
	state->eflags_def &= ~(1<<bit_cf);
    WRITE_RESULT(0, reg1);
    return 0;
}

#define ROL(a,b,n) a = ( a << (b) ) | ( a >> (((n) - (b))) )
#define ROR(a,b,n) a = ( a >> (b) ) | ( a << (((n) - (b))) )

static int emu_rol(cli_emu_t *state, instr_t *instr)
{
    uint8_t largeshift;
    uint32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);

    const desc_t *desc =
	(instr->arg[0].access_size == SIZE_INVALID) ?
	&instr->arg[0].add_reg :
	&mem_desc[instr->arg[0].access_size];
    largeshift = reg2 >= desc->carry_bit;

    /* See Intel manual 4-312 Vol. 2B */
    if (reg2 == 1)
	state->eflags_def |= 1 << bit_of;//OF defined
    else
	state->eflags_def &= ~(1 << bit_of);//OF undef

    reg2 &= 0x1f;
    uint8_t msb;
    uint8_t cf;
    switch (desc->carry_bit) {
	case 8:
	    reg2 %= 8;
	    if (!reg2)
		return 0;
	    ROL(reg1, reg2, 8);
	    cf = reg1 & 1;
	    msb = (reg1 >> 7)&1;
	    break;
	case 16:
	    reg2 %= 16;
	    if (!reg2)
		return 0;
	    ROL(reg1, reg2, 16);
	    cf = reg1 & 1;
	    msb = (reg1 >> 15)&1;
	case 32:
	    if (!reg2)
		return 0;
	    ROL(reg1, reg2, 32);
	    cf = reg1 & 1;
	    msb = (reg1 >> 31)&1;
	    break;
	default:
	    INVALID_SIZE;
    }

    uint8_t of = msb ^ cf;
    state->eflags = (state->eflags & ~((1<< bit_cf) | (1 << bit_of))) |
	(cf << bit_cf) |
	(of << bit_of);

    state->eflags_def |= (1 << bit_cf);
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_ror(cli_emu_t *state, instr_t *instr)
{
    uint8_t largeshift;
    uint32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);

    const desc_t *desc =
	(instr->arg[0].access_size == SIZE_INVALID) ?
	&instr->arg[0].add_reg :
	&mem_desc[instr->arg[0].access_size];
    largeshift = reg2 >= desc->carry_bit;

    /* See Intel manual 4-312 Vol. 2B */
    if (reg2 == 1)
	state->eflags_def |= 1 << bit_of;//OF defined
    else
	state->eflags_def &= ~(1 << bit_of);//OF undef

    reg2 &= 0x1f;
    uint8_t msb, of;
    switch (desc->carry_bit) {
	case 8:
	    reg2 %= 8;
	    if (!reg2)
		return 0;
	    ROR(reg1, reg2, 8);
	    msb = (reg1 >> 7)&1;
	    of = msb ^ (reg1 >> 6)&1;
	    break;
	case 16:
	    reg2 %= 16;
	    if (!reg2)
		return 0;
	    ROR(reg1, reg2, 16);
	    msb = (reg1 >> 15)&1;
	    of = msb ^ (reg1 >> 14)&1;
	case 32:
	    if (!reg2)
		return 0;
	    ROR(reg1, reg2, 32);
	    msb = (reg1 >> 31)&1;
	    of = msb ^ (reg1 >> 30)&1;
	    break;
	default:
	    INVALID_SIZE;
    }

    uint8_t cf = msb;
    state->eflags = (state->eflags & ~((1<< bit_cf) | (1 << bit_of))) |
	(cf << bit_cf) |
	(of << bit_of);

    state->eflags_def |= (1 << bit_cf);
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_and(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    reg1 &= reg2;
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_test(state, reg1, &instr->arg[0].add_reg);
    else
	calc_flags_test(state, reg1, &mem_desc[instr->arg[0].access_size]);
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_or(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    reg1 |= reg2;
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_test(state, reg1, &instr->arg[0].add_reg);
    else
	calc_flags_test(state, reg1, &mem_desc[instr->arg[0].access_size]);
    WRITE_RESULT(0, reg1);
    return 0;
}
static int emu_sub(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_addsub(state, reg1, reg2, &instr->arg[0].add_reg, 1);
    else
	calc_flags_addsub(state, reg1, reg2, &mem_desc[instr->arg[0].access_size], 1);
    reg1 -= reg2;
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_cmp(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_addsub(state, reg1, reg2, &instr->arg[0].add_reg, 1);
    else
	calc_flags_addsub(state, reg1, reg2, &mem_desc[instr->arg[0].access_size], 1);
    return 0;
}

static uint8_t always_inline emu_flags(const cli_emu_t *state, uint8_t bit)
{
    return (state->eflags >> bit) & 1;
}
static int emu_adc(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    reg1 += emu_flags(state, bit_cf);
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_addsub(state, reg1, reg2, &instr->arg[0].add_reg, 0);
    else
	calc_flags_addsub(state, reg1, reg2, &mem_desc[instr->arg[0].access_size], 0);
    reg1 += reg2;
    WRITE_RESULT(0, reg1);
    return 0;
}

static int emu_add(cli_emu_t *state, instr_t *instr)
{
    int32_t reg1, reg2;
    READ_OPERAND(reg1, 0);
    READ_OPERAND(reg2, 1);
    if (instr->arg[0].access_size == SIZE_INVALID)
	calc_flags_addsub(state, reg1, reg2, &instr->arg[0].add_reg, 0);
    else
	calc_flags_addsub(state, reg1, reg2, &mem_desc[instr->arg[0].access_size], 0);
    reg1 += reg2;
    WRITE_RESULT(0, reg1);
    return 0;
}


static int emu_loop(cli_emu_t *state, instr_t *instr)
{
    uint32_t cnt;
    if (read_reg(state, instr->address_size ? REG_CX : REG_ECX, (int32_t*)&cnt) == -1)
	return -1;
    if (--cnt) {
	/* branch cond = 1 */
	if (!instr->operation_size) {
	    int8_t rel = instr->arg[0].displacement;
	    state->eip += rel;
	} else {
	    /* Intel Manual 3-598 Vol. 2A */
	    /* TODO: is this right, rel8 not taken into account? */
	    state->eip &= 0xffff;
	}
    }
    if (write_reg(state, instr->address_size ? REG_CX : REG_ECX, cnt) == -1)
	return -1;
    return 0;
}

static int emu_jmp(cli_emu_t *state, instr_t *instr)
{
    struct dis_arg *arg = &instr->arg[0];
    if (arg->access_size == SIZE_REL) {
	state->eip += arg->displacement;
    } else {
	int32_t value;
	READ_OPERAND(value, 0);
	state->eip = value;
    }
    if (instr->operation_size)
	state->eip &= 0xffff;
    return 0;
}

static int emu_call(cli_emu_t *state, instr_t *instr)
{
    uint32_t esp, size;
    struct dis_arg *arg = &instr->arg[0];

    MEM_PUSH(state->eip);

    if (arg->access_size == SIZE_REL) {
	state->eip += arg->displacement;
    } else {
	int32_t value;
	READ_OPERAND(value, 0);
	state->eip = value;
    }
    if (instr->operation_size)
	state->eip &= 0xffff;
    return 0;
}

static int emu_ret(cli_emu_t *state, instr_t *instr)
{
    uint32_t esp, size;
    struct dis_arg *arg = &instr->arg[0];

    MEM_POP(&state->eip);
    esp = state->reg_val[REG_ESP];

    if (arg->displacement) {
	if (instr->address_size) {
	    uint16_t sp = esp;
	    sp += arg->displacement;
	    esp = (esp & 0xffff00000) | sp;
	}
	else
	    esp += arg->displacement;
    }
    state->reg_val[REG_ESP] = esp;
    return 0;
}

static int emu_movsx(cli_emu_t *state, instr_t *instr, enum DIS_SIZE size, uint32_t add)
{
    int32_t esi, edi;
    int32_t val;

    if (emu_prefix_pre(state, instr->address_size, 1))
	return 0;
    //TODO:address size
    do {
	if (read_reg(state, REG_ESI, &esi) == -1 ||
	    read_reg(state, REG_EDI, &edi) == -1 ||
	    mem_read(state, esi, size, &val) == -1 ||
	    mem_write(state, edi, size, val) == -1)
	    return -1;
	if (state->eflags & (1 << bit_df)) {
	    edi -= add;
	    esi -= add;
	} else {
	    edi += add;
	    esi += add;
	}
	if (write_reg(state, REG_ESI, esi) == -1 ||
	    write_reg(state, REG_EDI, edi) == -1)
	    return -1;
    } while (emu_prefix_post(state, instr->address_size, 1));
    return 0;
}

static int emu_pusha(cli_emu_t *state, instr_t *instr)
{
    uint32_t esp = state->reg_val[REG_ESP];
    if (instr->operation_size) {
	uint16_t data[8];
	/* 16 */
	esp -= 16;
	data[0] = le16_to_host(state->reg_val[REG_EDI]&0xffff);
	data[1] = le16_to_host(state->reg_val[REG_ESI]&0xffff);
	data[2] = le16_to_host(state->reg_val[REG_EBP]&0xffff);
	data[3] = le16_to_host(state->reg_val[REG_ESP]&0xffff);
	data[4] = le16_to_host(state->reg_val[REG_EBX]&0xffff);
	data[5] = le16_to_host(state->reg_val[REG_EDX]&0xffff);
	data[6] = le16_to_host(state->reg_val[REG_ECX]&0xffff);
	data[7] = le16_to_host(state->reg_val[REG_EAX]&0xffff);
	if (cli_emu_vmm_write(state->mem, esp, data, 16) < 0)
	    return -1;
    } else {
	uint32_t data[8];
	/* 32 */
	esp -= 32;
	cli_writeint32(&data[0], state->reg_val[REG_EDI]);
	cli_writeint32(&data[1], state->reg_val[REG_ESI]);
	cli_writeint32(&data[2], state->reg_val[REG_EBP]);
	cli_writeint32(&data[3], state->reg_val[REG_ESP]);
	cli_writeint32(&data[4], state->reg_val[REG_EBX]);
	cli_writeint32(&data[5], state->reg_val[REG_EDX]);
	cli_writeint32(&data[6], state->reg_val[REG_ECX]);
	cli_writeint32(&data[7], state->reg_val[REG_EAX]);
	if (cli_emu_vmm_write(state->mem, esp, data, 32) < 0)
	    return -1;
    }
    state->reg_val[REG_ESP] = esp;
    return 0;
}

static void write16reg(cli_emu_t *state, enum X86REGS reg, uint16_t val)
{
    state->reg_val[reg] &= 0xff00;
    state->reg_val[reg] |= val;
}

static int emu_popa(cli_emu_t *state, instr_t *instr)
{
    uint32_t esp = state->reg_val[REG_ESP];
    if (instr->operation_size) {
	uint16_t data[8];
	/* 16 */
	if (cli_emu_vmm_read_r(state->mem, esp, data, 16) < 0)
	    return -1;
	write16reg(state, REG_EDI, le16_to_host(data[0]));
	write16reg(state, REG_EDX, le16_to_host(data[1]));
	write16reg(state, REG_ESI, le16_to_host(data[2]));
	write16reg(state, REG_EBP, le16_to_host(data[3]));
	write16reg(state, REG_EBX, le16_to_host(data[4]));
	write16reg(state, REG_EDX, le16_to_host(data[5]));
	write16reg(state, REG_ECX, le16_to_host(data[6]));
	write16reg(state, REG_EAX, le16_to_host(data[7]));
	esp += 16;
    } else {
	uint32_t data[8];
	/* 32 */
	if (cli_emu_vmm_read_r(state->mem, esp, data, 32) < 0)
	    return -1;
	cli_writeint32(&state->reg_val[REG_EDI], data[0]);
	cli_writeint32(&state->reg_val[REG_EDX], data[1]);
	cli_writeint32(&state->reg_val[REG_ESI], data[2]);
	cli_writeint32(&state->reg_val[REG_EBP], data[3]);
	cli_writeint32(&state->reg_val[REG_EBX], data[4]);
	cli_writeint32(&state->reg_val[REG_EDX], data[5]);
	cli_writeint32(&state->reg_val[REG_ECX], data[6]);
	cli_writeint32(&state->reg_val[REG_EAX], data[7]);
	esp += 32;
    }
    state->reg_val[REG_ESP] = esp;
    return 0;
}

static int emu_scasx(cli_emu_t *state, instr_t *instr,
		     enum X86REGS reg, enum DIS_SIZE size, int8_t add)
{
    int32_t edi;
    uint32_t src;
    int32_t a;

    if (read_reg(state, instr->address_size ? REG_DI : REG_EDI, &edi) == -1 ||
	read_reg(state, reg, &a) == -1)
	return -1;
    if (emu_prefix_pre(state, instr->address_size, 0))
	return 0;
    do {
	desc_t reg_desc;
	mem_read(state, edi, size, &src);
	get_reg(&reg_desc, reg);
	calc_flags_addsub(state, a, src, &reg_desc, 1);
	if (state->eflags & (1 << bit_df)) {
	    edi -= add;
	} else {
	    edi += add;
	}
	if (instr->address_size)
	    edi &= 0xffff;
    } while (emu_prefix_post(state, instr->address_size, 0));
    write_reg(state, instr->address_size ? REG_DI : REG_EDI, edi);
    return 0;
}

static int emu_stc(cli_emu_t *state, instr_t *instr)
{
    state->eflags |= 1 << bit_cf;
    state->eflags_def |= 1 << bit_cf;
    return 0;
}

static int emu_clc(cli_emu_t *state, instr_t *instr)
{
    state->eflags &= ~(1 << bit_cf);
    state->eflags_def |= 1 << bit_cf;
    return 0;
}
static int emu_xchg(cli_emu_t *state, instr_t *instr)
{
    //TODO: FS segment support, the rest of segments are equal anyway on win32
    int32_t reg0, reg1;
    READ_OPERAND(reg0, 0);
    READ_OPERAND(reg1, 1);
    WRITE_RESULT(0, reg1);
    WRITE_RESULT(1, reg0);
    return 0;
}
static int emu_lea(cli_emu_t *state, instr_t *instr)
{
    const struct dis_arg *arg = &instr->arg[1];
    uint32_t addr = calcreg(state, arg);
    WRITE_RESULT(0, addr);
    return 0;
}

int cli_emulator_step(cli_emu_t *emu)
{
    int rc;
    struct dis_instr *instr;
    struct import_description *import;

    if (emu->eip == MAPPING_END) {
	cli_dbgmsg("emulated program exited\n");
	return -2;
    }
    import = cli_emu_vmm_get_import(emu->mem, emu->eip);
    if (import) {
	if (import->handler(emu, import->description, import->bytes) < 0)
	    return -1;
	return 0;
    }

    instr = disasm(emu);
    if (!instr) {
	printf("can't disasm\n");
	return -1;
    }
    emu->eip += instr->len;
    switch (instr->opcode) {
	case OP_MOV:
	    rc = emu_mov(emu, instr);
	    break;
	case OP_PUSH:
	    rc = emu_push(emu, instr);
	    break;
	case OP_POP:
	    rc = emu_pop(emu, instr);
	    break;
	case OP_INC:
	    rc = emu_inc(emu, instr);
	    break;
	case OP_DEC:
	    rc = emu_dec(emu, instr);
	    break;
	case OP_CLD:
	    rc = emu_cld(emu, instr);
	    break;
	case OP_STD:
	    rc = emu_std(emu, instr);
	    break;
	case OP_LODSB:
	    rc = emu_lodsx(emu, instr, SIZEB, REG_AL, 1);
	    break;
	case OP_LODSW:
	    rc = emu_lodsx(emu, instr, SIZEW, REG_AX, 2);
	    break;
	case OP_LODSD:
	    rc = emu_lodsx(emu, instr, SIZED, REG_EAX, 4);
	    break;
	case OP_STOSB:
	    rc = emu_stosx(emu, instr, SIZEB, REG_AL, 1);
	    break;
	case OP_STOSW:
	    rc = emu_stosx(emu, instr, SIZEW, REG_AX, 2);
	    break;
	case OP_STOSD:
	    rc = emu_stosx(emu, instr, SIZED, REG_EAX, 4);
	    break;
	case OP_MOVSB:
	    rc = emu_movsx(emu, instr, SIZEB, 1);
	    break;
	case OP_MOVSW:
	    rc = emu_movsx(emu, instr, SIZEW, 2);
	    break;
	case OP_MOVSD:
	    rc = emu_movsx(emu, instr, SIZED, 4);
	    break;
	case OP_XOR:
	    rc = emu_xor(emu, instr);
	    break;
	case OP_AND:
	    rc = emu_and(emu, instr);
	    break;
	case OP_OR:
	    rc = emu_or(emu, instr);
	    break;
	case OP_SUB:
	    rc = emu_sub(emu, instr);
	    break;
	case OP_ADC:
	    rc = emu_adc(emu, instr);
	    break;
	case OP_ADD:
	    rc = emu_add(emu, instr);
	    break;
	case OP_SHL:
	    rc = emu_shl(emu, instr);
	    break;
	case OP_SHR:
	    rc = emu_shr(emu, instr);
	    break;
	case OP_ROL:
	    rc = emu_rol(emu, instr);
	    break;
	case OP_ROR:
	    rc = emu_ror(emu, instr);
	    break;
	case OP_LOOP:
	    rc = emu_loop(emu, instr);
	    break;
	case OP_CMP:
	    rc = emu_cmp(emu, instr);
	    break;
	case OP_JMP:
	    rc = emu_jmp(emu, instr);
	    break;
	case OP_JO:
	    rc = (emu_flags(emu, bit_of) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JNO:
	    rc = (emu_flags(emu, bit_of) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JC:
	    rc = (emu_flags(emu, bit_cf) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JNC:
	    rc = (emu_flags(emu, bit_cf) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JZ:
	    rc = (emu_flags(emu, bit_zf) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JNZ:
	    rc = (emu_flags(emu, bit_zf) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JBE:
	    rc = (emu_flags(emu, bit_cf) == 1 ||
		    emu_flags(emu, bit_zf) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JA:
	    rc = (emu_flags(emu, bit_cf) == 0 &&
		    emu_flags(emu, bit_zf) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JS:
	    rc = (emu_flags(emu, bit_sf) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JNS:
	    rc = (emu_flags(emu, bit_sf) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JP:
	    rc = (emu_flags(emu, bit_pf) == 1) && emu_jmp(emu, instr);
	    break;
	case OP_JNP:
	    rc = (emu_flags(emu, bit_pf) == 0) && emu_jmp(emu, instr);
	    break;
	case OP_JL:
	    rc = (emu_flags(emu, bit_sf) != emu_flags(emu, bit_of)) && emu_jmp(emu, instr);
	    break;
	case OP_JGE:
	    rc = (emu_flags(emu, bit_sf) == emu_flags(emu, bit_of)) && emu_jmp(emu, instr);
	    break;
	case OP_JLE:
	    rc = (emu_flags(emu, bit_zf) == 1 ||
		    emu_flags(emu, bit_sf) != emu_flags(emu, bit_of)) && emu_jmp(emu, instr);
	    break;
	case OP_JG:
	    rc = (emu_flags(emu, bit_zf) == 0 &&
		    emu_flags(emu, bit_sf) == emu_flags(emu, bit_of)) && emu_jmp(emu, instr);
	    break;
	case OP_CALL:
	    rc = emu_call(emu, instr);
	    break;
	case OP_RETN:
	    rc = emu_ret(emu, instr);
	    break;
	case OP_PUSHAD:
	    rc = emu_pusha(emu, instr);
	    break;
	case OP_POPAD:
	    rc = emu_popa(emu, instr);
	    break;
	case OP_LEA:
	    rc = emu_lea(emu, instr);
	    break;
	case OP_XCHG:
	    rc = emu_xchg(emu, instr);
	    break;
	case OP_SCASB:
	    rc = emu_scasx(emu, instr, REG_AL, SIZEB, 1);
	    break;
	case OP_SCASW:
	    rc = emu_scasx(emu, instr, REG_AX, SIZEW, 2);
	    break;
	case OP_SCASD:
	    rc = emu_scasx(emu, instr, REG_EAX, SIZED, 4);
	    break;
	case OP_CLC:
	    rc = emu_clc(emu, instr);
	    break;
	case OP_STC:
	    rc = emu_stc(emu, instr);
	    break;
	case OP_NOP:
	    /* NOP is nop */
	    break;
	case OP_PREFIX_REPE:
	    emu->prefix_repe = 1;
	    /* TODO: check if prefix is valid in next instr */
	    /* TODO: only take into account last rep prefix, so just use one var
	     * here */
	    return 0;
	case OP_PREFIX_REPNE:
	    emu->prefix_repne = 1;
	    return 0;
	case OP_PREFIX_LOCK:
	    return 0;
	default:
	    cli_dbgmsg("opcode not yet implemented\n");
	    return -1;
    }
    emu->prefix_repe = 0;
    emu->prefix_repne = 0;
    return 0;
}

void cli_emulator_dbgstate(cli_emu_t *emu)
{
    printf("[cliemu               ] eip=0x%08x\n"
	   "[cliemu               ] eax=0x%08x  ecx=0x%08x  edx=0x%08x  ebx=0x%08x\n"
	   "[cliemu               ] esp=0x%08x  ebp=0x%08x  esi=0x%08x  edi=0x%08x\n"
	   "[cliemu               ] eflags=0x%08x\n",
	   emu->eip,
	   emu->reg_val[REG_EAX], emu->reg_val[REG_ECX], emu->reg_val[REG_EDX], emu->reg_val[REG_EBX],
	   emu->reg_val[REG_ESP], emu->reg_val[REG_EBP], emu->reg_val[REG_ESI], emu->reg_val[REG_EDI],
	   emu->eflags);
}

int hook_generic_stdcall(struct cli_emu *emu, const char *desc, unsigned bytes)
{
    if (bytes != 254) {
	uint32_t esp;
	printf("Called stdcall API %s@%d\n", desc ? desc : "??", bytes);
	if (mem_pop(emu, 4, &emu->eip) < 0)
	    return -1;
	emu->reg_val[REG_ESP] += bytes;
	emu->reg_val[REG_EAX] = 0;
	return 0;
    } else {
	/* 254 - magic for varargs */
	printf("Called varargs API %s\n", desc ? desc : "??");
	return -1;
    }
}
