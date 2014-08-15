/* Low-level functions for atomic operations. Mips version.
   Copyright (C) 2005 Free Software Foundation, Inc.
   This file is part of the GNU C Library.

   The GNU C Library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   The GNU C Library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with the GNU C Library; if not, write to the Free
   Software Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
   02111-1307 USA.  */

#ifndef _MIPS_BITS_ATOMIC_H
#define _MIPS_BITS_ATOMIC_H 1

#include <inttypes.h>
#include <sgidefs.h>

typedef int32_t atomic32_t;
typedef uint32_t uatomic32_t;
typedef int_fast32_t atomic_fast32_t;
typedef uint_fast32_t uatomic_fast32_t;

typedef int64_t atomic64_t;
typedef uint64_t uatomic64_t;
typedef int_fast64_t atomic_fast64_t;
typedef uint_fast64_t uatomic_fast64_t;

typedef intptr_t atomicptr_t;
typedef uintptr_t uatomicptr_t;
typedef intmax_t atomic_max_t;
typedef uintmax_t uatomic_max_t;

#ifdef __riscv_atomic

#define asm_amo(which, ordering, mem, value) ({ 		\
  typeof(*mem) __tmp; 						\
  if (sizeof(__tmp) == 4)					\
    asm volatile (which ".w" ordering "\t%0, %z1, (%2)" : "=r"(__tmp) : "dJ"(value), "r"(mem)); \
  else if (sizeof(__tmp) == 8)					\
    asm volatile (which ".d" ordering "\t%0, %z1, (%2)" : "=r"(__tmp) : "dJ"(value), "r"(mem)); \
  else								\
    abort();							\
  __tmp; })

#define asm_load_reserved(ordering, mem) ({ 			\
  typeof(*mem) __tmp; 						\
  if (sizeof(__tmp) == 4)					\
    asm volatile ("lr.w" ordering "\t%0, (%1)" : "=r"(__tmp) : "r"(mem)); \
  else if (sizeof(__tmp) == 8)					\
    asm volatile ("lr.d" ordering "\t%0, (%1)" : "=r"(__tmp) : "r"(mem)); \
  else								\
    abort();							\
  __tmp; })

#define asm_store_conditional(ordering, mem, value) \
  asm_amo("sc", ordering, mem, value)

/* Atomic compare and exchange. */

#define atomic_cas(ordering, mem, newval, oldval) ({ 	\
  typeof(*mem) __tmp; 						\
  int __tmp2; 							\
  if (sizeof(__tmp) == 4)					\
    asm volatile ("1: lr.w" ordering "\t%0, (%2)\n"		\
                  "bne\t%0, %z4, 1f\n"				\
		  "sc.w" ordering "\t%1, %z3, (%2)\n"		\
		  "bnez\t%1, 1b\n"				\
		  "1:"						\
		  : "=&r"(__tmp), "=&r"(__tmp2) : "r"(mem), "dJ"(newval), "dJ"(oldval)); \
  else if (sizeof(__tmp) == 8)					\
    asm volatile ("1: lr.d" ordering "\t%0, (%2)\n"		\
                  "bne\t%0, %z4, 1f\n"				\
		  "sc.d" ordering "\t%1, %z3, (%2)\n"		\
		  "bnez\t%1, 1b\n"				\
		  "1:"						\
		  : "=&r"(__tmp), "=&r"(__tmp2) : "r"(mem), "dJ"(newval), "dJ"(oldval)); \
  else								\
    abort();							\
  __tmp; })

#define atomic_cas_bool(ordering, mem, newval, oldval) ({ 	\
  __label__ failure, success;					\
  typeof(*mem) __tmp; 						\
  int __res;							\
  if (sizeof(__tmp) == 4)					\
    asm goto ("1: lr.w" ordering "\tt3, (%0)\n"			\
                  "bne\tt3, %z2, %l[failure]\n"			\
		  "sc.w" ordering "\tt3, %z1, (%0)\n"		\
		  "bnez\tt3, 1b"				\
		  : : "r"(mem), "dJ"(newval), "dJ"(oldval) : "t3" : failure); \
  else if (sizeof(__tmp) == 8)					\
    asm goto ("1: lr.d" ordering "\tt3, (%0)\n"			\
                  "bne\tt3, %z2, %l[failure]\n"			\
		  "sc.d" ordering "\tt3, %z1, (%0)\n"		\
		  "bnez\tt3, 1b"				\
		  : : "r"(mem), "dJ"(newval), "dJ"(oldval) : "t3" : failure); \
  else								\
    abort();							\
  __res = 0;							\
  goto success;							\
failure:							\
  __res = 1;							\
success:							\
  __res; })

#define atomic_compare_and_exchange_val_acq(mem, newval, oldval) \
  atomic_cas(".aq", mem, newval, oldval)

#define atomic_compare_and_exchange_val_rel(mem, newval, oldval) \
  atomic_cas(".rl", mem, newval, oldval)

#define atomic_compare_and_exchange_bool_acq(mem, newval, oldval) \
  atomic_cas_bool(".aq", mem, newval, oldval)

#define atomic_compare_and_exchange_bool_rel(mem, newval, oldval) \
  atomic_cas_bool(".rl", mem, newval, oldval)

/* Atomic exchange (without compare).  */

#define atomic_exchange_acq(mem, value) asm_amo("amoswap", ".aq", mem, value)
#define atomic_exchange_rel(mem, value) asm_amo("amoswap", ".rl", mem, value)


/* Atomically add value and return the previous (unincremented) value.  */

#define atomic_exchange_and_add(mem, value) asm_amo("amoadd", "", mem, value)

#define atomic_max(mem, value) asm_amo("amomaxu", "", mem, value)
#define atomic_min(mem, value) asm_amo("amominu", "", mem, value)

#define atomic_bit_test_set(mem, bit)                   \
  ({ typeof(*mem) __mask = (typeof(*mem))1 << (bit);    \
     asm_amo("amoor", "", mem, __mask) & __mask; })

#define atomic_full_barrier() __sync_synchronize()

#define catomic_exchange_and_add(mem, value)		\
  atomic_exchange_and_add(mem, value)
#define catomic_max(mem, value) atomic_max(mem, value)

#else /* !__riscv_atomic */

#include <sys/syscall.h>

#define __arch_compare_and_exchange_val_8_acq(mem, newval, oldval) \
  (abort (), (__typeof (*mem)) 0)

#define __arch_compare_and_exchange_val_16_acq(mem, newval, oldval) \
  (abort (), (__typeof (*mem)) 0)

/* The only basic operation needed is compare and exchange.  */
#define __arch_compare_and_exchange_val_32_acq(mem, newval, oldval) \
({ 									\
	long _sys_result;						\
									\
	{								\
	register long __v0 asm("v0") = __NR_sysriscv;			\
	register long __a0 asm("a0") = (long) (RISCV_ATOMIC_CMPXCHG);	\
	register long __a1 asm("a1") = (long) (mem); 			\
	register long __a2 asm("a2") = (long) (oldval);			\
	register long __a3 asm("a3") = (long) (newval);   		\
	__asm__ volatile ( 						\
	"scall\n\t" 							\
	: "+r" (__v0)							\
	: "r" (__v0), "r"(__a0), "r"(__a1), "r"(__a2), "r"(__a3)	\
	: "v1", "memory"); 						\
	_sys_result = __v0;						\
	}								\
	_sys_result;							\
})

#define __arch_compare_and_exchange_val_64_acq(mem, newval, oldval) \
({ 									\
	long _sys_result;						\
									\
	{								\
	register long __v0 asm("v0") = __NR_sysriscv;			\
	register long __a0 asm("a0") = (long) (RISCV_ATOMIC_CMPXCHG64);	\
	register long __a1 asm("a1") = (long) (mem); 			\
	register long __a2 asm("a2") = (long) (oldval);			\
	register long __a3 asm("a3") = (long) (newval);   		\
	__asm__ volatile ( 						\
	"scall\n\t" 							\
	: "+r" (__v0)							\
	: "r" (__v0), "r"(__a0), "r"(__a1), "r"(__a2), "r"(__a3)	\
	: "v1", "memory"); 						\
	_sys_result = __v0;						\
	}								\
	_sys_result;							\
})

#endif /* __riscv_atomic */

#endif /* bits/atomic.h */
