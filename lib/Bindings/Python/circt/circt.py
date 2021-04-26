import _circt
from mlir.ir import Context, Location, Module, InsertionPoint
from mlir.passmanager import PassManager


def build(MainClass):
  with Context() as ctx, Location.unknown():
    _circt.register_dialects(ctx)

    m = Module.create()
    buildMod = MainClass()
    with InsertionPoint(m.body):
      buildMod.main()

    passes = ["lower-seq-to-sv", "rtl-legalize-names",
              "rtl.module(rtl-cleanup)"]
    pm = PassManager.parse(",".join(passes))
    pm.run(m)
  return m
