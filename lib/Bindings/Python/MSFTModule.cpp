//===- MSFTModule.cpp - MSFT API pybind module ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "DialectModules.h"

#include "circt-c/Dialect/MSFT.h"
#include "circt/Dialect/MSFT/MSFTAttributes.h"
#include "circt/Support/LLVM.h"

#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Support.h"

#include "MLIRPybindAdaptors.h"
#include "PybindUtils.h"
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>
namespace py = pybind11;

using namespace circt;
using namespace circt::msft;

//===----------------------------------------------------------------------===//
// Functions that translate from something Pybind11 understands to MLIR C++.
//===----------------------------------------------------------------------===//

static void addPhysLocationAttr(MlirOperation cOp, std::string entityName,
                                DeviceType type, uint64_t x, uint64_t y,
                                uint64_t num) {

  Operation *op = unwrap(cOp);
  MLIRContext *ctxt = op->getContext();
  PhysLocationAttr loc =
      PhysLocationAttr::get(ctxt, DeviceTypeAttr::get(ctxt, type), x, y, num);
  SmallString<64> entity("loc:");
  entity.append(entityName);
  op->setAttr(entity, loc);
}

/// Populate the msft python module.
void circt::python::populateDialectMSFTSubmodule(py::module &m) {
  m.doc() = "MSFT dialect Python native extension";
  m.def("locate", &addPhysLocationAttr,
        "Attach a physical location to an op's entity.",
        py::arg("op_to_locate"), py::arg("entity_within"), py::arg("devtype"),
        py::arg("x"), py::arg("y"), py::arg("num"));
  py::enum_<DeviceType>(m, "DeviceType")
      .value("M20K", DeviceType::M20K)
      .value("DSP", DeviceType::DSP)
      .export_values();

  m.def("export_tcl", [](MlirModule mod, py::object fileObject) {
    circt::python::PyFileAccumulator accum(fileObject, false);
    py::gil_scoped_release();
    mlirMSFTExportTcl(mod, accum.getCallback(), accum.getUserData());
  });
}
