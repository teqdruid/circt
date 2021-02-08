// RUN: circt-opt %s -split-input-file -verify-diagnostics | FileCheck %s

// CHECK-LABEL: @connect_ports
// CHECK-SAME: (%[[IN:.+]] : [[TYPE:.+]]) ->
// CHECK-SAME: (%[[OUT:.+]] : [[TYPE]])
// CHECK-NEXT: llhd.con %[[IN]], %[[OUT]] : [[TYPE]]
llhd.entity @connect_ports(%in: !llhd.sig<i32>) -> (%out: !llhd.sig<i32>) {
  llhd.con %in, %out : !llhd.sig<i32>
}

// -----

// expected-note @+1 {{prior use here}}
llhd.entity @connect_different_types(%in: !llhd.sig<i8>) -> (%out: !llhd.sig<i32>) {
  // expected-error @+1 {{use of value '%out' expects different type}}
  llhd.con %in, %out : !llhd.sig<i8>
  // expected-error @+1 {{expected operation}}
}
// -----

llhd.entity @connect_non_signals(%in: !llhd.sig<i32>) -> (%out: !llhd.sig<i32>) {
  %0 = llhd.prb %in : !llhd.sig<i32>
  %1 = llhd.prb %out : !llhd.sig<i32>
  // expected-error @+1 {{'llhd.con' op operand #0 must be LLHD sig type}}
  llhd.con %0, %1 : i32
}
