#  Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
#  See https://llvm.org/LICENSE.txt for license information.
#  SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

# Generated tablegen dialects end up in the mlir.dialects package for now.
from mlir.dialects._seq_ops_gen import *


def reg(val, clk, rst, name=None):
  import circt.dialects.rtl as rtl
  from mlir.ir import IntegerAttr
  valType = val.type
  if rst is not None:
    reg_reset = rtl.ConstantOp(valType, IntegerAttr.get(valType, 0)).result
    return CompRegOp(valType, val, clk, rst, reg_reset, name=name).result
  else:
    return CompRegOp(valType, val, clk, None, None, name=name).result
