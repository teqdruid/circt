//===- ESI.cpp - C Interface for the ESI Dialect --------------------------===//
//
//===----------------------------------------------------------------------===//

#include "circt-c/Dialect/ESI.h"
#include "circt/Dialect/ESI/ESITypes.h"
#include "mlir/CAPI/IR.h"
#include "mlir/CAPI/Registration.h"
#include "mlir/CAPI/Support.h"

using namespace circt::esi;

MLIR_DEFINE_CAPI_DIALECT_REGISTRATION(ESI, esi, ESIDialect)

void registerESIPasses() { circt::esi::registerESIPasses(); }

bool circtESITypeIsAChannelType(MlirType type) {
  return unwrap(type).isa<ChannelPort>();
}

MlirType circtESIChannelTypeGet(MlirContext ctx, MlirType inner) {
  return wrap(ChannelPort::get(unwrap(ctx), unwrap(inner)));
}
