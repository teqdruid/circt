//===- ESI.cpp - C Interface for the ESI Dialect --------------------------===//
//
//===----------------------------------------------------------------------===//

#include "circt-c/Dialect/ESI.h"
#include "circt/Dialect/ESI/ESIDialect.h"
#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Registration.h"
#include "mlir/CAPI/Support.h"
#include "mlir/CAPI/Utils.h"

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(ESI, esi, circt::esi::ESIDialect)

void registerESIPasses() { circt::esi::registerESIPasses(); }

MlirLogicalResult circtESIExportCosimSchema(MlirModule module,
                                            MlirStringCallback callback,
                                            void *userData) {
  mlir::detail::CallbackOstream stream(callback, userData);
  return wrap(circt::esi::exportCosimSchema(unwrap(module), stream));
}
