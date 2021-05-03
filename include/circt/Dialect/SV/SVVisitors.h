//===- SVVisitors.h - SV Dialect Visitors -----------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines visitors that make it easier to work with SV IR.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_SV_SVVISITORS_H
#define CIRCT_DIALECT_SV_SVVISITORS_H

#include "circt/Dialect/SV/SVOps.h"
#include "llvm/ADT/TypeSwitch.h"

namespace circt {
namespace sv {

template <typename ConcreteType, typename ResultType = void,
          typename... ExtraArgs>
class Visitor {
public:
  ResultType dispatchSVVisitor(Operation *op, ExtraArgs... args) {
    auto *thisCast = static_cast<ConcreteType *>(this);
    return TypeSwitch<Operation *, ResultType>(op)
        .template Case<
            // Expressions
            ReadInOutOp, ArrayIndexInOutOp, VerbatimExprOp, ConstantXOp,
            ConstantZOp,
            // Declarations.
            RegOp, WireOp,
            // Control flow.
            IfDefOp, IfDefProceduralOp, IfOp, AlwaysOp, AlwaysCombOp,
            AlwaysFFOp, InitialOp, CaseZOp,
            // Other Statements.
            ConnectOp, BPAssignOp, PAssignOp, AliasOp, FWriteOp, FatalOp,
            FinishOp, VerbatimOp,
            // Type declarations.
            InterfaceOp, InterfaceSignalOp, InterfaceModportOp,
            InterfaceInstanceOp, GetModportOp, AssignInterfaceSignalOp,
            ReadInterfaceSignalOp, TypeDefOp,
            // Verification statements.
            AssertOp, AssumeOp, CoverOp,
            // Terminators.
            TypeDeclTerminatorOp>([&](auto expr) -> ResultType {
          return thisCast->visitSV(expr, args...);
        })
        .Default([&](auto expr) -> ResultType {
          return thisCast->visitInvalidSV(op, args...);
        });
  }

  /// This callback is invoked on any invalid operations.
  ResultType visitInvalidSV(Operation *op, ExtraArgs... args) {
    op->emitOpError("unknown SV node");
    abort();
  }

  /// This callback is invoked on any SV operations that are not handled by the
  /// concrete visitor.
  ResultType visitUnhandledSV(Operation *op, ExtraArgs... args) {
    return ResultType();
  }

#define HANDLE(OPTYPE, OPKIND)                                                 \
  ResultType visitSV(OPTYPE op, ExtraArgs... args) {                           \
    return static_cast<ConcreteType *>(this)->visit##OPKIND##SV(op, args...);  \
  }

  // Declarations
  HANDLE(RegOp, Unhandled);
  HANDLE(WireOp, Unhandled);

  // Expressions
  HANDLE(ReadInOutOp, Unhandled);
  HANDLE(ArrayIndexInOutOp, Unhandled);
  HANDLE(VerbatimExprOp, Unhandled);
  HANDLE(ConstantXOp, Unhandled);
  HANDLE(ConstantZOp, Unhandled);

  // Control flow.
  HANDLE(IfDefOp, Unhandled);
  HANDLE(IfDefProceduralOp, Unhandled);
  HANDLE(IfOp, Unhandled);
  HANDLE(AlwaysOp, Unhandled);
  HANDLE(AlwaysCombOp, Unhandled);
  HANDLE(AlwaysFFOp, Unhandled);
  HANDLE(InitialOp, Unhandled);
  HANDLE(CaseZOp, Unhandled);

  // Other Statements.
  HANDLE(ConnectOp, Unhandled);
  HANDLE(BPAssignOp, Unhandled);
  HANDLE(PAssignOp, Unhandled);
  HANDLE(AliasOp, Unhandled);
  HANDLE(FWriteOp, Unhandled);
  HANDLE(FatalOp, Unhandled);
  HANDLE(FinishOp, Unhandled);
  HANDLE(VerbatimOp, Unhandled);

  // Type declarations.
  HANDLE(InterfaceOp, Unhandled);
  HANDLE(InterfaceInstanceOp, Unhandled);
  HANDLE(InterfaceSignalOp, Unhandled);
  HANDLE(InterfaceModportOp, Unhandled);
  HANDLE(GetModportOp, Unhandled);
  HANDLE(AssignInterfaceSignalOp, Unhandled);
  HANDLE(ReadInterfaceSignalOp, Unhandled);
  HANDLE(TypeDefOp, Unhandled);

  // Verification statements.
  HANDLE(AssertOp, Unhandled);
  HANDLE(AssumeOp, Unhandled);
  HANDLE(CoverOp, Unhandled);

  // Terminators.
  HANDLE(TypeDeclTerminatorOp, Unhandled);
#undef HANDLE
};

} // namespace sv
} // namespace circt

#endif // CIRCT_DIALECT_SV_SVVISITORS_H
