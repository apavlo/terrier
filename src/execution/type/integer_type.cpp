//===----------------------------------------------------------------------===//
//
//                         Peloton
//
// integer_type.cpp
//
// Identification: src/execution/type/integer_type.cpp
//
// Copyright (c) 2015-2018, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "execution/type/integer_type.h"

#include "common/exception.h"
#include "execution/lang/if.h"
#include "execution/proxy/numeric_functions_proxy.h"
#include "execution/proxy/values_runtime_proxy.h"
#include "execution/type/boolean_type.h"
#include "execution/type/decimal_type.h"
#include "execution/value.h"
#include "runtime/string_util.h"
#include "type/limits.h"

namespace terrier::execution {

namespace type {

namespace {

/** Forward declarations:
 *  Subtraction
 */
struct Sub : public TypeSystem::BinaryOperatorHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override;

  Type ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const override;

  Value Impl(CodeGen &codegen, const Value &left, const Value &right,
             const TypeSystem::InvocationContext &ctx) const override;
};

////////////////////////////////////////////////////////////////////////////////
///
/// Casting
///
/// We do BIGINT -> {INTEGRAL_TYPE, DECIMAL, VARCHAR, BOOLEAN}
////////////////////////////////////////////////////////////////////////////////

struct CastInteger : public TypeSystem::CastHandleNull {
  bool SupportsTypes(const Type &from_type, const Type &to_type) const override {
    if (from_type.GetSqlType() != Integer::Instance()) {
      return false;
    }
    switch (to_type.GetSqlType().TypeId()) {
      case type::TypeId::BOOLEAN:
      case type::TypeId::TINYINT:
      case type::TypeId::SMALLINT:
      case type::TypeId::INTEGER:
      case type::TypeId::BIGINT:
      case type::TypeId::DECIMAL:
        return true;
      default:
        return false;
    }
  }

  Value Impl(CodeGen &codegen, const Value &value, const Type &to_type) const override {
    llvm::Value *result = nullptr;
    switch (to_type.GetSqlType().TypeId()) {
      case type::TypeId::BOOLEAN: {
        result = codegen->CreateTrunc(value.GetValue(), codegen.BoolType());
        break;
      }
      case type::TypeId::TINYINT: {
        result = codegen->CreateTrunc(value.GetValue(), codegen.Int8Type());
        break;
      }
      case type::TypeId::SMALLINT: {
        result = codegen->CreateTrunc(value.GetValue(), codegen.Int16Type());
        break;
      }
      case type::TypeId::INTEGER: {
        result = value.GetValue();
        break;
      }
      case type::TypeId::BIGINT: {
        result = codegen->CreateSExt(value.GetValue(), codegen.Int64Type());
        break;
      }
      case type::TypeId::DECIMAL: {
        result = codegen->CreateSIToFP(value.GetValue(), codegen.DoubleType());
        break;
      }
      case type::TypeId::VARCHAR:
      default: {
        throw Exception{StringUtil::Format("Cannot cast %s to %s", TypeIdToString(value.GetType().type_id).c_str(),
                                           TypeIdToString(to_type.type_id).c_str())};
      }
    }

    // We could be casting this non-nullable value to a nullable type
    llvm::Value *null = to_type.nullable ? codegen.ConstBool(false) : nullptr;

    // Return the result
    return Value{to_type, result, nullptr, null};
  }
};

////////////////////////////////////////////////////////////////////////////////
///
/// Comparison
///
////////////////////////////////////////////////////////////////////////////////

// Comparison
struct CompareInteger : public TypeSystem::SimpleComparisonHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override {
    return left_type == Integer::Instance() && left_type == right_type;
  }

  Value CompareLtImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpSLT(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareLteImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpSLE(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareEqImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpEQ(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareNeImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpNE(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareGtImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpSGT(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareGteImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    auto *raw_val = codegen->CreateICmpSGE(left.GetValue(), right.GetValue());
    return Value{Boolean::Instance(), raw_val, nullptr, nullptr};
  }

  Value CompareForSortImpl(CodeGen &codegen, const Value &left, const Value &right) const override {
    // For integer comparisons, just subtract left from right and cast the
    // result to a 32-bit value
    llvm::Value *diff = codegen->CreateSub(left.GetValue(), right.GetValue());
    return Value{Integer::Instance(), diff, nullptr, nullptr};
  }
};

////////////////////////////////////////////////////////////////////////////////
///
/// Unary operations
///
////////////////////////////////////////////////////////////////////////////////

// Abs
struct Abs : public TypeSystem::UnaryOperatorHandleNull {
  bool SupportsType(const Type &type) const override { return type.GetSqlType() == Integer::Instance(); }

  Type ResultType(UNUSED_ATTRIBUTE const Type &val_type) const override { return Type{Integer::Instance()}; }

  Value Impl(CodeGen &codegen, const Value &val, const TypeSystem::InvocationContext &ctx) const override {
    // The integer subtraction implementation
    Sub sub;
    // Zero place-holder
    auto zero = Value{type::Integer::Instance(), codegen.Const32(0)};

    // We want: raw_ret = (val < 0 ? 0 - val : val)
    auto sub_result = sub.Impl(codegen, zero, val, ctx);
    auto *lt_zero = codegen->CreateICmpSLT(val.GetValue(), zero.GetValue());
    auto *raw_ret = codegen->CreateSelect(lt_zero, sub_result.GetValue(), val.GetValue());
    return Value{Integer::Instance(), raw_ret};
  }
};

// Negation
struct Negate : public TypeSystem::UnaryOperatorHandleNull {
  bool SupportsType(const Type &type) const override { return type.GetSqlType() == Integer::Instance(); }

  Type ResultType(UNUSED_ATTRIBUTE const Type &val_type) const override { return Type{Integer::Instance()}; }

  Value Impl(CodeGen &codegen, const Value &val,
             UNUSED_ATTRIBUTE const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsType(val.GetType()));

    llvm::Value *overflow_bit = nullptr;
    llvm::Value *result = codegen.CallSubWithOverflow(codegen.Const32(0), val.GetValue(), overflow_bit);

    codegen.ThrowIfOverflow(overflow_bit);

    // Return result
    return Value{Integer::Instance(), result, nullptr, nullptr};
  }
};

// Floor
struct Floor : public TypeSystem::UnaryOperatorHandleNull {
  CastInteger cast;

  bool SupportsType(const Type &type) const override { return type.GetSqlType() == Integer::Instance(); }

  Type ResultType(UNUSED_ATTRIBUTE const Type &val_type) const override { return Type{Decimal::Instance()}; }

  Value Impl(CodeGen &codegen, const Value &val,
             UNUSED_ATTRIBUTE const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsType(val.GetType()));
    return cast.Impl(codegen, val, Decimal::Instance());
  }
};

// Ceiling
struct Ceil : public TypeSystem::UnaryOperatorHandleNull {
  CastInteger cast;

  bool SupportsType(const Type &type) const override { return type.GetSqlType() == Integer::Instance(); }

  Type ResultType(UNUSED_ATTRIBUTE const Type &val_type) const override { return Type{Decimal::Instance()}; }

  Value Impl(CodeGen &codegen, const Value &val,
             UNUSED_ATTRIBUTE const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsType(val.GetType()));
    return cast.Impl(codegen, val, Decimal::Instance());
  }
};

// Sqrt
struct Sqrt : public TypeSystem::UnaryOperatorHandleNull {
  CastInteger cast;

  bool SupportsType(const Type &type) const override { return type.GetSqlType() == Integer::Instance(); }

  Type ResultType(UNUSED_ATTRIBUTE const Type &val_type) const override { return Decimal::Instance(); }

 protected:
  Value Impl(CodeGen &codegen, const Value &val,
             UNUSED_ATTRIBUTE const TypeSystem::InvocationContext &ctx) const override {
    auto casted = cast.Impl(codegen, val, Decimal::Instance());
    auto *raw_ret = codegen.Sqrt(casted.GetValue());
    return Value{Decimal::Instance(), raw_ret};
  }
};

////////////////////////////////////////////////////////////////////////////////
///
/// Binary operations
///
////////////////////////////////////////////////////////////////////////////////

// Addition
struct Add : public TypeSystem::BinaryOperatorHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override {
    return left_type.GetSqlType() == Integer::Instance() && left_type == right_type;
  }

  Type ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const override {
    return Type{Integer::Instance()};
  }

  Value Impl(CodeGen &codegen, const Value &left, const Value &right,
             const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsTypes(left.GetType(), right.GetType()));

    // Do addition
    llvm::Value *overflow_bit = nullptr;
    llvm::Value *result = codegen.CallAddWithOverflow(left.GetValue(), right.GetValue(), overflow_bit);

    if (ctx.on_error == OnError::Exception) {
      codegen.ThrowIfOverflow(overflow_bit);
    }

    // Return result
    return Value{Integer::Instance(), result, nullptr, nullptr};
  }
};

// Subtraction
bool Sub::SupportsTypes(const Type &left_type, const Type &right_type) const {
  return left_type.GetSqlType() == Integer::Instance() && left_type == right_type;
}

Type Sub::ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const {
  return Type{Integer::Instance()};
}

Value Sub::Impl(CodeGen &codegen, const Value &left, const Value &right,
                const TypeSystem::InvocationContext &ctx) const {
  PELOTON_ASSERT(SupportsTypes(left.GetType(), right.GetType()));

  // Do subtraction
  llvm::Value *overflow_bit = nullptr;
  llvm::Value *result = codegen.CallSubWithOverflow(left.GetValue(), right.GetValue(), overflow_bit);

  if (ctx.on_error == OnError::Exception) {
    codegen.ThrowIfOverflow(overflow_bit);
  }

  // Return result
  return Value{Integer::Instance(), result};
};

// Multiplication
struct Mul : public TypeSystem::BinaryOperatorHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override {
    return left_type.GetSqlType() == Integer::Instance() && left_type == right_type;
  }

  Type ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const override {
    return Type{Integer::Instance()};
  }

  Value Impl(CodeGen &codegen, const Value &left, const Value &right,
             const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsTypes(left.GetType(), right.GetType()));

    // Do multiplication
    llvm::Value *overflow_bit = nullptr;
    llvm::Value *result = codegen.CallMulWithOverflow(left.GetValue(), right.GetValue(), overflow_bit);

    if (ctx.on_error == OnError::Exception) {
      codegen.ThrowIfOverflow(overflow_bit);
    }

    // Return result
    return Value{Integer::Instance(), result};
  }
};

// Division
struct Div : public TypeSystem::BinaryOperatorHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override {
    return left_type.GetSqlType() == Integer::Instance() && left_type == right_type;
  }

  Type ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const override {
    return Type{Integer::Instance()};
  }

  Value Impl(CodeGen &codegen, const Value &left, const Value &right,
             const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsTypes(left.GetType(), right.GetType()));

    // First, check if the divisor is zero
    auto *div0 = codegen->CreateICmpEQ(right.GetValue(), codegen.Const32(0));

    // Check if the caller cares about division-by-zero errors

    auto result = Value{Integer::Instance()};

    if (ctx.on_error == OnError::ReturnNull) {
      Value default_val, division_result;
      lang::If is_div0{codegen, div0, "div0"};
      {
        // The divisor is 0, return NULL because that's what the caller wants
        default_val = Integer::Instance().GetNullValue(codegen);
      }
      is_div0.ElseBlock();
      {
        // The divisor isn't 0, do the division
        auto *raw_val = codegen->CreateSDiv(left.GetValue(), right.GetValue());
        division_result = Value{Integer::Instance(), raw_val, nullptr, nullptr};
      }
      is_div0.EndIf();

      // Build PHI
      result = is_div0.BuildPHI(default_val, division_result);

    } else if (ctx.on_error == OnError::Exception) {
      // If the caller **does** care about the error, generate the exception
      codegen.ThrowIfDivideByZero(div0);

      // Do division
      auto *raw_val = codegen->CreateSDiv(left.GetValue(), right.GetValue());
      result = Value{Integer::Instance(), raw_val, nullptr, nullptr};
    }

    // Return result
    return result;
  }
};

// Modulo
struct Modulo : public TypeSystem::BinaryOperatorHandleNull {
  bool SupportsTypes(const Type &left_type, const Type &right_type) const override {
    return left_type.GetSqlType() == Integer::Instance() && left_type == right_type;
  }

  Type ResultType(UNUSED_ATTRIBUTE const Type &left_type, UNUSED_ATTRIBUTE const Type &right_type) const override {
    return Type{Integer::Instance()};
  }

  Value Impl(CodeGen &codegen, const Value &left, const Value &right,
             const TypeSystem::InvocationContext &ctx) const override {
    PELOTON_ASSERT(SupportsTypes(left.GetType(), right.GetType()));

    // First, check if the divisor is zero
    auto *div0 = codegen->CreateICmpEQ(right.GetValue(), codegen.Const32(0));

    // Check if the caller cares about division-by-zero errors

    auto result = Value{Integer::Instance()};

    if (ctx.on_error == OnError::ReturnNull) {
      Value default_val, division_result;
      lang::If is_div0{codegen, div0, "div0"};
      {
        // The divisor is 0, return NULL because that's what the caller wants
        default_val = Integer::Instance().GetNullValue(codegen);
      }
      is_div0.ElseBlock();
      {
        // The divisor isn't 0, do the division
        auto *raw_val = codegen->CreateSRem(left.GetValue(), right.GetValue());
        division_result = Value{Integer::Instance(), raw_val, nullptr, nullptr};
      }
      is_div0.EndIf();

      // Build PHI
      result = is_div0.BuildPHI(default_val, division_result);

    } else if (ctx.on_error == OnError::Exception) {
      // If the caller **does** care about the error, generate the exception
      codegen.ThrowIfDivideByZero(div0);

      // Do division
      auto *raw_val = codegen->CreateSRem(left.GetValue(), right.GetValue());
      result = Value{Integer::Instance(), raw_val, nullptr, nullptr};
    }

    // Return result
    return result;
  }
};

////////////////////////////////////////////////////////////////////////////////
///
/// Function tables
///
////////////////////////////////////////////////////////////////////////////////

// Implicit casts
std::vector<type::TypeId> kImplicitCastingTable = {
    type::TypeId::INTEGER, type::TypeId::BIGINT, type::TypeId::DECIMAL};

// clang-format off
// Explicit casting rules
CastInteger kCastInteger;
std::vector<TypeSystem::CastInfo> kExplicitCastingTable = {
    {type::TypeId::INTEGER, type::TypeId::BOOLEAN, kCastInteger},
    {type::TypeId::INTEGER, type::TypeId::TINYINT, kCastInteger},
    {type::TypeId::INTEGER, type::TypeId::SMALLINT, kCastInteger},
    {type::TypeId::INTEGER, type::TypeId::INTEGER, kCastInteger},
    {type::TypeId::INTEGER, type::TypeId::BIGINT, kCastInteger},
    {type::TypeId::INTEGER, type::TypeId::DECIMAL, kCastInteger}};
// clang-format on

// Comparison operations
CompareInteger kCompareInteger;
std::vector<TypeSystem::ComparisonInfo> kComparisonTable = {{kCompareInteger}};

// Unary operators
Negate kNegOp;
Abs kAbsOp;
Ceil kCeilOp;
Floor kFloorOp;
Sqrt kSqrt;
std::vector<TypeSystem::UnaryOpInfo> kUnaryOperatorTable = {{OperatorId::Negation, kNegOp},
                                                            {OperatorId::Abs, kAbsOp},
                                                            {OperatorId::Ceil, kCeilOp},
                                                            {OperatorId::Floor, kFloorOp},
                                                            {OperatorId::Sqrt, kSqrt}};

// Binary operations
Add kAddOp;
Sub kSubOp;
Mul kMulOp;
Div kDivOp;
Modulo kModuloOp;
std::vector<TypeSystem::BinaryOpInfo> kBinaryOperatorTable = {
    {OperatorId::Add, kAddOp}, {OperatorId::Sub, kSubOp},    {OperatorId::Mul, kMulOp},
    {OperatorId::Div, kDivOp}, {OperatorId::Mod, kModuloOp},
};

// Nary operations
std::vector<TypeSystem::NaryOpInfo> kNaryOperatorTable = {};

// No arg operations
std::vector<TypeSystem::NoArgOpInfo> kNoArgOperatorTable = {};

}  // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
///
/// INTEGER type initialization and configuration
///
////////////////////////////////////////////////////////////////////////////////

Integer::Integer()
    : SqlType(type::TypeId::INTEGER),
      type_system_(kImplicitCastingTable, kExplicitCastingTable, kComparisonTable, kUnaryOperatorTable,
                   kBinaryOperatorTable, kNaryOperatorTable, kNoArgOperatorTable) {}

Value Integer::GetMinValue(CodeGen &codegen) const {
  auto *raw_val = codegen.Const32(peloton::type::PELOTON_INT32_MIN);
  return Value{*this, raw_val, nullptr, nullptr};
}

Value Integer::GetMaxValue(CodeGen &codegen) const {
  auto *raw_val = codegen.Const32(peloton::type::PELOTON_INT32_MAX);
  return Value{*this, raw_val, nullptr, nullptr};
}

Value Integer::GetNullValue(CodeGen &codegen) const {
  auto *raw_val = codegen.Const32(peloton::type::PELOTON_INT32_NULL);
  return Value{Type{TypeId(), true}, raw_val, nullptr, codegen.ConstBool(true)};
}

void Integer::GetTypeForMaterialization(CodeGen &codegen, llvm::Type *&val_type, llvm::Type *&len_type) const {
  val_type = codegen.Int32Type();
  len_type = nullptr;
}

llvm::Function *Integer::GetInputFunction(CodeGen &codegen, UNUSED_ATTRIBUTE const Type &type) const {
  return NumericFunctionsProxy::InputInteger.GetFunction(codegen);
}

llvm::Function *Integer::GetOutputFunction(CodeGen &codegen, UNUSED_ATTRIBUTE const Type &type) const {
  return ValuesRuntimeProxy::OutputInteger.GetFunction(codegen);
}

}  // namespace type

}  // namespace terrier::execution