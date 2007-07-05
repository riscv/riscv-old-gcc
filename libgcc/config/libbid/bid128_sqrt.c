/* Copyright (C) 2007  Free Software Foundation, Inc.

This file is part of GCC.

GCC is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free
Software Foundation; either version 2, or (at your option) any later
version.

In addition to the permissions in the GNU General Public License, the
Free Software Foundation gives you unlimited permission to link the
compiled version of this file into combinations with other programs,
and to distribute those combinations without any restriction coming
from the use of this file.  (The General Public License restrictions
do apply in other respects; for example, they cover modification of
the file, and distribution when not linked into a combine
executable.)

GCC is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or
FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
for more details.

You should have received a copy of the GNU General Public License
along with GCC; see the file COPYING.  If not, write to the Free
Software Foundation, 51 Franklin Street, Fifth Floor, Boston, MA
02110-1301, USA.  */

#define BID_128RES
#include "sqrt_macros.h"

BID128_FUNCTION_ARG1(__bid128_sqrt, x)

  UINT256 M256, C256, C4, C8;
  UINT128 CX, CX1, CX2, A10, S2, T128, TP128, CS, CSM, res;
  UINT64 sign_x, Carry;
  SINT64 D;
  int_float fx, f64;
  int exponent_x = 0, bin_expon_cx;
  int digits, scale, exponent_q;

  // unpack arguments, check for NaN or Infinity
  if (!unpack_BID128_value (&sign_x, &exponent_x, &CX, x)) {
    res.w[1] = x.w[1];
    res.w[0] = x.w[0];
    // NaN ?
    if ((x.w[1] & 0x7c00000000000000ull) == 0x7c00000000000000ull) {
#ifdef SET_STATUS_FLAGS
      if ((x.w[1] & 0x7e00000000000000ull) == 0x7e00000000000000ull)	// sNaN
	__set_status_flags (pfpsf, INVALID_EXCEPTION);
#endif
	  res.w[1] &= QUIET_MASK64;
      BID_RETURN (res);
    }
    // x is Infinity?
    if ((x.w[1] & 0x7800000000000000ull) == 0x7800000000000000ull) {
      if (sign_x) {
	// -Inf, return NaN
	res.w[1] = 0x7c00000000000000ull;
#ifdef SET_STATUS_FLAGS
	__set_status_flags (pfpsf, INVALID_EXCEPTION);
#endif
      }
      BID_RETURN (res);
    }
    // x is 0 otherwise

    res.w[1] =
      sign_x |
      ((((UINT64) (exponent_x + DECIMAL_EXPONENT_BIAS_128)) >> 1) <<
       49);
    BID_RETURN (res);
  }
  if (sign_x) {
    res.w[1] = 0x7c00000000000000ull;
    res.w[0] = 0;
#ifdef SET_STATUS_FLAGS
    __set_status_flags (pfpsf, INVALID_EXCEPTION);
#endif
    BID_RETURN (res);
  }
  // 2^64
  f64.i = 0x5f800000;

  // fx ~ CX
  fx.d = (float) CX.w[1] * f64.d + (float) CX.w[0];
  bin_expon_cx = ((fx.i >> 23) & 0xff) - 0x7f;
  digits = __bid_estimate_decimal_digits[bin_expon_cx];

  A10 = CX;
  if (exponent_x & 1) {
    A10.w[1] = (CX.w[1] << 3) | (CX.w[0] >> 61);
    A10.w[0] = CX.w[0] << 3;
    CX2.w[1] = (CX.w[1] << 1) | (CX.w[0] >> 63);
    CX2.w[0] = CX.w[0] << 1;
    __add_128_128 (A10, A10, CX2);
  }

  CS.w[0] = short_sqrt128 (A10);
  CS.w[1] = 0;
  // check for exact result
  if (CS.w[0] * CS.w[0] == A10.w[0]) {
    __mul_64x64_to_128_fast (S2, CS.w[0], CS.w[0]);
    if (S2.w[1] == A10.w[1])	// && S2.w[0]==A10.w[0])
    {
      get_BID128_very_fast (&res, 0,
			    (exponent_x +
			     DECIMAL_EXPONENT_BIAS_128) >> 1, CS);
      BID_RETURN (res);
    }
  }
  // get number of digits in CX
  D = CX.w[1] - __bid_power10_index_binexp_128[bin_expon_cx].w[1];
  if (D > 0
      || (!D && CX.w[0] >= __bid_power10_index_binexp_128[bin_expon_cx].w[0]))
    digits++;

  // if exponent is odd, scale coefficient by 10
  scale = 67 - digits;
  exponent_q = exponent_x - scale;
  scale += (exponent_q & 1);	// exp. bias is even

  if (scale > 38) {
    T128 = __bid_power10_table_128[scale - 37];
    __mul_128x128_low (CX1, CX, T128);

    TP128 = __bid_power10_table_128[37];
    __mul_128x128_to_256 (C256, CX1, TP128);
  } else {
    T128 = __bid_power10_table_128[scale];
    __mul_128x128_to_256 (C256, CX, T128);
  }


  // 4*C256
  C4.w[3] = (C256.w[3] << 2) | (C256.w[2] >> 62);
  C4.w[2] = (C256.w[2] << 2) | (C256.w[1] >> 62);
  C4.w[1] = (C256.w[1] << 2) | (C256.w[0] >> 62);
  C4.w[0] = C256.w[0] << 2;

  long_sqrt128 (&CS, C256);

#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  if (!((rnd_mode) & 3)) {
#endif
#endif
    // compare to midpoints
    CSM.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
    CSM.w[0] = (CS.w[0] + CS.w[0]) | 1;
    // CSM^2
    //__mul_128x128_to_256(M256, CSM, CSM);
    __sqr128_to_256 (M256, CSM);

    if (C4.w[3] > M256.w[3]
	|| (C4.w[3] == M256.w[3]
	    && (C4.w[2] > M256.w[2]
		|| (C4.w[2] == M256.w[2]
		    && (C4.w[1] > M256.w[1]
			|| (C4.w[1] == M256.w[1]
			    && C4.w[0] > M256.w[0])))))) {
      // round up
      CS.w[0]++;
      if (!CS.w[0])
	CS.w[1]++;
    } else {
      C8.w[1] = (CS.w[1] << 3) | (CS.w[0] >> 61);
      C8.w[0] = CS.w[0] << 3;
      // M256 - 8*CSM
      __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
      __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
      __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
      M256.w[3] = M256.w[3] - Carry;

      // if CSM' > C256, round up
      if (M256.w[3] > C4.w[3]
	  || (M256.w[3] == C4.w[3]
	      && (M256.w[2] > C4.w[2]
		  || (M256.w[2] == C4.w[2]
		      && (M256.w[1] > C4.w[1]
			  || (M256.w[1] == C4.w[1]
			      && M256.w[0] > C4.w[0])))))) {
	// round down
	if (!CS.w[0])
	  CS.w[1]--;
	CS.w[0]--;
      }
    }
#ifndef IEEE_ROUND_NEAREST
#ifndef IEEE_ROUND_NEAREST_TIES_AWAY
  } else {
    __sqr128_to_256 (M256, CS);
    C8.w[1] = (CS.w[1] << 1) | (CS.w[0] >> 63);
    C8.w[0] = CS.w[0] << 1;
    if (M256.w[3] > C256.w[3]
	|| (M256.w[3] == C256.w[3]
	    && (M256.w[2] > C256.w[2]
		|| (M256.w[2] == C256.w[2]
		    && (M256.w[1] > C256.w[1]
			|| (M256.w[1] == C256.w[1]
			    && M256.w[0] > C256.w[0])))))) {
      __sub_borrow_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
      __sub_borrow_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
      __sub_borrow_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
      M256.w[3] = M256.w[3] - Carry;
      M256.w[0]++;
      if (!M256.w[0]) {
	M256.w[1]++;
	if (!M256.w[1]) {
	  M256.w[2]++;
	  if (!M256.w[2])
	    M256.w[3]++;
	}
      }

      if (!CS.w[0])
	CS.w[1]--;
      CS.w[0]--;

      if (M256.w[3] > C256.w[3]
	  || (M256.w[3] == C256.w[3]
	      && (M256.w[2] > C256.w[2]
		  || (M256.w[2] == C256.w[2]
		      && (M256.w[1] > C256.w[1]
			  || (M256.w[1] == C256.w[1]
			      && M256.w[0] > C256.w[0])))))) {

	if (!CS.w[0])
	  CS.w[1]--;
	CS.w[0]--;
      }
    }

    else {
      __add_carry_out (M256.w[0], Carry, M256.w[0], C8.w[0]);
      __add_carry_in_out (M256.w[1], Carry, M256.w[1], C8.w[1], Carry);
      __add_carry_in_out (M256.w[2], Carry, M256.w[2], 0, Carry);
      M256.w[3] = M256.w[3] + Carry;
      M256.w[0]++;
      if (!M256.w[0]) {
	M256.w[1]++;
	if (!M256.w[1]) {
	  M256.w[2]++;
	  if (!M256.w[2])
	    M256.w[3]++;
	}
      }
      if (M256.w[3] < C256.w[3]
	  || (M256.w[3] == C256.w[3]
	      && (M256.w[2] < C256.w[2]
		  || (M256.w[2] == C256.w[2]
		      && (M256.w[1] < C256.w[1]
			  || (M256.w[1] == C256.w[1]
			      && M256.w[0] <= C256.w[0])))))) {

	CS.w[0]++;
	if (!CS.w[0])
	  CS.w[1]++;
      }
    }
    // RU?
    if ((rnd_mode) == ROUNDING_UP) {
      CS.w[0]++;
      if (!CS.w[0])
	CS.w[1]++;
    }

  }
#endif
#endif

#ifdef SET_STATUS_FLAGS
  __set_status_flags (pfpsf, INEXACT_EXCEPTION);
#endif
  get_BID128_fast (&res, 0,
		   (exponent_q + DECIMAL_EXPONENT_BIAS_128) >> 1, CS);
  BID_RETURN (res);
}
