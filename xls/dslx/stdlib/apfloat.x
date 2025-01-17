// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Arbitrary-precision floating point routines.
import std

pub struct APFloat<EXP_SZ:u32, SFD_SZ:u32> {
  sign: bits[1],  // sign bit
  bexp: bits[EXP_SZ],  // biased exponent
  sfd:  bits[SFD_SZ],  // significand (no hidden bit)
}

pub enum APFloatTag : u3 {
  NAN       = 0,
  INFINITY  = 1,
  SUBNORMAL = 2,
  ZERO      = 3,
  NORMAL    = 4,
}

pub fn tag<EXP_SZ:u32, SFD_SZ:u32>(input_float: APFloat<EXP_SZ, SFD_SZ>) -> APFloatTag {
  const EXPR_MASK = std::mask_bits<EXP_SZ>();
  match (input_float.bexp, input_float.sfd) {
    (uN[EXP_SZ]:0, uN[SFD_SZ]:0) => APFloatTag::ZERO,
    (uN[EXP_SZ]:0,            _) => APFloatTag::SUBNORMAL,
    (   EXPR_MASK, uN[SFD_SZ]:0) => APFloatTag::INFINITY,
    (   EXPR_MASK,            _) => APFloatTag::NAN,
    (           _,            _) => APFloatTag::NORMAL,
  }
}

pub fn qnan<EXP_SZ:u32, SFD_SZ:u32>() -> APFloat<EXP_SZ, SFD_SZ> {
  APFloat<EXP_SZ, SFD_SZ> {
    sign: bits[1]:0,
    bexp: std::mask_bits<EXP_SZ>() as bits[EXP_SZ],
    sfd: bits[SFD_SZ]:1 << ((SFD_SZ - u32:1) as bits[SFD_SZ])
  }
}

#![test]
fn qnan_test() {
  let expected = APFloat<u32:8, u32:23> {
    sign: u1:0, bexp: u8:0xff, sfd: u23:0x400000,
  };
  let actual = qnan<u32:8, u32:23>();
  let _ = assert_eq(actual, expected);

  let expected = APFloat<u32:4, u32:2> {
    sign: u1:0, bexp: u4:0xf, sfd: u2:0x2,
  };
  let actual = qnan<u32:4, u32:2>();
  let _ = assert_eq(actual, expected);
  ()
}

pub fn zero<EXP_SZ:u32, SFD_SZ:u32>(sign: bits[1])
    -> APFloat<EXP_SZ, SFD_SZ> {
  APFloat<EXP_SZ, SFD_SZ>{
    sign: sign,
    bexp: bits[EXP_SZ]:0,
    sfd: bits[SFD_SZ]:0 }
}

#![test]
fn zero_test() {
  let expected = APFloat<u32:8, u32:23> {
    sign: u1:0, bexp: u8:0x0, sfd: u23:0x0,
  };
  let actual = zero<u32:8, u32:23>(u1:0);
  let _ = assert_eq(actual, expected);

  let expected = APFloat<u32:4, u32:2> {
    sign: u1:1, bexp: u4:0x0, sfd: u2:0x0,
  };
  let actual = zero<u32:4, u32:2>(u1:1);
  let _ = assert_eq(actual, expected);
  ()
}

pub fn one<EXP_SZ:u32, SFD_SZ:u32, MASK_SZ:u32 = EXP_SZ - u32:1>(
    sign: bits[1])
    -> APFloat<EXP_SZ, SFD_SZ> {
  APFloat<EXP_SZ, SFD_SZ>{
    sign: sign,
    bexp: std::mask_bits<MASK_SZ>() as bits[EXP_SZ],
    sfd: bits[SFD_SZ]:0
  }
}

#![test]
fn one_test() {
  let expected = APFloat<u32:8, u32:23> {
    sign: u1:0, bexp: u8:0x7f, sfd: u23:0x0,
  };
  let actual = one<u32:8, u32:23>(u1:0);
  let _ = assert_eq(actual, expected);

  let expected = APFloat<u32:4, u32:2> {
    sign: u1:0, bexp: u4:0x7, sfd: u2:0x0,
  };
  let actual = one<u32:4, u32:2>(u1:0);
  let _ = assert_eq(actual, expected);
  ()
}

pub fn inf<EXP_SZ:u32, SFD_SZ:u32>(sign: bits[1]) -> APFloat<EXP_SZ, SFD_SZ> {
  APFloat<EXP_SZ, SFD_SZ>{
    sign: sign,
    bexp: std::mask_bits<EXP_SZ>(),
    sfd: bits[SFD_SZ]:0
  }
}

#![test]
fn inf_test() {
  let expected = APFloat<u32:8, u32:23> {
    sign: u1:0, bexp: u8:0xff, sfd: u23:0x0,
  };
  let actual = inf<u32:8, u32:23>(u1:0);
  let _ = assert_eq(actual, expected);

  let expected = APFloat<u32:4, u32:2> {
    sign: u1:0, bexp: u4:0xf, sfd: u2:0x0,
  };
  let actual = inf<u32:4, u32:2>(u1:0);
  let _ = assert_eq(actual, expected);
  ()
}

// Accessor helpers for the F32 typedef.
pub fn unbiased_exponent<EXP_SZ:u32, SFD_SZ:u32, UEXP_SZ:u32 = EXP_SZ + u32:1, MASK_SZ:u32 = EXP_SZ - u32:1>(
    f: APFloat<EXP_SZ, SFD_SZ>)
    -> bits[UEXP_SZ] {
  (f.bexp as bits[UEXP_SZ]) - (std::mask_bits<MASK_SZ>() as bits[UEXP_SZ])
}

#![test]
fn unbiased_exponent_test() {
  let expected = u9:0x0;
  let actual = unbiased_exponent<u32:8, u32:23>(
      APFloat<u32:8, u32:23> { sign: u1:0, bexp: u8:0x7f, sfd: u23:0 });
  let _ = assert_eq(actual, expected);
  ()
}

pub fn bias<EXP_SZ: u32, SFD_SZ: u32, UEXP_SZ: u32 = EXP_SZ + u32:1,
    MASK_SZ: u32 = EXP_SZ - u32:1>(unbiased_exponent: bits[UEXP_SZ]) -> bits[EXP_SZ] {
  (unbiased_exponent + (std::mask_bits<MASK_SZ>() as bits[UEXP_SZ])) as bits[EXP_SZ]
}

#![test]
fn bias_test() {
  let expected = u8:127;
  let actual = bias<u32:8, u32:23>(u9:0);
  let _ = assert_eq(expected, actual);
  ()
}

pub fn flatten<EXP_SZ:u32, SFD_SZ:u32, TOTAL_SZ:u32 = u32:1+EXP_SZ+SFD_SZ>(
    x: APFloat<EXP_SZ, SFD_SZ>) -> bits[TOTAL_SZ] {
  x.sign ++ x.bexp ++ x.sfd
}

pub fn unflatten<EXP_SZ:u32, SFD_SZ:u32,
    TOTAL_SZ:u32 = u32:1+EXP_SZ+SFD_SZ,
    SIGN_OFFSET:u32 = EXP_SZ+SFD_SZ>(
    x: bits[TOTAL_SZ]) -> APFloat<EXP_SZ, SFD_SZ> {
  APFloat<EXP_SZ, SFD_SZ>{
      sign: (x >> (SIGN_OFFSET as bits[TOTAL_SZ])) as bits[1],
      bexp: (x >> (SFD_SZ as bits[TOTAL_SZ])) as bits[EXP_SZ],
      sfd: x as bits[SFD_SZ],
  }
}

pub fn subnormals_to_zero<EXP_SZ:u32, SFD_SZ:u32>(
    x: APFloat<EXP_SZ, SFD_SZ>) -> APFloat<EXP_SZ, SFD_SZ> {
  zero<EXP_SZ, SFD_SZ>(x.sign) if x.bexp == bits[EXP_SZ]:0 else x
}

// Returns a normalized APFloat with the given components. 'sfd_with_hidden' is the
// significand including the hidden bit. This function only normalizes in the
// direction of decreasing the exponent. Input must be a normal number or
// zero. Dernormals are flushed to zero in the result.
pub fn normalize<EXP_SZ:u32, SFD_SZ:u32, WIDE_SFD:u32 = SFD_SZ + u32:1>(
    sign: bits[1], exp: bits[EXP_SZ], sfd_with_hidden: bits[WIDE_SFD])
    -> APFloat<EXP_SZ, SFD_SZ> {
  let leading_zeros = clz(sfd_with_hidden) as bits[SFD_SZ]; // as bits[clog2(SFD_SZ)]?
  let zero_value = zero<EXP_SZ, SFD_SZ>(sign);
  let zero_sfd = WIDE_SFD as bits[SFD_SZ];
  let normalized_sfd = (sfd_with_hidden << (leading_zeros as bits[WIDE_SFD])) as bits[SFD_SZ];

  let is_denormal = exp <= (leading_zeros as bits[EXP_SZ]);
  match (is_denormal, leading_zeros) {
    // Significand is zero.
    (_, zero_sfd) => zero_value,
    // Flush denormals to zero.
    (true, _) => zero_value,
    // Normalize.
    _ => APFloat { sign: sign,
                   bexp: exp - (leading_zeros as bits[EXP_SZ]),
                   sfd: normalized_sfd },
  }
}

pub fn is_inf<EXP_SZ:u32, SFD_SZ:u32>(x: APFloat<EXP_SZ, SFD_SZ>) -> u1 {
  (x.bexp == std::mask_bits<EXP_SZ>() && x.sfd == bits[SFD_SZ]:0)
}

// Returns whether or not the given F32 represents NaN.
pub fn is_nan<EXP_SZ:u32, SFD_SZ:u32>(x: APFloat<EXP_SZ, SFD_SZ>) -> u1 {
  (x.bexp == std::mask_bits<EXP_SZ>() && x.sfd != bits[SFD_SZ]:0)
}

// TODO(rspringer): Create a broadly-applicable normalize test, that
// could be used for multiple type instantiations (without needing
// per-specialization data to be specified by a user).
// Returns whether or not the given APFloat represents an infinite quantity.
