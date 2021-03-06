//
// Copyright 2019 ZetaSQL Authors
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
//

#include "zetasql/public/numeric_value.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <type_traits>

#include "zetasql/base/logging.h"
#include "zetasql/common/errors.h"
#include "zetasql/common/fixed_int.h"
#include "absl/base/attributes.h"
#include <cstdint>
#include "absl/base/optimization.h"
#include "absl/strings/ascii.h"
#include "absl/strings/string_view.h"
#include "absl/types/optional.h"
#include "zetasql/base/bits.h"
#include "zetasql/base/endian.h"
#include "zetasql/base/status.h"
#include "zetasql/base/status_macros.h"
#include "zetasql/base/statusor.h"

namespace zetasql {

namespace {

// Maximum number of decimal digits in the integer and the fractional part of a
// NUMERIC value.
constexpr int kMaxIntegerDigits = 29;
constexpr int kMaxFractionalDigits = 9;
constexpr int kBigNumericMaxFractionalDigits = 38;

constexpr int kBitsPerByte = 8;
constexpr int kBitsPerUint64 = 64;
constexpr int kBitsPerInt64 = 64;
constexpr int kBitsPerInt128 = 128;
constexpr int kBytesPerInt64 = kBitsPerInt64 / kBitsPerByte;
constexpr int kBytesPerInt128 = kBitsPerInt128 / kBitsPerByte;

inline zetasql_base::Status MakeInvalidNumericError(absl::string_view str) {
  return MakeEvalError() << "Invalid NUMERIC value: " << str;
}

// Returns OK if the given character is a decimal ascii digit '0' to '9'.
// Returns an INVALID_ARGUMENT otherwise.
inline zetasql_base::Status ValidateAsciiDigit(char c, absl::string_view str) {
  if (ABSL_PREDICT_FALSE(!absl::ascii_isdigit(c))) {
    return MakeInvalidNumericError(str);
  }

  return zetasql_base::OkStatus();
}

// Returns -1, 0 or 1 if the given int128 number is negative, zero of positive
// respectively.
inline int int128_sign(__int128 x) {
  return (0 < x) - (x < 0);
}

inline unsigned __int128 int128_abs(__int128 x) {
  // Must cast to unsigned type before negation. Negation of signed integer
  // could overflow and has undefined behavior in C/C++.
  return (x >= 0) ? x : -static_cast<unsigned __int128>(x);
}

FixedInt<64, 6> GetScaledCovarianceNumerator(const FixedInt<64, 3>& sum_x,
                                             const FixedInt<64, 3>& sum_y,
                                             const FixedInt<64, 5>& sum_product,
                                             uint64_t count) {
  FixedInt<64, 6> numerator(sum_product);
  numerator *= count;
  numerator -= ExtendAndMultiply(sum_x, sum_y);
  return numerator;
}

double GetCovariance(const FixedInt<64, 3>& sum_x,
                     const FixedInt<64, 3>& sum_y,
                     const FixedInt<64, 5>& sum_product,
                     uint64_t count,
                     uint64_t count_offset) {
  FixedInt<64, 6> numerator(
      GetScaledCovarianceNumerator(sum_x, sum_y, sum_product, count));
  FixedUint<64, 3> denominator(count);
  denominator *= (count - count_offset);
  denominator *= (static_cast<uint64_t>(NumericValue::kScalingFactor) *
                  NumericValue::kScalingFactor);
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

template <int n>
void SerializeFixedInt(std::string* dest, const FixedInt<64, n>& num) {
  num.SerializeToBytes(dest);
}

template <int n1, int... n>
void SerializeFixedInt(std::string* dest, const FixedInt<64, n1>& num1,
                       const FixedInt<64, n>&... num) {
  static_assert(sizeof(num1) < 128);
  size_t old_size = dest->size();
  dest->push_back('\0');  // add a place holder for size
  num1.SerializeToBytes(dest);
  DCHECK_LE(dest->size() - old_size, 128);
  (*dest)[old_size] = static_cast<char>(dest->size() - old_size - 1);
  SerializeFixedInt(dest, num...);
}

template <int n>
bool DeserializeFixedInt(absl::string_view bytes, FixedInt<64, n>* num) {
  return num->DeserializeFromBytes(bytes);
}

template <int n1, int... n>
bool DeserializeFixedInt(absl::string_view bytes, FixedInt<64, n1>* num1,
                         FixedInt<64, n>*... num) {
  if (!bytes.empty()) {
    int len = bytes[0];
    return len < bytes.size() - 1 &&
           num1->DeserializeFromBytes(bytes.substr(1, len)) &&
           DeserializeFixedInt(bytes.substr(len + 1), num...);
  }
  return false;
}

// Helper method for appending a decimal value to a string. This function
// assumes the value is not zero, and the FixedInt string has already been
// appended to output. This function adds the decimal point and adjusts the
// leading and trailing zeros. Examples:
// (1, 9, "-123") -> "-0.000000123"
// (1, 9, "-123456789") -> "-0.123456789"
// (1, 9, "-1234567890") -> "-1.23456789"
void AddDecimalPointAndAdjustZeros(size_t first_digit_index, size_t scale,
                                   std::string* output) {
  size_t string_length = output->size();
  // Make a string_view that includes only the digits, so that find_last_not_of
  // does not search the substring before first_digit_index. This is for
  // performance instead of correctness. Note, std::string::find_last_not_of
  // doesn't have a signature that specifies the starting position.
  absl::string_view fixed_uint_str(*output);
  fixed_uint_str.remove_prefix(first_digit_index);
  size_t fixed_uint_length = fixed_uint_str.size();
  size_t zeros_to_truncate = std::min(
      fixed_uint_length - fixed_uint_str.find_last_not_of('0') - 1, scale);
  output->resize(string_length - zeros_to_truncate);
  if (fixed_uint_length < scale + 1) {
    // Add zeros and decimal point if smaller than 1.
    output->insert(first_digit_index, scale + 2 - fixed_uint_length, '0');
    (*output)[first_digit_index + 1] = '.';
  } else if (zeros_to_truncate < scale) {
    output->insert(string_length - scale, 1, '.');
  }
}

// PowersAsc<Word, first_value, base, size>() returns a std::array<Word, size>
// {first_value, first_value * base, ..., first_value * pow(base, size - 1)}.
template <typename Word, Word first_value, Word base, int size, typename... T>
constexpr std::enable_if_t<(sizeof...(T) == size), std::array<Word, size>>
PowersAsc(T... v) {
  return std::array<Word, size>{v...};
}

template <typename Word, Word first_value, Word base, int size, typename... T>
constexpr std::enable_if_t<(sizeof...(T) < size), std::array<Word, size>>
PowersAsc(T... v) {
  return PowersAsc<Word, first_value, base, size>(first_value, v * base...);
}

// PowersAsc<Word, last_value, base, size>() returns a std::array<Word, size>
// {last_value * pow(base, size - 1), last_value * pow(base, size - 2), ...,
//  last_value}.
template <typename Word, Word last_value, Word base, int size, typename... T>
constexpr std::enable_if_t<(sizeof...(T) == size), std::array<Word, size>>
PowersDesc(T... v) {
  return std::array<Word, size>{v...};
}

template <typename Word, Word last_value, Word base, int size, typename... T>
constexpr std::enable_if_t<(sizeof...(T) < size), std::array<Word, size>>
PowersDesc(T... v) {
  return PowersDesc<Word, last_value, base, size>(v * base..., last_value);
}

struct ENotationParts {
  bool negative = false;
  absl::string_view int_part;
  absl::string_view fract_part;
  absl::string_view exp_part;
};

#define RETURN_FALSE_IF(x) \
  if (ABSL_PREDICT_FALSE(x)) return false

bool SplitENotationParts(absl::string_view str, ENotationParts* parts) {
  const char* start = str.data();
  const char* end = str.data() + str.size();

  // Skip whitespace.
  for (; start < end && (absl::ascii_isspace(*start)); ++start) {
  }
  for (; start < end && absl::ascii_isspace(*(end - 1)); --end) {
  }

  // Empty or only spaces
  RETURN_FALSE_IF(start == end);
  *parts = ENotationParts();

  parts->negative = (*start == '-');
  start += (*start == '-' || *start == '+');
  for (const char* c = end; --c >= start;) {
    if (*c == 'e' || *c == 'E') {
      parts->exp_part = absl::string_view(c + 1, end - c - 1);
      RETURN_FALSE_IF(parts->exp_part.empty());
      end = c;
      break;
    }
  }
  for (const char* c = start; c < end; ++c) {
    if (*c == '.') {
      parts->fract_part = absl::string_view(c + 1, end - c - 1);
      end = c;
      break;
    }
  }
  parts->int_part = absl::string_view(start, end - start);
  return true;
}

// Parses <exp_part> and add <extra_scale> to the result.
// If <exp_part> represents an integer that is below int64min, the result is
// int64min.
bool ParseExponent(absl::string_view exp_part, uint extra_scale, int64_t* exp) {
  *exp = extra_scale;
  if (!exp_part.empty()) {
    FixedInt<64, 1> exp_fixed_int;
    if (ABSL_PREDICT_TRUE(exp_fixed_int.ParseFromStringStrict(exp_part))) {
      RETURN_FALSE_IF(exp_fixed_int.AddOverflow(*exp));
      *exp = exp_fixed_int.number()[0];
    } else if (exp_part.size() > 1 && exp_part[0] == '-') {
      // Still need to check whether exp_part contains only digits after '-'.
      exp_part.remove_prefix(1);
      for (char c : exp_part) {
        RETURN_FALSE_IF(!std::isdigit(c));
      }
      *exp = std::numeric_limits<int64_t>::min();
    } else {
      return false;
    }
  }
  return true;
}

// Parses <int_part>.<fract_part>E<exp> to FixedUint.
// If <strict> is true, treats the input as invalid if it does not represent
// a whole number. If <strict> is false, rounds the input away from zero to a
// whole number. Returns true iff the input is valid.
template <int n>
bool ParseNumber(absl::string_view int_part, absl::string_view fract_part,
                 int64_t exp, bool strict, FixedUint<64, n>* output) {
  *output = FixedUint<64, n>();
  bool round_up = false;
  if (exp >= 0) {
    // Promote up to exp fractional digits to the integer part.
    size_t num_promoted_fract_digits = fract_part.size();
    if (exp < fract_part.size()) {
      round_up = fract_part[exp] >= '5';
      num_promoted_fract_digits = exp;
    }
    absl::string_view promoted_fract_part(fract_part.data(),
                                          num_promoted_fract_digits);
    fract_part.remove_prefix(num_promoted_fract_digits);
    if (int_part.empty()) {
      RETURN_FALSE_IF(!output->ParseFromStringStrict(promoted_fract_part));
    } else {
      RETURN_FALSE_IF(
          !output->ParseFromStringSegments(int_part, {promoted_fract_part}));
      int_part = absl::string_view();
    }

    // If exp is greater than the number of promoted fractional digits,
    // scale the result up by pow(10, exp - num_promoted_fract_digits).
    size_t extra_exp = static_cast<size_t>(exp) - num_promoted_fract_digits;
    for (; extra_exp >= 19; extra_exp -= 19) {
      RETURN_FALSE_IF(output->MultiplyOverflow(internal::k1e19));
    }
    if (extra_exp != 0) {
      static constexpr std::array<uint64_t, 19> kPowers =
          PowersAsc<uint64_t, 1, 10, 19>();
      RETURN_FALSE_IF(output->MultiplyOverflow(kPowers[extra_exp]));
    }
  } else {  // exp < 0
    RETURN_FALSE_IF(int_part.size() + fract_part.size() == 0);
    // Demote up to -exp digits from int_part
    if (exp >= static_cast<int64_t>(-int_part.size())) {
      size_t int_digits = int_part.size() + exp;
      round_up = int_part[int_digits] >= '5';
      RETURN_FALSE_IF(int_digits != 0 &&
                      !output->ParseFromStringStrict(
                          absl::string_view(int_part.data(), int_digits)));
      int_part.remove_prefix(int_digits);
    }
  }
  // The remaining characters in int_part and fract_part have not been visited.
  // They represent the fractional digits to be discarded. In strict mode, they
  // must be zeros; otherwise they must be digits.
  if (strict) {
    for (char c : int_part) {
      RETURN_FALSE_IF(c != '0');
    }
    for (char c : fract_part) {
      RETURN_FALSE_IF(c != '0');
    }
  } else {
    for (char c : int_part) {
      RETURN_FALSE_IF(!std::isdigit(c));
    }
    for (char c : fract_part) {
      RETURN_FALSE_IF(!std::isdigit(c));
    }
  }
  RETURN_FALSE_IF(round_up && output->AddOverflow(uint64_t{1}));
  return true;
}
#undef RETURN_FALSE_IF

// Computes static_cast<double>(value / kScalingFactor) with minimal precision
// loss.
double RemoveScaleAndConvertToDouble(__int128 value) {
  if (value == 0) {
    return 0;
  }
  using uint128 = unsigned __int128;
  uint128 abs_value = int128_abs(value);
  // binary_scaling_factor must be a power of 2, so that the division by it
  // never loses any precision.
  double binary_scaling_factor = 1;
  // Make sure abs_value has at least 96 significant bits, so that after
  // dividing by kScalingFactor, it has at least 64 significant bits
  // before conversion to double.
  if (abs_value < (uint128{1} << 96)) {
    if (abs_value >= (uint128{1} << 64)) {
      abs_value <<= 32;
      binary_scaling_factor = static_cast<double>(uint128{1} << 32);
    } else if (abs_value >= (uint128{1} << 32)) {
      abs_value <<= 64;
      binary_scaling_factor = static_cast<double>(uint128{1} << 64);
    } else {
      abs_value <<= 96;
      binary_scaling_factor = static_cast<double>(uint128{1} << 96);
    }
  }
  // FixedUint<64, 2> / std::integral_constant<uint32_t, *> is much faster than
  // uint128 / uint32_t.
  FixedUint<64, 2> tmp(abs_value);
  uint32_t remainder;
  tmp.DivMod(NumericValue::kScalingFactor, &tmp, &remainder);
  std::array<uint64_t, 2> n = tmp.number();
  // If the remainder is not 0, set the least significant bit to 1 so that the
  // round-to-even in static_cast<double>() will not treat the value as a tie
  // between 2 nearest double values.
  n[0] |= (remainder != 0);
  double result =
      static_cast<double>(FixedUint<64, 2>(n)) / binary_scaling_factor;
  return value >= 0 ? result : -result;
}

}  // namespace

zetasql_base::StatusOr<NumericValue> NumericValue::FromStringStrict(
    absl::string_view str) {
  return FromStringInternal(str, /*is_strict=*/true);
}

zetasql_base::StatusOr<NumericValue> NumericValue::FromString(absl::string_view str) {
  return FromStringInternal(str, /*is_strict=*/false);
}

template <int kNumBitsPerWord, int kNumWords>
zetasql_base::StatusOr<NumericValue> NumericValue::FromFixedUint(
    const FixedUint<kNumBitsPerWord, kNumWords>& val, bool negate) {
  if (ABSL_PREDICT_TRUE(val.NonZeroLength() <= 128 / kNumBitsPerWord)) {
    unsigned __int128 v = static_cast<unsigned __int128>(val);
    if (ABSL_PREDICT_TRUE(v <= internal::kNumericMax)) {
      return NumericValue(static_cast<__int128>(negate ? -v : v));
    }
  }
  return MakeEvalError() << "numeric overflow";
}

size_t NumericValue::HashCode() const {
  return absl::Hash<NumericValue>()(*this);
}

void NumericValue::AppendToString(std::string* output) const {
  if (as_packed_int() == 0) {
    output->push_back('0');
    return;
  }
  size_t old_size = output->size();
  FixedInt<64, 2> value(as_packed_int());
  value.AppendToString(output);
  size_t first_digit_index = old_size + value.is_negative();
  AddDecimalPointAndAdjustZeros(first_digit_index, kMaxFractionalDigits,
                                output);
}

// Parses a textual representation of a NUMERIC value. Returns an error if the
// given string cannot be parsed as a number or if the textual numeric value
// exceeds NUMERIC precision. If 'is_strict' is true then the function will
// return an error if there are more that 9 digits in the fractional part,
// otherwise the number will be rounded to contain no more than 9 fractional
// digits.
zetasql_base::StatusOr<NumericValue> NumericValue::FromStringInternal(
    absl::string_view str, bool is_strict) {
  ENotationParts parts;
  int64_t exp;
  FixedUint<64, 2> abs;
  if (ABSL_PREDICT_TRUE(SplitENotationParts(str, &parts)) &&
      ABSL_PREDICT_TRUE(
          ParseExponent(parts.exp_part, kMaxFractionalDigits, &exp)) &&
      ABSL_PREDICT_TRUE(ParseNumber(parts.int_part, parts.fract_part, exp,
                                    is_strict, &abs))) {
    auto number_or_status = FromFixedUint(abs, parts.negative);
    if (number_or_status.ok()) {
      return number_or_status;
    }
  }
  return MakeInvalidNumericError(str);
}

double NumericValue::ToDouble() const {
  return RemoveScaleAndConvertToDouble(as_packed_int());
}

inline unsigned __int128 Scalemantissa(uint64_t mantissa, uint32_t scale) {
  return static_cast<unsigned __int128>(mantissa) * scale;
}

inline FixedUint<64, 4> Scalemantissa(uint64_t mantissa,
                                      unsigned __int128 scale) {
  return ExtendAndMultiply(FixedUint<64, 2>(mantissa), FixedUint<64, 2>(scale));
}

template <typename S, typename T>
bool ScaleAndRoundAwayFromZero(S scale, double value, T* result) {
  if (value == 0) {
    *result = T();
    return true;
  }
  constexpr int kNumOutputBits = sizeof(T) * 8;
  zetasql_base::MathUtil::DoubleParts parts = zetasql_base::MathUtil::Decompose(value);
  DCHECK_NE(parts.mantissa, 0) << value;
  if (parts.exponent <= -kNumOutputBits) {
    *result = T();
    return true;
  }
  // Because mantissa != 0, parts.exponent >= kNumOutputBits - 1 would mean that
  // (abs_mantissa * scale) << parts.exponent will exceed kNumOutputBits - 1
  // bits. Note, the most significant bit in abs_result cannot be set, or the
  // sign of *result will be wrong. We do not need to exempt the special case
  // of 1 << (kNumOutputBits - 1) which might keep the sign correct, because
  // <scale> is not a power of 2 and thus abs_result is never equal to
  // 1 << (kNumOutputBits - 1).
  if (ABSL_PREDICT_FALSE(parts.exponent >= kNumOutputBits - 1)) {
    return false;
  }
  bool negative = parts.mantissa < 0;
  uint64_t abs_mantissa =
      negative ? -static_cast<uint64_t>(parts.mantissa) : parts.mantissa;
  auto abs_result = Scalemantissa(abs_mantissa, scale);
  static_assert(sizeof(abs_result) == sizeof(T));
  if (parts.exponent < 0) {
    abs_result >>= -1 - parts.exponent;
    abs_result += uint64_t{1};  // round away from zero
    abs_result >>= 1;
  } else if (parts.exponent > 0) {
    int msb_idx =
        FixedUint<64, kNumOutputBits / 64>(abs_result).FindMSBSetNonZero();
    if (ABSL_PREDICT_FALSE(msb_idx >= kNumOutputBits - 1 - parts.exponent)) {
      return false;
    }
    abs_result <<= parts.exponent;
  }
  static_assert(sizeof(T) > sizeof(S) + sizeof(uint64_t));
  // Because sizeof(T) is bigger than sizeof(S) + sizeof(uint64_t), the sign bit
  // of abs_result cannot be 1 when parts.exponent = 0. Same for the cases
  // where parts.exponent != 0. Therefore, we do not need to check overflow in
  // negation.
  T rv(abs_result);
  DCHECK(rv >= T()) << value;
  *result = negative ? -rv : rv;
  return true;
}

zetasql_base::StatusOr<NumericValue> NumericValue::FromDouble(double value) {
  if (ABSL_PREDICT_FALSE(!std::isfinite(value))) {
    // This error message should be kept consistent with the error message found
    // in .../public/functions/convert.h.
    if (std::isnan(value)) {
      // Don't show the negative sign for -nan values.
      value = std::numeric_limits<double>::quiet_NaN();
    }
    return MakeEvalError() << "Illegal conversion of non-finite floating point "
                              "number to numeric: "
                           << value;
  }
  __int128 result = 0;
  if (ScaleAndRoundAwayFromZero(kScalingFactor, value, &result)) {
    zetasql_base::StatusOr<NumericValue> value_status = FromPackedInt(result);
    if (ABSL_PREDICT_TRUE(value_status.ok())) {
      return value_status;
    }
  }
  return MakeEvalError() << "numeric out of range: " << value;
}

zetasql_base::StatusOr<NumericValue> NumericValue::Multiply(NumericValue rh) const {
  const __int128 value = as_packed_int();
  const __int128 rh_value = rh.as_packed_int();
  bool negative = value < 0;
  bool rh_negative = rh_value < 0;
  FixedUint<64, 4> product =
      ExtendAndMultiply(FixedUint<64, 2>(int128_abs(value)),
                        FixedUint<64, 2>(int128_abs(rh_value)));

  // This value represents kNumericMax * kScalingFactor + kScalingFactor / 2.
  // At this value, <res> would be internal::kNumericMax + 1 and overflow.
  static constexpr FixedUint<64, 4> kOverflowThreshold(std::array<uint64_t, 4>{
      6450984253243169536ULL, 13015503840481697412ULL, 293873587ULL, 0ULL});
  if (ABSL_PREDICT_TRUE(product < kOverflowThreshold)) {
    // Now we need to adjust the scale of the result. With a 32-bit constant
    // divisor, the compiler is expected to emit no div instructions for the
    // code below. We care about div instructions because they are much more
    // expensive than multiplication (for example on Skylake throughput of a
    // 64-bit multiplication is 1 cycle, compared to ~80-95 cycles for a
    // division).
    product += kScalingFactor / 2;
    FixedUint<32, 5> res(product);
    res /= kScalingFactor;
    unsigned __int128 v = static_cast<unsigned __int128>(res);
    // We already checked the value range, so no need to call FromPackedInt.
    return NumericValue(
        static_cast<__int128>(negative == rh_negative ? v : -v));
  }
  return MakeEvalError() << "numeric overflow: " << ToString() << " * "
                         << rh.ToString();
}

NumericValue NumericValue::Abs(NumericValue value) {
  // The result is expected to be within the valid range.
  return NumericValue(static_cast<__int128>(int128_abs(value.as_packed_int())));
}

NumericValue NumericValue::Sign(NumericValue value) {
  return NumericValue(
      static_cast<int64_t>(int128_sign(value.as_packed_int())));
}

zetasql_base::StatusOr<NumericValue> NumericValue::Power(NumericValue exp) const {
  auto res_or_status = PowerInternal(exp);
  if (res_or_status.ok()) {
    return res_or_status;
  }
  return zetasql_base::StatusBuilder(res_or_status.status()).SetAppend()
         << ": POW(" << ToString() << ", " << exp.ToString() << ")";
}

namespace {
constexpr uint64_t kScalingFactorSquare =
    static_cast<uint64_t>(NumericValue::kScalingFactor) *
    static_cast<uint64_t>(NumericValue::kScalingFactor);
constexpr unsigned __int128 kScalingFactorCube =
    static_cast<unsigned __int128>(kScalingFactorSquare) *
    NumericValue::kScalingFactor;

// Divides input by kScalingFactor ^ 2 with rounding and store the result to
// output. Returns false if the result cannot fit into FixedUint<64, 3>.
template <int size>
inline bool RemoveDoubleScale(FixedUint<64, size>* input,
                              FixedUint<64, size - 1>* output) {
  if (ABSL_PREDICT_TRUE(!input->AddOverflow(kScalingFactorSquare / 2)) &&
      ABSL_PREDICT_TRUE(input->number()[size - 1] < kScalingFactorSquare)) {
    *input /= NumericValue::kScalingFactor;
    *input /= NumericValue::kScalingFactor;
    *output = FixedUint<64, size - 1>(*input);
    return true;
  }
  return false;
}

// Raises value to exp. *double_scaled_value (input and output) is scaled
// by kScalingFactorSquare. Extra scaling is used for preserving
// precision during computations.
// Returns false if the result is too big (not necessarily an error).
bool DoubleScaledPower(FixedUint<64, 3>* double_scaled_value,
                       FixedUint<64, 2> unscaled_exp) {
  FixedUint<64, 3> double_scaled_result(kScalingFactorSquare);
  FixedUint<64, 3> double_scaled_power(*double_scaled_value);
  unsigned __int128 exp = static_cast<unsigned __int128>(unscaled_exp);
  while (true) {
    if ((exp & 1) != 0) {
      FixedUint<64, 6> tmp_scaled_4x =
          ExtendAndMultiply(double_scaled_result, double_scaled_power);
      if (ABSL_PREDICT_FALSE(tmp_scaled_4x.number()[4] != 0) ||
          ABSL_PREDICT_FALSE(tmp_scaled_4x.number()[5] != 0)) {
        return false;
      }
      FixedUint<64, 4> truncated_tmp_scaled_4x(tmp_scaled_4x);
      if (ABSL_PREDICT_FALSE(!RemoveDoubleScale(&truncated_tmp_scaled_4x,
                                                &double_scaled_result))) {
        return false;
      }
    }
    if (exp <= 1) {
      *double_scaled_value = double_scaled_result;
      return true;
    }
    if (ABSL_PREDICT_FALSE((double_scaled_power.number()[2] != 0))) {
      return false;
    }
    FixedUint<64, 2> truncated_power(double_scaled_power);
    FixedUint<64, 4> tmp_scaled_4x =
        ExtendAndMultiply(truncated_power, truncated_power);
    if (ABSL_PREDICT_FALSE(
            !RemoveDoubleScale(&tmp_scaled_4x, &double_scaled_power))) {
      return false;
    }
    exp >>= 1;
  }
}

// *dest *= pow(abs_value / kScalingFactor, fract_exp / kScalingFactor) *
// kScalingFactor
zetasql_base::Status MultiplyByFractionalPower(unsigned __int128 abs_value,
                                       int64_t fract_exp,
                                       FixedUint<64, 3>* dest) {
  // We handle the fractional part of the exponent by raising the original value
  // to the fractional part of the exponent by converting them to doubles and
  // using the standard library's pow() function.
  // TODO Using std::pow() gives a result with reasonable precision
  // (comparable to MS SQL and MySQL), but we can probably do better here.
  // Explore a more accurate implementation in the future.
  double fract_pow = std::pow(RemoveScaleAndConvertToDouble(abs_value),
                              RemoveScaleAndConvertToDouble(fract_exp));
  ZETASQL_ASSIGN_OR_RETURN(NumericValue fract_term,
                   NumericValue::FromDouble(fract_pow));
  FixedUint<64, 5> ret = ExtendAndMultiply(
      *dest, FixedUint<64, 2>(
                 static_cast<unsigned __int128>(fract_term.as_packed_int())));
  if (ABSL_PREDICT_TRUE(ret.number()[3] == 0) &&
      ABSL_PREDICT_TRUE(ret.number()[4] == 0)) {
    *dest = FixedUint<64, 3>(ret);
    return zetasql_base::OkStatus();
  }
  return MakeEvalError() << "numeric overflow";
}
}  // namespace

zetasql_base::StatusOr<NumericValue> NumericValue::PowerInternal(
    NumericValue exp) const {
  // Any value raised to a zero power is always one.
  if (exp == NumericValue()) {
    return NumericValue(1);
  }

  const bool exp_is_negative = exp.as_packed_int() < 0;
  if (*this == NumericValue()) {
    // An attempt to raise zero to a negative power results in division by zero.
    if (exp_is_negative) {
      return MakeEvalError() << "division by zero";
    }
    // Otherwise zero raised to any power is still zero.
    return NumericValue();
  }
  FixedUint<64, 2> abs_integer_exp;
  uint32_t abs_fract_exp;
  FixedUint<64, 2>(int128_abs(exp.as_packed_int()))
      .DivMod(kScalingFactor, &abs_integer_exp, &abs_fract_exp);
  int64_t fract_exp = abs_fract_exp;
  if (exp.as_packed_int() < 0) {
    fract_exp = -fract_exp;
  }

  bool result_is_negative = false;
  unsigned __int128 abs_value = int128_abs(as_packed_int());
  if (as_packed_int() < 0) {
    if (fract_exp != 0) {
      return MakeEvalError()
             << "Negative NUMERIC value cannot be raised to a fractional power";
    }
    result_is_negative = (abs_integer_exp.number()[0] & 1) != 0;
  }

  FixedUint<64, 3> double_scaled_value;
  if (!exp_is_negative) {
    double_scaled_value = FixedUint<64, 3>(abs_value);
    double_scaled_value *= kScalingFactor;
  } else {
    // If the exponent is negative and abs_value is > 1, then we compute
    // compute 1 / (abs_value ^ (-integer_exp)). Note, computing
    // (1 / abs_value) ^ (-integer_exp) would lose precision in the division
    // because the input of DoubleScaledPower can have only 9 digits after the
    // decimal point.
    if (abs_value > kScalingFactor) {
      double_scaled_value = FixedUint<64, 3>(abs_value);
      double_scaled_value *= kScalingFactor;
      if (!DoubleScaledPower(&double_scaled_value, abs_integer_exp) ||
          double_scaled_value > FixedUint<64, 3>(kScalingFactorCube * 2)) {
        return NumericValue();
      }
      DCHECK(static_cast<unsigned __int128>(double_scaled_value) != 0);
      if (fract_exp == 0) {
        FixedUint<64, 3> numerator(kScalingFactorCube);  // triple-scaled
        numerator.DivAndRoundAwayFromZero(double_scaled_value);
        return NumericValue::FromFixedUint(numerator, result_is_negative);
      }
      FixedUint<64, 3> numerator(kScalingFactorSquare);
      // Because fract_exp < 0, the upper bound of pow(abs_value, fract_exp)
      // is pow(1e-9, -1) = 1e9 with scaled value = kScalingFactor ^ 2, which
      // means MultiplyByFractionalPower should not overflow.
      ZETASQL_RETURN_IF_ERROR(
          MultiplyByFractionalPower(abs_value, fract_exp, &numerator));
      // Now numerator is triple-scaled.
      numerator.DivAndRoundAwayFromZero(double_scaled_value);
      return NumericValue::FromFixedUint(numerator, result_is_negative);
    }
    // If the exponent is negative and abs_value is <= 1, then we compute
    // (1 / abs_value) ^ (-abs_integer_exp).
    double_scaled_value = FixedUint<64, 3>(kScalingFactorCube);
    FixedUint<64, 3> denominator(abs_value);
    double_scaled_value.DivAndRoundAwayFromZero(denominator);
  }

  if (!DoubleScaledPower(&double_scaled_value, abs_integer_exp)) {
    return MakeEvalError() << "numeric overflow";
  }

  if (fract_exp == 0) {
    // Divide double_scaled_value by kScalingFactor to make it single-scaled.
    double_scaled_value.DivAndRoundAwayFromZero(kScalingFactor);
    return NumericValue::FromFixedUint(double_scaled_value, result_is_negative);
  }

  ZETASQL_RETURN_IF_ERROR(
      MultiplyByFractionalPower(abs_value, fract_exp, &double_scaled_value));
  // After MultiplyByFractionalPower, tmp is triple-scaled. Divide it by
  // kScalingFactor ^ 2 to make it single-scaled.
  FixedUint<64, 2> ret;
  if (ABSL_PREDICT_FALSE(!RemoveDoubleScale(&double_scaled_value, &ret))) {
    return MakeEvalError() << "numeric overflow";
  }
  return NumericValue::FromFixedUint(ret, result_is_negative);
}

namespace {

template <int32_t divisor>
inline void RoundConst32(bool round_away_from_zero, __int128* dividend) {
  if (round_away_from_zero) {
    *dividend += *dividend >= 0 ? divisor / 2 : divisor / -2;
  }
  int32_t remainder;
  // This is much faster than "dividend % divisor".
  FixedInt<64, 2>(*dividend).DivMod(std::integral_constant<int32_t, divisor>(),
                                    nullptr, &remainder);
  *dividend -= remainder;
}
}  // namespace

zetasql_base::StatusOr<NumericValue> NumericValue::RoundInternal(
    int64_t digits, bool round_away_from_zero) const {
  if (digits >= kMaxFractionalDigits) {
    // Rounding beyond the max number of supported fractional digits has no
    // effect.
    return *this;
  }

  if (digits < -kMaxIntegerDigits) {
    // Rounding (kMaxIntegerDigits + 1) digits away results in zero.
    // Rounding kMaxIntegerDigits digits away might result in overflow instead
    // of zero.
    return NumericValue();
  }

  __int128 value = as_packed_int();
  switch (digits) {
    // Fast paths for some common values of the second argument.
    case 0:
      RoundConst32<internal::k1e9>(round_away_from_zero, &value);
      break;
    case 1:
      RoundConst32<100000000>(round_away_from_zero, &value);
      break;
    case 2:
      RoundConst32<10000000>(round_away_from_zero, &value);
      break;
    case 3:
      RoundConst32<1000000>(round_away_from_zero, &value);
      break;
    default: {
      constexpr int kMaxDigits = kMaxFractionalDigits + kMaxIntegerDigits;
      static constexpr std::array<__int128, kMaxDigits> kTruncFactors =
          PowersDesc<__int128, 10, 10, kMaxDigits>();
      __int128 trunc_factor = kTruncFactors[digits + kMaxIntegerDigits];
      if (round_away_from_zero) {
        __int128 offset = trunc_factor >> 1;
        // The max result is < 1.5e38 < pow(2, 127); no need to check overflow.
        value += (value < 0 ? -offset : offset);
      }
      value -= value % trunc_factor;
    }
  }
  auto res_status = NumericValue::FromPackedInt(value);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: ROUND(" << ToString() << ", "
                         << digits << ")";
}

zetasql_base::StatusOr<NumericValue> NumericValue::Round(int64_t digits) const {
  return RoundInternal(digits, /*round_away_from_zero*/ true);
}

NumericValue NumericValue::Trunc(int64_t digits) const {
  return RoundInternal(digits, /*round_away_from_zero*/ false).ValueOrDie();
}

zetasql_base::StatusOr<NumericValue> NumericValue::Ceiling() const {
  __int128 value = as_packed_int();
  int64_t fract_part = GetFractionalPart();
  value -= fract_part > 0 ? fract_part - kScalingFactor : fract_part;
  auto res_status = NumericValue::FromPackedInt(value);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: CEIL(" << ToString() << ")";
}

zetasql_base::StatusOr<NumericValue> NumericValue::Floor() const {
  __int128 value = as_packed_int();
  int64_t fract_part = GetFractionalPart();
  value -= fract_part < 0 ? fract_part + kScalingFactor : fract_part;
  auto res_status = NumericValue::FromPackedInt(value);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: FLOOR(" << ToString() << ")";
}

zetasql_base::StatusOr<NumericValue> NumericValue::Divide(NumericValue rh) const {
  const __int128 value = as_packed_int();
  const __int128 rh_value = rh.as_packed_int();
  const bool is_negative = value < 0;
  const bool rh_is_negative = rh_value < 0;

  if (ABSL_PREDICT_TRUE(rh_value != 0)) {
    FixedUint<64, 3> dividend(int128_abs(value));
    unsigned __int128 divisor = int128_abs(rh_value);

    // To preserve the scale of the result we need to multiply the dividend by
    // the scaling factor first.
    dividend *= kScalingFactor;
    // Not using DivAndRoundAwayFromZero because the addition never overflows
    // and shifting unsigned __int128 is more efficient.
    dividend += FixedUint<64, 3>(divisor >> 1);
    dividend /= FixedUint<64, 3>(divisor);

    auto res_or_status =
        NumericValue::FromFixedUint(dividend, is_negative != rh_is_negative);
    if (ABSL_PREDICT_TRUE(res_or_status.ok())) {
      return res_or_status;
    }
    return zetasql_base::StatusBuilder(res_or_status.status()).SetAppend()
           << ": " << ToString() << " / " << rh.ToString();
  }
  return MakeEvalError() << "division by zero: " << ToString() << " / "
                         << rh.ToString();
}

zetasql_base::StatusOr<NumericValue> NumericValue::IntegerDivide(
    NumericValue rh) const {
  __int128 rh_value = rh.as_packed_int();
  if (ABSL_PREDICT_TRUE(rh_value != 0)) {
    __int128 value = as_packed_int() / rh_value;
    if (ABSL_PREDICT_TRUE(value <= internal::kNumericMax / kScalingFactor) &&
        ABSL_PREDICT_TRUE(value >= internal::kNumericMin / kScalingFactor)) {
      return NumericValue(value * kScalingFactor);
    }
    return MakeEvalError() << "numeric overflow: " << ToString() << " / "
                           << rh.ToString();
  }
  return MakeEvalError() << "division by zero: " << ToString() << " / "
                         << rh.ToString();
}

zetasql_base::StatusOr<NumericValue> NumericValue::Mod(NumericValue rh) const {
  __int128 rh_value = rh.as_packed_int();
  if (ABSL_PREDICT_TRUE(rh_value != 0)) {
    return NumericValue(as_packed_int() % rh_value);
  }
  return MakeEvalError() << "division by zero: " << ToString() << " / "
                         << rh.ToString();
}

std::string NumericValue::SerializeAsProtoBytes() const {
  __int128 value = as_packed_int();

  std::string ret;

  if (value == 0) {
    ret.push_back(0);
    return ret;
  }

  const unsigned __int128 abs_value = int128_abs(value);
  const uint64_t abs_value_hi = static_cast<uint64_t>(abs_value >> kBitsPerUint64);
  const uint64_t abs_value_lo = static_cast<uint64_t>(abs_value);

  int non_zero_bit_idx = 0;
  if (abs_value_hi != 0) {
    non_zero_bit_idx =
        zetasql_base::Bits::FindMSBSetNonZero64(abs_value_hi) + kBitsPerUint64;
  } else {
    non_zero_bit_idx = zetasql_base::Bits::FindMSBSetNonZero64(abs_value_lo);
  }

  int non_zero_byte_idx = non_zero_bit_idx / kBitsPerByte;
  const char non_zero_byte = static_cast<char>(
      abs_value >> (non_zero_byte_idx * kBitsPerByte));
  if ((non_zero_byte & 0x80) != 0) {
    ++non_zero_byte_idx;
  }

  ret.resize(non_zero_byte_idx + 1);
#ifdef IS_BIG_ENDIAN
  // Big endian platforms are not supported in production right now, so we do
  // not care about optimizations here.
  for (int i = 0; i < non_zero_byte_idx + 1; ++i, value >>= kBitsPerByte) {
    ret[i] = static_cast<char>(value);
  }
#else
  // TODO explore unrolling this call to memcpy.
  memcpy(&ret[0], reinterpret_cast<const void*>(&value),
         non_zero_byte_idx + 1);
#endif

  return ret;
}

void NumericValue::SerializeAndAppendToProtoBytes(std::string* bytes) const {
  FixedInt<64, 2>(as_packed_int()).SerializeToBytes(bytes);
}

zetasql_base::StatusOr<NumericValue> NumericValue::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  FixedInt<64, 2> value;
  if (ABSL_PREDICT_TRUE(value.DeserializeFromBytes(bytes))) {
    return NumericValue::FromPackedInt(static_cast<__int128>(value));
  }
  return MakeEvalError() << "Invalid numeric encoding";
}

std::ostream& operator<<(std::ostream& out, NumericValue value) {
  return out << value.ToString();
}

zetasql_base::StatusOr<NumericValue> NumericValue::Aggregator::GetSum() const {
  if (sum_upper_ != 0) {
    return MakeEvalError() << "numeric overflow: SUM";
  }

  auto res_status = NumericValue::FromPackedInt(sum_lower_);
  if (!res_status.ok()) {
    return MakeEvalError() << "numeric overflow: SUM";
  }
  return res_status.ValueOrDie();
}

zetasql_base::StatusOr<NumericValue>
NumericValue::Aggregator::GetAverage(uint64_t count) const {
  constexpr __int128 kInt128Min = static_cast<__int128>(
      static_cast<unsigned __int128>(1) << (kBitsPerInt128 - 1));

  if (count == 0) {
    return MakeEvalError() << "division by zero: AVG";
  }

  // The code below constructs an unsigned 196 bits of FixedUint<64, 3> from
  // sum_upper_ and sum_lower_. The following cases need to be considered:
  // 1) If sum_upper_ is zero, the entire value (including the sign) comes
  //    from sum_lower_. We need to get abs(sum_lower_) because the division
  //    works on unsigned values.
  // 2) If sum_upper_ is non-zero, the sign comes from it and sum_lower
  //    may have a different sign. For example, if sum_upper_ is 3 and
  //    sum_lower_ is -123, that means that the total value is
  //    3 * 2^128 - 123. Since we pass an unsigned value to the division
  //    method, the upper part needs to be adjusted by -1 in that case so that
  //    the dividend looks like 2 * 2^128 + some_very_large_128_bit_value
  //    where some_very_large_128_bit_value is -123 casted to an unsigned value.
  // 3) If sum_lower_ is kInt128Min we can't change its sign because there is
  //    no positive number that complements kInt128Min. We leave it as it is
  //    because kInt128Min (0x8000...0000) complements itself when converted to
  //    the unsigned 128 bit value.
  bool negate = false;
  __int128 lower;
  uint64_t upper_abs = std::abs(sum_upper_);

  if (ABSL_PREDICT_TRUE(upper_abs == 0)) {
    negate = sum_lower_ < 0;
    lower = (sum_lower_ != kInt128Min) ? int128_abs(sum_lower_) : sum_lower_;
  } else {
    negate = sum_upper_ < 0;
    lower = (negate && sum_lower_ != kInt128Min) ? -sum_lower_ : sum_lower_;
    if (lower < 0)
      upper_abs--;
  }

  // The reason we need 224 bits of precision is because the constructor
  // (uint64_t hi, unsigned __int128 low) needs 192 bits; on top of that we need
  // 32 bits to be able to normalize the numbers before performing long division
  // (see LongDivision()).
  FixedUint<64, 3> dividend(upper_abs, static_cast<unsigned __int128>(lower));
  dividend += FixedUint<64, 3>(count >> 1);
  dividend /= FixedUint<64, 3>(count);

  auto res_status = NumericValue::FromFixedUint(dividend, negate);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: AVG";
}

void NumericValue::Aggregator::MergeWith(const Aggregator& other) {
  if (ABSL_PREDICT_FALSE(internal::int128_add_overflow(
      sum_lower_, other.sum_lower_, &sum_lower_))) {
    sum_upper_ += other.sum_lower_ < 0 ? -1 : 1;
  }

  sum_upper_ += other.sum_upper_;
}

std::string NumericValue::Aggregator::SerializeAsProtoBytes() const {
  std::string res;
  res.resize(kBytesPerInt128 + kBytesPerInt64);
  uint64_t sum_lower_lo = static_cast<uint64_t>(sum_lower_ & ~uint64_t{0});
  uint64_t sum_lower_hi = static_cast<uint64_t>(
      static_cast<unsigned __int128>(sum_lower_) >> kBitsPerUint64);
  zetasql_base::LittleEndian::Store64(&res[0], sum_lower_lo);
  zetasql_base::LittleEndian::Store64(&res[kBytesPerInt64], sum_lower_hi);
  zetasql_base::LittleEndian::Store64(&res[kBytesPerInt128], sum_upper_);
  return res;
}

zetasql_base::StatusOr<NumericValue::Aggregator>
NumericValue::Aggregator::DeserializeFromProtoBytes(absl::string_view bytes) {
  if (bytes.size() != kBytesPerInt128 + kBytesPerInt64) {
    return MakeEvalError() << "Invalid NumericValue::Aggregator encoding";
  }

  Aggregator res;
  uint64_t sum_lower_lo = zetasql_base::LittleEndian::Load64(bytes.data());
  uint64_t sum_lower_hi =
      zetasql_base::LittleEndian::Load64(bytes.substr(kBytesPerInt64).data());
  res.sum_lower_ = static_cast<__int128>(
      (static_cast<unsigned __int128>(sum_lower_hi) << 64) + sum_lower_lo);
  res.sum_upper_ = zetasql_base::LittleEndian::Load64(bytes.substr(kBytesPerInt128).data());
  return res;
}

zetasql_base::StatusOr<NumericValue> NumericValue::SumAggregator::GetSum() const {
  auto res_status = NumericValue::FromFixedInt(sum_);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: SUM";
}

zetasql_base::StatusOr<NumericValue> NumericValue::SumAggregator::GetAverage(
    uint64_t count) const {
  if (count == 0) {
    return MakeEvalError() << "division by zero: AVG";
  }

  FixedInt<64, 3> dividend = sum_;
  dividend.DivAndRoundAwayFromZero(count);

  auto res_status = NumericValue::FromFixedInt(dividend);
  if (res_status.ok()) {
    return res_status;
  }
  return MakeEvalError() << "numeric overflow: AVG";
}

void NumericValue::SumAggregator::MergeWith(const SumAggregator& other) {
  sum_ += other.sum_;
}

std::string NumericValue::SumAggregator::SerializeAsProtoBytes() const {
  std::string str;
  sum_.SerializeToBytes(&str);
  return str;
}

zetasql_base::StatusOr<NumericValue::SumAggregator>
NumericValue::SumAggregator::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  NumericValue::SumAggregator out;
  if (out.sum_.DeserializeFromBytes(bytes)) {
    return out;
  }
  return MakeEvalError() << "Invalid NumericValue::SumAggregator encoding";
}

void NumericValue::VarianceAggregator::Add(NumericValue value) {
  sum_ += FixedInt<64, 3>(value.as_packed_int());
  FixedInt<64, 2> v(value.as_packed_int());
  sum_square_ += FixedInt<64, 5>(ExtendAndMultiply(v, v));
}

void NumericValue::VarianceAggregator::Subtract(NumericValue value) {
  sum_ -= FixedInt<64, 3>(value.as_packed_int());
  FixedInt<64, 2> v(value.as_packed_int());
  sum_square_ -= FixedInt<64, 5>(ExtendAndMultiply(v, v));
}

absl::optional<double> NumericValue::VarianceAggregator::GetPopulationVariance(
    uint64_t count) const {
  if (count > 0) {
    return GetCovariance(sum_, sum_, sum_square_, count, 0);
  }
  return absl::nullopt;
}

absl::optional<double> NumericValue::VarianceAggregator::GetSamplingVariance(
    uint64_t count) const {
  if (count > 1) {
    return GetCovariance(sum_, sum_, sum_square_, count, 1);
  }
  return absl::nullopt;
}

absl::optional<double> NumericValue::VarianceAggregator::GetPopulationStdDev(
    uint64_t count) const {
  if (count > 0) {
    return std::sqrt(
        GetCovariance(sum_, sum_, sum_square_, count, 0));
  }
  return absl::nullopt;
}

absl::optional<double> NumericValue::VarianceAggregator::GetSamplingStdDev(
    uint64_t count) const {
  if (count > 1) {
    return std::sqrt(
        GetCovariance(sum_, sum_, sum_square_, count, 1));
  }
  return absl::nullopt;
}

void NumericValue::VarianceAggregator::MergeWith(
    const VarianceAggregator& other) {
  sum_ += other.sum_;
  sum_square_ += other.sum_square_;
}

std::string NumericValue::VarianceAggregator::SerializeAsProtoBytes() const {
  std::string out;
  SerializeFixedInt(&out, sum_, sum_square_);
  return out;
}

zetasql_base::StatusOr<NumericValue::VarianceAggregator>
NumericValue::VarianceAggregator::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  VarianceAggregator out;
  if (DeserializeFixedInt(bytes, &out.sum_, &out.sum_square_)) {
    return out;
  }
  return MakeEvalError() << "Invalid NumericValue::VarianceAggregator encoding";
}

void NumericValue::CovarianceAggregator::Add(NumericValue x,
                                             NumericValue y) {
  sum_x_ += FixedInt<64, 3>(x.as_packed_int());
  sum_y_ += FixedInt<64, 3>(y.as_packed_int());
  FixedInt<64, 2> x_num(x.as_packed_int());
  FixedInt<64, 2> y_num(y.as_packed_int());
  sum_product_ += FixedInt<64, 5>(ExtendAndMultiply(x_num, y_num));
}

void NumericValue::CovarianceAggregator::Subtract(NumericValue x,
                                                  NumericValue y) {
  sum_x_ -= FixedInt<64, 3>(x.as_packed_int());
  sum_y_ -= FixedInt<64, 3>(y.as_packed_int());
  FixedInt<64, 2> x_num(x.as_packed_int());
  FixedInt<64, 2> y_num(y.as_packed_int());
  sum_product_ -= FixedInt<64, 5>(ExtendAndMultiply(x_num, y_num));
}

absl::optional<double>
NumericValue::CovarianceAggregator::GetPopulationCovariance(
    uint64_t count) const {
  if (count > 0) {
    return GetCovariance(sum_x_, sum_y_, sum_product_, count, 0);
  }
  return absl::nullopt;
}

absl::optional<double>
NumericValue::CovarianceAggregator::GetSamplingCovariance(uint64_t count) const {
  if (count > 1) {
    return GetCovariance(sum_x_, sum_y_, sum_product_, count, 1);
  }
  return absl::nullopt;
}

void NumericValue::CovarianceAggregator::MergeWith(
    const CovarianceAggregator& other) {
  sum_x_ += other.sum_x_;
  sum_y_ += other.sum_y_;
  sum_product_ += other.sum_product_;
}

std::string NumericValue::CovarianceAggregator::SerializeAsProtoBytes() const {
  std::string out;
  SerializeFixedInt(&out, sum_product_, sum_x_, sum_y_);
  return out;
}

zetasql_base::StatusOr<NumericValue::CovarianceAggregator>
NumericValue::CovarianceAggregator::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  CovarianceAggregator out;
  if (DeserializeFixedInt(bytes, &out.sum_product_, &out.sum_x_, &out.sum_y_)) {
    return out;
  }
  return MakeEvalError()
         << "Invalid NumericValue::CovarianceAggregator encoding";
}

void NumericValue::CorrelationAggregator::Add(NumericValue x,
                                             NumericValue y) {
  cov_agg_.Add(x, y);
  FixedInt<64, 2> x_num(x.as_packed_int());
  FixedInt<64, 2> y_num(y.as_packed_int());
  sum_square_x_ += FixedInt<64, 5>(ExtendAndMultiply(x_num, x_num));
  sum_square_y_ += FixedInt<64, 5>(ExtendAndMultiply(y_num, y_num));
}

void NumericValue::CorrelationAggregator::Subtract(NumericValue x,
                                                  NumericValue y) {
  cov_agg_.Subtract(x, y);
  FixedInt<64, 2> x_num(x.as_packed_int());
  FixedInt<64, 2> y_num(y.as_packed_int());
  sum_square_x_ -= FixedInt<64, 5>(ExtendAndMultiply(x_num, x_num));
  sum_square_y_ -= FixedInt<64, 5>(ExtendAndMultiply(y_num, y_num));
}

absl::optional<double>
NumericValue::CorrelationAggregator::GetCorrelation(uint64_t count) const {
  if (count > 1) {
    FixedInt<64, 6> numerator = GetScaledCovarianceNumerator(
        cov_agg_.sum_x_, cov_agg_.sum_y_, cov_agg_.sum_product_, count);
    FixedInt<64, 6> variance_numerator_x = GetScaledCovarianceNumerator(
        cov_agg_.sum_x_, cov_agg_.sum_x_, sum_square_x_, count);
    FixedInt<64, 6> variance_numerator_y = GetScaledCovarianceNumerator(
        cov_agg_.sum_y_, cov_agg_.sum_y_, sum_square_y_, count);
    FixedInt<64, 12> denominator_square =
        ExtendAndMultiply(variance_numerator_x, variance_numerator_y);
    return static_cast<double>(numerator) /
           std::sqrt(static_cast<double>(denominator_square));
  }
  return absl::nullopt;
}

void NumericValue::CorrelationAggregator::MergeWith(
    const CorrelationAggregator& other) {
  cov_agg_.MergeWith(other.cov_agg_);
  sum_square_x_ += other.sum_square_x_;
  sum_square_y_ += other.sum_square_y_;
}

std::string NumericValue::CorrelationAggregator::SerializeAsProtoBytes() const {
  std::string out;
  SerializeFixedInt(&out, cov_agg_.sum_product_, cov_agg_.sum_x_,
                    cov_agg_.sum_y_, sum_square_x_, sum_square_y_);
  return out;
}

zetasql_base::StatusOr<NumericValue::CorrelationAggregator>
NumericValue::CorrelationAggregator::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  CorrelationAggregator out;
  if (DeserializeFixedInt(bytes, &out.cov_agg_.sum_product_,
                          &out.cov_agg_.sum_x_, &out.cov_agg_.sum_y_,
                          &out.sum_square_x_, &out.sum_square_y_)) {
    return out;
  }
  return MakeEvalError()
         << "Invalid NumericValue::CorrelationAggregator encoding";
}

inline zetasql_base::Status MakeInvalidBigNumericError(absl::string_view str) {
  return MakeEvalError() << "Invalid BIGNUMERIC value: " << str;
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::Multiply(
    const BigNumericValue& rh) const {
  bool lh_negative = value_.is_negative();
  bool rh_negative = rh.value_.is_negative();
  FixedUint<64, 8> abs_result_64x8 =
      ExtendAndMultiply(value_.abs(), rh.value_.abs());
  if (ABSL_PREDICT_TRUE(abs_result_64x8.number()[6] == 0) &&
      ABSL_PREDICT_TRUE(abs_result_64x8.number()[7] == 0)) {
    FixedUint<64, 5> abs_result_64x5 =
        RemoveScalingFactor(FixedUint<64, 6>(abs_result_64x8));
    if (ABSL_PREDICT_TRUE(abs_result_64x5.number()[4] == 0)) {
      FixedInt<64, 4> result;
      FixedUint<64, 4> abs_result_64x4(abs_result_64x5);
      if (ABSL_PREDICT_TRUE(result.SetSignAndAbs(lh_negative != rh_negative,
                                                 abs_result_64x4))) {
        return BigNumericValue(result);
      }
    }
  }
  return MakeEvalError() << "BigNumeric overflow: " << ToString() << " * "
                         << rh.ToString();
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::Divide(
    const BigNumericValue& rh) const {
  bool lh_negative = value_.is_negative();
  bool rh_negative = rh.value_.is_negative();
  if (ABSL_PREDICT_TRUE(!rh.value_.is_zero())) {
    FixedUint<64, 4> abs_value = value_.abs();
    FixedUint<64, 6> rh_abs_value(rh.value_.abs());
    FixedUint<64, 6> scaled_abs_value =
        ExtendAndMultiply(abs_value, FixedUint<64, 2>(ScalingFactor()));
    scaled_abs_value.DivAndRoundAwayFromZero(rh_abs_value);
    if (ABSL_PREDICT_TRUE(scaled_abs_value.number()[4] == 0 &&
                          scaled_abs_value.number()[5] == 0)) {
      FixedUint<64, 4> abs_result(scaled_abs_value);
      FixedInt<64, 4> result;
      if (ABSL_PREDICT_TRUE(
              result.SetSignAndAbs(lh_negative != rh_negative, abs_result))) {
        return BigNumericValue(result);
      }
    }
    return MakeEvalError() << "BigNumeric overflow: " << ToString() << " / "
                           << rh.ToString();
  }
  return MakeEvalError() << "division by zero: " << ToString() << " / "
                         << rh.ToString();
}

double BigNumericValue::RemoveScaleAndConvertToDouble(
    const FixedInt<64, 4>& value) {
  bool is_negative = value.is_negative();
  FixedUint<64, 4> abs_value = value.abs();
  int num_32bit_words = FixedUint<32, 8>(abs_value).NonZeroLength();
  static constexpr std::array<uint32_t, 14> kPowersOf5 =
      PowersAsc<uint32_t, 1, 5, 14>();
  double binary_scaling_factor = 1;
  // To ensure precision, the number should have more than 54 bits after scaled
  // down by the all factors as 5 in scaling factor (5^38, 89 bits). Since
  // dividing the double by 2 won't produce precision loss, the value can be
  // divided by 5 factors in the scaling factor for 3 times, and divided by all
  // 2 factors in the scaling factor and binary scaling factor after converted
  // to double.
  switch (num_32bit_words) {
    case 0:
      return 0;
    case 1:
      abs_value <<= 144;
      // std::exp2, std::pow and std::ldexp are not constexpr.
      // Use static_cast from integers to compute the value at compile time.
      binary_scaling_factor = static_cast<double>(__int128{1} << 100) *
                              static_cast<double>(__int128{1} << 82);
      break;
    case 2:
      abs_value <<= 112;
      binary_scaling_factor = static_cast<double>(__int128{1} << 100) *
                              static_cast<double>(__int128{1} << 50);
      break;
    case 3:
      abs_value <<= 80;
      binary_scaling_factor = static_cast<double>(__int128{1} << 118);
      break;
    case 4:
      abs_value <<= 48;
      binary_scaling_factor = static_cast<double>(__int128{1} << 86);
      break;
    case 5:
      abs_value <<= 16;
      binary_scaling_factor = static_cast<double>(__int128{1} << 54);
      break;
    default:
      // shifting bits <= 0
      binary_scaling_factor = static_cast<double>(__int128{1} << 38);
  }
  uint32_t remainder_bits;
  abs_value.DivMod(std::integral_constant<uint32_t, kPowersOf5[13]>(), &abs_value,
                   &remainder_bits);
  uint32_t remainder;
  abs_value.DivMod(std::integral_constant<uint32_t, kPowersOf5[13]>(), &abs_value,
                   &remainder);
  remainder_bits |= remainder;
  abs_value.DivMod(std::integral_constant<uint32_t, kPowersOf5[12]>(), &abs_value,
                   &remainder);
  remainder_bits |= remainder;
  std::array<uint64_t, 4> n = abs_value.number();
  n[0] |= (remainder_bits != 0);
  double result =
      static_cast<double>(FixedUint<64, 4>(n)) / binary_scaling_factor;
  return is_negative ? -result : result;
}

// Parses a textual representation of a BIGNUMERIC value. Returns an error if
// the given string cannot be parsed as a number or if the textual numeric value
// exceeds BIGNUMERIC range. If 'is_strict' is true then the function will
// return an error if there are more that 38 digits in the fractional part,
// otherwise the number will be rounded to contain no more than 38 fractional
// digits.
zetasql_base::StatusOr<BigNumericValue> BigNumericValue::FromStringInternal(
    absl::string_view str, bool is_strict) {
  ENotationParts parts;
  int64_t exp;
  FixedUint<64, 4> abs;
  BigNumericValue result;
  if (ABSL_PREDICT_TRUE(SplitENotationParts(str, &parts)) &&
      ABSL_PREDICT_TRUE(ParseExponent(parts.exp_part,
                                      kBigNumericMaxFractionalDigits, &exp)) &&
      ABSL_PREDICT_TRUE(ParseNumber(parts.int_part, parts.fract_part, exp,
                                    is_strict, &abs)) &&
      ABSL_PREDICT_TRUE(result.value_.SetSignAndAbs(parts.negative, abs))) {
    return result;
  }
  return MakeInvalidBigNumericError(str);
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::FromStringStrict(
    absl::string_view str) {
  return FromStringInternal(str, /*is_strict=*/true);
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::FromString(
    absl::string_view str) {
  return FromStringInternal(str, /*is_strict=*/false);
}

size_t BigNumericValue::HashCode() const {
  return absl::Hash<BigNumericValue>()(*this);
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::FromDouble(double value) {
  if (ABSL_PREDICT_FALSE(!std::isfinite(value))) {
    // This error message should be kept consistent with the error message found
    // in .../public/functions/convert.h.
    if (std::isnan(value)) {
      // Don't show the negative sign for -nan values.
      value = std::numeric_limits<double>::quiet_NaN();
    }
    return MakeEvalError() << "Illegal conversion of non-finite floating point "
                              "number to BigNumeric: "
                           << value;
  }
  FixedInt<64, 4> result;
  if (ScaleAndRoundAwayFromZero(ScalingFactor(), value, &result)) {
    return BigNumericValue(result);
  }
  return MakeEvalError() << "BigNumeric out of range: " << value;
}

void BigNumericValue::AppendToString(std::string* output) const {
  if (value_.is_zero()) {
    output->push_back('0');
    return;
  }
  size_t old_size = output->size();
  value_.AppendToString(output);
  size_t first_digit_index = old_size + value_.is_negative();
  AddDecimalPointAndAdjustZeros(first_digit_index, 38, output);
}

void BigNumericValue::SerializeAndAppendToProtoBytes(std::string* bytes) const {
  value_.SerializeToBytes(bytes);
}

zetasql_base::StatusOr<BigNumericValue> BigNumericValue::DeserializeFromProtoBytes(
    absl::string_view bytes) {
  BigNumericValue out;
  if (out.value_.DeserializeFromBytes(bytes)) {
    return out;
  }
  return MakeEvalError() << "Invalid BigNumericValue encoding";
}


}  // namespace zetasql
