/* Subroutines used for MIPS code generation.
   Copyright (C) 1989, 1990, 1991, 1993, 1994, 1995, 1996, 1997, 1998,
   1999, 2000, 2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010,
   2011
   Free Software Foundation, Inc.
   Contributed by A. Lichnewsky, lich@inria.inria.fr.
   Changes by Michael Meissner, meissner@osf.org.
   64-bit r4000 support by Ian Lance Taylor, ian@cygnus.com, and
   Brendan Eich, brendan@microunity.com.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 3, or (at your option)
any later version.

GCC is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING3.  If not see
<http://www.gnu.org/licenses/>.  */

#include "config.h"
#include "system.h"
#include "coretypes.h"
#include "tm.h"
#include "rtl.h"
#include "regs.h"
#include "hard-reg-set.h"
#include "insn-config.h"
#include "conditions.h"
#include "insn-attr.h"
#include "recog.h"
#include "output.h"
#include "tree.h"
#include "function.h"
#include "expr.h"
#include "optabs.h"
#include "libfuncs.h"
#include "flags.h"
#include "reload.h"
#include "tm_p.h"
#include "ggc.h"
#include "gstab.h"
#include "hashtab.h"
#include "debug.h"
#include "target.h"
#include "target-def.h"
#include "integrate.h"
#include "langhooks.h"
#include "cfglayout.h"
#include "sched-int.h"
#include "gimple.h"
#include "bitmap.h"
#include "diagnostic.h"
#include "target-globals.h"
#include "symcat.h"
#include <stdint.h>

/*----------------------------------------------------------------------*/
/* RISCV_SYSCFG_VLEN_MAX                                                */
/*----------------------------------------------------------------------*/
/* Eventually we want to include syscfg.h here so that we can use the
   common definition of RISCV_SYSCFG_VLEN_MAX, but for now it is not
   clear how to do this. syscfg.h in in libgloss which is not used when
   building the actual cross-compiler. We kind of want to use the
   "version" in sims - the one for native programs instead of RISC-V
   programs. Even if we could include syscfg.h though, we would still
   need to figure out a way to include it in the mips-riscv.md since the
   machine description file also refers to these modes. */

#define RISCV_SYSCFG_VLEN_MAX 32

/*----------------------------------------------------------------------*/
/* MIPS_RISCV_VECTOR_MODE_NAME                                          */
/*----------------------------------------------------------------------*/
/* This is a helper macro which creates a RISC-V vector mode name from
   the given inner_mode. It does this by concatenating a 'V' prefix, the
   maximum RISC-V vector length, and the inner mode together. For
   example, MIPS_RISCV_VECTOR_MODE_NAME(SI) should expand to V32SI if
   the RISC-V maximum vector length is 32. We need to use the nested
   macros to make sure RISCV_SYSCFG_VLEN_MAX is expanded _before_
   concatenation. */

#define MIPS_RISCV_VECTOR_MODE_NAME_H2( res_ ) res_

#define MIPS_RISCV_VECTOR_MODE_NAME_H1( arg0_, arg1_ ) \
  MIPS_RISCV_VECTOR_MODE_NAME_H2( V ## arg0_ ## arg1_ ## mode )

#define MIPS_RISCV_VECTOR_MODE_NAME_H0( arg0_, arg1_ ) \
  MIPS_RISCV_VECTOR_MODE_NAME_H1( arg0_, arg1_ )

#define MIPS_RISCV_VECTOR_MODE_NAME( inner_mode_ ) \
  MIPS_RISCV_VECTOR_MODE_NAME_H0( RISCV_SYSCFG_VLEN_MAX, inner_mode_ )

/* True if X is an UNSPEC wrapper around a SYMBOL_REF or LABEL_REF.  */
#define UNSPEC_ADDRESS_P(X)					\
  (GET_CODE (X) == UNSPEC					\
   && XINT (X, 1) >= UNSPEC_ADDRESS_FIRST			\
   && XINT (X, 1) < UNSPEC_ADDRESS_FIRST + NUM_SYMBOL_TYPES)

/* Extract the symbol or label from UNSPEC wrapper X.  */
#define UNSPEC_ADDRESS(X) \
  XVECEXP (X, 0, 0)

/* Extract the symbol type from UNSPEC wrapper X.  */
#define UNSPEC_ADDRESS_TYPE(X) \
  ((enum mips_symbol_type) (XINT (X, 1) - UNSPEC_ADDRESS_FIRST))

/* The maximum distance between the top of the stack frame and the
   value $sp has when we save and restore registers.

   The value for normal-mode code must be a SMALL_OPERAND and must
   preserve the maximum stack alignment.  We therefore use a value
   of 0x7ff0 in this case.

   MIPS16e SAVE and RESTORE instructions can adjust the stack pointer by
   up to 0x7f8 bytes and can usually save or restore all the registers
   that we need to save or restore.  (Note that we can only use these
   instructions for o32, for which the stack alignment is 8 bytes.)

   We use a maximum gap of 0x100 or 0x400 for MIPS16 code when SAVE and
   RESTORE are not available.  We can then use unextended instructions
   to save and restore registers, and to allocate and deallocate the top
   part of the frame.  */
#define MIPS_MAX_FIRST_STACK_STEP (RISCV_IMM_REACH/2 - 16)

/* True if INSN is a mips.md pattern or asm statement.  */
#define USEFUL_INSN_P(INSN)						\
  (NONDEBUG_INSN_P (INSN)						\
   && GET_CODE (PATTERN (INSN)) != USE					\
   && GET_CODE (PATTERN (INSN)) != CLOBBER				\
   && GET_CODE (PATTERN (INSN)) != ADDR_VEC				\
   && GET_CODE (PATTERN (INSN)) != ADDR_DIFF_VEC)

/* True if bit BIT is set in VALUE.  */
#define BITSET_P(VALUE, BIT) (((VALUE) & (1 << (BIT))) != 0)

/* Classifies an address.

   ADDRESS_REG
       A natural register + offset address.  The register satisfies
       mips_valid_base_register_p and the offset is a const_arith_operand.

   ADDRESS_LO_SUM
       A LO_SUM rtx.  The first operand is a valid base register and
       the second operand is a symbolic address.

   ADDRESS_CONST_INT
       A signed 16-bit constant address.

   ADDRESS_SYMBOLIC:
       A constant symbolic address.  */
enum mips_address_type {
  ADDRESS_REG,
  ADDRESS_LO_SUM,
  ADDRESS_CONST_INT,
  ADDRESS_SYMBOLIC
};

/* Macros to create an enumeration identifier for a function prototype.  */
#define MIPS_FTYPE_NAME1(A, B) MIPS_##A##_FTYPE_##B
#define MIPS_FTYPE_NAME2(A, B, C) MIPS_##A##_FTYPE_##B##_##C
#define MIPS_FTYPE_NAME3(A, B, C, D) MIPS_##A##_FTYPE_##B##_##C##_##D
#define MIPS_FTYPE_NAME4(A, B, C, D, E) MIPS_##A##_FTYPE_##B##_##C##_##D##_##E

/* Classifies the prototype of a built-in function.  */
enum mips_function_type {
#define DEF_MIPS_FTYPE(NARGS, LIST) MIPS_FTYPE_NAME##NARGS LIST,
#include "config/riscv/riscv-ftypes.def"
#undef DEF_MIPS_FTYPE
  MIPS_MAX_FTYPE_MAX
};

/* Specifies how a built-in function should be converted into rtl.  */
enum mips_builtin_type {
  /* The function corresponds directly to an .md pattern.  The return
     value is mapped to operand 0 and the arguments are mapped to
     operands 1 and above.  */
  MIPS_BUILTIN_DIRECT,

  /* The function corresponds directly to an .md pattern.  There is no return
     value and the arguments are mapped to operands 0 and above.  */
  MIPS_BUILTIN_DIRECT_NO_TARGET
};

/* Information about a function's frame layout.  */
struct GTY(())  riscv_frame_info {
  /* The size of the frame in bytes.  */
  HOST_WIDE_INT total_size;

  /* Bit X is set if the function saves or restores GPR X.  */
  unsigned int mask;

  /* Likewise FPR X.  */
  unsigned int fmask;

  /* Offsets of fixed-point and floating-point save areas from frame bottom */
  HOST_WIDE_INT gp_sp_offset;
  HOST_WIDE_INT fp_sp_offset;

  /* Offset of virtual frame pointer from stack pointer/frame bottom */
  HOST_WIDE_INT frame_pointer_offset;

  /* Offset of hard frame pointer from stack pointer/frame bottom */
  HOST_WIDE_INT hard_frame_pointer_offset;

  /* The offset of arg_pointer_rtx from the bottom of the frame.  */
  HOST_WIDE_INT arg_pointer_offset;
};

struct GTY(())  machine_function {
  /* The number of extra stack bytes taken up by register varargs.
     This area is allocated by the callee at the very top of the frame.  */
  int varargs_size;

  /* The current frame information, calculated by riscv_compute_frame_info.  */
  struct riscv_frame_info frame;
};

/* Information about a single argument.  */
struct mips_arg_info {
  /* True if the argument is passed in a floating-point register, or
     would have been if we hadn't run out of registers.  */
  bool fpr_p;

  /* The number of words passed in registers, rounded up.  */
  unsigned int reg_words;

  /* For EABI, the offset of the first register from GP_ARG_FIRST or
     FP_ARG_FIRST.  For other ABIs, the offset of the first register from
     the start of the ABI's argument structure (see the CUMULATIVE_ARGS
     comment for details).

     The value is MAX_ARGS_IN_REGISTERS if the argument is passed entirely
     on the stack.  */
  unsigned int reg_offset;

  /* The number of words that must be passed on the stack, rounded up.  */
  unsigned int stack_words;

  /* The offset from the start of the stack overflow area of the argument's
     first stack word.  Only meaningful when STACK_WORDS is nonzero.  */
  unsigned int stack_offset;
};

/* Information about an address described by mips_address_type.

   ADDRESS_CONST_INT
       No fields are used.

   ADDRESS_REG
       REG is the base register and OFFSET is the constant offset.

   ADDRESS_LO_SUM
       REG and OFFSET are the operands to the LO_SUM and SYMBOL_TYPE
       is the type of symbol it references.

   ADDRESS_SYMBOLIC
       SYMBOL_TYPE is the type of symbol that the address references.  */
struct mips_address_info {
  enum mips_address_type type;
  rtx reg;
  rtx offset;
  enum mips_symbol_type symbol_type;
};

/* One stage in a constant building sequence.  These sequences have
   the form:

	A = VALUE[0]
	A = A CODE[1] VALUE[1]
	A = A CODE[2] VALUE[2]
	...

   where A is an accumulator, each CODE[i] is a binary rtl operation
   and each VALUE[i] is a constant integer.  CODE[0] is undefined.  */
struct mips_integer_op {
  enum rtx_code code;
  unsigned HOST_WIDE_INT value;
};

/* The largest number of operations needed to load an integer constant.
   The worst accepted case for 64-bit constants is LUI,ORI,SLL,ORI,SLL,ORI.
   When the lowest bit is clear, we can try, but reject a sequence with
   an extra SLL at the end.  */
#define MIPS_MAX_INTEGER_OPS 32

/* Costs of various operations on the different architectures.  */

struct mips_rtx_cost_data
{
  unsigned short fp_add;
  unsigned short fp_mult_sf;
  unsigned short fp_mult_df;
  unsigned short fp_div_sf;
  unsigned short fp_div_df;
  unsigned short int_mult_si;
  unsigned short int_mult_di;
  unsigned short int_div_si;
  unsigned short int_div_di;
  unsigned short branch_cost;
  unsigned short memory_latency;
};

/* Global variables for machine-dependent things.  */

/* The processor that we should tune the code for.  */
enum processor mips_tune;

/* Which cost information to use.  */
static const struct mips_rtx_cost_data *mips_cost;

/* Index [M][R] is true if register R is allowed to hold a value of mode M.  */
bool mips_hard_regno_mode_ok[(int) MAX_MACHINE_MODE][FIRST_PSEUDO_REGISTER];

/* mips_lo_relocs[X] is the relocation to use when a symbol of type X
   appears in a LO_SUM.  It can be null if such LO_SUMs aren't valid or
   if they are matched by a special .md file pattern.  */
const char *mips_lo_relocs[NUM_SYMBOL_TYPES];

/* Likewise for HIGHs.  */
const char *mips_hi_relocs[NUM_SYMBOL_TYPES];

/* Index R is the smallest register class that contains register R.  */
const enum reg_class mips_regno_to_class[FIRST_PSEUDO_REGISTER] = {
  GR_REGS,	GR_REGS,	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS,	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS,	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS,	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS, 	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS,	GR_REGS,	GR_REGS,
  GR_REGS,	GR_REGS,	T_REGS,		T_REGS,
  T_REGS,	T_REGS,		T_REGS,		GR_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  FP_REGS,	FP_REGS,	FP_REGS,	FP_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,	VEC_GR_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,	VEC_FP_REGS,
  FRAME_REGS,	FRAME_REGS,
};

/* The value of TARGET_ATTRIBUTE_TABLE.  */
static const struct attribute_spec mips_attribute_table[] = {
  /* { name, min_len, max_len, decl_req, type_req, fn_type_req, handler } */
  { "long_call",   0, 0, false, true,  true,  NULL },
  { "far",     	   0, 0, false, true,  true,  NULL },
  { "near",        0, 0, false, true,  true,  NULL },
  { "utfunc",      0, 0, true,  false, false, NULL },
  { NULL,	   0, 0, false, false, false, NULL }
};

/* A table describing all the processors GCC knows about.  Names are
   matched in the order listed.  The first mention of an ISA level is
   taken as the canonical name for that ISA.

   To ease comparison, please keep this table in the same order
   as GAS's mips_cpu_info_table.  Please also make sure that
   MIPS_ISA_LEVEL_SPEC and MIPS_ARCH_FLOAT_SPEC handle all -march
   options correctly.  */
static const struct mips_cpu_info mips_cpu_info_table[] = {
  /* Entries for generic ISAs.  */
  { "rocket", PROCESSOR_ROCKET, 0 },
};

/* Default costs.  If these are used for a processor we should look
   up the actual costs.  */
#define DEFAULT_COSTS COSTS_N_INSNS (8),  /* fp_add */       \
                      COSTS_N_INSNS (8),  /* fp_mult_sf */   \
                      COSTS_N_INSNS (8),  /* fp_mult_df */   \
                      COSTS_N_INSNS (20), /* fp_div_sf */    \
                      COSTS_N_INSNS (20), /* fp_div_df */    \
                      COSTS_N_INSNS (4),  /* int_mult_si */  \
                      COSTS_N_INSNS (4),  /* int_mult_di */  \
                      COSTS_N_INSNS (6),  /* int_div_si */   \
                      COSTS_N_INSNS (6),  /* int_div_di */   \
                                       2, /* branch_cost */  \
                                       7  /* memory_latency */

/* Floating-point costs for processors without an FPU.  Just assume that
   all floating-point libcalls are very expensive.  */
#define SOFT_FP_COSTS COSTS_N_INSNS (256), /* fp_add */       \
                      COSTS_N_INSNS (256), /* fp_mult_sf */   \
                      COSTS_N_INSNS (256), /* fp_mult_df */   \
                      COSTS_N_INSNS (256), /* fp_div_sf */    \
                      COSTS_N_INSNS (256)  /* fp_div_df */

/* Costs to use when optimizing for size.  */
static const struct mips_rtx_cost_data mips_rtx_cost_optimize_size = {
  COSTS_N_INSNS (1),            /* fp_add */
  COSTS_N_INSNS (1),            /* fp_mult_sf */
  COSTS_N_INSNS (1),            /* fp_mult_df */
  COSTS_N_INSNS (1),            /* fp_div_sf */
  COSTS_N_INSNS (1),            /* fp_div_df */
  COSTS_N_INSNS (1),            /* int_mult_si */
  COSTS_N_INSNS (1),            /* int_mult_di */
  COSTS_N_INSNS (1),            /* int_div_si */
  COSTS_N_INSNS (1),            /* int_div_di */
		   1,           /* branch_cost */
		   1            /* memory_latency */
};

/* Costs to use when optimizing for speed, indexed by processor.  */
static const struct mips_rtx_cost_data
  mips_rtx_cost_data[NUM_PROCESSOR_VALUES] = {
  { /* Rocket */ DEFAULT_COSTS},
};

static int mips_register_move_cost (enum machine_mode, reg_class_t,
				    reg_class_t);
static unsigned int mips_function_arg_boundary (enum machine_mode, const_tree);

/* Predicates to test for presence of "near" and "far"/"long_call"
   attributes on the given TYPE.  */

static bool
mips_near_type_p (const_tree type)
{
  return lookup_attribute ("near", TYPE_ATTRIBUTES (type)) != NULL;
}

static bool
mips_far_type_p (const_tree type)
{
  return (lookup_attribute ("long_call", TYPE_ATTRIBUTES (type)) != NULL
	  || lookup_attribute ("far", TYPE_ATTRIBUTES (type)) != NULL);
}

/* Implement TARGET_COMP_TYPE_ATTRIBUTES.  */

static int
mips_comp_type_attributes (const_tree type1, const_tree type2)
{
  /* Disallow mixed near/far attributes.  */
  if (mips_far_type_p (type1) && mips_near_type_p (type2))
    return 0;
  if (mips_near_type_p (type1) && mips_far_type_p (type2))
    return 0;
  return 1;
}

/* If X is a PLUS of a CONST_INT, return the two terms in *BASE_PTR
   and *OFFSET_PTR.  Return X in *BASE_PTR and 0 in *OFFSET_PTR otherwise.  */

static void
mips_split_plus (rtx x, rtx *base_ptr, HOST_WIDE_INT *offset_ptr)
{
  if (GET_CODE (x) == PLUS && GET_CODE (XEXP (x, 1)) == CONST_INT)
    {
      *base_ptr = XEXP (x, 0);
      *offset_ptr = INTVAL (XEXP (x, 1));
    }
  else
    {
      *base_ptr = x;
      *offset_ptr = 0;
    }
}

/* Fill CODES with a sequence of rtl operations to load VALUE.
   Return the number of operations needed.  */

static int
riscv_build_integer_simple (struct mips_integer_op *codes, HOST_WIDE_INT value)
{
  HOST_WIDE_INT low_part = RISCV_CONST_LOW_PART (value);
  int cost = INT_MAX, alt_cost;
  struct mips_integer_op alt_codes[MIPS_MAX_INTEGER_OPS];

  if (SMALL_OPERAND (value) || LUI_OPERAND (value))
    {
      /* Simply ADDI or LUI */
      codes[0].code = UNKNOWN;
      codes[0].value = value;
      return 1;
    }

  /* End with ADDI */
  if (low_part != 0)
    {
      cost = 1 + riscv_build_integer_simple (codes, value - low_part);
      codes[cost-1].code = PLUS;
      codes[cost-1].value = low_part;
    }

  /* End with XORI */
  if (low_part < 0)
    {
      alt_cost = 1 + riscv_build_integer_simple (alt_codes, value ^ low_part);
      alt_codes[alt_cost-1].code = XOR;
      alt_codes[alt_cost-1].value = low_part;
      if (alt_cost < cost)
	cost = alt_cost, memcpy (codes, alt_codes, sizeof(alt_codes));
    }

  /* Eliminate trailing zeros and end with SLLI */
  if ((value & 1) == 0)
    {
      int shift = __builtin_ctzl(value);
      alt_cost = 1 + riscv_build_integer_simple (alt_codes, value >> shift);
      alt_codes[alt_cost-1].code = ASHIFT;
      alt_codes[alt_cost-1].value = shift;
      if (alt_cost < cost)
	cost = alt_cost, memcpy (codes, alt_codes, sizeof(alt_codes));
    }

  gcc_assert (cost <= MIPS_MAX_INTEGER_OPS);
  return cost;
}

static int
riscv_build_integer (struct mips_integer_op *codes, HOST_WIDE_INT value)
{
  int cost = riscv_build_integer_simple (codes, value);

  /* Eliminate leading zeros and end with SRLI */
  if (value > 0 && cost > 2)
    {
      struct mips_integer_op alt_codes[MIPS_MAX_INTEGER_OPS];
      int alt_cost, shift;

      shift = __builtin_clzl(value);
      alt_cost = 1 + riscv_build_integer_simple (alt_codes, value << shift);
      alt_codes[alt_cost-1].code = LSHIFTRT;
      alt_codes[alt_cost-1].value = shift;
      if (alt_cost < cost)
	cost = alt_cost, memcpy (codes, alt_codes, sizeof(alt_codes));

      /* Also try filling discarded bits with 1s */
      shift = __builtin_clzl(value);
      alt_cost = 1 + riscv_build_integer_simple (alt_codes,
			value << shift | ((1L<<shift)-1));
      alt_codes[alt_cost-1].code = LSHIFTRT;
      alt_codes[alt_cost-1].value = shift;
      if (alt_cost < cost)
	cost = alt_cost, memcpy (codes, alt_codes, sizeof(alt_codes));
    }

  return cost;
}

static int
riscv_split_integer_cost (HOST_WIDE_INT val)
{
  int cost;
  int32_t loval = val, hival = (val - (int32_t)val) >> 32;
  struct mips_integer_op codes[MIPS_MAX_INTEGER_OPS];

  cost = 2 + riscv_build_integer(codes, loval);
  if (loval != hival)
    cost += riscv_build_integer(codes, hival);

  return cost;
}

static int
riscv_integer_cost (HOST_WIDE_INT val)
{
  struct mips_integer_op codes[MIPS_MAX_INTEGER_OPS];
  return MIN (riscv_build_integer (codes, val), riscv_split_integer_cost (val));
}

/* Try to split a 64b integer into 32b parts, then reassemble. */

static rtx
riscv_split_integer (HOST_WIDE_INT val, enum machine_mode mode)
{
  int32_t loval = val, hival = (val - (int32_t)val) >> 32;
  rtx hi = gen_reg_rtx (mode), lo = gen_reg_rtx (mode);

  mips_move_integer (hi, hi, hival);
  mips_move_integer (lo, lo, loval);

  hi = gen_rtx_fmt_ee (ASHIFT, mode, hi, GEN_INT (32));
  hi = force_reg (mode, hi);

  return gen_rtx_fmt_ee (PLUS, mode, hi, lo);
}

/* Return true if X is a thread-local symbol.  */

static bool
mips_tls_symbol_p (const_rtx x)
{
  return GET_CODE (x) == SYMBOL_REF && SYMBOL_REF_TLS_MODEL (x) != 0;
}

static bool
riscv_symbol_binds_local_p (const_rtx x)
{
  return (SYMBOL_REF_DECL (x)
	  ? targetm.binds_local_p (SYMBOL_REF_DECL (x))
	  : SYMBOL_REF_LOCAL_P (x));
}

/* Return the method that should be used to access SYMBOL_REF or
   LABEL_REF X in context CONTEXT.  */

static enum mips_symbol_type
mips_classify_symbol (const_rtx x)
{
  if (mips_tls_symbol_p (x))
    return SYMBOL_TLS;

  if (GET_CODE (x) == LABEL_REF)
    {
      if (LABEL_REF_NONLOCAL_P (x))
	return SYMBOL_GOT_DISP;
      return SYMBOL_ABSOLUTE;
    }

  gcc_assert (GET_CODE (x) == SYMBOL_REF);

  if (flag_pic && !riscv_symbol_binds_local_p (x))
    return SYMBOL_GOT_DISP;

  return SYMBOL_ABSOLUTE;
}

/* Classify the base of symbolic expression X, given that X appears in
   context CONTEXT.  */

static enum mips_symbol_type
mips_classify_symbolic_expression (rtx x)
{
  rtx offset;

  split_const (x, &x, &offset);
  if (UNSPEC_ADDRESS_P (x))
    return UNSPEC_ADDRESS_TYPE (x);

  return mips_classify_symbol (x);
}

/* Return true if X is a symbolic constant that can be used in context
   CONTEXT.  If it is, store the type of the symbol in *SYMBOL_TYPE.  */

bool
mips_symbolic_constant_p (rtx x, enum mips_symbol_type *symbol_type)
{
  rtx offset;

  split_const (x, &x, &offset);
  if (UNSPEC_ADDRESS_P (x))
    {
      *symbol_type = UNSPEC_ADDRESS_TYPE (x);
      x = UNSPEC_ADDRESS (x);
    }
  else if (GET_CODE (x) == SYMBOL_REF || GET_CODE (x) == LABEL_REF)
    *symbol_type = mips_classify_symbol (x);
  else
    return false;

  if (offset == const0_rtx)
    return true;

  /* Check whether a nonzero offset is valid for the underlying
     relocations.  */
  switch (*symbol_type)
    {
    case SYMBOL_ABSOLUTE:
      return true;

    case SYMBOL_TLS:
    case SYMBOL_TLS_LE:
    case SYMBOL_TLS_IE:
    case SYMBOL_GOT_DISP:
      return false;
    }
  gcc_unreachable ();
}

/* Returns the number of instructions necessary to reference a symbol. */

static int riscv_symbol_insns (enum mips_symbol_type type)
{
  switch (type)
  {
    case SYMBOL_TLS: return 0; /* Depends on the TLS model. */
    case SYMBOL_ABSOLUTE: return 2; /* LUI + the reference itself */
    case SYMBOL_TLS_LE: return 3; /* LUI + ADD TP + the reference itself */
    case SYMBOL_TLS_IE: return 4; /* LUI + LD + ADD TP + the reference itself */
    case SYMBOL_GOT_DISP: return 3; /* AUIPC + LD GOT + the reference itself */
    default: gcc_unreachable();
  }
}

/* A for_each_rtx callback.  Stop the search if *X references a
   thread-local symbol.  */

static int
mips_tls_symbol_ref_1 (rtx *x, void *data ATTRIBUTE_UNUSED)
{
  return mips_tls_symbol_p (*x);
}

/* Implement TARGET_CANNOT_FORCE_CONST_MEM.  */

static bool
mips_cannot_force_const_mem (rtx x)
{
  enum mips_symbol_type type;
  rtx base, offset;

  /* There is no assembler syntax for expressing an address-sized
     high part.  */
  if (GET_CODE (x) == HIGH)
    return true;

  /* As an optimization, reject constants that mips_legitimize_move
     can expand inline.

     Suppose we have a multi-instruction sequence that loads constant C
     into register R.  If R does not get allocated a hard register, and
     R is used in an operand that allows both registers and memory
     references, reload will consider forcing C into memory and using
     one of the instruction's memory alternatives.  Returning false
     here will force it to use an input reload instead.  */
  if (CONST_INT_P (x) && LEGITIMATE_CONSTANT_P (x))
    return true;

  split_const (x, &base, &offset);
  if (mips_symbolic_constant_p (base, &type))
    {
      /* The same optimization as for CONST_INT.  */
      if (SMALL_INT (offset) && riscv_symbol_insns (type) > 0)
	return true;
    }

  /* TLS symbols must be computed by mips_legitimize_move.  */
  if (for_each_rtx (&x, &mips_tls_symbol_ref_1, NULL))
    return true;

  return false;
}

/* Return true if register REGNO is a valid base register for mode MODE.
   STRICT_P is true if REG_OK_STRICT is in effect.  */

int
mips_regno_mode_ok_for_base_p (int regno, enum machine_mode mode ATTRIBUTE_UNUSED,
			       bool strict_p)
{
  if (!HARD_REGISTER_NUM_P (regno))
    {
      if (!strict_p)
	return true;
      regno = reg_renumber[regno];
    }

  /* These fake registers will be eliminated to either the stack or
     hard frame pointer, both of which are usually valid base registers.
     Reload deals with the cases where the eliminated form isn't valid.  */
  if (regno == ARG_POINTER_REGNUM || regno == FRAME_POINTER_REGNUM)
    return true;

  return GP_REG_P (regno);
}

/* Return true if X is a valid base register for mode MODE.
   STRICT_P is true if REG_OK_STRICT is in effect.  */

static bool
mips_valid_base_register_p (rtx x, enum machine_mode mode, bool strict_p)
{
  if (!strict_p && GET_CODE (x) == SUBREG)
    x = SUBREG_REG (x);

  return (REG_P (x)
	  && mips_regno_mode_ok_for_base_p (REGNO (x), mode, strict_p));
}

/* Return true if, for every base register BASE_REG, (plus BASE_REG X)
   can address a value of mode MODE.  */

static bool
mips_valid_offset_p (rtx x, enum machine_mode mode)
{
  /* Check that X is a signed 12-bit number.  */
  if (!const_arith_operand (x, Pmode))
    return false;

  /* We may need to split multiword moves, so make sure that every word
     is accessible.  */
  if (GET_MODE_SIZE (mode) > UNITS_PER_WORD
      && !SMALL_OPERAND (INTVAL (x) + GET_MODE_SIZE (mode) - UNITS_PER_WORD))
    return false;

  return true;
}

/* Return true if a LO_SUM can address a value of mode MODE when the
   LO_SUM symbol has type SYMBOL_TYPE.  */

static bool
mips_valid_lo_sum_p (enum mips_symbol_type symbol_type, enum machine_mode mode)
{
  /* Check that symbols of type SYMBOL_TYPE can be used to access values
     of mode MODE.  */
  if (riscv_symbol_insns (symbol_type) == 0)
    return false;

  /* Check that there is a known low-part relocation.  */
  if (mips_lo_relocs[symbol_type] == NULL)
    return false;

  /* We may need to split multiword moves, so make sure that each word
     can be accessed without inducing a carry.  This is mainly needed
     for o64, which has historically only guaranteed 64-bit alignment
     for 128-bit types.  */
  if (GET_MODE_SIZE (mode) > UNITS_PER_WORD
      && GET_MODE_BITSIZE (mode) > GET_MODE_ALIGNMENT (mode))
    return false;

  return true;
}

/* Return true if X is a valid address for machine mode MODE.  If it is,
   fill in INFO appropriately.  STRICT_P is true if REG_OK_STRICT is in
   effect.  */

static bool
mips_classify_address (struct mips_address_info *info, rtx x,
		       enum machine_mode mode, bool strict_p)
{
  switch (GET_CODE (x))
    {
    case REG:
    case SUBREG:
      info->type = ADDRESS_REG;
      info->reg = x;
      info->offset = const0_rtx;
      return mips_valid_base_register_p (info->reg, mode, strict_p);

    case PLUS:
      info->type = ADDRESS_REG;
      info->reg = XEXP (x, 0);
      info->offset = XEXP (x, 1);
      return (mips_valid_base_register_p (info->reg, mode, strict_p)
	      && mips_valid_offset_p (info->offset, mode));

    case LO_SUM:
      info->type = ADDRESS_LO_SUM;
      info->reg = XEXP (x, 0);
      info->offset = XEXP (x, 1);
      /* We have to trust the creator of the LO_SUM to do something vaguely
	 sane.  Target-independent code that creates a LO_SUM should also
	 create and verify the matching HIGH.  Target-independent code that
	 adds an offset to a LO_SUM must prove that the offset will not
	 induce a carry.  Failure to do either of these things would be
	 a bug, and we are not required to check for it here.  The MIPS
	 backend itself should only create LO_SUMs for valid symbolic
	 constants, with the high part being either a HIGH or a copy
	 of _gp. */
      info->symbol_type
	= mips_classify_symbolic_expression (info->offset);
      return (mips_valid_base_register_p (info->reg, mode, strict_p)
	      && mips_valid_lo_sum_p (info->symbol_type, mode));

    case CONST_INT:
      /* Small-integer addresses don't occur very often, but they
	 are legitimate if $0 is a valid base register.  */
      info->type = ADDRESS_CONST_INT;
      return SMALL_INT (x);

    case CONST:
    case LABEL_REF:
    case SYMBOL_REF:
      if (mips_symbolic_constant_p (x, &info->symbol_type)
	  && riscv_symbol_insns (info->symbol_type) > 0
	  && !mips_hi_relocs[info->symbol_type]
	  && mips_lo_relocs[info->symbol_type])
	{
	  info->type = ADDRESS_LO_SUM;
	  info->reg = gen_rtx_REG (Pmode, GP_REGNUM);
	  info->offset = x;
	  return true;
	}

    default:
      return false;
    }
}

/* Implement TARGET_LEGITIMATE_ADDRESS_P.  */

static bool
mips_legitimate_address_p (enum machine_mode mode, rtx x, bool strict_p)
{
  struct mips_address_info addr;

  return mips_classify_address (&addr, x, mode, strict_p);
}

/* Return the number of instructions needed to load or store a value
   of mode MODE at address X.  Return 0 if X isn't valid for MODE.
   Assume that multiword moves may need to be split into word moves
   if MIGHT_SPLIT_P, otherwise assume that a single load or store is
   enough. */

int
riscv_address_insns (rtx x, enum machine_mode mode, bool might_split_p)
{
  struct mips_address_info addr;
  int n = 1;

  if (!mips_classify_address (&addr, x, mode, false))
    return 0;

  /* BLKmode is used for single unaligned loads and stores and should
     not count as a multiword mode. */
  if (mode != BLKmode && might_split_p)
    n += (GET_MODE_SIZE (mode) + UNITS_PER_WORD - 1) / UNITS_PER_WORD;

  if (addr.type == ADDRESS_SYMBOLIC)
    n *= riscv_symbol_insns (addr.symbol_type);

  return n;
}

/* Return the number of instructions needed to load constant X.
   Return 0 if X isn't a valid constant.  */

int
mips_const_insns (rtx x)
{
  enum mips_symbol_type symbol_type;
  rtx offset;

  switch (GET_CODE (x))
    {
    case HIGH:
      if (!mips_symbolic_constant_p (XEXP (x, 0), &symbol_type)
	  || !mips_hi_relocs[symbol_type])
	return 0;

      /* This is simply an LUI. */
      return 1;

    case CONST_INT:
      {
	int cost = riscv_integer_cost (INTVAL (x));
	/* Force complicated constants to memory. */
	return cost < 4 ? cost : 0;
      }

    case CONST_DOUBLE:
    case CONST_VECTOR:
      /* Allow zeros for normal mode, where we can use x0.  */
      return x == CONST0_RTX (GET_MODE (x)) ? 1 : 0;

    case CONST:
      /* See if we can refer to X directly.  */
      if (mips_symbolic_constant_p (x, &symbol_type))
	return riscv_symbol_insns (symbol_type);

      /* Otherwise try splitting the constant into a base and offset.
	 If the offset is a 16-bit value, we can load the base address
	 into a register and then use (D)ADDIU to add in the offset.
	 If the offset is larger, we can load the base and offset
	 into separate registers and add them together with (D)ADDU.
	 However, the latter is only possible before reload; during
	 and after reload, we must have the option of forcing the
	 constant into the pool instead.  */
      split_const (x, &x, &offset);
      if (offset != 0)
	{
	  int n = mips_const_insns (x);
	  if (n != 0)
	    {
	      if (SMALL_INT (offset))
		return n + 1;
	      else if (!targetm.cannot_force_const_mem (x))
		return n + 1 + riscv_integer_cost (INTVAL (offset));
	    }
	}
      return 0;

    case SYMBOL_REF:
    case LABEL_REF:
      return riscv_symbol_insns (mips_classify_symbol (x));

    default:
      return 0;
    }
}

/* X is a doubleword constant that can be handled by splitting it into
   two words and loading each word separately.  Return the number of
   instructions required to do this.  */

int
mips_split_const_insns (rtx x)
{
  unsigned int low, high;

  low = mips_const_insns (mips_subword (x, false));
  high = mips_const_insns (mips_subword (x, true));
  gcc_assert (low > 0 && high > 0);
  return low + high;
}

/* Return the number of instructions needed to implement INSN,
   given that it loads from or stores to MEM.  Count extended
   MIPS16 instructions as two instructions.  */

int
mips_load_store_insns (rtx mem, rtx insn)
{
  enum machine_mode mode;
  bool might_split_p;
  rtx set;

  gcc_assert (MEM_P (mem));
  mode = GET_MODE (mem);

  /* Try to prove that INSN does not need to be split.  */
  might_split_p = true;
  if (GET_MODE_BITSIZE (mode) == 64)
    {
      set = single_set (insn);
      if (set && !mips_split_64bit_move_p (SET_DEST (set), SET_SRC (set)))
	might_split_p = false;
    }

  return riscv_address_insns (XEXP (mem, 0), mode, might_split_p);
}

/* Emit a move from SRC to DEST.  Assume that the move expanders can
   handle all moves if !can_create_pseudo_p ().  The distinction is
   important because, unlike emit_move_insn, the move expanders know
   how to force Pmode objects into the constant pool even when the
   constant pool address is not itself legitimate.  */

rtx
mips_emit_move (rtx dest, rtx src)
{
  return (can_create_pseudo_p ()
	  ? emit_move_insn (dest, src)
	  : emit_move_insn_1 (dest, src));
}

/* Emit an instruction of the form (set TARGET (CODE OP0 OP1)).  */

static void
mips_emit_binary (enum rtx_code code, rtx target, rtx op0, rtx op1)
{
  emit_insn (gen_rtx_SET (VOIDmode, target,
			  gen_rtx_fmt_ee (code, GET_MODE (target), op0, op1)));
}

/* Compute (CODE OP0 OP1) and store the result in a new register
   of mode MODE.  Return that new register.  */

static rtx
mips_force_binary (enum machine_mode mode, enum rtx_code code, rtx op0, rtx op1)
{
  rtx reg;

  reg = gen_reg_rtx (mode);
  mips_emit_binary (code, reg, op0, op1);
  return reg;
}

/* Copy VALUE to a register and return that register.  If new pseudos
   are allowed, copy it into a new register, otherwise use DEST.  */

static rtx
mips_force_temporary (rtx dest, rtx value)
{
  if (can_create_pseudo_p ())
    return force_reg (Pmode, value);
  else
    {
      mips_emit_move (dest, value);
      return dest;
    }
}

/* Wrap symbol or label BASE in an UNSPEC address of type SYMBOL_TYPE,
   then add CONST_INT OFFSET to the result.  */

static rtx
mips_unspec_address_offset (rtx base, rtx offset,
			    enum mips_symbol_type symbol_type)
{
  base = gen_rtx_UNSPEC (Pmode, gen_rtvec (1, base),
			 UNSPEC_ADDRESS_FIRST + symbol_type);
  if (offset != const0_rtx)
    base = gen_rtx_PLUS (Pmode, base, offset);
  return gen_rtx_CONST (Pmode, base);
}

/* Return an UNSPEC address with underlying address ADDRESS and symbol
   type SYMBOL_TYPE.  */

rtx
mips_unspec_address (rtx address, enum mips_symbol_type symbol_type)
{
  rtx base, offset;

  split_const (address, &base, &offset);
  return mips_unspec_address_offset (base, offset, symbol_type);
}

/* If OP is an UNSPEC address, return the address to which it refers,
   otherwise return OP itself.  */

static rtx
mips_strip_unspec_address (rtx op)
{
  rtx base, offset;

  split_const (op, &base, &offset);
  if (UNSPEC_ADDRESS_P (base))
    op = plus_constant (UNSPEC_ADDRESS (base), INTVAL (offset));
  return op;
}

/* If mips_unspec_address (ADDR, SYMBOL_TYPE) is a 32-bit value, add the
   high part to BASE and return the result.  Just return BASE otherwise.
   TEMP is as for mips_force_temporary.

   The returned expression can be used as the first operand to a LO_SUM.  */

static rtx
mips_unspec_offset_high (rtx temp, rtx addr, enum mips_symbol_type symbol_type)
{
  addr = gen_rtx_HIGH (Pmode, mips_unspec_address (addr, symbol_type));
  return mips_force_temporary (temp, addr);
}

/* Load an entry from the GOT. */
static rtx riscv_got_load_tls_gd(rtx dest, rtx sym)
{
  return (Pmode == DImode ? gen_got_load_tls_gddi(dest, sym) : gen_got_load_tls_gdsi(dest, sym));
}

static rtx riscv_got_load_tls_ie(rtx dest, rtx sym)
{
  return (Pmode == DImode ? gen_got_load_tls_iedi(dest, sym) : gen_got_load_tls_iesi(dest, sym));
}

static rtx riscv_got_load_tls_ie_hi(rtx dest, rtx sym)
{
  return (Pmode == DImode ? gen_got_load_tls_ie_hidi(dest, sym) : gen_got_load_tls_ie_hisi(dest, sym));
}

static rtx riscv_got_load_tls_ie_lo(rtx dest, rtx base, rtx sym)
{
  return (Pmode == DImode ? gen_got_load_tls_ie_lodi(dest, base, sym) : gen_got_load_tls_ie_losi(dest, base, sym));
}

static rtx riscv_tls_add_tp_ie(rtx dest, rtx base, rtx sym)
{
  rtx tp = gen_rtx_REG (Pmode, THREAD_POINTER_REGNUM);
  return (Pmode == DImode ? gen_tls_add_tp_iedi(dest, base, tp, sym) : gen_tls_add_tp_iesi(dest, base, tp, sym));
}

static rtx riscv_tls_add_tp_le(rtx dest, rtx base, rtx sym)
{
  rtx tp = gen_rtx_REG (Pmode, THREAD_POINTER_REGNUM);
  return (Pmode == DImode ? gen_tls_add_tp_ledi(dest, base, tp, sym) : gen_tls_add_tp_lesi(dest, base, tp, sym));
}

/* If MODE is MAX_MACHINE_MODE, ADDR appears as a move operand, otherwise
   it appears in a MEM of that mode.  Return true if ADDR is a legitimate
   constant in that context and can be split into high and low parts.
   If so, and if LOW_OUT is nonnull, emit the high part and store the
   low part in *LOW_OUT.  Leave *LOW_OUT unchanged otherwise.

   TEMP is as for mips_force_temporary and is used to load the high
   part into a register.

   When MODE is MAX_MACHINE_MODE, the low part is guaranteed to be
   a legitimize SET_SRC for an .md pattern, otherwise the low part
   is guaranteed to be a legitimate address for mode MODE.  */

bool
mips_split_symbol (rtx temp, rtx addr, enum machine_mode mode, rtx *low_out)
{
  enum mips_symbol_type symbol_type;
  rtx high;

  if ((GET_CODE (addr) == HIGH && mode == MAX_MACHINE_MODE)
      || !mips_symbolic_constant_p (addr, &symbol_type)
      || riscv_symbol_insns (symbol_type) == 0
      || !mips_hi_relocs[symbol_type])
    return false;

  if (low_out)
    {
      switch (symbol_type)
	{
	case SYMBOL_ABSOLUTE:
	  high = gen_rtx_HIGH (Pmode, copy_rtx (addr));
      	  high = mips_force_temporary (temp, high);
      	  *low_out = gen_rtx_LO_SUM (Pmode, high, addr);
	  break;
	
	default:
	  gcc_unreachable ();
	}
    }

  return true;
}

/* Return a legitimate address for REG + OFFSET.  TEMP is as for
   mips_force_temporary; it is only needed when OFFSET is not a
   SMALL_OPERAND.  */

static rtx
mips_add_offset (rtx temp, rtx reg, HOST_WIDE_INT offset)
{
  if (!SMALL_OPERAND (offset))
    {
      rtx high;

      /* Leave OFFSET as a 16-bit offset and put the excess in HIGH.
         The addition inside the macro CONST_HIGH_PART may cause an
         overflow, so we need to force a sign-extension check.  */
      high = gen_int_mode (RISCV_CONST_HIGH_PART (offset), Pmode);
      offset = RISCV_CONST_LOW_PART (offset);
      high = mips_force_temporary (temp, high);
      reg = mips_force_temporary (temp, gen_rtx_PLUS (Pmode, high, reg));
    }
  return plus_constant (reg, offset);
}

/* The __tls_get_attr symbol.  */
static GTY(()) rtx mips_tls_symbol;

/* Return an instruction sequence that calls __tls_get_addr.  SYM is
   the TLS symbol we are referencing and TYPE is the symbol type to use
   (either global dynamic or local dynamic).  V0 is an RTX for the
   return value location.  */

static rtx
mips_call_tls_get_addr (rtx sym, rtx v0)
{
  rtx insn, a0;

  a0 = gen_rtx_REG (Pmode, GP_ARG_FIRST);

  if (!mips_tls_symbol)
    mips_tls_symbol = init_one_libfunc ("__tls_get_addr");

  start_sequence ();
  
  emit_insn (riscv_got_load_tls_gd(a0, sym));
  insn = mips_expand_call (false, v0, mips_tls_symbol, const0_rtx);
  RTL_CONST_CALL_P (insn) = 1;
  use_reg (&CALL_INSN_FUNCTION_USAGE (insn), a0);
  insn = get_insns ();

  end_sequence ();

  return insn;
}

/* Generate the code to access LOC, a thread-local SYMBOL_REF, and return
   its address.  The return value will be both a valid address and a valid
   SET_SRC (either a REG or a LO_SUM).  */

static rtx
mips_legitimize_tls_address (rtx loc)
{
  rtx dest, insn, v0, tp, tmp1;

  switch (SYMBOL_REF_TLS_MODEL (loc))
    {
    case TLS_MODEL_LOCAL_DYNAMIC:
      /* Rely on section anchors for the optimization that LDM TLS
	 provides.  The anchor's address is loaded with GD TLS. */
    case TLS_MODEL_GLOBAL_DYNAMIC:
      v0 = gen_rtx_REG (Pmode, GP_RETURN);
      insn = mips_call_tls_get_addr (loc, v0);
      dest = gen_reg_rtx (Pmode);
      emit_libcall_block (insn, dest, v0, loc);
      break;

    case TLS_MODEL_INITIAL_EXEC:
      if (flag_pic)
	{
	  /* la.tls.ie; tp-relative add */
	  tp = gen_rtx_REG (Pmode, THREAD_POINTER_REGNUM);
	  tmp1 = gen_reg_rtx (Pmode);
	  emit_insn (riscv_got_load_tls_ie (tmp1, loc));
	  dest = gen_reg_rtx (Pmode);
	  emit_insn (gen_add3_insn (dest, tmp1, tp));
	}
      else
	{
	  /* lui %tls_ie_hi; ld %tls_le_lo; tp-relative add */
	  tmp1 = gen_reg_rtx (Pmode);
	  emit_insn (riscv_got_load_tls_ie_hi (tmp1, loc));
	  dest = gen_reg_rtx (Pmode);
	  emit_insn (riscv_got_load_tls_ie_lo (dest, tmp1, loc));
	  tmp1 = gen_reg_rtx (Pmode);
	  emit_insn (riscv_tls_add_tp_ie (tmp1, dest, loc));
	  dest = gen_rtx_LO_SUM (Pmode, tmp1,
				 mips_unspec_address (loc, SYMBOL_TLS_IE));
	}
      break;

    case TLS_MODEL_LOCAL_EXEC:
      tmp1 = mips_unspec_offset_high (NULL, loc, SYMBOL_TLS_LE);
      dest = gen_reg_rtx (Pmode);
      emit_insn (riscv_tls_add_tp_le (dest, tmp1, loc));
      dest = gen_rtx_LO_SUM (Pmode, dest,
			     mips_unspec_address (loc, SYMBOL_TLS_LE));
      break;

    default:
      gcc_unreachable ();
    }
  return dest;
}

/* If X is not a valid address for mode MODE, force it into a register.  */

static rtx
mips_force_address (rtx x, enum machine_mode mode)
{
  if (!mips_legitimate_address_p (mode, x, false))
    x = force_reg (Pmode, x);
  return x;
}

/* This function is used to implement LEGITIMIZE_ADDRESS.  If X can
   be legitimized in a way that the generic machinery might not expect,
   return a new address, otherwise return NULL.  MODE is the mode of
   the memory being accessed.  */

static rtx
mips_legitimize_address (rtx x, rtx oldx ATTRIBUTE_UNUSED,
			 enum machine_mode mode)
{
  rtx addr;

  if (mips_tls_symbol_p (x))
    return mips_legitimize_tls_address (x);

  /* See if the address can split into a high part and a LO_SUM.  */
  if (mips_split_symbol (NULL, x, mode, &addr))
    return mips_force_address (addr, mode);

  /* Handle BASE + OFFSET using mips_add_offset.  */
  if (GET_CODE (x) == PLUS && CONST_INT_P (XEXP (x, 1))
      && INTVAL (XEXP (x, 1)) != 0)
    {
      rtx base = XEXP (x, 0);
      HOST_WIDE_INT offset = INTVAL (XEXP (x, 1));

      if (!mips_valid_base_register_p (base, mode, false))
	base = copy_to_mode_reg (Pmode, base);
      addr = mips_add_offset (NULL, base, offset);
      return mips_force_address (addr, mode);
    }

  return x;
}

/* Load VALUE into DEST.  TEMP is as for mips_force_temporary.  */

void
mips_move_integer (rtx temp, rtx dest, HOST_WIDE_INT value)
{
  struct mips_integer_op codes[MIPS_MAX_INTEGER_OPS];
  enum machine_mode mode;
  int i, num_ops;
  rtx x;

  mode = GET_MODE (dest);
  num_ops = riscv_build_integer (codes, value);

  if (can_create_pseudo_p () && num_ops > 2 /* not a simple constant */
      && num_ops >= riscv_split_integer_cost (value))
    x = riscv_split_integer (value, mode);
  else
    {
      /* Apply each binary operation to X. */
      x = GEN_INT (codes[0].value);

      for (i = 1; i < num_ops; i++)
        {
          if (!can_create_pseudo_p ())
            {
              emit_insn (gen_rtx_SET (VOIDmode, temp, x));
              x = temp;
            }
          else
            x = force_reg (mode == HImode ? SImode : mode, x);

          x = gen_rtx_fmt_ee (codes[i].code, mode, x, GEN_INT (codes[i].value));
        }
    }

  emit_insn (gen_rtx_SET (VOIDmode, dest, x));
}

/* Subroutine of mips_legitimize_move.  Move constant SRC into register
   DEST given that SRC satisfies immediate_operand but doesn't satisfy
   move_operand.  */

static void
mips_legitimize_const_move (enum machine_mode mode, rtx dest, rtx src)
{
  rtx base, offset;

  /* Split moves of big integers into smaller pieces.  */
  if (splittable_const_int_operand (src, mode))
    {
      mips_move_integer (dest, dest, INTVAL (src));
      return;
    }

  /* Split moves of symbolic constants into high/low pairs.  */
  if (mips_split_symbol (dest, src, MAX_MACHINE_MODE, &src))
    {
      emit_insn (gen_rtx_SET (VOIDmode, dest, src));
      return;
    }

  /* Generate the appropriate access sequences for TLS symbols.  */
  if (mips_tls_symbol_p (src))
    {
      mips_emit_move (dest, mips_legitimize_tls_address (src));
      return;
    }

  /* If we have (const (plus symbol offset)), and that expression cannot
     be forced into memory, load the symbol first and add in the offset.
     In non-MIPS16 mode, prefer to do this even if the constant _can_ be
     forced into memory, as it usually produces better code.  */
  split_const (src, &base, &offset);
  if (offset != const0_rtx
      && (targetm.cannot_force_const_mem (src)
	  || can_create_pseudo_p ()))
    {
      base = mips_force_temporary (dest, base);
      mips_emit_move (dest, mips_add_offset (NULL, base, INTVAL (offset)));
      return;
    }

  src = force_const_mem (mode, src);

  /* When using explicit relocs, constant pool references are sometimes
     not legitimate addresses.  */
  mips_split_symbol (dest, XEXP (src, 0), mode, &XEXP (src, 0));
  mips_emit_move (dest, src);
}

/* If (set DEST SRC) is not a valid move instruction, emit an equivalent
   sequence that is valid.  */

bool
mips_legitimize_move (enum machine_mode mode, rtx dest, rtx src)
{
  if (!register_operand (dest, mode) && !reg_or_0_operand (src, mode))
    {
      mips_emit_move (dest, force_reg (mode, src));
      return true;
    }

  /* We need to deal with constants that would be legitimate
     immediate_operands but aren't legitimate move_operands.  */
  if (CONSTANT_P (src) && !move_operand (src, mode))
    {
      mips_legitimize_const_move (mode, dest, src);
      set_unique_reg_note (get_last_insn (), REG_EQUAL, copy_rtx (src));
      return true;
    }
  return false;
}

bool
mips_legitimize_vector_move (enum machine_mode mode, rtx dest, rtx src)
{
  bool dest_mem, dest_mem_reg;
  bool src_mem, src_mem_reg;

  dest_mem = (GET_CODE(dest) == MEM);
  dest_mem_reg = dest_mem && GET_CODE(XEXP(dest, 0)) == REG;

  src_mem = (GET_CODE(src) == MEM);
  src_mem_reg = src_mem && GET_CODE(XEXP(src, 0)) == REG;

  if (dest_mem && !dest_mem_reg)
  {
    rtx add, scratch, base, move;
    HOST_WIDE_INT offset;

    mips_split_plus(XEXP(dest,0), &base, &offset);

    scratch = gen_reg_rtx(Pmode);
    add = gen_add3_insn(scratch, base, GEN_INT(offset));
    emit_insn(add);

    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DI):
        move = gen_movv32di(gen_rtx_MEM(mode, scratch), src);
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(SI):
        move = gen_movv32si(gen_rtx_MEM(mode, scratch), src);
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(HI):
        move = gen_movv32hi(gen_rtx_MEM(mode, scratch), src);
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(QI):
        move = gen_movv32qi(gen_rtx_MEM(mode, scratch), src);
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(DF):
        move = gen_movv32df(gen_rtx_MEM(mode, scratch), src);
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(SF):
        move = gen_movv32sf(gen_rtx_MEM(mode, scratch), src);
        break;
      default:
        gcc_unreachable();
    }

    emit_insn(move);

    return true;
  }

  if (src_mem && !src_mem_reg)
  {
    rtx add, scratch, base, move;
    HOST_WIDE_INT offset;

    mips_split_plus(XEXP(src,0), &base, &offset);

    scratch = gen_reg_rtx(Pmode);
    add = gen_add3_insn(scratch, base, GEN_INT(offset));
    emit_insn(add);

    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DI):
        move = gen_movv32di(dest, gen_rtx_MEM(mode, scratch));
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(SI):
        move = gen_movv32si(dest, gen_rtx_MEM(mode, scratch));
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(HI):
        move = gen_movv32hi(dest, gen_rtx_MEM(mode, scratch));
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(QI):
        move = gen_movv32qi(dest, gen_rtx_MEM(mode, scratch));
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(DF):
        move = gen_movv32df(dest, gen_rtx_MEM(mode, scratch));
        break;
      case MIPS_RISCV_VECTOR_MODE_NAME(SF):
        move = gen_movv32sf(dest, gen_rtx_MEM(mode, scratch));
        break;
      default:
        gcc_unreachable();
    }

    emit_insn(move);

    return true;
  }

  return false;
}

/* The cost of loading values from the constant pool.  It should be
   larger than the cost of any constant we want to synthesize inline.  */
#define CONSTANT_POOL_COST COSTS_N_INSNS (8)

/* Return true if there is a non-MIPS16 instruction that implements CODE
   and if that instruction accepts X as an immediate operand.  */

static int
mips_immediate_operand_p (int code, HOST_WIDE_INT x)
{
  switch (code)
    {
    case ASHIFT:
    case ASHIFTRT:
    case LSHIFTRT:
      /* All shift counts are truncated to a valid constant.  */
      return true;

    case AND:
    case IOR:
    case XOR:
    case PLUS:
    case LT:
    case LTU:
      /* These instructions take 12-bit signed immediates.  */
      return SMALL_OPERAND (x);

    case LE:
      /* We add 1 to the immediate and use SLT.  */
      return SMALL_OPERAND (x + 1);

    case LEU:
      /* Likewise SLTU, but reject the always-true case.  */
      return SMALL_OPERAND (x + 1) && x + 1 != 0;

    case GE:
    case GEU:
      /* We can emulate an immediate of 1 by using GT/GTU against x0. */
      return x == 1;

    default:
      /* By default assume that x0 can be used for 0.  */
      return x == 0;
    }
}

/* Return the cost of binary operation X, given that the instruction
   sequence for a word-sized or smaller operation has cost SINGLE_COST
   and that the sequence of a double-word operation has cost DOUBLE_COST.
   If SPEED is true, optimize for speed otherwise optimize for size.  */

static int
mips_binary_cost (rtx x, int single_cost, int double_cost, bool speed)
{
  if (GET_MODE_SIZE (GET_MODE (x)) == UNITS_PER_WORD * 2)
    single_cost = double_cost;

  return (single_cost
	  + rtx_cost (XEXP (x, 0), SET, speed)
	  + rtx_cost (XEXP (x, 1), GET_CODE (x), speed));
}

/* Return the cost of floating-point multiplications of mode MODE.  */

static int
mips_fp_mult_cost (enum machine_mode mode)
{
  return mode == DFmode ? mips_cost->fp_mult_df : mips_cost->fp_mult_sf;
}

/* Return the cost of floating-point divisions of mode MODE.  */

static int
mips_fp_div_cost (enum machine_mode mode)
{
  return mode == DFmode ? mips_cost->fp_div_df : mips_cost->fp_div_sf;
}

/* Return the cost of sign-extending OP to mode MODE, not including the
   cost of OP itself.  */

static int
mips_sign_extend_cost (enum machine_mode mode, rtx op)
{
  if (MEM_P (op))
    /* Extended loads are as cheap as unextended ones.  */
    return 0;

  if (TARGET_64BIT && mode == DImode && GET_MODE (op) == SImode)
    /* A sign extension from SImode to DImode in 64-bit mode is free.  */
    return 0;

  /* We need to use a shift left and a shift right.  */
  return COSTS_N_INSNS (2);
}

/* Return the cost of zero-extending OP to mode MODE, not including the
   cost of OP itself.  */

static int
mips_zero_extend_cost (enum machine_mode mode, rtx op)
{
  if (MEM_P (op))
    /* Extended loads are as cheap as unextended ones.  */
    return 0;

  if ((TARGET_64BIT && mode == DImode && GET_MODE (op) == SImode) ||
      ((mode == DImode || mode == SImode) && GET_MODE (op) == HImode))
    /* We need a shift left by 32 bits and a shift right by 32 bits.  */
    return COSTS_N_INSNS (2);

  /* We can use ANDI.  */
  return COSTS_N_INSNS (1);
}

/* Implement TARGET_RTX_COSTS.  */

static bool
mips_rtx_costs (rtx x, int code, int outer_code, int *total, bool speed)
{
  enum machine_mode mode = GET_MODE (x);
  bool float_mode_p = FLOAT_MODE_P (mode);
  int cost;
  rtx addr;

  /* The cost of a COMPARE is hard to define for MIPS.  COMPAREs don't
     appear in the instruction stream, and the cost of a comparison is
     really the cost of the branch or scc condition.  At the time of
     writing, GCC only uses an explicit outer COMPARE code when optabs
     is testing whether a constant is expensive enough to force into a
     register.  We want optabs to pass such constants through the MIPS
     expanders instead, so make all constants very cheap here.  */
  if (outer_code == COMPARE)
    {
      gcc_assert (CONSTANT_P (x));
      *total = 0;
      return true;
    }

  switch (code)
    {
    case CONST_INT:
      /* When not optimizing for size, we care more about the cost
         of hot code, and hot code is often in a loop.  If a constant
         operand needs to be forced into a register, we will often be
         able to hoist the constant load out of the loop, so the load
         should not contribute to the cost.  */
      if (speed || mips_immediate_operand_p (outer_code, INTVAL (x)))
        {
          *total = 0;
          return true;
        }
      /* Fall through.  */

    case CONST:
    case SYMBOL_REF:
    case LABEL_REF:
    case CONST_DOUBLE:
      cost = mips_const_insns (x);
      if (cost > 0)
	{
	  /* If the constant is likely to be stored in a GPR, SETs of
	     single-insn constants are as cheap as register sets; we
	     never want to CSE them.

	     Don't reduce the cost of storing a floating-point zero in
	     FPRs.  If we have a zero in an FPR for other reasons, we
	     can get better cfg-cleanup and delayed-branch results by
	     using it consistently, rather than using $0 sometimes and
	     an FPR at other times.  Also, moves between floating-point
	     registers are sometimes cheaper than (D)MTC1 $0.  */
	  if (cost == 1
	      && outer_code == SET
	      && code == CONST_INT
	      && !(float_mode_p && TARGET_HARD_FLOAT))
	    cost = 0;
	  *total = COSTS_N_INSNS (cost);
	  return true;
	}
      /* The value will need to be fetched from the constant pool.  */
      *total = CONSTANT_POOL_COST;
      return true;

    case MEM:
      /* If the address is legitimate, return the number of
	 instructions it needs.  */
      addr = XEXP (x, 0);
      cost = riscv_address_insns (addr, mode, true);
      if (cost > 0)
	{
	  *total = COSTS_N_INSNS (cost + riscv_memory_latency);
	  return true;
	}
      /* Otherwise use the default handling.  */
      return false;

    case FFS:
      *total = COSTS_N_INSNS (6);
      return false;

    case NOT:
      *total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > UNITS_PER_WORD ? 2 : 1);
      return false;

    case AND:
    case IOR:
    case XOR:
      /* Double-word operations use two single-word operations.  */
      *total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (2),
				 speed);
      return true;

    case ASHIFT:
    case ASHIFTRT:
    case LSHIFTRT:
    case ROTATE:
    case ROTATERT:
      if (CONSTANT_P (XEXP (x, 1)))
	*total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (4),
				   speed);
      else
	*total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (12),
				   speed);
      return true;

    case ABS:
      if (float_mode_p)
        *total = mips_cost->fp_add;
      else
        *total = COSTS_N_INSNS (4);
      return false;

    case LO_SUM:
      *total = (COSTS_N_INSNS (1)
		+ rtx_cost (XEXP (x, 0), SET, speed));
      return true;

    case LT:
    case LTU:
    case LE:
    case LEU:
    case GT:
    case GTU:
    case GE:
    case GEU:
    case EQ:
    case NE:
    case UNORDERED:
    case LTGT:
      /* Branch comparisons have VOIDmode, so use the first operand's
	 mode instead.  */
      mode = GET_MODE (XEXP (x, 0));
      if (FLOAT_MODE_P (mode))
	{
	  *total = mips_cost->fp_add;
	  return false;
	}
      *total = mips_binary_cost (x, COSTS_N_INSNS (1), COSTS_N_INSNS (4),
				 speed);
      return true;

    case MINUS:
      if (float_mode_p
	  && !HONOR_NANS (mode)
	  && !HONOR_SIGNED_ZEROS (mode))
	{
	  /* See if we can use NMADD or NMSUB.  See mips.md for the
	     associated patterns.  */
	  rtx op0 = XEXP (x, 0);
	  rtx op1 = XEXP (x, 1);
	  if (GET_CODE (op0) == MULT && GET_CODE (XEXP (op0, 0)) == NEG)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (XEXP (XEXP (op0, 0), 0), SET, speed)
			+ rtx_cost (XEXP (op0, 1), SET, speed)
			+ rtx_cost (op1, SET, speed));
	      return true;
	    }
	  if (GET_CODE (op1) == MULT)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (op0, SET, speed)
			+ rtx_cost (XEXP (op1, 0), SET, speed)
			+ rtx_cost (XEXP (op1, 1), SET, speed));
	      return true;
	    }
	}
      /* Fall through.  */

    case PLUS:
      if (float_mode_p)
	{
	  /* If this is part of a MADD or MSUB, treat the PLUS as
	     being free.  */
	  if (GET_CODE (XEXP (x, 0)) == MULT)
	    *total = 0;
	  else
	    *total = mips_cost->fp_add;
	  return false;
	}

      /* Double-word operations require three single-word operations and
	 an SLTU.  The MIPS16 version then needs to move the result of
	 the SLTU from $24 to a MIPS16 register.  */
      *total = mips_binary_cost (x, COSTS_N_INSNS (1),
				 COSTS_N_INSNS (4),
				 speed);
      return true;

    case NEG:
      if (float_mode_p
	  && !HONOR_NANS (mode)
	  && HONOR_SIGNED_ZEROS (mode))
	{
	  /* See if we can use NMADD or NMSUB.  See mips.md for the
	     associated patterns.  */
	  rtx op = XEXP (x, 0);
	  if ((GET_CODE (op) == PLUS || GET_CODE (op) == MINUS)
	      && GET_CODE (XEXP (op, 0)) == MULT)
	    {
	      *total = (mips_fp_mult_cost (mode)
			+ rtx_cost (XEXP (XEXP (op, 0), 0), SET, speed)
			+ rtx_cost (XEXP (XEXP (op, 0), 1), SET, speed)
			+ rtx_cost (XEXP (op, 1), SET, speed));
	      return true;
	    }
	}

      if (float_mode_p)
	*total = mips_cost->fp_add;
      else
	*total = COSTS_N_INSNS (GET_MODE_SIZE (mode) > UNITS_PER_WORD ? 4 : 1);
      return false;

    case MULT:
      if (float_mode_p)
	*total = mips_fp_mult_cost (mode);
      else if (mode == DImode && !TARGET_64BIT)
	/* We use a MUL and a MULH[[S]U]. */
	*total = mips_cost->int_mult_si * 2;
      else if (!speed)
	*total = 1;
      else if (mode == DImode)
	*total = mips_cost->int_mult_di;
      else
	*total = mips_cost->int_mult_si;
      return false;

    case DIV:
      /* Check for a reciprocal.  */
      if (float_mode_p
	  && flag_unsafe_math_optimizations
	  && XEXP (x, 0) == CONST1_RTX (mode))
	{
	  if (outer_code == SQRT || GET_CODE (XEXP (x, 1)) == SQRT)
	    /* An rsqrt<mode>a or rsqrt<mode>b pattern.  Count the
	       division as being free.  */
	    *total = rtx_cost (XEXP (x, 1), SET, speed);
	  else
	    *total = (mips_fp_div_cost (mode)
		      + rtx_cost (XEXP (x, 1), SET, speed));
	  return true;
	}
      /* Fall through.  */

    case SQRT:
    case MOD:
      if (float_mode_p)
	{
	  *total = mips_fp_div_cost (mode);
	  return false;
	}
      /* Fall through.  */

    case UDIV:
    case UMOD:
      if (!speed)
	*total = 1;
      else if (mode == DImode)
        *total = mips_cost->int_div_di;
      else
	*total = mips_cost->int_div_si;
      return false;

    case SIGN_EXTEND:
      *total = mips_sign_extend_cost (mode, XEXP (x, 0));
      return false;

    case ZERO_EXTEND:
      *total = mips_zero_extend_cost (mode, XEXP (x, 0));
      return false;

    case FLOAT:
    case UNSIGNED_FLOAT:
    case FIX:
    case FLOAT_EXTEND:
    case FLOAT_TRUNCATE:
      *total = mips_cost->fp_add;
      return false;

    default:
      return false;
    }
}

/* Implement TARGET_ADDRESS_COST.  */

static int
riscv_address_cost (rtx addr, bool speed ATTRIBUTE_UNUSED)
{
  return riscv_address_insns (addr, SImode, false);
}

/* Return one word of double-word value OP.  HIGH_P is true to select the
   high part or false to select the low part. */

rtx
mips_subword (rtx op, bool high_p)
{
  unsigned int byte;
  enum machine_mode mode;

  mode = GET_MODE (op);
  if (mode == VOIDmode)
    mode = TARGET_64BIT ? TImode : DImode;

  byte = high_p ? UNITS_PER_WORD : 0;

  if (FP_REG_RTX_P (op))
    return gen_rtx_REG (word_mode, REGNO (op) + high_p);

  if (MEM_P (op))
    return adjust_address (op, word_mode, byte);

  return simplify_gen_subreg (word_mode, op, mode, byte);
}

/* Return true if a 64-bit move from SRC to DEST should be split into two.  */

bool
mips_split_64bit_move_p (rtx dest, rtx src)
{
  /* All 64b moves are legal in 64b mode.  All 64b FPR <-> FPR and
     FPR <-> MEM moves are legal in 32b mode, too.  Although
     FPR <-> GPR moves are not available in general in 32b mode,
     we can at least load 0 into an FPR with fcvt.d.w fpr, x0. */
  return !(TARGET_64BIT
	   || (FP_REG_RTX_P (src) && FP_REG_RTX_P (dest))
	   || (FP_REG_RTX_P (dest) && MEM_P (src))
	   || (FP_REG_RTX_P (src) && MEM_P (dest))
	   || (FP_REG_RTX_P(dest) && src == CONST0_RTX(GET_MODE(src))));
}

/* Split a doubleword move from SRC to DEST.  On 32-bit targets,
   this function handles 64-bit moves for which mips_split_64bit_move_p
   holds.  For 64-bit targets, this function handles 128-bit moves.  */

void
mips_split_doubleword_move (rtx dest, rtx src)
{
  rtx low_dest;

   /* The operation can be split into two normal moves.  Decide in
      which order to do them.  */
   low_dest = mips_subword (dest, false);
   if (REG_P (low_dest) && reg_overlap_mentioned_p (low_dest, src))
     {
       mips_emit_move (mips_subword (dest, true), mips_subword (src, true));
       mips_emit_move (low_dest, mips_subword (src, false));
     }
   else
     {
       mips_emit_move (low_dest, mips_subword (src, false));
       mips_emit_move (mips_subword (dest, true), mips_subword (src, true));
     }
}

/* Return the appropriate instructions to move SRC into DEST.  Assume
   that SRC is operand 1 and DEST is operand 0.  */

const char *
mips_output_move (rtx dest, rtx src)
{
  enum rtx_code dest_code, src_code;
  enum machine_mode mode;
  bool dbl_p;

  dest_code = GET_CODE (dest);
  src_code = GET_CODE (src);
  mode = GET_MODE (dest);
  dbl_p = (GET_MODE_SIZE (mode) == 8);

  if (dbl_p && mips_split_64bit_move_p (dest, src))
    return "#";

  if ((src_code == REG && GP_REG_P (REGNO (src)))
      || (src == CONST0_RTX (mode)))
    {
      if (dest_code == REG)
	{
	  if (GP_REG_P (REGNO (dest)))
	    return "move\t%0,%z1";

	  if (FP_REG_P (REGNO (dest)))
	    {
	      if (!dbl_p)
		return "fmv.s.x\t%0,%z1";
	      if (TARGET_64BIT)
		return "fmv.d.x\t%0,%z1";
	      /* in RV32, we can emulate fmv.d.x %0, x0 using fcvt.d.w */
	      gcc_assert (src == CONST0_RTX (mode));
	      return "fcvt.d.w\t%0,x0";
	    }
	}
      if (dest_code == MEM)
	switch (GET_MODE_SIZE (mode))
	  {
	  case 1: return "sb\t%z1,%0";
	  case 2: return "sh\t%z1,%0";
	  case 4: return "sw\t%z1,%0";
	  case 8: return "sd\t%z1,%0";
	  }
    }
  if (dest_code == REG && GP_REG_P (REGNO (dest)))
    {
      if (src_code == REG)
	{
	  if (FP_REG_P (REGNO (src)))
	    return dbl_p ? "fmv.x.d\t%0,%1" : "fmv.x.s\t%0,%1";
	}

      if (src_code == MEM)
	switch (GET_MODE_SIZE (mode))
	  {
	  case 1: return "lbu\t%0,%1";
	  case 2: return "lhu\t%0,%1";
	  case 4: return "lw\t%0,%1";
	  case 8: return "ld\t%0,%1";
	  }

      if (src_code == CONST_INT)
	return "li\t%0,%1";

      if (src_code == HIGH)
	return "lui\t%0,%h1";

      if (symbolic_operand (src, VOIDmode))
	{
	  if (SYMBOL_REF_LOCAL_P (src)
	      || (src_code == LABEL_REF && !LABEL_REF_NONLOCAL_P (src)))
	    return "lla\t%0,%1";
	  return "la\t%0,%1";
	}
    }
  if (src_code == REG && FP_REG_P (REGNO (src)))
    {
      if (dest_code == REG && FP_REG_P (REGNO (dest)))
	return dbl_p ? "fmv.d\t%0,%1" : "fmv.s\t%0,%1";

      if (dest_code == MEM)
	return dbl_p ? "fsd\t%1,%0" : "fsw\t%1,%0";
    }
  if (dest_code == REG && FP_REG_P (REGNO (dest)))
    {
      if (src_code == MEM)
	return dbl_p ? "fld\t%0,%1" : "flw\t%0,%1";
    }
  gcc_unreachable ();
}

/* Return true if CMP1 is a suitable second operand for integer ordering
   test CODE.  See also the *sCC patterns in mips.md.  */

static bool
mips_int_order_operand_ok_p (enum rtx_code code, rtx cmp1)
{
  switch (code)
    {
    case GT:
    case GTU:
      return reg_or_0_operand (cmp1, VOIDmode);

    case GE:
    case GEU:
      return cmp1 == const1_rtx;

    case LT:
    case LTU:
      return arith_operand (cmp1, VOIDmode);

    case LE:
      return sle_operand (cmp1, VOIDmode);

    case LEU:
      return sleu_operand (cmp1, VOIDmode);

    default:
      gcc_unreachable ();
    }
}

/* Return true if *CMP1 (of mode MODE) is a valid second operand for
   integer ordering test *CODE, or if an equivalent combination can
   be formed by adjusting *CODE and *CMP1.  When returning true, update
   *CODE and *CMP1 with the chosen code and operand, otherwise leave
   them alone.  */

static bool
mips_canonicalize_int_order_test (enum rtx_code *code, rtx *cmp1,
				  enum machine_mode mode)
{
  HOST_WIDE_INT plus_one;

  if (mips_int_order_operand_ok_p (*code, *cmp1))
    return true;

  if (CONST_INT_P (*cmp1))
    switch (*code)
      {
      case LE:
	plus_one = trunc_int_for_mode (UINTVAL (*cmp1) + 1, mode);
	if (INTVAL (*cmp1) < plus_one)
	  {
	    *code = LT;
	    *cmp1 = force_reg (mode, GEN_INT (plus_one));
	    return true;
	  }
	break;

      case LEU:
	plus_one = trunc_int_for_mode (UINTVAL (*cmp1) + 1, mode);
	if (plus_one != 0)
	  {
	    *code = LTU;
	    *cmp1 = force_reg (mode, GEN_INT (plus_one));
	    return true;
	  }
	break;

      default:
	break;
      }
  return false;
}

/* Compare CMP0 and CMP1 using ordering test CODE and store the result
   in TARGET.  CMP0 and TARGET are register_operands.  If INVERT_PTR
   is nonnull, it's OK to set TARGET to the inverse of the result and
   flip *INVERT_PTR instead.  */

static void
mips_emit_int_order_test (enum rtx_code code, bool *invert_ptr,
			  rtx target, rtx cmp0, rtx cmp1)
{
  enum machine_mode mode;

  /* First see if there is a MIPS instruction that can do this operation.
     If not, try doing the same for the inverse operation.  If that also
     fails, force CMP1 into a register and try again.  */
  mode = GET_MODE (cmp0);
  if (mips_canonicalize_int_order_test (&code, &cmp1, mode))
    mips_emit_binary (code, target, cmp0, cmp1);
  else
    {
      enum rtx_code inv_code = reverse_condition (code);
      if (!mips_canonicalize_int_order_test (&inv_code, &cmp1, mode))
	{
	  cmp1 = force_reg (mode, cmp1);
	  mips_emit_int_order_test (code, invert_ptr, target, cmp0, cmp1);
	}
      else if (invert_ptr == 0)
	{
	  rtx inv_target;

	  inv_target = mips_force_binary (GET_MODE (target),
					  inv_code, cmp0, cmp1);
	  mips_emit_binary (XOR, target, inv_target, const1_rtx);
	}
      else
	{
	  *invert_ptr = !*invert_ptr;
	  mips_emit_binary (inv_code, target, cmp0, cmp1);
	}
    }
}

/* Return a register that is zero iff CMP0 and CMP1 are equal.
   The register will have the same mode as CMP0.  */

static rtx
mips_zero_if_equal (rtx cmp0, rtx cmp1)
{
  if (cmp1 == const0_rtx)
    return cmp0;

  return expand_binop (GET_MODE (cmp0), sub_optab,
		       cmp0, cmp1, 0, 0, OPTAB_DIRECT);
}

/* Convert *CODE into a code that can be used in a floating-point
   scc instruction (C.cond.fmt).  Return true if the values of
   the condition code registers will be inverted, with 0 indicating
   that the condition holds.  */

static bool
mips_reversed_fp_cond (enum rtx_code *code)
{
  switch (*code)
    {
    case NE:
    case LTGT:
    case ORDERED:
      *code = reverse_condition_maybe_unordered (*code);
      return true;

    default:
      return false;
    }
}

/* Convert a comparison into something that can be used in a branch or
   conditional move.  On entry, *OP0 and *OP1 are the values being
   compared and *CODE is the code used to compare them.

   Update *CODE, *OP0 and *OP1 so that they describe the final comparison. */

static void
mips_emit_compare (enum rtx_code *code, rtx *op0, rtx *op1)
{
  rtx cmp_op0 = *op0;
  rtx cmp_op1 = *op1;

  if (GET_MODE_CLASS (GET_MODE (*op0)) == MODE_INT)
    {
      if (*op1 == const0_rtx)
	;
      else if (*code == EQ || *code == NE)
	*op1 = force_reg (GET_MODE (cmp_op0), cmp_op1);
      else
	{
	  /* The comparison needs a separate scc instruction.  Store the
	     result of the scc in *OP0 and compare it against zero.  */
	  bool invert = false;
	  *op0 = gen_reg_rtx (GET_MODE (cmp_op0));
	  mips_emit_int_order_test (*code, &invert, *op0, cmp_op0, cmp_op1);
	  *code = (invert ? EQ : NE);
	  *op1 = const0_rtx;
	}
    }
  else
    {
      enum rtx_code cmp_code;

      /* Floating-point tests use a separate C.cond.fmt comparison to
	 set a condition code register.  The branch or conditional move
	 will then compare that register against zero.

	 Set CMP_CODE to the code of the comparison instruction and
	 *CODE to the code that the branch or move should use.  */
      cmp_code = *code;
      *code = mips_reversed_fp_cond (&cmp_code) ? EQ : NE;
      *op0 = gen_reg_rtx (SImode);
      *op1 = const0_rtx;
      mips_emit_binary (cmp_code, *op0, cmp_op0, cmp_op1);
    }
}

/* Try performing the comparison in OPERANDS[1], whose arms are OPERANDS[2]
   and OPERAND[3].  Store the result in OPERANDS[0].

   On 64-bit targets, the mode of the comparison and target will always be
   SImode, thus possibly narrower than that of the comparison's operands.  */

void
mips_expand_scc (rtx operands[])
{
  rtx target = operands[0];
  enum rtx_code code = GET_CODE (operands[1]);
  rtx op0 = operands[2];
  rtx op1 = operands[3];

  gcc_assert (GET_MODE_CLASS (GET_MODE (op0)) == MODE_INT);

  if (code == EQ || code == NE)
    {
      rtx zie = mips_zero_if_equal (op0, op1);
      mips_emit_binary (code, target, zie, const0_rtx);
    }
  else
    mips_emit_int_order_test (code, 0, target, op0, op1);
}

/* Compare OPERANDS[1] with OPERANDS[2] using comparison code
   CODE and jump to OPERANDS[3] if the condition holds.  */

void
mips_expand_conditional_branch (rtx *operands)
{
  enum rtx_code code = GET_CODE (operands[0]);
  rtx op0 = operands[1];
  rtx op1 = operands[2];
  rtx condition;

  mips_emit_compare (&code, &op0, &op1);
  condition = gen_rtx_fmt_ee (code, VOIDmode, op0, op1);
  emit_jump_insn (gen_condjump (condition, operands[3]));
}

/* Initialize *CUM for a call to a function of type FNTYPE.  */

void
mips_init_cumulative_args (CUMULATIVE_ARGS *cum, tree fntype ATTRIBUTE_UNUSED)
{
  memset (cum, 0, sizeof (*cum));
}

/* Fill INFO with information about a single argument.  CUM is the
   cumulative state for earlier arguments.  MODE is the mode of this
   argument and TYPE is its type (if known).  NAMED is true if this
   is a named (fixed) argument rather than a variable one.  */

static void
mips_get_arg_info (struct mips_arg_info *info, const CUMULATIVE_ARGS *cum,
		   enum machine_mode mode, const_tree type, bool named)
{
  bool doubleword_aligned_p;
  unsigned int num_bytes, num_words, max_regs;

  /* Work out the size of the argument.  */
  num_bytes = type ? int_size_in_bytes (type) : GET_MODE_SIZE (mode);
  num_words = (num_bytes + UNITS_PER_WORD - 1) / UNITS_PER_WORD;

  /* Scalar, complex and vector floating-point types are passed in
     floating-point registers, as long as this is a named rather
     than a variable argument.  */
  info->fpr_p = (named
		 && (type == 0 || FLOAT_TYPE_P (type))
		 && (GET_MODE_CLASS (mode) == MODE_FLOAT
		     || GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT
		     || GET_MODE_CLASS (mode) == MODE_VECTOR_FLOAT)
		 && GET_MODE_UNIT_SIZE (mode) <= UNITS_PER_FPVALUE);

  /* Complex floats should only go into FPRs if there are two FPRs free,
     otherwise they should be passed in the same way as a struct
     containing two floats.  */
  if (info->fpr_p
      && GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT
      && GET_MODE_UNIT_SIZE (mode) < UNITS_PER_FPVALUE)
    {
      if (cum->num_gprs >= MAX_ARGS_IN_REGISTERS - 1)
        info->fpr_p = false;
      else
        num_words = 2;
    }

  /* See whether the argument has doubleword alignment.  */
  doubleword_aligned_p = (mips_function_arg_boundary (mode, type)
			  > BITS_PER_WORD);

  /* Set REG_OFFSET to the register count we're interested in.
     The EABI allocates the floating-point registers separately,
     but the other ABIs allocate them like integer registers.  */
  info->reg_offset = cum->num_gprs;

  /* Advance to an even register if the argument is doubleword-aligned.  */
  if (doubleword_aligned_p)
    info->reg_offset += info->reg_offset & 1;

  /* Work out the offset of a stack argument.  */
  info->stack_offset = cum->stack_words;
  if (doubleword_aligned_p)
    info->stack_offset += info->stack_offset & 1;

  max_regs = MAX_ARGS_IN_REGISTERS - info->reg_offset;

  /* Partition the argument between registers and stack.  */
  info->reg_words = MIN (num_words, max_regs);
  info->stack_words = num_words - info->reg_words;
}

/* INFO describes a register argument that has the normal format for the
   argument's mode.  Return the register it uses, assuming that FPRs are
   available if HARD_FLOAT_P.  */

static unsigned int
mips_arg_regno (const struct mips_arg_info *info, bool hard_float_p)
{
  if (!info->fpr_p || !hard_float_p)
    return GP_ARG_FIRST + info->reg_offset;
  else
    return FP_ARG_FIRST + info->reg_offset;
}

/* Implement TARGET_FUNCTION_ARG.  */

static rtx
mips_function_arg (CUMULATIVE_ARGS *cum, enum machine_mode mode,
		   const_tree type, bool named)
{
  struct mips_arg_info info;

  if (mode == VOIDmode)
    return NULL;

  mips_get_arg_info (&info, cum, mode, type, named);

  /* Return straight away if the whole argument is passed on the stack.  */
  if (info.reg_offset == MAX_ARGS_IN_REGISTERS)
    return NULL;

  /* The n32 and n64 ABIs say that if any 64-bit chunk of the structure
     contains a double in its entirety, then that 64-bit chunk is passed
     in a floating-point register.  */
  if (TARGET_HARD_FLOAT
      && named
      && type != 0
      && TREE_CODE (type) == RECORD_TYPE
      && TYPE_SIZE_UNIT (type)
      && host_integerp (TYPE_SIZE_UNIT (type), 1))
    {
      tree field;

      /* First check to see if there is any such field.  */
      for (field = TYPE_FIELDS (type); field; field = DECL_CHAIN (field))
	if (TREE_CODE (field) == FIELD_DECL
	    && SCALAR_FLOAT_TYPE_P (TREE_TYPE (field))
	    && TYPE_PRECISION (TREE_TYPE (field)) == BITS_PER_WORD
	    && host_integerp (bit_position (field), 0)
	    && int_bit_position (field) % BITS_PER_WORD == 0)
	  break;

      if (field != 0)
	{
	  /* Now handle the special case by returning a PARALLEL
	     indicating where each 64-bit chunk goes.  INFO.REG_WORDS
	     chunks are passed in registers.  */
	  unsigned int i;
	  HOST_WIDE_INT bitpos;
	  rtx ret;

	  /* assign_parms checks the mode of ENTRY_PARM, so we must
	     use the actual mode here.  */
	  ret = gen_rtx_PARALLEL (mode, rtvec_alloc (info.reg_words));

	  bitpos = 0;
	  field = TYPE_FIELDS (type);
	  for (i = 0; i < info.reg_words; i++)
	    {
	      rtx reg;

	      for (; field; field = DECL_CHAIN (field))
		if (TREE_CODE (field) == FIELD_DECL
		    && int_bit_position (field) >= bitpos)
		  break;

	      if (field
		  && int_bit_position (field) == bitpos
		  && SCALAR_FLOAT_TYPE_P (TREE_TYPE (field))
		  && TYPE_PRECISION (TREE_TYPE (field)) == BITS_PER_WORD)
		reg = gen_rtx_REG (DFmode, FP_ARG_FIRST + info.reg_offset + i);
	      else
		reg = gen_rtx_REG (DImode, GP_ARG_FIRST + info.reg_offset + i);

	      XVECEXP (ret, 0, i)
		= gen_rtx_EXPR_LIST (VOIDmode, reg,
				     GEN_INT (bitpos / BITS_PER_UNIT));

	      bitpos += BITS_PER_WORD;
	    }
	  return ret;
	}
    }

  /* Handle the n32/n64 conventions for passing complex floating-point
     arguments in FPR pairs.  The real part goes in the lower register
     and the imaginary part goes in the upper register.  */
  if (info.fpr_p
      && GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
    {
      rtx real, imag;
      enum machine_mode inner;
      unsigned int regno;

      inner = GET_MODE_INNER (mode);
      regno = FP_ARG_FIRST + info.reg_offset;
      if (info.reg_words * UNITS_PER_WORD == GET_MODE_SIZE (inner))
	{
	  /* Real part in registers, imaginary part on stack.  */
	  gcc_assert (info.stack_words == info.reg_words);
	  return gen_rtx_REG (inner, regno);
	}
      else
	{
	  gcc_assert (info.stack_words == 0);
	  real = gen_rtx_EXPR_LIST (VOIDmode,
				    gen_rtx_REG (inner, regno),
				    const0_rtx);
	  imag = gen_rtx_EXPR_LIST (VOIDmode,
				    gen_rtx_REG (inner,
						 regno + info.reg_words / 2),
				    GEN_INT (GET_MODE_SIZE (inner)));
	  return gen_rtx_PARALLEL (mode, gen_rtvec (2, real, imag));
	}
    }

  return gen_rtx_REG (mode, mips_arg_regno (&info, TARGET_HARD_FLOAT));
}

/* Implement TARGET_FUNCTION_ARG_ADVANCE.  */

static void
mips_function_arg_advance (CUMULATIVE_ARGS *cum, enum machine_mode mode,
			   const_tree type, bool named)
{
  struct mips_arg_info info;

  mips_get_arg_info (&info, cum, mode, type, named);

  /* Advance the register count.  This has the effect of setting
     num_gprs to MAX_ARGS_IN_REGISTERS if a doubleword-aligned
     argument required us to skip the final GPR and pass the whole
     argument on the stack.  */
  cum->num_gprs = info.reg_offset + info.reg_words;

  /* Advance the stack word count.  */
  if (info.stack_words > 0)
    cum->stack_words = info.stack_offset + info.stack_words;
}

/* Implement TARGET_ARG_PARTIAL_BYTES.  */

static int
mips_arg_partial_bytes (CUMULATIVE_ARGS *cum,
			enum machine_mode mode, tree type, bool named)
{
  struct mips_arg_info info;

  mips_get_arg_info (&info, cum, mode, type, named);
  return info.stack_words > 0 ? info.reg_words * UNITS_PER_WORD : 0;
}

/* Implement TARGET_FUNCTION_ARG_BOUNDARY.  Every parameter gets at
   least PARM_BOUNDARY bits of alignment, but will be given anything up
   to STACK_BOUNDARY bits if the type requires it.  */

static unsigned int
mips_function_arg_boundary (enum machine_mode mode, const_tree type)
{
  unsigned int alignment;

  alignment = type ? TYPE_ALIGN (type) : GET_MODE_ALIGNMENT (mode);
  if (alignment < PARM_BOUNDARY)
    alignment = PARM_BOUNDARY;
  if (alignment > STACK_BOUNDARY)
    alignment = STACK_BOUNDARY;
  return alignment;
}

/* See whether VALTYPE is a record whose fields should be returned in
   floating-point registers.  If so, return the number of fields and
   list them in FIELDS (which should have two elements).  Return 0
   otherwise.

   For n32 & n64, a structure with one or two fields is returned in
   floating-point registers as long as every field has a floating-point
   type.  */

static int
mips_fpr_return_fields (const_tree valtype, tree *fields)
{
  tree field;
  int i;

  if (TREE_CODE (valtype) != RECORD_TYPE)
    return 0;

  i = 0;
  for (field = TYPE_FIELDS (valtype); field != 0; field = DECL_CHAIN (field))
    {
      if (TREE_CODE (field) != FIELD_DECL)
	continue;

      if (!SCALAR_FLOAT_TYPE_P (TREE_TYPE (field)))
	return 0;

      if (i == 2)
	return 0;

      fields[i++] = field;
    }
  return i;
}

/* Return true if the function return value MODE will get returned in a
   floating-point register.  */

static bool
mips_return_mode_in_fpr_p (enum machine_mode mode)
{
  return ((GET_MODE_CLASS (mode) == MODE_FLOAT
	   || GET_MODE_CLASS (mode) == MODE_VECTOR_FLOAT
	   || GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
	  && GET_MODE_UNIT_SIZE (mode) <= UNITS_PER_HWFPVALUE);
}

/* Return the representation of an FPR return register when the
   value being returned in FP_RETURN has mode VALUE_MODE and the
   return type itself has mode TYPE_MODE.  On NewABI targets,
   the two modes may be different for structures like:

       struct __attribute__((packed)) foo { float f; }

   where we return the SFmode value of "f" in FP_RETURN, but where
   the structure itself has mode BLKmode.  */

static rtx
mips_return_fpr_single (enum machine_mode type_mode,
			enum machine_mode value_mode)
{
  rtx x;

  x = gen_rtx_REG (value_mode, FP_RETURN);
  if (type_mode != value_mode)
    {
      x = gen_rtx_EXPR_LIST (VOIDmode, x, const0_rtx);
      x = gen_rtx_PARALLEL (type_mode, gen_rtvec (1, x));
    }
  return x;
}

/* Return a composite value in a pair of floating-point registers.
   MODE1 and OFFSET1 are the mode and byte offset for the first value,
   likewise MODE2 and OFFSET2 for the second.  MODE is the mode of the
   complete value.

   For n32 & n64, $f0 always holds the first value and $f2 the second.
   Otherwise the values are packed together as closely as possible.  */

static rtx
mips_return_fpr_pair (enum machine_mode mode,
		      enum machine_mode mode1, HOST_WIDE_INT offset1,
		      enum machine_mode mode2, HOST_WIDE_INT offset2)
{
  return gen_rtx_PARALLEL
    (mode,
     gen_rtvec (2,
		gen_rtx_EXPR_LIST (VOIDmode,
				   gen_rtx_REG (mode1, FP_RETURN),
				   GEN_INT (offset1)),
		gen_rtx_EXPR_LIST (VOIDmode,
				   gen_rtx_REG (mode2, FP_RETURN + 1),
				   GEN_INT (offset2))));

}

/* Implement FUNCTION_VALUE and LIBCALL_VALUE.  For normal calls,
   VALTYPE is the return type and MODE is VOIDmode.  For libcalls,
   VALTYPE is null and MODE is the mode of the return value.  */

rtx
mips_function_value (const_tree valtype, const_tree func, enum machine_mode mode)
{
  if (valtype)
    {
      tree fields[2];
      int unsigned_p;

      mode = TYPE_MODE (valtype);
      unsigned_p = TYPE_UNSIGNED (valtype);

      /* Since TARGET_PROMOTE_FUNCTION_MODE unconditionally promotes,
	 return values, promote the mode here too.  */
      mode = promote_function_mode (valtype, mode, &unsigned_p, func, 1);

      /* Handle structures whose fields are returned in $f0/$f2.  */
      switch (mips_fpr_return_fields (valtype, fields))
	{
	case 1:
	  return mips_return_fpr_single (mode,
					 TYPE_MODE (TREE_TYPE (fields[0])));

	case 2:
	  return mips_return_fpr_pair (mode,
				       TYPE_MODE (TREE_TYPE (fields[0])),
				       int_byte_position (fields[0]),
				       TYPE_MODE (TREE_TYPE (fields[1])),
				       int_byte_position (fields[1]));
	}

      /* Only use FPRs for scalar, complex or vector types.  */
      if (!FLOAT_TYPE_P (valtype))
	return gen_rtx_REG (mode, GP_RETURN);
    }

  /* Handle long doubles for n32 & n64.  */
  if (mode == TFmode)
    return mips_return_fpr_pair (mode,
    			     DImode, 0,
    			     DImode, GET_MODE_SIZE (mode) / 2);

  if (mips_return_mode_in_fpr_p (mode))
    {
      if (GET_MODE_CLASS (mode) == MODE_COMPLEX_FLOAT)
        return mips_return_fpr_pair (mode,
    				 GET_MODE_INNER (mode), 0,
    				 GET_MODE_INNER (mode),
    				 GET_MODE_SIZE (mode) / 2);
      else
        return gen_rtx_REG (mode, FP_RETURN);
    }

  return gen_rtx_REG (mode, GP_RETURN);
}

/* Implement TARGET_RETURN_IN_MEMORY.  Scalars and small structures
   that fit in two registers are returned in v0/v1. */

static bool
mips_return_in_memory (const_tree type, const_tree fndecl ATTRIBUTE_UNUSED)
{
  return !IN_RANGE (int_size_in_bytes (type), 0, 2 * UNITS_PER_WORD);
}

/* Implement TARGET_PASS_BY_REFERENCE. */

static bool
riscv_pass_by_reference (CUMULATIVE_ARGS *cum ATTRIBUTE_UNUSED,
			  enum machine_mode mode, const_tree type,
			  bool named ATTRIBUTE_UNUSED)
{
  if (type && mips_return_in_memory (type, NULL_TREE))
    return true;
  return targetm.calls.must_pass_in_stack (mode, type);
}

/* Implement TARGET_SETUP_INCOMING_VARARGS.  */

static void
mips_setup_incoming_varargs (CUMULATIVE_ARGS *cum, enum machine_mode mode,
			     tree type, int *pretend_size ATTRIBUTE_UNUSED,
			     int no_rtl)
{
  CUMULATIVE_ARGS local_cum;
  int gp_saved;

  /* The caller has advanced CUM up to, but not beyond, the last named
     argument.  Advance a local copy of CUM past the last "real" named
     argument, to find out how many registers are left over.  */
  local_cum = *cum;
  mips_function_arg_advance (&local_cum, mode, type, true);

  /* Found out how many registers we need to save.  */
  gp_saved = MAX_ARGS_IN_REGISTERS - local_cum.num_gprs;

  if (!no_rtl && gp_saved > 0)
    {
      rtx ptr, mem;

      ptr = plus_constant (virtual_incoming_args_rtx,
			   REG_PARM_STACK_SPACE (cfun->decl)
			   - gp_saved * UNITS_PER_WORD);
      mem = gen_frame_mem (BLKmode, ptr);
      set_mem_alias_set (mem, get_varargs_alias_set ());

      move_block_from_reg (local_cum.num_gprs + GP_ARG_FIRST,
			   mem, gp_saved);
    }
  if (REG_PARM_STACK_SPACE (cfun->decl) == 0)
    cfun->machine->varargs_size = gp_saved * UNITS_PER_WORD;
}

/* Implement TARGET_EXPAND_BUILTIN_VA_START.  */

static void
mips_va_start (tree valist, rtx nextarg)
{
  nextarg = plus_constant (nextarg, -cfun->machine->varargs_size);
  std_expand_builtin_va_start (valist, nextarg);
}

/* Expand a call of type TYPE.  RESULT is where the result will go (null
   for "call"s and "sibcall"s), ADDR is the address of the function,
   ARGS_SIZE is the size of the arguments and AUX is the value passed
   to us by mips_function_arg.  LAZY_P is true if this call already
   involves a lazily-bound function address (such as when calling
   functions through a MIPS16 hard-float stub).

   Return the call itself.  */

rtx
mips_expand_call (bool sibcall_p, rtx result, rtx addr, rtx args_size)
{
  rtx pattern;

  if (!call_insn_operand (addr, VOIDmode))
    {
      rtx reg = MIPS_EPILOGUE_TEMP (Pmode);
      mips_emit_move (reg, addr);
      addr = reg;
    }

  if (result == 0)
    {
      rtx (*fn) (rtx, rtx);

      if (sibcall_p)
	fn = gen_sibcall_internal;
      else
	fn = gen_call_internal;

      pattern = fn (addr, args_size);
    }
  else if (GET_CODE (result) == PARALLEL && XVECLEN (result, 0) == 2)
    {
      /* Handle return values created by mips_return_fpr_pair.  */
      rtx (*fn) (rtx, rtx, rtx, rtx);
      rtx reg1, reg2;

      if (sibcall_p)
	fn = gen_sibcall_value_multiple_internal;
      else
	fn = gen_call_value_multiple_internal;

      reg1 = XEXP (XVECEXP (result, 0, 0), 0);
      reg2 = XEXP (XVECEXP (result, 0, 1), 0);
      pattern = fn (reg1, addr, args_size, reg2);
    }
  else
    {
      rtx (*fn) (rtx, rtx, rtx);

      if (sibcall_p)
	fn = gen_sibcall_value_internal;
      else
	fn = gen_call_value_internal;

      /* Handle return values created by mips_return_fpr_single.  */
      if (GET_CODE (result) == PARALLEL && XVECLEN (result, 0) == 1)
	result = XEXP (XVECEXP (result, 0, 0), 0);
      pattern = fn (result, addr, args_size);
    }

  return emit_call_insn (pattern);
}

/* Emit straight-line code to move LENGTH bytes from SRC to DEST.
   Assume that the areas do not overlap.  */

static void
mips_block_move_straight (rtx dest, rtx src, HOST_WIDE_INT length)
{
  HOST_WIDE_INT offset, delta;
  unsigned HOST_WIDE_INT bits;
  int i;
  enum machine_mode mode;
  rtx *regs;

  bits = MAX( BITS_PER_UNIT,
             MIN( BITS_PER_WORD, MIN( MEM_ALIGN(src),MEM_ALIGN(dest) ) ) );

  mode = mode_for_size (bits, MODE_INT, 0);
  delta = bits / BITS_PER_UNIT;

  /* Allocate a buffer for the temporary registers.  */
  regs = XALLOCAVEC (rtx, length / delta);

  /* Load as many BITS-sized chunks as possible.  Use a normal load if
     the source has enough alignment, otherwise use left/right pairs.  */
  for (offset = 0, i = 0; offset + delta <= length; offset += delta, i++)
    {
      regs[i] = gen_reg_rtx (mode);
	mips_emit_move (regs[i], adjust_address (src, mode, offset));
    }

  /* Copy the chunks to the destination.  */
  for (offset = 0, i = 0; offset + delta <= length; offset += delta, i++)
      mips_emit_move (adjust_address (dest, mode, offset), regs[i]);

  /* Mop up any left-over bytes.  */
  if (offset < length)
    {
      src = adjust_address (src, BLKmode, offset);
      dest = adjust_address (dest, BLKmode, offset);
      move_by_pieces (dest, src, length - offset,
		      MIN (MEM_ALIGN (src), MEM_ALIGN (dest)), 0);
    }
}

/* Helper function for doing a loop-based block operation on memory
   reference MEM.  Each iteration of the loop will operate on LENGTH
   bytes of MEM.

   Create a new base register for use within the loop and point it to
   the start of MEM.  Create a new memory reference that uses this
   register.  Store them in *LOOP_REG and *LOOP_MEM respectively.  */

static void
mips_adjust_block_mem (rtx mem, HOST_WIDE_INT length,
		       rtx *loop_reg, rtx *loop_mem)
{
  *loop_reg = copy_addr_to_reg (XEXP (mem, 0));

  /* Although the new mem does not refer to a known location,
     it does keep up to LENGTH bytes of alignment.  */
  *loop_mem = change_address (mem, BLKmode, *loop_reg);
  set_mem_align (*loop_mem, MIN (MEM_ALIGN (mem), length * BITS_PER_UNIT));
}

/* Move LENGTH bytes from SRC to DEST using a loop that moves BYTES_PER_ITER
   bytes at a time.  LENGTH must be at least BYTES_PER_ITER.  Assume that
   the memory regions do not overlap.  */

static void
mips_block_move_loop (rtx dest, rtx src, HOST_WIDE_INT length,
		      HOST_WIDE_INT bytes_per_iter)
{
  rtx label, src_reg, dest_reg, final_src, test;
  HOST_WIDE_INT leftover;

  leftover = length % bytes_per_iter;
  length -= leftover;

  /* Create registers and memory references for use within the loop.  */
  mips_adjust_block_mem (src, bytes_per_iter, &src_reg, &src);
  mips_adjust_block_mem (dest, bytes_per_iter, &dest_reg, &dest);

  /* Calculate the value that SRC_REG should have after the last iteration
     of the loop.  */
  final_src = expand_simple_binop (Pmode, PLUS, src_reg, GEN_INT (length),
				   0, 0, OPTAB_WIDEN);

  /* Emit the start of the loop.  */
  label = gen_label_rtx ();
  emit_label (label);

  /* Emit the loop body.  */
  mips_block_move_straight (dest, src, bytes_per_iter);

  /* Move on to the next block.  */
  mips_emit_move (src_reg, plus_constant (src_reg, bytes_per_iter));
  mips_emit_move (dest_reg, plus_constant (dest_reg, bytes_per_iter));

  /* Emit the loop condition.  */
  test = gen_rtx_NE (VOIDmode, src_reg, final_src);
  if (Pmode == DImode)
    emit_jump_insn (gen_cbranchdi4 (test, src_reg, final_src, label));
  else
    emit_jump_insn (gen_cbranchsi4 (test, src_reg, final_src, label));

  /* Mop up any left-over bytes.  */
  if (leftover)
    mips_block_move_straight (dest, src, leftover);
}

/* Expand a movmemsi instruction, which copies LENGTH bytes from
   memory reference SRC to memory reference DEST.  */

bool
mips_expand_block_move (rtx dest, rtx src, rtx length)
{
  if (CONST_INT_P (length))
    {
      HOST_WIDE_INT factor, align;
      
      align = MIN (MIN (MEM_ALIGN (src), MEM_ALIGN (dest)), BITS_PER_WORD);
      factor = BITS_PER_WORD / align;

      if (INTVAL (length) <= MIPS_MAX_MOVE_BYTES_STRAIGHT / factor)
	{
	  mips_block_move_straight (dest, src, INTVAL (length));
	  return true;
	}
      else if (optimize && align >= BITS_PER_WORD)
	{
	  mips_block_move_loop (dest, src, INTVAL (length),
				MIPS_MAX_MOVE_BYTES_PER_LOOP_ITER / factor);
	  return true;
	}
    }
  return false;
}

/* (Re-)Initialize mips_lo_relocs and mips_hi_relocs.  */

static void
mips_init_relocs (void)
{
  memset (mips_hi_relocs, '\0', sizeof (mips_hi_relocs));
  memset (mips_lo_relocs, '\0', sizeof (mips_lo_relocs));

  if (!flag_pic)
    {
      mips_hi_relocs[SYMBOL_ABSOLUTE] = "%hi(";
      mips_lo_relocs[SYMBOL_ABSOLUTE] = "%lo(";

      mips_hi_relocs[SYMBOL_TLS_LE] = "%tprel_hi(";
      mips_lo_relocs[SYMBOL_TLS_LE] = "%tprel_lo(";

      mips_lo_relocs[SYMBOL_TLS_IE] = "%tls_ie_off(";
    }
}

/* Print symbolic operand OP, which is part of a HIGH or LO_SUM
   in context CONTEXT.  RELOCS is the array of relocations to use.  */

static void
mips_print_operand_reloc (FILE *file, rtx op, const char **relocs)
{
  enum mips_symbol_type symbol_type;
  const char *p;

  symbol_type = mips_classify_symbolic_expression (op);
  gcc_assert (relocs[symbol_type]);

  fputs (relocs[symbol_type], file);
  output_addr_const (file, mips_strip_unspec_address (op));
  for (p = relocs[symbol_type]; *p != 0; p++)
    if (*p == '(')
      fputc (')', file);
}

/* Implement TARGET_PRINT_OPERAND.  The MIPS-specific operand codes are:

   'h'	Print the high-part relocation associated with OP, after stripping
	  any outermost HIGH.
   'R'	Print the low-part relocation associated with OP.
   'C'	Print the integer branch condition for comparison OP.
   'N'	Print the inverse of the integer branch condition for comparison OP.
   'S'	Print the swapped integer branch condition for comparison OP.
   'z'	Print $0 if OP is zero, otherwise print OP normally.  */

static void
mips_print_operand (FILE *file, rtx op, int letter)
{
  enum rtx_code code;

  gcc_assert (op);
  code = GET_CODE (op);

  switch (letter)
    {
    case 'h':
      if (code == HIGH)
	op = XEXP (op, 0);
      mips_print_operand_reloc (file, op, mips_hi_relocs);
      break;

    case 'R':
      mips_print_operand_reloc (file, op, mips_lo_relocs);
      break;

    case 'C':
      /* The RTL names match the instruction names. */
      fputs (GET_RTX_NAME (code), file);
      break;

    case 'N':
      fputs (GET_RTX_NAME (reverse_condition (code)), file);
      break;

    case 'S':
      fputs (GET_RTX_NAME (swap_condition (code)), file);
      break;

    default:
      switch (code)
	{
	case REG:
	  if (letter && letter != 'z')
	    output_operand_lossage ("invalid use of '%%%c'", letter);
	  fprintf (file, "%s", reg_names[REGNO (op)]);
	  break;

	case MEM:
	  if (letter == 'y')
	    fprintf (file, "%s", reg_names[REGNO(XEXP(op, 0))]);
	  else if (letter && letter != 'z')
	    output_operand_lossage ("invalid use of '%%%c'", letter);
	  else
	    output_address (XEXP (op, 0));
	  break;

	default:
	  if (letter == 'z' && op == CONST0_RTX (GET_MODE (op)))
	    fputs (reg_names[GP_REG_FIRST], file);
	  else if (letter && letter != 'z')
	    output_operand_lossage ("invalid use of '%%%c'", letter);
	  else
	    output_addr_const (file, mips_strip_unspec_address (op));
	  break;
	}
    }
}

/* Implement TARGET_PRINT_OPERAND_ADDRESS.  */

static void
mips_print_operand_address (FILE *file, rtx x)
{
  struct mips_address_info addr;

  if (mips_classify_address (&addr, x, word_mode, true))
    switch (addr.type)
      {
      case ADDRESS_REG:
	mips_print_operand (file, addr.offset, 0);
	fprintf (file, "(%s)", reg_names[REGNO (addr.reg)]);
	return;

      case ADDRESS_LO_SUM:
	mips_print_operand_reloc (file, addr.offset, mips_lo_relocs);
	fprintf (file, "(%s)", reg_names[REGNO (addr.reg)]);
	return;

      case ADDRESS_CONST_INT:
	output_addr_const (file, x);
	fprintf (file, "(%s)", reg_names[GP_REG_FIRST]);
	return;

      case ADDRESS_SYMBOLIC:
	output_addr_const (file, mips_strip_unspec_address (x));
	return;
      }
  gcc_unreachable ();
}

bool
riscv_size_ok_for_small_data_p (int size)
{
  return g_switch_value && IN_RANGE (size, 1, g_switch_value);
}

/* Return true if EXP should be placed in the small data section. */

static bool
riscv_in_small_data_p (const_tree x)
{
  if (TREE_CODE (x) == STRING_CST || TREE_CODE (x) == FUNCTION_DECL)
    return false;

  if (TREE_CODE (x) == VAR_DECL && DECL_SECTION_NAME (x))
    {
      const char *sec = TREE_STRING_POINTER (DECL_SECTION_NAME (x));
      return strcmp (sec, ".sdata") == 0 || strcmp (sec, ".sbss") == 0;
    }

  return riscv_size_ok_for_small_data_p (int_size_in_bytes (TREE_TYPE (x)));
}

/* Return a section for X, handling small data. */

static section *
riscv_elf_select_rtx_section (enum machine_mode mode, rtx x,
			      unsigned HOST_WIDE_INT align)
{
  section *s = default_elf_select_rtx_section (mode, x, align);

  if (riscv_size_ok_for_small_data_p (GET_MODE_SIZE (mode)))
    {
      if (strncmp (s->named.name, ".rodata.cst", strlen (".rodata.cst")) == 0)
	{
	  /* Rename .rodata.cst* to .srodata.cst*. */
	  char name[32];
	  sprintf (name, ".s%s", s->named.name + 1);
	  return get_section (name, s->named.common.flags, NULL);
	}

      if (s == data_section)
	return sdata_section;
    }

  return s;
}

/* The MIPS debug format wants all automatic variables and arguments
   to be in terms of the virtual frame pointer (stack pointer before
   any adjustment in the function), while the MIPS 3.0 linker wants
   the frame pointer to be the stack pointer after the initial
   adjustment.  So, we do the adjustment here.  The arg pointer (which
   is eliminated) points to the virtual frame pointer, while the frame
   pointer (which may be eliminated) points to the stack pointer after
   the initial adjustments.  */

HOST_WIDE_INT
mips_debugger_offset (rtx addr, HOST_WIDE_INT offset)
{
  rtx offset2 = const0_rtx;
  rtx reg = eliminate_constant_term (addr, &offset2);

  if (offset == 0)
    offset = INTVAL (offset2);
  
  if (reg == stack_pointer_rtx)
    offset -= cfun->machine->frame.total_size;
  else
    gcc_assert (reg == frame_pointer_rtx || reg == hard_frame_pointer_rtx);

  return offset;
}

/* Implement TARGET_ASM_OUTPUT_DWARF_DTPREL.  */

static void ATTRIBUTE_UNUSED
mips_output_dwarf_dtprel (FILE *file, int size, rtx x)
{
  switch (size)
    {
    case 4:
      fputs ("\t.dtprelword\t", file);
      break;

    case 8:
      fputs ("\t.dtpreldword\t", file);
      break;

    default:
      gcc_unreachable ();
    }
  output_addr_const (file, x);
  fputs ("+0x8000", file);
}

/* Make the last instruction frame-related and note that it performs
   the operation described by FRAME_PATTERN.  */

static void
mips_set_frame_expr (rtx frame_pattern)
{
  rtx insn;

  insn = get_last_insn ();
  RTX_FRAME_RELATED_P (insn) = 1;
  REG_NOTES (insn) = alloc_EXPR_LIST (REG_FRAME_RELATED_EXPR,
				      frame_pattern,
				      REG_NOTES (insn));
}

/* Return a frame-related rtx that stores REG at MEM.
   REG must be a single register.  */

static rtx
mips_frame_set (rtx mem, rtx reg)
{
  rtx set;

  set = gen_rtx_SET (VOIDmode, mem, reg);
  RTX_FRAME_RELATED_P (set) = 1;

  return set;
}

/* Return true if the current function must save register REGNO.  */

static bool
mips_save_reg_p (unsigned int regno)
{
  bool call_saved = !global_regs[regno] && !call_really_used_regs[regno];
  bool might_clobber = crtl->saves_all_registers
		       || df_regs_ever_live_p (regno)
		       || (regno == HARD_FRAME_POINTER_REGNUM
			   && frame_pointer_needed);

  return (call_saved && might_clobber)
	 || (regno == RETURN_ADDR_REGNUM && crtl->calls_eh_return);
}

/* Populate the current function's riscv_frame_info structure.

   RISC-V stack frames grown downward.  High addresses are at the top.

	+-------------------------------+
	|                               |
	|  incoming stack arguments     |
	|                               |
	+-------------------------------+
	|                               |
	|  caller-allocated save area   |
      A |  for register arguments       |
	|                               |
	+-------------------------------+ <-- incoming stack pointer
	|                               |
	|  callee-allocated save area   |
      B |  for arguments that are       |
	|  split between registers and  |
	|  the stack                    |
	|                               |
	+-------------------------------+ <-- arg_pointer_rtx
	|                               |
      C |  callee-allocated save area   |
	|  for register varargs         |
	|                               |
	+-------------------------------+ <-- hard_frame_pointer_rtx;
	|                               |     stack_pointer_rtx + gp_sp_offset
	|  GPR save area                |       + UNITS_PER_WORD
	|                               |
	+-------------------------------+ <-- stack_pointer_rtx + fp_sp_offset
	|                               |       + UNITS_PER_HWVALUE
	|  FPR save area                |
	|                               |
	+-------------------------------+ <-- frame_pointer_rtx (virtual)
	|                               |
	|  local variables              |
	|                               |
      P +-------------------------------+
	|                               |
	|  outgoing stack arguments     |
	|                               |
	+-------------------------------+
	|                               |
	|  caller-allocated save area   |
	|  for register arguments       |
	|                               |
	+-------------------------------+ <-- stack_pointer_rtx

   At least two of A, B and C will be empty.

   Dynamic stack allocations such as alloca insert data at point P.
   They decrease stack_pointer_rtx but leave frame_pointer_rtx and
   hard_frame_pointer_rtx unchanged.  */

static void
riscv_compute_frame_info (void)
{
  struct riscv_frame_info *frame;
  HOST_WIDE_INT offset;
  unsigned int regno, i;

  frame = &cfun->machine->frame;
  memset (frame, 0, sizeof (*frame));

  /* Find out which GPRs we need to save.  */
  for (regno = GP_REG_FIRST; regno <= GP_REG_LAST; regno++)
    if (mips_save_reg_p (regno))
      frame->mask |= 1 << (regno - GP_REG_FIRST);

  /* If this function calls eh_return, we must also save and restore the
     EH data registers.  */
  if (crtl->calls_eh_return)
    for (i = 0; EH_RETURN_DATA_REGNO (i) != INVALID_REGNUM; i++)
      frame->mask |= 1 << (EH_RETURN_DATA_REGNO (i) - GP_REG_FIRST);

  /* Find out which FPRs we need to save.  This loop must iterate over
     the same space as its companion in mips_for_each_saved_gpr_and_fpr.  */
  if (TARGET_HARD_FLOAT)
    for (regno = FP_REG_FIRST; regno <= FP_REG_LAST; regno++)
      if (mips_save_reg_p (regno))
        frame->fmask |= 1 << (regno - FP_REG_FIRST);

  /* At the bottom of the frame are any outgoing stack arguments. */
  offset = crtl->outgoing_args_size;
  /* Next are local stack variables. */
  offset += MIPS_STACK_ALIGN (get_frame_size ());
  /* The virtual frame pointer points above the local variables. */
  frame->frame_pointer_offset = offset;
  /* Next are the callee-saved FPRs. */
  if (frame->fmask)
    {
      unsigned num_saved = __builtin_popcount(frame->fmask);
      offset += MIPS_STACK_ALIGN (num_saved * UNITS_PER_FPREG);
      frame->fp_sp_offset = offset - UNITS_PER_HWFPVALUE;
    }
  /* Next are the callee-saved GPRs. */
  if (frame->mask)
    {
      unsigned num_saved = __builtin_popcount(frame->mask);
      offset += MIPS_STACK_ALIGN (num_saved * UNITS_PER_WORD);
      frame->gp_sp_offset = offset - UNITS_PER_WORD;
    }
  /* The hard frame pointer points above the callee-saved GPRs. */
  frame->hard_frame_pointer_offset = offset;
  /* Above the hard frame pointer is the callee-allocated varags save area. */
  offset += MIPS_STACK_ALIGN (cfun->machine->varargs_size);
  frame->arg_pointer_offset = offset;
  /* Next is the callee-allocated area for pretend stack arguments.  */
  offset += crtl->args.pretend_args_size;
  frame->total_size = offset;
  /* Next points the incoming stack pointer and any incoming arguments. */
}

/* Make sure that we're not trying to eliminate to the wrong hard frame
   pointer.  */

static bool
mips_can_eliminate (const int from ATTRIBUTE_UNUSED, const int to)
{
  return (to == HARD_FRAME_POINTER_REGNUM || to == STACK_POINTER_REGNUM);
}

/* Implement INITIAL_ELIMINATION_OFFSET.  FROM is either the frame pointer
   or argument pointer.  TO is either the stack pointer or hard frame
   pointer.  */

HOST_WIDE_INT
mips_initial_elimination_offset (int from, int to)
{
  HOST_WIDE_INT src, dest;

  riscv_compute_frame_info ();

  if (to == HARD_FRAME_POINTER_REGNUM)
    dest = cfun->machine->frame.hard_frame_pointer_offset;
  else if (to == STACK_POINTER_REGNUM)
    dest = 0; /* this is the base of all offsets */
  else
    gcc_unreachable ();

  if (from == FRAME_POINTER_REGNUM)
    src = cfun->machine->frame.frame_pointer_offset;
  else if (from == ARG_POINTER_REGNUM)
    src = cfun->machine->frame.arg_pointer_offset;
  else
    gcc_unreachable ();

  return src - dest;
}

/* Implement RETURN_ADDR_RTX.  We do not support moving back to a
   previous frame.  */

rtx
mips_return_addr (int count, rtx frame ATTRIBUTE_UNUSED)
{
  if (count != 0)
    return const0_rtx;

  return get_hard_reg_initial_val (Pmode, RETURN_ADDR_REGNUM);
}

/* Emit code to change the current function's return address to
   ADDRESS.  SCRATCH is available as a scratch register, if needed.
   ADDRESS and SCRATCH are both word-mode GPRs.  */

void
mips_set_return_address (rtx address, rtx scratch)
{
  rtx slot_address;

  gcc_assert (BITSET_P (cfun->machine->frame.mask, RETURN_ADDR_REGNUM));
  slot_address = mips_add_offset (scratch, stack_pointer_rtx,
				  cfun->machine->frame.gp_sp_offset);
  mips_emit_move (gen_frame_mem (GET_MODE (address), slot_address), address);
}

/* A function to save or store a register.  The first argument is the
   register and the second is the stack slot.  */
typedef void (*mips_save_restore_fn) (rtx, rtx);

/* Use FN to save or restore register REGNO.  MODE is the register's
   mode and OFFSET is the offset of its save slot from the current
   stack pointer.  */

static void
mips_save_restore_reg (enum machine_mode mode, int regno,
		       HOST_WIDE_INT offset, mips_save_restore_fn fn)
{
  rtx mem;

  mem = gen_frame_mem (mode, plus_constant (stack_pointer_rtx, offset));
  fn (gen_rtx_REG (mode, regno), mem);
}

/* Call FN for each register that is saved by the current function.
   SP_OFFSET is the offset of the current stack pointer from the start
   of the frame.  */

static void
mips_for_each_saved_gpr_and_fpr (HOST_WIDE_INT sp_offset,
				 mips_save_restore_fn fn)
{
  HOST_WIDE_INT offset;
  int regno;

  /* Save the link register and s-registers. */
  offset = cfun->machine->frame.gp_sp_offset - sp_offset;
  for (regno = GP_REG_FIRST; regno <= GP_REG_LAST-1; regno++)
    if (BITSET_P (cfun->machine->frame.mask, regno - GP_REG_FIRST))
      {
        mips_save_restore_reg (word_mode, regno, offset, fn);
        offset -= UNITS_PER_WORD;
      }

  /* This loop must iterate over the same space as its companion in
     riscv_compute_frame_info.  */
  offset = cfun->machine->frame.fp_sp_offset - sp_offset;
  for (regno = FP_REG_FIRST; regno <= FP_REG_LAST; regno++)
    if (BITSET_P (cfun->machine->frame.fmask, regno - FP_REG_FIRST))
      {
	mips_save_restore_reg (DFmode, regno, offset, fn);
	offset -= GET_MODE_SIZE (DFmode);
      }
}

/* Return true if a move between register REGNO and its save slot (MEM)
   can be done in a single move.  LOAD_P is true if we are loading
   from the slot, false if we are storing to it.  */

static bool
mips_direct_save_slot_move_p (unsigned int regno, rtx mem, bool load_p)
{
  return mips_secondary_reload_class (REGNO_REG_CLASS (regno),
				      GET_MODE (mem), mem, load_p) == NO_REGS;
}

/* Emit a move from SRC to DEST, given that one of them is a register
   save slot and that the other is a register.  TEMP is a temporary
   GPR of the same mode that is available if need be.  */

void
mips_emit_save_slot_move (rtx dest, rtx src, rtx temp)
{
  unsigned int regno;
  rtx mem;

  if (REG_P (src))
    {
      regno = REGNO (src);
      mem = dest;
    }
  else
    {
      regno = REGNO (dest);
      mem = src;
    }

  if (mips_direct_save_slot_move_p (regno, mem, mem == src))
    mips_emit_move (dest, src);
  else
    {
      gcc_assert (!reg_overlap_mentioned_p (dest, temp));
      mips_emit_move (temp, src);
      mips_emit_move (dest, temp);
    }
  if (MEM_P (dest))
    mips_set_frame_expr (mips_frame_set (dest, src));
}

/* Save register REG to MEM.  Make the instruction frame-related.  */

static void
mips_save_reg (rtx reg, rtx mem)
{
  mips_emit_save_slot_move (mem, reg, MIPS_PROLOGUE_TEMP (GET_MODE (reg)));
}


/* Expand the "prologue" pattern.  */

void
mips_expand_prologue (void)
{
  const struct riscv_frame_info *frame;
  HOST_WIDE_INT size;
  rtx insn;

  frame = &cfun->machine->frame;
  size = frame->total_size;

  if (flag_stack_usage)
    current_function_static_stack_size = size;

  /* Save the registers.  Allocate up to MIPS_MAX_FIRST_STACK_STEP
     bytes beforehand; this is enough to cover the register save area
     without going out of range.  */
  if ((frame->mask | frame->fmask) != 0)
    {
      HOST_WIDE_INT step1;

      step1 = MIN (size, MIPS_MAX_FIRST_STACK_STEP);
      insn = gen_add3_insn (stack_pointer_rtx,
			    stack_pointer_rtx,
			    GEN_INT (-step1));
      RTX_FRAME_RELATED_P (emit_insn (insn)) = 1;
      size -= step1;
      mips_for_each_saved_gpr_and_fpr (size, mips_save_reg);
    }

  /* Set up the frame pointer, if we're using one.  */
  if (frame_pointer_needed)
    {
      insn = gen_add3_insn (hard_frame_pointer_rtx, stack_pointer_rtx,
                            GEN_INT (frame->hard_frame_pointer_offset - size));
      RTX_FRAME_RELATED_P (emit_insn (insn)) = 1;
    }

  /* Allocate the rest of the frame.  */
  if (size > 0)
    {
      if (SMALL_OPERAND (-size))
	RTX_FRAME_RELATED_P (emit_insn (gen_add3_insn (stack_pointer_rtx,
						       stack_pointer_rtx,
						       GEN_INT (-size)))) = 1;
      else
	{
	  mips_emit_move (MIPS_PROLOGUE_TEMP (Pmode), GEN_INT (size));
	  emit_insn (gen_sub3_insn (stack_pointer_rtx,
				    stack_pointer_rtx,
				    MIPS_PROLOGUE_TEMP (Pmode)));

	  /* Describe the combined effect of the previous instructions.  */
	  mips_set_frame_expr
	    (gen_rtx_SET (VOIDmode, stack_pointer_rtx,
			  plus_constant (stack_pointer_rtx, -size)));
	}
    }
}

/* Emit instructions to restore register REG from slot MEM.  */

static void
mips_restore_reg (rtx reg, rtx mem)
{
  mips_emit_save_slot_move (reg, mem, MIPS_EPILOGUE_TEMP (GET_MODE (reg)));
}

/* Expand an "epilogue" or "sibcall_epilogue" pattern; SIBCALL_P
   says which.  */

static bool riscv_in_utfunc = false;

void
mips_expand_epilogue (bool sibcall_p)
{
  const struct riscv_frame_info *frame;
  HOST_WIDE_INT step1, step2;

  if (!sibcall_p && mips_can_use_return_insn ())
    {
      if (riscv_in_utfunc)
        emit_insn(gen_riscv_stop());

      emit_jump_insn (gen_return ());
      return;
    }

  /* Split the frame into two.  STEP1 is the amount of stack we should
     deallocate before restoring the registers.  STEP2 is the amount we
     should deallocate afterwards.

     Start off by assuming that no registers need to be restored.  */
  frame = &cfun->machine->frame;
  step1 = frame->total_size;
  step2 = 0;

  /* Move past any dynamic stack allocations. */
  if (cfun->calls_alloca)
    {
      rtx adjust = GEN_INT (-frame->hard_frame_pointer_offset);
      if (!SMALL_INT (adjust))
	{
	  mips_emit_move (MIPS_EPILOGUE_TEMP (Pmode), adjust);
	  adjust = MIPS_EPILOGUE_TEMP (Pmode);
	}

      emit_insn (gen_add3_insn (stack_pointer_rtx, hard_frame_pointer_rtx, adjust));
    }

  /* If we need to restore registers, deallocate as much stack as
     possible in the second step without going out of range.  */
  if ((frame->mask | frame->fmask) != 0)
    {
      step2 = MIN (step1, MIPS_MAX_FIRST_STACK_STEP);
      step1 -= step2;
    }

  /* Set TARGET to BASE + STEP1.  */
  if (step1 > 0)
    {
      /* Get an rtx for STEP1 that we can add to BASE.  */
      rtx adjust = GEN_INT (step1);
      if (!SMALL_OPERAND (step1))
	{
	  mips_emit_move (MIPS_EPILOGUE_TEMP (Pmode), adjust);
	  adjust = MIPS_EPILOGUE_TEMP (Pmode);
	}

      emit_insn (gen_add3_insn (stack_pointer_rtx, stack_pointer_rtx, adjust));
    }

  /* Restore the registers.  */
  mips_for_each_saved_gpr_and_fpr (frame->total_size - step2, mips_restore_reg);

  /* Deallocate the final bit of the frame.  */
  if (step2 > 0)
    emit_insn (gen_add3_insn (stack_pointer_rtx, stack_pointer_rtx,
			      GEN_INT (step2)));

  /* Add in the __builtin_eh_return stack adjustment.  We need to
     use a temporary in MIPS16 code.  */
  if (crtl->calls_eh_return)
    emit_insn (gen_add3_insn (stack_pointer_rtx, stack_pointer_rtx,
			      EH_RETURN_STACKADJ_RTX));

  if (!sibcall_p)
    emit_jump_insn (gen_return_internal (gen_rtx_REG (Pmode, RETURN_ADDR_REGNUM)));
}

/* Return nonzero if this function is known to have a null epilogue.
   This allows the optimizer to omit jumps to jumps if no stack
   was created.  */

bool
mips_can_use_return_insn (void)
{
  return reload_completed && cfun->machine->frame.total_size == 0;
}

/* Return true if register REGNO can store a value of mode MODE.
   The result of this function is cached in mips_hard_regno_mode_ok.  */

static bool
mips_hard_regno_mode_ok_p (unsigned int regno, enum machine_mode mode)
{
  unsigned int size;
  enum mode_class mclass;

  if (VECTOR_MODE_P(mode))
  {
    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DI):
      case MIPS_RISCV_VECTOR_MODE_NAME(SI):
      case MIPS_RISCV_VECTOR_MODE_NAME(HI):
      case MIPS_RISCV_VECTOR_MODE_NAME(QI):
        return VEC_GP_REG_P(regno);

      case MIPS_RISCV_VECTOR_MODE_NAME(DF):
      case MIPS_RISCV_VECTOR_MODE_NAME(SF):
        return VEC_FP_REG_P(regno);

      default:
        return false;
    }
  }

  if (mode == CCmode)
    return GP_REG_P (regno);

  size = GET_MODE_SIZE (mode);
  mclass = GET_MODE_CLASS (mode);

  if (GP_REG_P (regno))
    /* Double-word values must be even-register-aligned. */
    return ((regno - GP_REG_FIRST) & 1) == 0 || size <= UNITS_PER_WORD;

  if (FP_REG_P (regno))
    {
      /* Allow TFmode for CCmode reloads.  */
      if (mode == TFmode)
	return true;

      if (mclass == MODE_FLOAT
	  || mclass == MODE_COMPLEX_FLOAT
	  || mclass == MODE_VECTOR_FLOAT)
	return size <= UNITS_PER_FPVALUE;
    }

  return false;
}

/* Implement HARD_REGNO_NREGS.  */

unsigned int
mips_hard_regno_nregs (int regno, enum machine_mode mode)
{
  if (VECTOR_MODE_P(mode))
    return 1;

  if (VEC_GP_REG_P(regno) || VEC_FP_REG_P(regno))
    return 1;

  if (FP_REG_P (regno))
    return (GET_MODE_SIZE (mode) + UNITS_PER_FPREG - 1) / UNITS_PER_FPREG;

  /* All other registers are word-sized.  */
  return (GET_MODE_SIZE (mode) + UNITS_PER_WORD - 1) / UNITS_PER_WORD;
}

/* Implement CLASS_MAX_NREGS, taking the maximum of the cases
   in mips_hard_regno_nregs.  */

int
mips_class_max_nregs (enum reg_class rclass, enum machine_mode mode)
{
  int size;
  HARD_REG_SET left;

  if (VECTOR_MODE_P(mode))
    return 1;

  if ((rclass == VEC_GR_REGS) || (rclass == VEC_FP_REGS))
    return 1;

  size = 0x8000;
  COPY_HARD_REG_SET (left, reg_class_contents[(int) rclass]);
  if (hard_reg_set_intersect_p (left, reg_class_contents[(int) FP_REGS]))
    {
      size = MIN (size, UNITS_PER_FPREG);
      AND_COMPL_HARD_REG_SET (left, reg_class_contents[(int) FP_REGS]);
    }
  if (!hard_reg_set_empty_p (left))
    size = MIN (size, UNITS_PER_WORD);
  return (GET_MODE_SIZE (mode) + size - 1) / size;
}

/* Implement CANNOT_CHANGE_MODE_CLASS.  */

bool
mips_cannot_change_mode_class (enum machine_mode from ATTRIBUTE_UNUSED,
			       enum machine_mode to ATTRIBUTE_UNUSED,
			       enum reg_class rclass)
{
  /* An FP register written in one format is undefined if interpreted in
     another format. */
  return reg_classes_intersect_p (FP_REGS, rclass);
}

/* Return true if moves in mode MODE can use the fsgnj instruction.  */

static bool
riscv_mode_ok_for_fsgnj_p (enum machine_mode mode)
{
  switch (mode)
    {
    case SFmode:
    case DFmode:
      return TARGET_HARD_FLOAT;

    default:
      return false;
    }
}

/* Implement MODES_TIEABLE_P.  */

bool
mips_modes_tieable_p (enum machine_mode mode1, enum machine_mode mode2)
{
  /* FPRs allow no mode punning, so it's not worth tying modes if we'd
     prefer to put one of them in FPRs.  */
  return (mode1 == mode2
	  || (!riscv_mode_ok_for_fsgnj_p (mode1)
	      && !riscv_mode_ok_for_fsgnj_p (mode2)));
}

/* Implement TARGET_PREFERRED_RELOAD_CLASS.  */

static reg_class_t
mips_preferred_reload_class (rtx x, reg_class_t rclass)
{
  if (reg_class_subset_p (FP_REGS, rclass)
      && riscv_mode_ok_for_fsgnj_p (GET_MODE (x)))
    return FP_REGS;

  if (reg_class_subset_p (GR_REGS, rclass))
    rclass = GR_REGS;

  return rclass;
}

/* RCLASS is a class involved in a REGISTER_MOVE_COST calculation.
   Return a "canonical" class to represent it in later calculations.  */

static reg_class_t
mips_canonicalize_move_class (reg_class_t rclass)
{
  if (reg_class_subset_p (rclass, GENERAL_REGS))
    rclass = GENERAL_REGS;

  return rclass;
}

/* Return the cost of moving a value of mode MODE from a register of
   class FROM to a GPR.  Return 0 for classes that are unions of other
   classes handled by this function.  */

static int
mips_move_to_gpr_cost (reg_class_t from)
{
  switch (from)
    {
    case GENERAL_REGS:
      return 1;

    case FP_REGS:
      /* FP->int moves can cause recoupling on decoupled implementations */
      return 4;

    default:
      return 0;
    }
}

/* Return the cost of moving a value of mode MODE from a GPR to a
   register of class TO.  Return 0 for classes that are unions of
   other classes handled by this function.  */

static int
mips_move_from_gpr_cost (reg_class_t to)
{
  switch (to)
    {
    case GENERAL_REGS:
    case FP_REGS:
      return 1;

    default:
      return 0;
    }
}

/* Implement TARGET_REGISTER_MOVE_COST.  Return 0 for classes that are the
   maximum of the move costs for subclasses; regclass will work out
   the maximum for us.  */

static int
mips_register_move_cost (enum machine_mode mode,
			 reg_class_t from, reg_class_t to)
{
  int cost1, cost2;

  from = mips_canonicalize_move_class (from);
  to = mips_canonicalize_move_class (to);

  /* Handle moves that can be done without using general-purpose registers.  */
  if (from == FP_REGS && to == FP_REGS && riscv_mode_ok_for_fsgnj_p (mode))
      /* fsgnj.fmt.  */
      return 1;

  /* Handle cases in which only one class deviates from the ideal.  */
  if (from == GENERAL_REGS)
    return mips_move_from_gpr_cost (to);
  if (to == GENERAL_REGS)
    return mips_move_to_gpr_cost (from);

  /* Handles cases that require a GPR temporary.  */
  cost1 = mips_move_to_gpr_cost (from);
  if (cost1 != 0)
    {
      cost2 = mips_move_from_gpr_cost (to);
      if (cost2 != 0)
	return cost1 + cost2;
    }

  return 0;
}

/* Implement TARGET_MEMORY_MOVE_COST.  */

static int
mips_memory_move_cost (enum machine_mode mode, reg_class_t rclass, bool in)
{
  return (riscv_memory_latency
	  + memory_move_secondary_cost (mode, rclass, in));
} 

/* Implement TARGET_IRA_COVER_CLASSES.  */

static const reg_class_t *
mips_ira_cover_classes (void)
{
  static const reg_class_t no_acc_classes[] = {
    GR_REGS, FP_REGS, VEC_GR_REGS, VEC_FP_REGS,
    LIM_REG_CLASSES
  };

  return no_acc_classes;
}

/* Return the register class required for a secondary register when
   copying between one of the registers in RCLASS and value X, which
   has mode MODE.  X is the source of the move if IN_P, otherwise it
   is the destination.  Return NO_REGS if no secondary register is
   needed.  */

enum reg_class
mips_secondary_reload_class (enum reg_class rclass,
			     enum machine_mode mode, rtx x,
			     bool in_p ATTRIBUTE_UNUSED)
{
  int regno;

  regno = true_regnum (x);

  if (reg_class_subset_p (rclass, FP_REGS))
    {
      if (MEM_P (x) && (GET_MODE_SIZE (mode) == 4 || GET_MODE_SIZE (mode) == 8))
	/* We can use flw/fld/fsw/fsd. */
	return NO_REGS;

      if (GP_REG_P (regno) || x == CONST0_RTX (mode))
	/* We can use fmv or go through memory when mode > Pmode. */
	return NO_REGS;

      if (CONSTANT_P (x) && !targetm.cannot_force_const_mem (x))
	/* We can force the constant to memory and use flw/fld. */
	return NO_REGS;

      if (FP_REG_P (regno) && riscv_mode_ok_for_fsgnj_p (mode))
	/* We can use fsgnj. */
	return NO_REGS;

      /* Otherwise, we need to reload through an integer register.  */
      return GR_REGS;
    }
  if (FP_REG_P (regno))
    return reg_class_subset_p (rclass, GR_REGS) ? NO_REGS : GR_REGS;

  return NO_REGS;
}

/* Implement TARGET_MODE_REP_EXTENDED.  */

static int
mips_mode_rep_extended (enum machine_mode mode, enum machine_mode mode_rep)
{
  /* On 64-bit targets, SImode register values are sign-extended to DImode.  */
  if (TARGET_64BIT && mode == SImode && mode_rep == DImode)
    return SIGN_EXTEND;

  return UNKNOWN;
}

/* Implement TARGET_VECTOR_MODE_SUPPORTED_P.  */

static bool
mips_vector_mode_supported_p (enum machine_mode mode ATTRIBUTE_UNUSED)
{
  switch (mode)
    {
    case MIPS_RISCV_VECTOR_MODE_NAME(DI):
    case MIPS_RISCV_VECTOR_MODE_NAME(SI):
    case MIPS_RISCV_VECTOR_MODE_NAME(HI):
    case MIPS_RISCV_VECTOR_MODE_NAME(QI):
    case MIPS_RISCV_VECTOR_MODE_NAME(DF):
    case MIPS_RISCV_VECTOR_MODE_NAME(SF):
      return true;

    default:
      return false;
    }
}

/* Implement TARGET_SCALAR_MODE_SUPPORTED_P.  */

static bool
mips_scalar_mode_supported_p (enum machine_mode mode)
{
  if (ALL_FIXED_POINT_MODE_P (mode)
      && GET_MODE_PRECISION (mode) <= 2 * BITS_PER_WORD)
    return true;

  return default_scalar_mode_supported_p (mode);
}

/* Return the assembly code for INSN, which has the operands given by
   OPERANDS, and which branches to OPERANDS[0] if some condition is true.
   BRANCH_IF_TRUE is the asm template that should be used if OPERANDS[0]
   is in range of a direct branch.  BRANCH_IF_FALSE is an inverted
   version of BRANCH_IF_TRUE.  */

const char *
mips_output_conditional_branch (rtx insn, rtx *operands,
				const char *branch_if_true,
				const char *branch_if_false)
{
  unsigned int length;
  rtx taken, not_taken;

  gcc_assert (LABEL_P (operands[0]));

  length = get_attr_length (insn);
  if (length <= 4)
    return branch_if_true;

  /* Generate a reversed branch around a direct jump.  This fallback does
     not use branch-likely instructions.  */
  not_taken = gen_label_rtx ();
  taken = operands[0];

  /* Generate the reversed branch to NOT_TAKEN.  */
  operands[0] = not_taken;
  output_asm_insn (branch_if_false, operands);

  /* Output the unconditional branch to TAKEN.  */
  output_asm_insn ("j\t%0", &taken);

  /* Output NOT_TAKEN.  */
  targetm.asm_out.internal_label (asm_out_file, "L",
				  CODE_LABEL_NUMBER (not_taken));
  return "";
}

/* Implement TARGET_SCHED_ADJUST_COST.  We assume that anti and output
   dependencies have no cost. */

static int
mips_adjust_cost (rtx insn ATTRIBUTE_UNUSED, rtx link,
		  rtx dep ATTRIBUTE_UNUSED, int cost)
{
  if (REG_NOTE_KIND (link) != 0)
    return 0;
  return cost;
}

/* Return the number of instructions that can be issued per cycle.  */

static int
mips_issue_rate (void)
{
  switch (mips_tune)
    {
    case PROCESSOR_ROCKET:
      return 1;

    default:
      return 1;
    }
}

/* This structure describes a single built-in function.  */
struct mips_builtin_description {
  /* The code of the main .md file instruction.  See mips_builtin_type
     for more information.  */
  enum insn_code icode;

  /* The name of the built-in function.  */
  const char *name;

  /* Specifies how the function should be expanded.  */
  enum mips_builtin_type builtin_type;

  /* The function's prototype.  */
  enum mips_function_type function_type;

  /* Whether the function is available.  */
  unsigned int (*avail) (void);
};

static unsigned int
mips_builtin_avail_riscv (void)
{
  return 1;
}

/* Construct a mips_builtin_description from the given arguments.

   INSN is the name of the associated instruction pattern, without the
   leading CODE_FOR_mips_.

   CODE is the floating-point condition code associated with the
   function.  It can be 'f' if the field is not applicable.

   NAME is the name of the function itself, without the leading
   "__builtin_mips_".

   BUILTIN_TYPE and FUNCTION_TYPE are mips_builtin_description fields.

   AVAIL is the name of the availability predicate, without the leading
   mips_builtin_avail_.  */
#define MIPS_BUILTIN(INSN, NAME, BUILTIN_TYPE, FUNCTION_TYPE, AVAIL)	\
  { CODE_FOR_mips_ ## INSN, "__builtin_mips_" NAME,			\
    BUILTIN_TYPE, FUNCTION_TYPE, mips_builtin_avail_ ## AVAIL }

/* Define __builtin_mips_<INSN>, which is a MIPS_BUILTIN_DIRECT function
   mapped to instruction CODE_FOR_mips_<INSN>,  FUNCTION_TYPE and AVAIL
   are as for MIPS_BUILTIN.  */
#define DIRECT_BUILTIN(INSN, FUNCTION_TYPE, AVAIL)			\
  MIPS_BUILTIN (INSN, #INSN, MIPS_BUILTIN_DIRECT, FUNCTION_TYPE, AVAIL)

/* Define __builtin_mips_<INSN>, which is a MIPS_BUILTIN_DIRECT_NO_TARGET
   function mapped to instruction CODE_FOR_mips_<INSN>,  FUNCTION_TYPE
   and AVAIL are as for MIPS_BUILTIN.  */
#define DIRECT_NO_TARGET_BUILTIN(INSN, FUNCTION_TYPE, AVAIL)		\
  MIPS_BUILTIN (INSN, #INSN, MIPS_BUILTIN_DIRECT_NO_TARGET,		\
		FUNCTION_TYPE, AVAIL)

static const struct mips_builtin_description mips_builtins[] = {
  DIRECT_BUILTIN( riscv_vload_vdi, MIPS_VDI_FTYPE_CPOINTER, riscv ),
  DIRECT_BUILTIN( riscv_vload_vsi, MIPS_VSI_FTYPE_CPOINTER, riscv ),
  DIRECT_BUILTIN( riscv_vload_vhi, MIPS_VHI_FTYPE_CPOINTER, riscv ),
  DIRECT_BUILTIN( riscv_vload_vqi, MIPS_VQI_FTYPE_CPOINTER, riscv ),
  DIRECT_BUILTIN( riscv_vload_vdf, MIPS_VDF_FTYPE_CPOINTER, riscv ),
  DIRECT_BUILTIN( riscv_vload_vsf, MIPS_VSF_FTYPE_CPOINTER, riscv ),

  DIRECT_BUILTIN( riscv_vload_strided_vdi, MIPS_VDI_FTYPE_CPOINTER_DI, riscv ),
  DIRECT_BUILTIN( riscv_vload_strided_vsi, MIPS_VSI_FTYPE_CPOINTER_DI, riscv ),
  DIRECT_BUILTIN( riscv_vload_strided_vhi, MIPS_VHI_FTYPE_CPOINTER_DI, riscv ),
  DIRECT_BUILTIN( riscv_vload_strided_vqi, MIPS_VQI_FTYPE_CPOINTER_DI, riscv ),
  DIRECT_BUILTIN( riscv_vload_strided_vdf, MIPS_VDF_FTYPE_CPOINTER_DI, riscv ),
  DIRECT_BUILTIN( riscv_vload_strided_vsf, MIPS_VSF_FTYPE_CPOINTER_DI, riscv ),

  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vdi, MIPS_VOID_FTYPE_VDI_POINTER, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vsi, MIPS_VOID_FTYPE_VSI_POINTER, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vhi, MIPS_VOID_FTYPE_VHI_POINTER, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vqi, MIPS_VOID_FTYPE_VQI_POINTER, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vdf, MIPS_VOID_FTYPE_VDF_POINTER, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_vsf, MIPS_VOID_FTYPE_VSF_POINTER, riscv ),

  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vdi, MIPS_VOID_FTYPE_VDI_POINTER_DI, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vsi, MIPS_VOID_FTYPE_VSI_POINTER_DI, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vhi, MIPS_VOID_FTYPE_VHI_POINTER_DI, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vqi, MIPS_VOID_FTYPE_VQI_POINTER_DI, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vdf, MIPS_VOID_FTYPE_VDF_POINTER_DI, riscv ),
  DIRECT_NO_TARGET_BUILTIN( riscv_vstore_strided_vsf, MIPS_VOID_FTYPE_VSF_POINTER_DI, riscv ),
};

/* Index I is the function declaration for mips_builtins[I], or null if the
   function isn't defined on this target.  */
static GTY(()) tree mips_builtin_decls[ARRAY_SIZE (mips_builtins)];

/* MODE is a vector mode whose elements have type TYPE.  Return the type
   of the vector itself.  */

static tree
mips_builtin_vector_type (tree type, enum machine_mode mode)
{
  static tree types[2 * (int) MAX_MACHINE_MODE];
  int mode_index;

  mode_index = (int) mode;

  if (TREE_CODE (type) == INTEGER_TYPE && TYPE_UNSIGNED (type))
    mode_index += MAX_MACHINE_MODE;

  if (types[mode_index] == NULL_TREE)
    types[mode_index] = build_vector_type_for_mode (type, mode);
  return types[mode_index];
}

/* Source-level argument types.  */
#define MIPS_ATYPE_VOID void_type_node
#define MIPS_ATYPE_INT integer_type_node
#define MIPS_ATYPE_POINTER ptr_type_node
#define MIPS_ATYPE_CPOINTER const_ptr_type_node

/* Standard mode-based argument types.  */
#define MIPS_ATYPE_UQI unsigned_intQI_type_node
#define MIPS_ATYPE_SI intSI_type_node
#define MIPS_ATYPE_USI unsigned_intSI_type_node
#define MIPS_ATYPE_DI intDI_type_node
#define MIPS_ATYPE_UDI unsigned_intDI_type_node
#define MIPS_ATYPE_SF float_type_node
#define MIPS_ATYPE_DF double_type_node

/* Vector argument types.  */
#define MIPS_ATYPE_V2SF mips_builtin_vector_type (float_type_node, V2SFmode)
#define MIPS_ATYPE_V2HI mips_builtin_vector_type (intHI_type_node, V2HImode)
#define MIPS_ATYPE_V2SI mips_builtin_vector_type (intSI_type_node, V2SImode)
#define MIPS_ATYPE_V4QI mips_builtin_vector_type (intQI_type_node, V4QImode)
#define MIPS_ATYPE_V4HI mips_builtin_vector_type (intHI_type_node, V4HImode)
#define MIPS_ATYPE_V8QI mips_builtin_vector_type (intQI_type_node, V8QImode)
#define MIPS_ATYPE_UV2SI					\
  mips_builtin_vector_type (unsigned_intSI_type_node, V2SImode)
#define MIPS_ATYPE_UV4HI					\
  mips_builtin_vector_type (unsigned_intHI_type_node, V4HImode)
#define MIPS_ATYPE_UV8QI					\
  mips_builtin_vector_type (unsigned_intQI_type_node, V8QImode)

#define MIPS_ATYPE_VDI \
  mips_builtin_vector_type( intDI_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(DI) )

#define MIPS_ATYPE_VSI \
  mips_builtin_vector_type( intSI_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(SI) )

#define MIPS_ATYPE_VHI \
  mips_builtin_vector_type( intHI_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(HI) )

#define MIPS_ATYPE_VQI \
  mips_builtin_vector_type( intQI_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(QI) )

#define MIPS_ATYPE_VDF \
  mips_builtin_vector_type( double_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(DF) )

#define MIPS_ATYPE_VSF \
  mips_builtin_vector_type( float_type_node, \
    MIPS_RISCV_VECTOR_MODE_NAME(SF) )

/* MIPS_FTYPE_ATYPESN takes N MIPS_FTYPES-like type codes and lists
   their associated MIPS_ATYPEs.  */
#define MIPS_FTYPE_ATYPES1(A, B) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B

#define MIPS_FTYPE_ATYPES2(A, B, C) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C

#define MIPS_FTYPE_ATYPES3(A, B, C, D) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C, MIPS_ATYPE_##D

#define MIPS_FTYPE_ATYPES4(A, B, C, D, E) \
  MIPS_ATYPE_##A, MIPS_ATYPE_##B, MIPS_ATYPE_##C, MIPS_ATYPE_##D, \
  MIPS_ATYPE_##E

/* Return the function type associated with function prototype TYPE.  */

static tree
mips_build_function_type (enum mips_function_type type)
{
  static tree types[(int) MIPS_MAX_FTYPE_MAX];

  if (types[(int) type] == NULL_TREE)
    switch (type)
      {
#define DEF_MIPS_FTYPE(NUM, ARGS)					\
  case MIPS_FTYPE_NAME##NUM ARGS:					\
    types[(int) type]							\
      = build_function_type_list (MIPS_FTYPE_ATYPES##NUM ARGS,		\
				  NULL_TREE);				\
    break;
#include "config/riscv/riscv-ftypes.def"
#undef DEF_MIPS_FTYPE
      default:
	gcc_unreachable ();
      }

  return types[(int) type];
}

/* Implement TARGET_INIT_BUILTINS.  */

static void
mips_init_builtins (void)
{
  const struct mips_builtin_description *d;
  unsigned int i;

  /* Iterate through all of the bdesc arrays, initializing all of the
     builtin functions.  */
  for (i = 0; i < ARRAY_SIZE (mips_builtins); i++)
    {
      d = &mips_builtins[i];
      if (d->avail ())
	mips_builtin_decls[i]
	  = add_builtin_function (d->name,
				  mips_build_function_type (d->function_type),
				  i, BUILT_IN_MD, NULL, NULL);
    }
}

/* Implement TARGET_BUILTIN_DECL.  */

static tree
mips_builtin_decl (unsigned int code, bool initialize_p ATTRIBUTE_UNUSED)
{
  if (code >= ARRAY_SIZE (mips_builtins))
    return error_mark_node;
  return mips_builtin_decls[code];
}

/* Take argument ARGNO from EXP's argument list and convert it into a
   form suitable for input operand OPNO of instruction ICODE.  Return the
   value.  */

static rtx
mips_prepare_builtin_arg (enum insn_code icode,
			  unsigned int opno, tree exp, unsigned int argno)
{
  tree arg;
  rtx value;
  enum machine_mode mode;

  arg = CALL_EXPR_ARG (exp, argno);
  value = expand_normal (arg);
  mode = insn_data[icode].operand[opno].mode;
  if (!insn_data[icode].operand[opno].predicate (value, mode))
    {
      /* We need to get the mode from ARG for two reasons:

	   - to cope with address operands, where MODE is the mode of the
	     memory, rather than of VALUE itself.

	   - to cope with special predicates like pmode_register_operand,
	     where MODE is VOIDmode.  */
      value = copy_to_mode_reg (TYPE_MODE (TREE_TYPE (arg)), value);

      /* Check the predicate again.  */
      if (!insn_data[icode].operand[opno].predicate (value, mode))
	{
	  error ("invalid argument to built-in function");
	  return const0_rtx;
	}
    }

  return value;
}

/* Return an rtx suitable for output operand OP of instruction ICODE.
   If TARGET is non-null, try to use it where possible.  */

static rtx
mips_prepare_builtin_target (enum insn_code icode, unsigned int op, rtx target)
{
  enum machine_mode mode;

  mode = insn_data[icode].operand[op].mode;
  if (target == 0 || !insn_data[icode].operand[op].predicate (target, mode))
    target = gen_reg_rtx (mode);

  return target;
}

/* Expand a MIPS_BUILTIN_DIRECT or MIPS_BUILTIN_DIRECT_NO_TARGET function;
   HAS_TARGET_P says which.  EXP is the CALL_EXPR that calls the function
   and ICODE is the code of the associated .md pattern.  TARGET, if nonnull,
   suggests a good place to put the result.  */

static rtx
mips_expand_builtin_direct (enum insn_code icode, rtx target, tree exp,
			    bool has_target_p)
{
  rtx ops[MAX_RECOG_OPERANDS];
  int opno, argno;

  /* Map any target to operand 0.  */
  opno = 0;
  if (has_target_p)
    {
      target = mips_prepare_builtin_target (icode, opno, target);
      ops[opno] = target;
      opno++;
    }

  /* Map the arguments to the other operands.  The n_operands value
     for an expander includes match_dups and match_scratches as well as
     match_operands, so n_operands is only an upper bound on the number
     of arguments to the expander function.  */
  gcc_assert (opno + call_expr_nargs (exp) <= insn_data[icode].n_operands);
  for (argno = 0; argno < call_expr_nargs (exp); argno++, opno++)
    ops[opno] = mips_prepare_builtin_arg (icode, opno, exp, argno);

  switch (opno)
    {
    case 2:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1]));
      break;

    case 3:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1], ops[2]));
      break;

    case 4:
      emit_insn (GEN_FCN (icode) (ops[0], ops[1], ops[2], ops[3]));
      break;

    default:
      gcc_unreachable ();
    }
  return target;
}

/* Implement TARGET_EXPAND_BUILTIN.  */

static rtx
mips_expand_builtin (tree exp, rtx target, rtx subtarget ATTRIBUTE_UNUSED,
		     enum machine_mode mode ATTRIBUTE_UNUSED,
		     int ignore ATTRIBUTE_UNUSED)
{
  tree fndecl;
  unsigned int fcode, avail;
  const struct mips_builtin_description *d;

  fndecl = TREE_OPERAND (CALL_EXPR_FN (exp), 0);
  fcode = DECL_FUNCTION_CODE (fndecl);
  gcc_assert (fcode < ARRAY_SIZE (mips_builtins));
  d = &mips_builtins[fcode];
  avail = d->avail ();
  gcc_assert (avail != 0);
  switch (d->builtin_type)
    {
    case MIPS_BUILTIN_DIRECT:
      return mips_expand_builtin_direct (d->icode, target, exp, true);

    case MIPS_BUILTIN_DIRECT_NO_TARGET:
      return mips_expand_builtin_direct (d->icode, target, exp, false);
    }
  gcc_unreachable ();
}

/* This structure records that the current function has a LO_SUM
   involving SYMBOL_REF or LABEL_REF BASE and that MAX_OFFSET is
   the largest offset applied to BASE by all such LO_SUMs.  */
struct mips_lo_sum_offset {
  rtx base;
  HOST_WIDE_INT offset;
};

/* Return a hash value for SYMBOL_REF or LABEL_REF BASE.  */

static hashval_t
mips_hash_base (rtx base)
{
  int do_not_record_p;

  return hash_rtx (base, GET_MODE (base), &do_not_record_p, NULL, false);
}

/* Hash-table callbacks for mips_lo_sum_offsets.  */

static hashval_t
mips_lo_sum_offset_hash (const void *entry)
{
  return mips_hash_base (((const struct mips_lo_sum_offset *) entry)->base);
}

static int
mips_lo_sum_offset_eq (const void *entry, const void *value)
{
  return rtx_equal_p (((const struct mips_lo_sum_offset *) entry)->base,
		      (const_rtx) value);
}

/* Look up symbolic constant X in HTAB, which is a hash table of
   mips_lo_sum_offsets.  If OPTION is NO_INSERT, return true if X can be
   paired with a recorded LO_SUM, otherwise record X in the table.  */

static bool
mips_lo_sum_offset_lookup (htab_t htab, rtx x, enum insert_option option)
{
  rtx base, offset;
  void **slot;
  struct mips_lo_sum_offset *entry;

  /* Split X into a base and offset.  */
  split_const (x, &base, &offset);
  if (UNSPEC_ADDRESS_P (base))
    base = UNSPEC_ADDRESS (base);

  /* Look up the base in the hash table.  */
  slot = htab_find_slot_with_hash (htab, base, mips_hash_base (base), option);
  if (slot == NULL)
    return false;

  entry = (struct mips_lo_sum_offset *) *slot;
  if (option == INSERT)
    {
      if (entry == NULL)
	{
	  entry = XNEW (struct mips_lo_sum_offset);
	  entry->base = base;
	  entry->offset = INTVAL (offset);
	  *slot = entry;
	}
      else
	{
	  if (INTVAL (offset) > entry->offset)
	    entry->offset = INTVAL (offset);
	}
    }
  return INTVAL (offset) <= entry->offset;
}

/* A for_each_rtx callback for which DATA is a mips_lo_sum_offset hash table.
   Record every LO_SUM in *LOC.  */

static int
mips_record_lo_sum (rtx *loc, void *data)
{
  if (GET_CODE (*loc) == LO_SUM)
    mips_lo_sum_offset_lookup ((htab_t) data, XEXP (*loc, 1), INSERT);
  return 0;
}

/* Return true if INSN is a SET of an orphaned high-part relocation.
   HTAB is a hash table of mips_lo_sum_offsets that describes all the
   LO_SUMs in the current function.  */

static bool
mips_orphaned_high_part_p (htab_t htab, rtx insn)
{
  rtx x, set;

  set = single_set (insn);
  if (set)
    {
      /* Check for %his.  */
      x = SET_SRC (set);
      if (GET_CODE (x) == HIGH
	  && absolute_symbolic_operand (XEXP (x, 0), VOIDmode))
	return !mips_lo_sum_offset_lookup (htab, XEXP (x, 0), NO_INSERT);
    }
  return false;
}

/* Delete any high-part relocations whose partnering low parts are dead. */

static void
mips_reorg_process_insns (void)
{
  rtx insn, next_insn;
  htab_t htab;

  /* Force all instructions to be split into their final form.  */
  split_all_insns_noflow ();

  /* Recalculate instruction lengths without taking nops into account.  */
  shorten_branches (get_insns ());

  htab = htab_create (37, mips_lo_sum_offset_hash,
		      mips_lo_sum_offset_eq, free);

  /* Make a first pass over the instructions, recording all the LO_SUMs.  */
  for (insn = get_insns (); insn != 0; insn = NEXT_INSN (insn))
    if (USEFUL_INSN_P (insn))
      for_each_rtx (&PATTERN (insn), mips_record_lo_sum, htab);

  /* Make a second pass over the instructions.  Delete orphaned
     high-part relocations or turn them into NOPs.  Avoid hazards
     by inserting NOPs.  */
  for (insn = get_insns (); insn != 0; insn = next_insn)
    {
      next_insn = NEXT_INSN (insn);
      if (USEFUL_INSN_P (insn))
	{
	  /* INSN is a single instruction.  Delete it if it's an
	     orphaned high-part relocation.  */
	  if (mips_orphaned_high_part_p (htab, insn))
	    delete_insn (insn);
	}
    }

  htab_delete (htab);
}

/* Implement TARGET_MACHINE_DEPENDENT_REORG.  */

static void
mips_reorg (void)
{
  mips_reorg_process_insns ();
}

/* Implement TARGET_ASM_OUTPUT_MI_THUNK.  Generate rtl rather than asm text
   in order to avoid duplicating too much logic from elsewhere.  */

static void
mips_output_mi_thunk (FILE *file, tree thunk_fndecl ATTRIBUTE_UNUSED,
		      HOST_WIDE_INT delta, HOST_WIDE_INT vcall_offset,
		      tree function)
{
  rtx this_rtx, temp1, temp2, insn, fnaddr;
  bool use_sibcall_p;

  /* Pretend to be a post-reload pass while generating rtl.  */
  reload_completed = 1;

  /* Mark the end of the (empty) prologue.  */
  emit_note (NOTE_INSN_PROLOGUE_END);

  /* Determine if we can use a sibcall to call FUNCTION directly.  */
  fnaddr = XEXP (DECL_RTL (function), 0);
  use_sibcall_p = absolute_symbolic_operand (fnaddr, Pmode);

  /* We need two temporary registers in some cases.  */
  temp1 = gen_rtx_REG (Pmode, GP_RETURN);
  temp2 = gen_rtx_REG (Pmode, GP_RETURN + 1);

  /* Find out which register contains the "this" pointer.  */
  if (aggregate_value_p (TREE_TYPE (TREE_TYPE (function)), function))
    this_rtx = gen_rtx_REG (Pmode, GP_ARG_FIRST + 1);
  else
    this_rtx = gen_rtx_REG (Pmode, GP_ARG_FIRST);

  /* Add DELTA to THIS_RTX.  */
  if (delta != 0)
    {
      rtx offset = GEN_INT (delta);
      if (!SMALL_OPERAND (delta))
	{
	  mips_emit_move (temp1, offset);
	  offset = temp1;
	}
      emit_insn (gen_add3_insn (this_rtx, this_rtx, offset));
    }

  /* If needed, add *(*THIS_RTX + VCALL_OFFSET) to THIS_RTX.  */
  if (vcall_offset != 0)
    {
      rtx addr;

      /* Set TEMP1 to *THIS_RTX.  */
      mips_emit_move (temp1, gen_rtx_MEM (Pmode, this_rtx));

      /* Set ADDR to a legitimate address for *THIS_RTX + VCALL_OFFSET.  */
      addr = mips_add_offset (temp2, temp1, vcall_offset);

      /* Load the offset and add it to THIS_RTX.  */
      mips_emit_move (temp1, gen_rtx_MEM (Pmode, addr));
      emit_insn (gen_add3_insn (this_rtx, this_rtx, temp1));
    }

  /* Jump to the target function.  Use a sibcall if direct jumps are
     allowed, otherwise load the address into a register first.  */
  if (use_sibcall_p)
    {
      insn = emit_call_insn (gen_sibcall_internal (fnaddr, const0_rtx));
      SIBLING_CALL_P (insn) = 1;
    }
  else
    {
      mips_emit_move(temp1, fnaddr);
      emit_jump_insn (gen_indirect_jump (temp1));
    }

  /* Run just enough of rest_of_compilation.  This sequence was
     "borrowed" from alpha.c.  */
  insn = get_insns ();
  insn_locators_alloc ();
  split_all_insns_noflow ();
  shorten_branches (insn);
  final_start_function (insn, file, 1);
  final (insn, file, 1);
  final_end_function ();

  /* Clean up the vars set above.  Note that final_end_function resets
     the global pointer for us.  */
  reload_completed = 0;
}

/* Implement TARGET_SET_CURRENT_FUNCTION.  Decide whether the current
   function should use the MIPS16 ISA and switch modes accordingly.  */

static void
mips_set_current_function (tree fndecl)
{
  bool utfunc = fndecl && (lookup_attribute("utfunc", DECL_ATTRIBUTES(fndecl)) != NULL);
  if (riscv_in_utfunc != utfunc)
    reinit_regs();
  riscv_in_utfunc = utfunc;
}

/* Allocate a chunk of memory for per-function machine-dependent data.  */

static struct machine_function *
mips_init_machine_status (void)
{
  return ggc_alloc_cleared_machine_function ();
}

/* Return the mips_cpu_info entry for the processor or ISA given
   by CPU_STRING.  Return null if the string isn't recognized.

   A similar function exists in GAS.  */

static const struct mips_cpu_info *
mips_parse_cpu (const char *cpu_string)
{
  unsigned int i;

  for (i = 0; i < ARRAY_SIZE (mips_cpu_info_table); i++)
    if (strcmp (mips_cpu_info_table[i].name, cpu_string) == 0)
      return mips_cpu_info_table + i;

  return NULL;
}

/* Implement TARGET_HANDLE_OPTION.  */

static bool
mips_handle_option (size_t code, const char *arg, int value ATTRIBUTE_UNUSED)
{
  switch (code)
    {
    case OPT_mtune_:
      return mips_parse_cpu (arg) != 0;

    default:
      return true;
    }
}

/* Implement TARGET_OPTION_OVERRIDE.  */

static void
mips_option_override (void)
{
  int regno, mode;
  const struct mips_cpu_info *info;

#ifdef SUBTARGET_OVERRIDE_OPTIONS
  SUBTARGET_OVERRIDE_OPTIONS;
#endif

  info = mips_parse_cpu (MIPS_CPU_STRING_DEFAULT);
  gcc_assert (info);
  mips_tune = info->cpu;

  if (mips_tune_string != 0)
    {
      const struct mips_cpu_info *tune = mips_parse_cpu (mips_tune_string);
      if (tune)
	mips_tune = tune->cpu;
    }

  flag_pcc_struct_return = 0;

  /* Decide which rtx_costs structure to use.  */
  if (optimize_size)
    mips_cost = &mips_rtx_cost_optimize_size;
  else
    mips_cost = &mips_rtx_cost_data[mips_tune];

  /* If the user hasn't specified a branch cost, use the processor's
     default.  */
  if (mips_branch_cost == 0)
    mips_branch_cost = mips_cost->branch_cost;

  if (riscv_memory_latency == 0)
    riscv_memory_latency = mips_cost->memory_latency;

  if (!TARGET_USE_GP)
    g_switch_value = 0;

  /* Prefer a call to memcpy over inline code when optimizing for size,
     though see MOVE_RATIO in mips.h.  */
  if (optimize_size && (target_flags_explicit & MASK_MEMCPY) == 0)
    target_flags |= MASK_MEMCPY;

  /* Use atomic instructions, if user did not specify a preference */
  if ((target_flags_explicit & MASK_ATOMIC) == 0)
    target_flags |= MASK_ATOMIC;

#ifdef MIPS_TFMODE_FORMAT
  REAL_MODE_FORMAT (TFmode) = &MIPS_TFMODE_FORMAT;
#endif

  /* Set up mips_hard_regno_mode_ok.  */
  for (mode = 0; mode < MAX_MACHINE_MODE; mode++)
    for (regno = 0; regno < FIRST_PSEUDO_REGISTER; regno++)
      mips_hard_regno_mode_ok[mode][regno]
	= mips_hard_regno_mode_ok_p (regno, (enum machine_mode) mode);

  /* Function to allocate machine-dependent function status.  */
  init_machine_status = &mips_init_machine_status;

  mips_init_relocs ();
}

/* Implement TARGET_OPTION_OPTIMIZATION_TABLE.  */
static const struct default_options mips_option_optimization_table[] =
  {
    { OPT_LEVELS_1_PLUS, OPT_fsection_anchors, NULL, 1 },
    { OPT_LEVELS_1_PLUS, OPT_fomit_frame_pointer, NULL, 1 },
    { OPT_LEVELS_NONE, 0, NULL, 0 }
  };

/* Implement TARGET_CONDITIONAL_REGISTER_USAGE.  */

static void
mips_conditional_register_usage (void)
{
  int regno;

  if (!TARGET_HARD_FLOAT)
    {
      for (regno = FP_REG_FIRST; regno <= FP_REG_LAST; regno++)
	fixed_regs[regno] = call_used_regs[regno] = 1;
    }

  /* Mark all callee-saved registers as caller-saved when riscv_in_utfunc */
  for (regno = CALLEE_SAVED_GP_REG_FIRST;
       regno <= CALLEE_SAVED_GP_REG_LAST; regno++)
    call_used_regs[regno] = call_really_used_regs[regno] = riscv_in_utfunc;

  if (TARGET_HARD_FLOAT)
    {
      for (regno = CALLEE_SAVED_FP_REG_FIRST;
           regno <= CALLEE_SAVED_FP_REG_LAST; regno++)
        call_used_regs[regno] = call_really_used_regs[regno] = riscv_in_utfunc;
    }

  call_used_regs[RETURN_ADDR_REGNUM] =
    call_really_used_regs[RETURN_ADDR_REGNUM] = riscv_in_utfunc;

  fixed_regs[GP_REGNUM] = call_used_regs[GP_REGNUM] = !riscv_in_utfunc;
}

/* Initialize vector TARGET to VALS.  */

void
mips_expand_vector_init (rtx target, rtx vals)
{
  enum machine_mode mode;
  enum machine_mode inner;
  unsigned int i, n_elts;
  rtx mem;

  mode = GET_MODE (target);
  inner = GET_MODE_INNER (mode);
  n_elts = GET_MODE_NUNITS (mode);

  gcc_assert (VECTOR_MODE_P (mode));

  mem = assign_stack_temp (mode, GET_MODE_SIZE (mode), 0);
  for (i = 0; i < n_elts; i++)
    emit_move_insn (adjust_address_nv (mem, inner, i * GET_MODE_SIZE (inner)),
                    XVECEXP (vals, 0, i));

  emit_move_insn (target, mem);
}

/* Implement EPILOGUE_USES.  */

bool
mips_epilogue_uses (unsigned int regno)
{
  /* Say that the epilogue uses the return address register.  Note that
     in the case of sibcalls, the values "used by the epilogue" are
     considered live at the start of the called function.  */
  if (regno == RETURN_ADDR_REGNUM)
    return true;

  return false;
}

/* Implement TARGET_TRAMPOLINE_INIT.  */

static void
mips_trampoline_init (rtx m_tramp, tree fndecl, rtx chain_value)
{
  rtx addr, end_addr, mem;
  rtx trampoline[4];
  unsigned int i, label;
  HOST_WIDE_INT static_chain_offset, target_function_offset;

  /* Work out the offsets of the pointers from the start of the
     trampoline code.  */
  gcc_assert (ARRAY_SIZE (trampoline) * 4 == TRAMPOLINE_CODE_SIZE);
  static_chain_offset = TRAMPOLINE_CODE_SIZE;
  target_function_offset = static_chain_offset + GET_MODE_SIZE (ptr_mode);

  /* Get pointers to the beginning and end of the code block.  */
  addr = force_reg (Pmode, XEXP (m_tramp, 0));
  end_addr = mips_force_binary (Pmode, PLUS, addr, GEN_INT (TRAMPOLINE_CODE_SIZE));
  label = 4;

#define OP(X) gen_int_mode (X, SImode)
#define MATCH_LREG ((Pmode) == DImode ? MATCH_LD : MATCH_LW)

  /* jal     v0, 1f
   1:l[wd]   v1, target_function_offset(v0)
     l[wd]   $static_chain, static_chain_offset(v0)
     jr      v1
  */

  trampoline[0] = OP (RISCV_UJTYPE (JAL, STATIC_CHAIN_REGNUM, label));
  trampoline[1] = OP (RISCV_ITYPE (LREG, MIPS_PROLOGUE_TEMP_REGNUM,
		    STATIC_CHAIN_REGNUM, target_function_offset - label));
  trampoline[2] = OP (RISCV_ITYPE (LREG, STATIC_CHAIN_REGNUM,
		    STATIC_CHAIN_REGNUM, static_chain_offset - label));
  trampoline[3] = OP (RISCV_ITYPE (JALR, 0, MIPS_PROLOGUE_TEMP_REGNUM, 0));

#undef MATCH_LREG
#undef OP

  /* Copy the trampoline code.  Leave any padding uninitialized.  */
  for (i = 0; i < ARRAY_SIZE (trampoline); i++)
    {
      mem = adjust_address (m_tramp, SImode, i * GET_MODE_SIZE (SImode));
      mips_emit_move (mem, trampoline[i]);
    }

  /* Set up the static chain pointer field.  */
  mem = adjust_address (m_tramp, ptr_mode, static_chain_offset);
  mips_emit_move (mem, chain_value);

  /* Set up the target function field.  */
  mem = adjust_address (m_tramp, ptr_mode, target_function_offset);
  mips_emit_move (mem, XEXP (DECL_RTL (fndecl), 0));

  /* Flush the code part of the trampoline.  */
  emit_insn (gen_add3_insn (end_addr, addr, GEN_INT (TRAMPOLINE_SIZE)));
  emit_insn (gen_clear_cache (addr, end_addr));
}

const char*
mips_riscv_output_vector_move(enum machine_mode mode, rtx dest, rtx src)
{
  bool dest_mem, dest_vgp_reg, dest_vfp_reg;
  bool src_mem, src_vgp_reg, src_vfp_reg;

  dest_mem = (GET_CODE(dest) == MEM);
  dest_vgp_reg = (GET_CODE(dest) == REG) && VEC_GP_REG_P(REGNO(dest));
  dest_vfp_reg = (GET_CODE(dest) == REG) && VEC_FP_REG_P(REGNO(dest));

  src_mem = (GET_CODE(src) == MEM);
  src_vgp_reg = (GET_CODE(src) == REG) && VEC_GP_REG_P(REGNO(src));
  src_vfp_reg = (GET_CODE(src) == REG) && VEC_FP_REG_P(REGNO(src));

  if (dest_vgp_reg && src_vgp_reg)
    return "vmvv\t%0,%1";

  if (dest_vfp_reg && src_vfp_reg)
    return "vfmvv\t%0,%1";

  if (dest_vgp_reg && src_mem)
  {
    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DI): return "vld\t%0,%y1";
      case MIPS_RISCV_VECTOR_MODE_NAME(SI): return "vlw\t%0,%y1";
      case MIPS_RISCV_VECTOR_MODE_NAME(HI): return "vlh\t%0,%y1";
      case MIPS_RISCV_VECTOR_MODE_NAME(QI): return "vlb\t%0,%y1";
      default: gcc_unreachable();
    }
  }

  if (dest_vfp_reg && src_mem)
  {
    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DF): return "vfld\t%0,%y1";
      case MIPS_RISCV_VECTOR_MODE_NAME(SF): return "vflw\t%0,%y1";
      default: gcc_unreachable();
    }
  }

  if (dest_mem && src_vgp_reg)
  {
    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DI): return "vsd\t%1,%y0";
      case MIPS_RISCV_VECTOR_MODE_NAME(SI): return "vsw\t%1,%y0";
      case MIPS_RISCV_VECTOR_MODE_NAME(HI): return "vsh\t%1,%y0";
      case MIPS_RISCV_VECTOR_MODE_NAME(QI): return "vsb\t%1,%y0";
      default: gcc_unreachable();
    }
  }

  if (dest_mem && src_vfp_reg)
  {
    switch (mode)
    {
      case MIPS_RISCV_VECTOR_MODE_NAME(DF): return "vfsd\t%1,%y0";
      case MIPS_RISCV_VECTOR_MODE_NAME(SF): return "vfsw\t%1,%y0";
      default: gcc_unreachable();
    }
  }

  gcc_unreachable();
}

/* Initialize the GCC target structure.  */
#undef TARGET_ASM_ALIGNED_HI_OP
#define TARGET_ASM_ALIGNED_HI_OP "\t.half\t"
#undef TARGET_ASM_ALIGNED_SI_OP
#define TARGET_ASM_ALIGNED_SI_OP "\t.word\t"
#undef TARGET_ASM_ALIGNED_DI_OP
#define TARGET_ASM_ALIGNED_DI_OP "\t.dword\t"

#undef TARGET_OPTION_OVERRIDE
#define TARGET_OPTION_OVERRIDE mips_option_override
#undef TARGET_OPTION_OPTIMIZATION_TABLE
#define TARGET_OPTION_OPTIMIZATION_TABLE mips_option_optimization_table

#undef TARGET_LEGITIMIZE_ADDRESS
#define TARGET_LEGITIMIZE_ADDRESS mips_legitimize_address

#undef TARGET_SCHED_ADJUST_COST
#define TARGET_SCHED_ADJUST_COST mips_adjust_cost
#undef TARGET_SCHED_ISSUE_RATE
#define TARGET_SCHED_ISSUE_RATE mips_issue_rate

#undef TARGET_DEFAULT_TARGET_FLAGS
#define TARGET_DEFAULT_TARGET_FLAGS		\
  (TARGET_DEFAULT				\
   | TARGET_CPU_DEFAULT				\
   | (TARGET_64BIT_DEFAULT ? 0 : MASK_32BIT))
#undef TARGET_HANDLE_OPTION
#define TARGET_HANDLE_OPTION mips_handle_option

#undef TARGET_FUNCTION_OK_FOR_SIBCALL
#define TARGET_FUNCTION_OK_FOR_SIBCALL hook_bool_tree_tree_true

#undef TARGET_SET_CURRENT_FUNCTION
#define TARGET_SET_CURRENT_FUNCTION mips_set_current_function

#undef TARGET_REGISTER_MOVE_COST
#define TARGET_REGISTER_MOVE_COST mips_register_move_cost
#undef TARGET_MEMORY_MOVE_COST
#define TARGET_MEMORY_MOVE_COST mips_memory_move_cost
#undef TARGET_RTX_COSTS
#define TARGET_RTX_COSTS mips_rtx_costs
#undef TARGET_ADDRESS_COST
#define TARGET_ADDRESS_COST riscv_address_cost

#undef TARGET_MACHINE_DEPENDENT_REORG
#define TARGET_MACHINE_DEPENDENT_REORG mips_reorg

#undef  TARGET_PREFERRED_RELOAD_CLASS
#define TARGET_PREFERRED_RELOAD_CLASS mips_preferred_reload_class

#undef TARGET_ASM_FILE_START_FILE_DIRECTIVE
#define TARGET_ASM_FILE_START_FILE_DIRECTIVE true

#undef TARGET_EXPAND_BUILTIN_VA_START
#define TARGET_EXPAND_BUILTIN_VA_START mips_va_start

#undef  TARGET_PROMOTE_FUNCTION_MODE
#define TARGET_PROMOTE_FUNCTION_MODE default_promote_function_mode_always_promote

#undef TARGET_RETURN_IN_MEMORY
#define TARGET_RETURN_IN_MEMORY mips_return_in_memory

#undef TARGET_ASM_OUTPUT_MI_THUNK
#define TARGET_ASM_OUTPUT_MI_THUNK mips_output_mi_thunk
#undef TARGET_ASM_CAN_OUTPUT_MI_THUNK
#define TARGET_ASM_CAN_OUTPUT_MI_THUNK hook_bool_const_tree_hwi_hwi_const_tree_true

#undef TARGET_PRINT_OPERAND
#define TARGET_PRINT_OPERAND mips_print_operand
#undef TARGET_PRINT_OPERAND_ADDRESS
#define TARGET_PRINT_OPERAND_ADDRESS mips_print_operand_address

#undef TARGET_SETUP_INCOMING_VARARGS
#define TARGET_SETUP_INCOMING_VARARGS mips_setup_incoming_varargs
#undef TARGET_STRICT_ARGUMENT_NAMING
#define TARGET_STRICT_ARGUMENT_NAMING hook_bool_CUMULATIVE_ARGS_true
#undef TARGET_MUST_PASS_IN_STACK
#define TARGET_MUST_PASS_IN_STACK must_pass_in_stack_var_size
#undef TARGET_PASS_BY_REFERENCE
#define TARGET_PASS_BY_REFERENCE riscv_pass_by_reference
#undef TARGET_ARG_PARTIAL_BYTES
#define TARGET_ARG_PARTIAL_BYTES mips_arg_partial_bytes
#undef TARGET_FUNCTION_ARG
#define TARGET_FUNCTION_ARG mips_function_arg
#undef TARGET_FUNCTION_ARG_ADVANCE
#define TARGET_FUNCTION_ARG_ADVANCE mips_function_arg_advance
#undef TARGET_FUNCTION_ARG_BOUNDARY
#define TARGET_FUNCTION_ARG_BOUNDARY mips_function_arg_boundary

#undef TARGET_MODE_REP_EXTENDED
#define TARGET_MODE_REP_EXTENDED mips_mode_rep_extended

#undef TARGET_VECTOR_MODE_SUPPORTED_P
#define TARGET_VECTOR_MODE_SUPPORTED_P mips_vector_mode_supported_p

#undef TARGET_SCALAR_MODE_SUPPORTED_P
#define TARGET_SCALAR_MODE_SUPPORTED_P mips_scalar_mode_supported_p

#undef TARGET_INIT_BUILTINS
#define TARGET_INIT_BUILTINS mips_init_builtins
#undef TARGET_BUILTIN_DECL
#define TARGET_BUILTIN_DECL mips_builtin_decl
#undef TARGET_EXPAND_BUILTIN
#define TARGET_EXPAND_BUILTIN mips_expand_builtin

#undef TARGET_HAVE_TLS
#define TARGET_HAVE_TLS HAVE_AS_TLS

#undef TARGET_CANNOT_FORCE_CONST_MEM
#define TARGET_CANNOT_FORCE_CONST_MEM mips_cannot_force_const_mem

#undef TARGET_ATTRIBUTE_TABLE
#define TARGET_ATTRIBUTE_TABLE mips_attribute_table
/* All our function attributes are related to how out-of-line copies should
   be compiled or called.  They don't in themselves prevent inlining.  */
#undef TARGET_FUNCTION_ATTRIBUTE_INLINABLE_P
#define TARGET_FUNCTION_ATTRIBUTE_INLINABLE_P hook_bool_const_tree_true

#undef TARGET_USE_BLOCKS_FOR_CONSTANT_P
#define TARGET_USE_BLOCKS_FOR_CONSTANT_P hook_bool_mode_const_rtx_true

#undef  TARGET_COMP_TYPE_ATTRIBUTES
#define TARGET_COMP_TYPE_ATTRIBUTES mips_comp_type_attributes

#ifdef HAVE_AS_DTPRELWORD
#undef TARGET_ASM_OUTPUT_DWARF_DTPREL
#define TARGET_ASM_OUTPUT_DWARF_DTPREL mips_output_dwarf_dtprel
#endif

#undef TARGET_IRA_COVER_CLASSES
#define TARGET_IRA_COVER_CLASSES mips_ira_cover_classes

#undef TARGET_LEGITIMATE_ADDRESS_P
#define TARGET_LEGITIMATE_ADDRESS_P	mips_legitimate_address_p

#undef TARGET_CAN_ELIMINATE
#define TARGET_CAN_ELIMINATE mips_can_eliminate

#undef TARGET_CONDITIONAL_REGISTER_USAGE
#define TARGET_CONDITIONAL_REGISTER_USAGE mips_conditional_register_usage

#undef TARGET_TRAMPOLINE_INIT
#define TARGET_TRAMPOLINE_INIT mips_trampoline_init

#undef TARGET_IN_SMALL_DATA_P
#define TARGET_IN_SMALL_DATA_P riscv_in_small_data_p

#undef TARGET_ASM_SELECT_RTX_SECTION
#define TARGET_ASM_SELECT_RTX_SECTION  riscv_elf_select_rtx_section

#undef TARGET_MIN_ANCHOR_OFFSET
#define TARGET_MIN_ANCHOR_OFFSET (-RISCV_IMM_REACH/2)

#undef TARGET_MAX_ANCHOR_OFFSET
#define TARGET_MAX_ANCHOR_OFFSET (RISCV_IMM_REACH/2-1)

struct gcc_target targetm = TARGET_INITIALIZER;

#include "gt-riscv.h"
