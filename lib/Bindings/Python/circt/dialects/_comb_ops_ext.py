from mlir.ir import OpView, StringAttr


class AddOp:

  def __init__(self,
               operands,
               *,
               name=None,
               loc=None,
               ip=None):
    results = []
    attributes = {}
    retType = operands[0].type
    results.append(retType)
    if name is not None:
      attributes["name"] = StringAttr.get(name)

    OpView.__init__(
        self,
        self.build_generic(
            attributes=attributes,
            results=results,
            operands=operands,
            loc=loc,
            ip=ip,
        ),
    )
