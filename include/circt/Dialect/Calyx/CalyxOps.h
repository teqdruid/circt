//===- CalyxOps.h - Declare Calyx dialect operations ------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares the operation class for the Calyx IR.
//
//===----------------------------------------------------------------------===//

#ifndef CIRCT_DIALECT_CALYX_OPS_H
#define CIRCT_DIALECT_CALYX_OPS_H

#include "circt/Dialect/Calyx/CalyxDialect.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/FunctionSupport.h"
#include "mlir/IR/OpImplementation.h"
#include "mlir/IR/RegionKindInterface.h"
#include "mlir/IR/SymbolTable.h"
#include "mlir/Interfaces/InferTypeOpInterface.h"
#include "mlir/Interfaces/SideEffectInterfaces.h"

#define GET_OP_CLASSES
#include "circt/Dialect/Calyx/Calyx.h.inc"

#endif // CIRCT_DIALECT_CALYX_OPS_H
