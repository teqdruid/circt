//===- RTLOps.cpp - Implement the RTL operations --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implement the RTL ops.
//
//===----------------------------------------------------------------------===//

#include "circt/Dialect/RTL/RTLOps.h"
#include "circt/Dialect/Comb/CombOps.h"
#include "circt/Dialect/RTL/RTLTypes.h"
#include "circt/Dialect/RTL/RTLVisitors.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/FunctionImplementation.h"

using namespace circt;
using namespace rtl;

/// Return true if the specified operation is a combinatorial logic op.
bool rtl::isCombinatorial(Operation *op) {
  struct IsCombClassifier : public TypeOpVisitor<IsCombClassifier, bool> {
    bool visitInvalidTypeOp(Operation *op) { return false; }
    bool visitUnhandledTypeOp(Operation *op) { return true; }
  };

  return op->getDialect()->getNamespace() == "comb" ||
         IsCombClassifier().dispatchTypeOpVisitor(op);
}

//===----------------------------------------------------------------------===//
// ConstantOp
//===----------------------------------------------------------------------===//

static void printConstantOp(OpAsmPrinter &p, ConstantOp &op) {
  p << "rtl.constant ";
  p.printAttribute(op.valueAttr());
  p.printOptionalAttrDict(op->getAttrs(), /*elidedAttrs=*/{"value"});
}

static ParseResult parseConstantOp(OpAsmParser &parser,
                                   OperationState &result) {
  IntegerAttr valueAttr;

  if (parser.parseAttribute(valueAttr, "value", result.attributes) ||
      parser.parseOptionalAttrDict(result.attributes))
    return failure();

  result.addTypes(valueAttr.getType());
  return success();
}

static LogicalResult verifyConstantOp(ConstantOp constant) {
  // If the result type has a bitwidth, then the attribute must match its width.
  if (constant.value().getBitWidth() != constant.getType().getWidth())
    return constant.emitError(
        "rtl.constant attribute bitwidth doesn't match return type");

  return success();
}

/// Build a ConstantOp from an APInt, infering the result type from the
/// width of the APInt.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       const APInt &value) {

  auto type = IntegerType::get(builder.getContext(), value.getBitWidth());
  auto attr = builder.getIntegerAttr(type, value);
  return build(builder, result, type, attr);
}

/// Build a ConstantOp from an APInt, infering the result type from the
/// width of the APInt.
void ConstantOp::build(OpBuilder &builder, OperationState &result,
                       IntegerAttr value) {
  return build(builder, result, value.getType(), value);
}

/// This builder allows construction of small signed integers like 0, 1, -1
/// matching a specified MLIR IntegerType.  This shouldn't be used for general
/// constant folding because it only works with values that can be expressed in
/// an int64_t.  Use APInt's instead.
void ConstantOp::build(OpBuilder &builder, OperationState &result, Type type,
                       int64_t value) {
  auto numBits = type.cast<IntegerType>().getWidth();
  build(builder, result, APInt(numBits, (uint64_t)value, /*isSigned=*/true));
}

void ConstantOp::getAsmResultNames(
    function_ref<void(Value, StringRef)> setNameFn) {
  auto intTy = getType();
  auto intCst = getValue();

  // Sugar i1 constants with 'true' and 'false'.
  if (intTy.getWidth() == 1)
    return setNameFn(getResult(), intCst.isNullValue() ? "false" : "true");

  // Otherwise, build a complex name with the value and type.
  SmallVector<char, 32> specialNameBuffer;
  llvm::raw_svector_ostream specialName(specialNameBuffer);
  specialName << 'c' << intCst << '_' << intTy;
  setNameFn(getResult(), specialName.str());
}

OpFoldResult ConstantOp::fold(ArrayRef<Attribute> constants) {
  assert(constants.empty() && "constant has no operands");
  return valueAttr();
}

//===----------------------------------------------------------------------===//
// RTLModuleOp
//===----------------------------------------------------------------------===/

/// Return true if this is an rtl.module, external module, generated module etc.
bool rtl::isAnyModule(Operation *module) {
  return isa<RTLModuleOp>(module) || isa<RTLModuleExternOp>(module) ||
         isa<RTLModuleGeneratedOp>(module);
}

/// Return the signature for the specified module as a function type.
FunctionType rtl::getModuleType(Operation *module) {
  auto typeAttr =
      module->getAttrOfType<TypeAttr>(RTLModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

/// Return the name to use for the Verilog module that we're referencing
/// here.  This is typically the symbol, but can be overridden with the
/// verilogName attribute.
StringAttr rtl::getVerilogModuleNameAttr(Operation *module) {
  auto nameAttr = module->getAttrOfType<StringAttr>("verilogName");
  if (nameAttr)
    return nameAttr;

  return module->getAttrOfType<StringAttr>(
      ::mlir::SymbolTable::getSymbolAttrName());
}

/// Return the port name for the specified argument or result.
StringAttr rtl::getModuleArgumentNameAttr(Operation *module, size_t argNo) {
  return module->getAttrOfType<ArrayAttr>("argNames")[argNo].cast<StringAttr>();
}

StringAttr rtl::getModuleResultNameAttr(Operation *module, size_t resultNo) {
  return module->getAttrOfType<ArrayAttr>("resultNames")[resultNo]
      .cast<StringAttr>();
}

void rtl::setModuleArgumentNames(Operation *module, ArrayRef<Attribute> names) {
  assert(getModuleType(module).getNumInputs() == names.size() &&
         "incorrect number of arguments names specified");
  module->setAttr("argNames", ArrayAttr::get(module->getContext(), names));
}

void rtl::setModuleResultNames(Operation *module, ArrayRef<Attribute> names) {
  assert(getModuleType(module).getNumResults() == names.size() &&
         "incorrect number of arguments names specified");
  module->setAttr("resultNames", ArrayAttr::get(module->getContext(), names));
}

// Flag for parsing different module types
enum ExternModKind { PlainMod, ExternMod, GenMod };

static void buildModule(OpBuilder &builder, OperationState &result,
                        StringAttr name, ArrayRef<ModulePortInfo> ports,
                        ArrayRef<NamedAttribute> attributes) {
  using namespace mlir::impl;

  // Add an attribute for the name.
  result.addAttribute(::mlir::SymbolTable::getSymbolAttrName(), name);

  SmallVector<Type, 4> argTypes;
  SmallVector<Type, 4> resultTypes;
  for (auto elt : ports) {
    if (elt.isOutput())
      resultTypes.push_back(elt.type);
    else {
      if (elt.direction == PortDirection::INOUT &&
          !elt.type.isa<rtl::InOutType>())
        elt.type = rtl::InOutType::get(elt.type);
      argTypes.push_back(elt.type);
    }
  }

  // Record the argument and result types as an attribute.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // Record the names of the arguments if present.
  SmallVector<Attribute> argNames, resultNames;
  for (const ModulePortInfo &port : ports) {
    SmallVector<NamedAttribute, 2> argAttrs;
    if (port.isOutput())
      resultNames.push_back(port.name);
    else
      argNames.push_back(port.name);
  }

  result.addAttribute("argNames", builder.getArrayAttr(argNames));
  result.addAttribute("resultNames", builder.getArrayAttr(resultNames));
  result.addAttributes(attributes);
  result.addRegion();
}

void RTLModuleOp::build(OpBuilder &builder, OperationState &result,
                        StringAttr name, ArrayRef<ModulePortInfo> ports,
                        ArrayRef<NamedAttribute> attributes) {
  buildModule(builder, result, name, ports, attributes);

  // Create a region and a block for the body.
  auto *bodyRegion = result.regions[0].get();
  Block *body = new Block();
  bodyRegion->push_back(body);

  // Add arguments to the body block.
  for (auto elt : ports)
    if (!elt.isOutput())
      body->addArgument(elt.type);

  RTLModuleOp::ensureTerminator(*bodyRegion, builder, result.location);
}

/// Return the name to use for the Verilog module that we're referencing
/// here.  This is typically the symbol, but can be overridden with the
/// verilogName attribute.
StringRef RTLModuleExternOp::getVerilogModuleName() {
  if (auto vname = verilogName())
    return vname.getValue();
  return getName();
}

/// Return the name to use for the Verilog module that we're referencing
/// here.  This is typically the symbol, but can be overridden with the
/// verilogName attribute.
StringAttr RTLModuleExternOp::getVerilogModuleNameAttr() {
  if (auto vName = verilogNameAttr())
    return vName;

  return (*this)->getAttrOfType<StringAttr>(
      ::mlir::SymbolTable::getSymbolAttrName());
}

void RTLModuleExternOp::build(OpBuilder &builder, OperationState &result,
                              StringAttr name, ArrayRef<ModulePortInfo> ports,
                              StringRef verilogName,
                              ArrayRef<NamedAttribute> attributes) {
  buildModule(builder, result, name, ports, attributes);

  if (!verilogName.empty())
    result.addAttribute("verilogName", builder.getStringAttr(verilogName));
}

void RTLModuleGeneratedOp::build(OpBuilder &builder, OperationState &result,
                                 FlatSymbolRefAttr genKind, StringAttr name,
                                 ArrayRef<ModulePortInfo> ports,
                                 StringRef verilogName,
                                 ArrayRef<NamedAttribute> attributes) {
  buildModule(builder, result, name, ports, attributes);
  result.addAttribute("generatorKind", genKind);
  if (!verilogName.empty())
    result.addAttribute("verilogName", builder.getStringAttr(verilogName));
}

SmallVector<ModulePortInfo> rtl::getModulePortInfo(Operation *op) {
  assert(isAnyModule(op) && "Can only get module ports from a module");
  SmallVector<ModulePortInfo> results;
  auto argTypes = getModuleType(op).getInputs();

  auto argNames = op->getAttrOfType<ArrayAttr>("argNames");
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    bool isInOut = false;
    auto type = argTypes[i];

    if (auto inout = type.dyn_cast<InOutType>()) {
      isInOut = true;
      type = inout.getElementType();
    }

    auto direction = isInOut ? PortDirection::INOUT : PortDirection::INPUT;
    results.push_back({argNames[i].cast<StringAttr>(), direction, type, i});
  }

  auto resultNames = op->getAttrOfType<ArrayAttr>("resultNames");
  auto resultTypes = getModuleType(op).getResults();
  for (unsigned i = 0, e = resultTypes.size(); i < e; ++i) {
    results.push_back({resultNames[i].cast<StringAttr>(), PortDirection::OUTPUT,
                       resultTypes[i], i});
  }
  return results;
}

static StringAttr getPortNameAttr(MLIRContext *context, StringRef name) {
  if (!name.empty()) {
    // Ignore numeric names like %42
    assert(name.size() > 1 && name[0] == '%' && "Unknown MLIR name");
    if (isdigit(name[1]))
      name = StringRef();
    else
      name = name.drop_front();
  }
  return StringAttr::get(context, name);
}

/// Parse a function result list.
///
///   function-result-list ::= function-result-list-parens
///   function-result-list-parens ::= `(` `)`
///                                 | `(` function-result-list-no-parens `)`
///   function-result-list-no-parens ::= function-result (`,` function-result)*
///   function-result ::= (percent-identifier `:`) type attribute-dict?
///
static ParseResult
parseFunctionResultList(OpAsmParser &parser, SmallVectorImpl<Type> &resultTypes,
                        SmallVectorImpl<NamedAttrList> &resultAttrs,
                        SmallVectorImpl<Attribute> &resultNames) {
  if (parser.parseLParen())
    return failure();

  // Special case for an empty set of parens.
  if (succeeded(parser.parseOptionalRParen()))
    return success();

  auto *context = parser.getBuilder().getContext();
  // Parse individual function results.
  do {
    resultTypes.emplace_back();
    resultAttrs.emplace_back();

    OpAsmParser::OperandType operandName;
    auto namePresent = parser.parseOptionalOperand(operandName);
    StringRef implicitName;
    if (namePresent.hasValue()) {
      if (namePresent.getValue() || parser.parseColon())
        return failure();

      // If the name was specified, then we will use it.
      implicitName = operandName.name;
    }
    resultNames.push_back(getPortNameAttr(context, implicitName));

    if (parser.parseType(resultTypes.back()) ||
        parser.parseOptionalAttrDict(resultAttrs.back()))
      return failure();
  } while (succeeded(parser.parseOptionalComma()));
  return parser.parseRParen();
}

/// This is a variant of mlor::parseFunctionSignature that allows names on
/// result arguments.
static ParseResult parseModuleFunctionSignature(
    OpAsmParser &parser, SmallVectorImpl<OpAsmParser::OperandType> &argNames,
    SmallVectorImpl<Type> &argTypes, SmallVectorImpl<NamedAttrList> &argAttrs,
    bool &isVariadic, SmallVectorImpl<Type> &resultTypes,
    SmallVectorImpl<NamedAttrList> &resultAttrs,
    SmallVectorImpl<Attribute> &resultNames) {

  using namespace mlir::impl;
  bool allowArgAttrs = true;
  bool allowVariadic = false;
  if (parseFunctionArgumentList(parser, allowArgAttrs, allowVariadic, argNames,
                                argTypes, argAttrs, isVariadic))
    return failure();

  if (succeeded(parser.parseOptionalArrow()))
    return parseFunctionResultList(parser, resultTypes, resultAttrs,
                                   resultNames);
  return success();
}

static bool hasAttribute(StringRef name, ArrayRef<NamedAttribute> attrs) {
  for (auto &argAttr : attrs)
    if (argAttr.first == name)
      return true;
  return false;
}

static ParseResult parseRTLModuleOp(OpAsmParser &parser, OperationState &result,
                                    ExternModKind modKind = PlainMod) {
  using namespace mlir::impl;

  SmallVector<OpAsmParser::OperandType, 4> entryArgs;
  SmallVector<NamedAttrList, 4> argAttrs;
  SmallVector<NamedAttrList, 4> resultAttrs;
  SmallVector<Type, 4> argTypes;
  SmallVector<Type, 4> resultTypes;
  auto &builder = parser.getBuilder();

  // Parse the name as a symbol.
  StringAttr nameAttr;
  if (parser.parseSymbolName(nameAttr, ::mlir::SymbolTable::getSymbolAttrName(),
                             result.attributes))
    return failure();

  FlatSymbolRefAttr kindAttr;
  if (modKind == GenMod) {
    if (parser.parseComma() ||
        parser.parseAttribute(kindAttr, "generatorKind", result.attributes)) {
      return failure();
    }
  }

  // Parse the function signature.
  bool isVariadic = false;
  SmallVector<Attribute> resultNames;
  if (parseModuleFunctionSignature(parser, entryArgs, argTypes, argAttrs,
                                   isVariadic, resultTypes, resultAttrs,
                                   resultNames))
    return failure();

  // Record the argument and result types as an attribute.  This is necessary
  // for external modules.
  auto type = builder.getFunctionType(argTypes, resultTypes);
  result.addAttribute(getTypeAttrName(), TypeAttr::get(type));

  // If function attributes are present, parse them.
  if (parser.parseOptionalAttrDictWithKeyword(result.attributes))
    return failure();

  auto *context = result.getContext();

  // Use the argument and result names if not already specified.
  if (!hasAttribute("argNames", result.attributes)) {
    SmallVector<Attribute> argNames;
    if (!entryArgs.empty()) {
      for (auto &arg : entryArgs)
        argNames.push_back(getPortNameAttr(context, arg.name));
    } else if (!argTypes.empty()) {
      // The parser returns empty names in a special way.
      argNames.assign(argTypes.size(), StringAttr::get(context, ""));
    }

    result.addAttribute("argNames", ArrayAttr::get(context, argNames));
  }
  if (!hasAttribute("resultNames", result.attributes))
    result.addAttribute("resultNames", ArrayAttr::get(context, resultNames));

  assert(argAttrs.size() == argTypes.size());
  assert(resultAttrs.size() == resultTypes.size());

  // Add the attributes to the function arguments.
  addArgAndResultAttrs(builder, result, argAttrs, resultAttrs);

  // Parse the optional function body.
  auto *body = result.addRegion();
  if (modKind == PlainMod) {
    if (parser.parseRegion(*body, entryArgs,
                           entryArgs.empty() ? ArrayRef<Type>() : argTypes))
      return failure();

    RTLModuleOp::ensureTerminator(*body, parser.getBuilder(), result.location);
  }
  return success();
}

static ParseResult parseRTLModuleExternOp(OpAsmParser &parser,
                                          OperationState &result) {
  return parseRTLModuleOp(parser, result, ExternMod);
}

static ParseResult parseRTLModuleGeneratedOp(OpAsmParser &parser,
                                             OperationState &result) {
  return parseRTLModuleOp(parser, result, GenMod);
}

FunctionType getRTLModuleOpType(Operation *op) {
  auto typeAttr = op->getAttrOfType<TypeAttr>(RTLModuleOp::getTypeAttrName());
  return typeAttr.getValue().cast<FunctionType>();
}

static void printModuleSignature(OpAsmPrinter &p, Operation *op,
                                 ArrayRef<Type> argTypes, bool isVariadic,
                                 ArrayRef<Type> resultTypes,
                                 bool &needArgNamesAttr) {
  Region &body = op->getRegion(0);
  bool isExternal = body.empty();
  SmallString<32> resultNameStr;

  p << '(';
  for (unsigned i = 0, e = argTypes.size(); i < e; ++i) {
    if (i > 0)
      p << ", ";

    auto argName = getModuleArgumentName(op, i);

    if (!isExternal) {
      // Get the printed format for the argument name.
      resultNameStr.clear();
      llvm::raw_svector_ostream tmpStream(resultNameStr);
      p.printOperand(body.front().getArgument(i), tmpStream);

      // If the name wasn't printable in a way that agreed with argName, make
      // sure to print out an explicit argNames attribute.
      if (tmpStream.str().drop_front() != argName)
        needArgNamesAttr = true;

      p << tmpStream.str() << ": ";
    } else if (!argName.empty()) {
      p << '%' << argName << ": ";
    }

    p.printType(argTypes[i]);
    p.printOptionalAttrDict(::mlir::impl::getArgAttrs(op, i));
  }

  if (isVariadic) {
    if (!argTypes.empty())
      p << ", ";
    p << "...";
  }

  p << ')';

  // We print result types specially since we support named arguments.
  if (!resultTypes.empty()) {
    auto &os = p.getStream();
    os << " -> (";
    for (size_t i = 0, e = resultTypes.size(); i < e; ++i) {
      if (i != 0)
        os << ", ";
      StringRef name = getModuleResultName(op, i);
      if (!name.empty())
        os << '%' << name << ": ";

      auto resultAttrs = ::mlir::impl::getResultAttrs(op, i);
      p.printType(resultTypes[i]);
      p.printOptionalAttrDict(resultAttrs);
    }
    os << ')';
  }
}

static void printModuleOp(OpAsmPrinter &p, Operation *op,
                          ExternModKind modKind) {
  using namespace mlir::impl;

  FunctionType fnType = getRTLModuleOpType(op);
  auto argTypes = fnType.getInputs();
  auto resultTypes = fnType.getResults();

  // Print the operation and the function name.
  auto funcName =
      op->getAttrOfType<StringAttr>(::mlir::SymbolTable::getSymbolAttrName())
          .getValue();
  p << op->getName() << ' ';
  p.printSymbolName(funcName);
  if (modKind == GenMod) {
    p << ", ";
    p.printSymbolName(cast<RTLModuleGeneratedOp>(op).generatorKind());
  }

  bool needArgNamesAttr = false;
  printModuleSignature(p, op, argTypes, /*isVariadic=*/false, resultTypes,
                       needArgNamesAttr);

  SmallVector<StringRef, 3> omittedAttrs;
  if (modKind == GenMod)
    omittedAttrs.push_back("generatorKind");
  if (!needArgNamesAttr)
    omittedAttrs.push_back("argNames");
  omittedAttrs.push_back("resultNames");

  printFunctionAttributes(p, op, argTypes.size(), resultTypes.size(),
                          omittedAttrs);
}

static void print(OpAsmPrinter &p, RTLModuleExternOp op) {
  printModuleOp(p, op, ExternMod);
}
static void print(OpAsmPrinter &p, RTLModuleGeneratedOp op) {
  printModuleOp(p, op, GenMod);
}

static void print(OpAsmPrinter &p, RTLModuleOp op) {
  printModuleOp(p, op, PlainMod);

  // Print the body if this is not an external function.
  Region &body = op.getBody();
  if (!body.empty())
    p.printRegion(body, /*printEntryBlockArgs=*/false,
                  /*printBlockTerminators=*/true);
}

static LogicalResult verifyModuleCommon(Operation *module) {
  assert(isAnyModule(module) &&
         "verifier hook should only be called on modules");

  auto moduleType = getModuleType(module);
  auto argNames = module->getAttrOfType<ArrayAttr>("argNames");
  auto resultNames = module->getAttrOfType<ArrayAttr>("resultNames");
  if (argNames.size() != moduleType.getNumInputs())
    return module->emitOpError("incorrect number of argument names");
  if (resultNames.size() != moduleType.getNumResults())
    return module->emitOpError("incorrect number of result names");
  return success();
}

static LogicalResult verifyRTLModuleOp(RTLModuleOp op) {
  return verifyModuleCommon(op);
}

static LogicalResult verifyRTLModuleExternOp(RTLModuleExternOp op) {
  return verifyModuleCommon(op);
}

/// Lookup the generator for the symbol.  This returns null on
/// invalid IR.
Operation *RTLModuleGeneratedOp::getGeneratorKindOp() {
  auto topLevelModuleOp = (*this)->getParentOfType<ModuleOp>();
  return topLevelModuleOp.lookupSymbol(generatorKind());
}

static LogicalResult verifyRTLModuleGeneratedOp(RTLModuleGeneratedOp op) {
  if (failed(verifyModuleCommon(op)))
    return failure();

  auto referencedKind = op.getGeneratorKindOp();
  if (referencedKind == nullptr)
    return op.emitError("Cannot find generator definition '")
           << op.generatorKind() << "'";

  if (!isa<RTLGeneratorSchemaOp>(referencedKind))
    return op.emitError("Symbol resolved to '")
           << referencedKind->getName()
           << "' which is not a RTLGeneratorSchemaOp";

  auto referencedKindOp = dyn_cast<RTLGeneratorSchemaOp>(referencedKind);
  auto paramRef = referencedKindOp.requiredAttrs();
  auto dict = op->getAttrDictionary();
  for (auto str : paramRef) {
    auto strAttr = str.dyn_cast<StringAttr>();
    if (!strAttr)
      return op.emitError("Unknown attribute type, expected a string");
    if (!dict.get(strAttr.getValue()))
      return op.emitError("Missing attribute '") << strAttr.getValue() << "'";
  }

  return success();
}

//===----------------------------------------------------------------------===//
// InstanceOp
//===----------------------------------------------------------------------===//

/// Lookup the module or extmodule for the symbol.  This returns null on
/// invalid IR.
Operation *InstanceOp::getReferencedModule() {
  auto topLevelModuleOp = (*this)->getParentOfType<ModuleOp>();
  if (!topLevelModuleOp)
    return nullptr;

  return topLevelModuleOp.lookupSymbol(moduleName());
}

// Helper function to verify instance op types
static LogicalResult verifyInstanceOpTypes(InstanceOp op,
                                           Operation *referencedModule) {
  assert(referencedModule && "referenced module must not be null");

  // Check operand types first.
  auto numOperands = op->getNumOperands();
  auto expectedOperandTypes = getModuleType(referencedModule).getInputs();

  if (expectedOperandTypes.size() != numOperands) {
    auto diag = op.emitOpError()
                << "has a wrong number of operands; expected "
                << expectedOperandTypes.size() << " but got " << numOperands;
    diag.attachNote(referencedModule->getLoc())
        << "original module declared here";

    return failure();
  }

  for (size_t i = 0; i != numOperands; ++i) {
    auto expectedType = expectedOperandTypes[i];
    auto operandType = op.getOperand(i).getType();
    if (operandType != expectedType) {
      auto diag = op.emitOpError()
                  << "#" << i << " operand type must be " << expectedType
                  << ", but got " << operandType;

      diag.attachNote(referencedModule->getLoc())
          << "original module declared here";
      return failure();
    }
  }

  // Checke result types.
  auto numResults = op->getNumResults();
  auto expectedResultTypes = getModuleType(referencedModule).getResults();

  if (expectedResultTypes.size() != numResults) {
    auto diag = op.emitOpError()
                << "has a wrong number of results; expected "
                << expectedResultTypes.size() << " but got " << numResults;
    diag.attachNote(referencedModule->getLoc())
        << "original module declared here";

    return failure();
  }

  for (size_t i = 0; i != numResults; ++i) {
    auto expectedType = expectedResultTypes[i];
    auto resultType = op.getResult(i).getType();
    if (resultType != expectedType) {
      auto diag = op.emitOpError()
                  << "#" << i << " result type must be " << expectedType
                  << ", but got " << resultType;

      diag.attachNote(referencedModule->getLoc())
          << "original module declared here";
      return failure();
    }
  }

  return success();
}

static LogicalResult verifyInstanceOp(InstanceOp op) {
  // Check that this instance is inside a module.
  auto module = dyn_cast<RTLModuleOp>(op->getParentOp());
  if (!module) {
    op.emitOpError("should be embedded in an 'rtl.module'");
    return failure();
  }

  auto referencedModule = op.getReferencedModule();
  if (referencedModule == nullptr)
    return op.emitError("Cannot find module definition '")
           << op.moduleName() << "'";

  if (!isAnyModule(referencedModule))
    return op.emitError("Symbol resolved to '")
           << referencedModule->getName()
           << "' which is not a RTL[Ext|Generated]ModuleOp";

  if (auto paramDictOpt = op.parameters()) {
    DictionaryAttr paramDict = paramDictOpt.getValue();
    auto checkParmValue = [&](NamedAttribute elt) -> bool {
      auto value = elt.second;
      if (value.isa<IntegerAttr>() || value.isa<StringAttr>() ||
          value.isa<FloatAttr>())
        return true;
      op.emitError() << "has unknown extmodule parameter value '" << elt.first
                     << "' = " << value;
      return false;
    };

    if (!llvm::all_of(paramDict, checkParmValue))
      return failure();
  }

  // If the referenced moudle is internal, check that input and result types are
  // consistent with the referenced module.
  if (!isa<RTLModuleOp>(referencedModule))
    return success();

  return verifyInstanceOpTypes(op, referencedModule);
}

StringAttr InstanceOp::getResultName(size_t idx) {
  auto *module = getReferencedModule();
  if (!module)
    return {};

  for (auto &port : getModulePortInfo(module)) {
    if (!port.isOutput())
      continue;
    if (idx == 0)
      return port.name;
    --idx;
  }

  return StringAttr();
}

/// Suggest a name for each result value based on the saved result names
/// attribute.
void InstanceOp::getAsmResultNames(OpAsmSetValueNameFn setNameFn) {
  auto *module = getReferencedModule();
  if (!module)
    return;

  // Provide default names for instance results.
  std::string name = instanceName().str() + ".";
  size_t baseNameLen = name.size();

  for (size_t i = 0, e = getNumResults(); i != e; ++i) {
    auto resName = getModuleResultName(module, i);
    name.resize(baseNameLen);
    if (!resName.empty())
      name += resName.str();
    else
      name += std::to_string(i);
    setNameFn(getResult(i), name);
  }
}

//===----------------------------------------------------------------------===//
// RTLOutputOp
//===----------------------------------------------------------------------===//

/// Verify that the num of operands and types fit the declared results.
static LogicalResult verifyOutputOp(OutputOp *op) {
  OperandRange outputValues = op->getOperands();
  auto opParent = (*op)->getParentOp();

  // Check that we are in the correct region. OutputOp should be directly
  // contained by an RTLModuleOp region. We'll loosen this restriction if
  // there's a compelling use case.
  if (!isa<RTLModuleOp>(opParent)) {
    op->emitOpError("operation expected to be in a RTLModuleOp.");
    return failure();
  }

  // Check that the we (rtl.output) have the same number of operands as our
  // region has results.
  FunctionType modType = getModuleType(opParent);
  ArrayRef<Type> modResults = modType.getResults();
  if (modResults.size() != outputValues.size()) {
    op->emitOpError("must have same number of operands as region results.");
    return failure();
  }

  // Check that the types of our operands and the region's results match.
  for (size_t i = 0, e = modResults.size(); i < e; ++i) {
    if (modResults[i] != outputValues[i].getType()) {
      op->emitOpError("output types must match module. In "
                      "operand ")
          << i << ", expected " << modResults[i] << ", but got "
          << outputValues[i].getType() << ".";
      return failure();
    }
  }

  return success();
}

//===----------------------------------------------------------------------===//
// Other Operations
//===----------------------------------------------------------------------===//

static ParseResult parseSliceTypes(OpAsmParser &p, Type &srcType,
                                   Type &idxType) {
  ArrayType arrType;
  if (p.parseType(arrType))
    return failure();
  srcType = arrType;
  unsigned idxWidth = llvm::Log2_64_Ceil(arrType.getSize());
  idxType = IntegerType::get(p.getBuilder().getContext(), idxWidth);
  return success();
}

static void printSliceTypes(OpAsmPrinter &p, Operation *, Type srcType,
                            Type idxType) {
  p.printType(srcType);
}

static ParseResult parseArrayCreateOp(OpAsmParser &parser,
                                      OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  llvm::SmallVector<OpAsmParser::OperandType, 16> operands;
  Type elemType;

  if (parser.parseOperandList(operands) ||
      parser.parseOptionalAttrDict(result.attributes) || parser.parseColon() ||
      parser.parseLParen() || parser.parseType(elemType) ||
      parser.parseRParen())
    return failure();

  if (operands.size() == 0)
    return parser.emitError(inputOperandsLoc,
                            "Cannot construct an array of length 0");
  result.addTypes({ArrayType::get(elemType, operands.size())});

  for (auto operand : operands)
    if (parser.resolveOperand(operand, elemType, result.operands))
      return failure();
  return success();
}

static void print(OpAsmPrinter &p, ArrayCreateOp op) {
  p << "rtl.array_create ";
  p.printOperands(op.inputs());
  p << " : (" << op.inputs()[0].getType() << ")";
}

void ArrayCreateOp::build(OpBuilder &b, OperationState &state,
                          ValueRange values) {
  assert(values.size() > 0 && "Cannot build array of zero elements");
  Type elemType = values[0].getType();
  assert(llvm::all_of(
             values,
             [elemType](Value v) -> bool { return v.getType() == elemType; }) &&
         "All values must have same type.");
  build(b, state, ArrayType::get(elemType, values.size()), values);
}

static ParseResult parseArrayConcatTypes(OpAsmParser &p,
                                         SmallVectorImpl<Type> &inputTypes,
                                         Type &resultType) {
  Type elemType;
  uint64_t resultSize = 0;
  do {
    ArrayType ty;
    if (p.parseType(ty))
      return p.emitError(p.getCurrentLocation(), "Expected !rtl.array type");
    if (elemType && elemType != ty.getElementType())
      return p.emitError(p.getCurrentLocation(), "Expected array element type ")
             << elemType;

    elemType = ty.getElementType();
    inputTypes.push_back(ty);
    resultSize += ty.getSize();
  } while (!p.parseOptionalComma());

  resultType = ArrayType::get(elemType, resultSize);
  return success();
}

static void printArrayConcatTypes(OpAsmPrinter &p, Operation *,
                                  TypeRange inputTypes, Type resultType) {
  llvm::interleaveComma(inputTypes, p, [&p](Type t) { p << t; });
}

void ArrayConcatOp::build(OpBuilder &b, OperationState &state,
                          ValueRange values) {
  assert(!values.empty() && "Cannot build array of zero elements");
  ArrayType arrayTy = values[0].getType().cast<ArrayType>();
  Type elemTy = arrayTy.getElementType();
  assert(llvm::all_of(values,
                      [elemTy](Value v) -> bool {
                        return v.getType().isa<ArrayType>() &&
                               v.getType().cast<ArrayType>().getElementType() ==
                                   elemTy;
                      }) &&
         "All values must be of ArrayType with the same element type.");

  uint64_t resultSize = 0;
  for (Value val : values)
    resultSize += val.getType().cast<ArrayType>().getSize();
  build(b, state, ArrayType::get(elemTy, resultSize), values);
}

//===----------------------------------------------------------------------===//
// StructCreateOp
//===----------------------------------------------------------------------===//

static ParseResult parseStructCreateOp(OpAsmParser &parser,
                                       OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  llvm::SmallVector<OpAsmParser::OperandType, 4> operands;
  StructType declType;

  if (parser.parseLParen() || parser.parseOperandList(operands) ||
      parser.parseRParen() || parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();

  llvm::SmallVector<Type, 4> structInnerTypes;
  declType.getInnerTypes(structInnerTypes);
  result.addTypes(declType);

  if (parser.resolveOperands(operands, structInnerTypes, inputOperandsLoc,
                             result.operands))
    return failure();
  return success();
}

static void print(OpAsmPrinter &printer, rtl::StructCreateOp op) {
  printer << op.getOperationName() << " (";
  printer.printOperands(op.input());
  printer << ")";
  printer.printOptionalAttrDict(op->getAttrs());
  printer << " : " << op.getType();
}

//===----------------------------------------------------------------------===//
// StructExplodeOp
//===----------------------------------------------------------------------===//

static ParseResult parseStructExplodeOp(OpAsmParser &parser,
                                        OperationState &result) {
  OpAsmParser::OperandType operand;
  StructType declType;

  if (parser.parseOperand(operand) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();

  llvm::SmallVector<Type, 4> structInnerTypes;
  declType.getInnerTypes(structInnerTypes);
  result.addTypes(structInnerTypes);

  if (parser.resolveOperand(operand, declType, result.operands))
    return failure();
  return success();
}

static void print(OpAsmPrinter &printer, rtl::StructExplodeOp op) {
  printer << op.getOperationName() << " ";
  printer.printOperand(op.input());
  printer.printOptionalAttrDict(op->getAttrs());
  printer << " : " << op.input().getType();
}

//===----------------------------------------------------------------------===//
// StructExtractOp
//===----------------------------------------------------------------------===//

/// Use the same parser for both struct_extract and union_extract since the
/// syntax is identical.
template <typename AggregateType>
static ParseResult parseExtractOp(OpAsmParser &parser, OperationState &result) {
  OpAsmParser::OperandType operand;
  StringAttr fieldName;
  Type declType;

  if (parser.parseOperand(operand) || parser.parseLSquare() ||
      parser.parseAttribute(fieldName, "field", result.attributes) ||
      parser.parseRSquare() ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();

  auto structType = getCanonicalType(declType).dyn_cast<AggregateType>();
  if (!structType)
    return parser.emitError(
        parser.getNameLoc(),
        "expected canonical type to be either Struct or Union type");
  Type resultType = structType.getFieldType(fieldName.getValue());
  if (!resultType) {
    parser.emitError(parser.getNameLoc(), "invalid field name specified");
    return failure();
  }
  result.addTypes(resultType);

  if (parser.resolveOperand(operand, declType, result.operands))
    return failure();
  return success();
}

/// Use the same printer for both struct_extract and union_extract since the
/// syntax is identical.
template <typename AggType>
static void printExtractOp(OpAsmPrinter &printer, AggType op) {
  printer << op.getOperationName() << " ";
  printer.printOperand(op.input());
  printer << "[\"" << op.field() << "\"]";
  printer.printOptionalAttrDict(op->getAttrs(), {"field"});
  printer << " : " << op.input().getType();
}

static ParseResult parseStructExtractOp(OpAsmParser &parser,
                                        OperationState &result) {
  return parseExtractOp<StructType>(parser, result);
}

static void print(OpAsmPrinter &printer, rtl::StructExtractOp op) {
  printExtractOp(printer, op);
}

void StructExtractOp::build(::mlir::OpBuilder &odsBuilder,
                            ::mlir::OperationState &odsState, Value input,
                            StructType::FieldInfo field) {
  build(odsBuilder, odsState, field.type, input, field.name);
}

//===----------------------------------------------------------------------===//
// StructInjectOp
//===----------------------------------------------------------------------===//

static ParseResult parseStructInjectOp(OpAsmParser &parser,
                                       OperationState &result) {
  llvm::SMLoc inputOperandsLoc = parser.getCurrentLocation();
  OpAsmParser::OperandType operand, val;
  StringAttr fieldName;
  StructType declType;

  if (parser.parseOperand(operand) || parser.parseLSquare() ||
      parser.parseAttribute(fieldName, "field", result.attributes) ||
      parser.parseRSquare() || parser.parseComma() ||
      parser.parseOperand(val) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();

  Type resultType = declType.getFieldType(fieldName.getValue());
  if (!resultType) {
    parser.emitError(inputOperandsLoc, "invalid field name specified");
    return failure();
  }
  result.addTypes(declType);

  if (parser.resolveOperands({operand, val}, {declType, resultType},
                             inputOperandsLoc, result.operands))
    return failure();
  return success();
}

static void print(OpAsmPrinter &printer, rtl::StructInjectOp op) {
  printer << op.getOperationName() << " ";
  printer.printOperand(op.input());
  printer << "[\"" << op.field() << "\"], ";
  printer.printOperand(op.newValue());
  printer.printOptionalAttrDict(op->getAttrs(), {"field"});
  printer << " : " << op.input().getType();
}

//===----------------------------------------------------------------------===//
// UnionCreateOp
//===----------------------------------------------------------------------===//

static ParseResult parseUnionCreateOp(OpAsmParser &parser,
                                      OperationState &result) {
  UnionType declType;
  StringAttr field;
  OpAsmParser::OperandType input;
  llvm::SMLoc fieldLoc = parser.getCurrentLocation();

  if (parser.parseAttribute(field, "field", result.attributes) ||
      parser.parseComma() || parser.parseOperand(input) ||
      parser.parseOptionalAttrDict(result.attributes) ||
      parser.parseColonType(declType))
    return failure();

  Type inputType = declType.getFieldType(field.getValue());
  if (!inputType) {
    parser.emitError(fieldLoc, "cannot find union field '")
        << field.getValue() << '\'';
    return failure();
  }

  if (parser.resolveOperand(input, inputType, result.operands))
    return failure();
  result.addTypes({declType});
  return success();
}

static void print(OpAsmPrinter &printer, rtl::UnionCreateOp op) {
  printer << op.getOperationName() << " \"" << op.field() << "\", ";
  printer.printOperand(op.input());
  printer.printOptionalAttrDict(op->getAttrs(), {"field"});
  printer << " : " << op.getType();
}

//===----------------------------------------------------------------------===//
// UnionExtractOp
//===----------------------------------------------------------------------===//

static ParseResult parseUnionExtractOp(OpAsmParser &parser,
                                       OperationState &result) {
  return parseExtractOp<UnionType>(parser, result);
}

static void print(OpAsmPrinter &printer, rtl::UnionExtractOp op) {
  printExtractOp(printer, op);
}

//===----------------------------------------------------------------------===//
// ArrayGetOp
//===----------------------------------------------------------------------===//

void ArrayGetOp::build(OpBuilder &builder, OperationState &result, Value input,
                       Value index) {
  auto resultType = input.getType().cast<ArrayType>().getElementType();
  build(builder, result, resultType, input, index);
}

//===----------------------------------------------------------------------===//
// TableGen generated logic.
//===----------------------------------------------------------------------===//

// Provide the autogenerated implementation guts for the Op classes.
#define GET_OP_CLASSES
#include "circt/Dialect/RTL/RTL.cpp.inc"
