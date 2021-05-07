//===- RTLTypes.h - types for the RTL dialect -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Types for the RTL dialect are mostly in tablegen. This file should contain
// C++ types used in MLIR type parameters.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_RTL_TYPES_H
#define CIRCT_DIALECT_RTL_TYPES_H

#include "mlir/IR/Types.h"

namespace circt {
namespace rtl {
namespace detail {
/// Struct defining a field. Used in structs and unions.
struct FieldInfo {
  mlir::StringRef name;
  mlir::Type type;
  FieldInfo allocateInto(mlir::TypeStorageAllocator &alloc) const;
};
} // namespace detail
} // namespace rtl
} // namespace circt

#define GET_TYPEDEF_CLASSES
#include "circt/Dialect/RTL/RTLTypes.h.inc"

namespace circt {
namespace rtl {

/// Return true if the specified type is a value RTL Integer type.  This checks
/// that it is a signless standard dialect type and that it isn't zero bits.
bool isRTLIntegerType(mlir::Type type);

/// Return true if the specified type can be used as an RTL value type, that is
/// the set of types that can be composed together to represent synthesized,
/// hardware but not marker types like InOutType or unknown types from other
/// dialects.
bool isRTLValueType(mlir::Type type);

/// Return the hardware bit width of a type. Does not reflect any encoding,
/// padding, or storage scheme, just the bit (and wire width) of a
/// statically-size type. Reflects the number of wires needed to transmit a
/// value of this type. Returns -1 if the type is not known or cannot be
/// statically computed.
int64_t getBitWidth(mlir::Type type);

/// Return true if the specified type contains known marker types like
/// InOutType.  Unlike isRTLValueType, this is not conservative, it only returns
/// false on known InOut types, rather than any unknown types.
bool hasRTLInOutType(mlir::Type type);

/// Return the canonical type, that is, the type with all type aliases
/// recursively resolved to a base type.
mlir::Type getCanonicalType(mlir::Type type);

} // namespace rtl
} // namespace circt

#endif // CIRCT_DIALECT_RTL_TYPES_H
