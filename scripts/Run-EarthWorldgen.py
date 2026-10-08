"""Run the upstream M4 CLI with its documented stock-bridge compatibility view.

An upstream wrapper attempts dataclasses.replace with M9-only fields on M4
specifications. Route only those older dataclasses through upstream's existing
read-through proxy; leave every supported M9 specification untouched.
"""
import dataclasses
import multiprocessing
from pathlib import Path
import sys

def main():
    sys.path.insert(0, str(Path(sys.argv.pop(1)).resolve() / "src"))
    from cwr_worldgen import bridge_render_policy, cli
    original = bridge_render_policy._stock_bridge_spec

    def compatible_stock_bridge_spec(spec):
        required = {"procedural_bridges", "bridge_module_length"}
        if dataclasses.is_dataclass(spec) and not required.issubset(spec.__dataclass_fields__):
            return bridge_render_policy._StockBridgeSpecProxy(spec)
        return original(spec)

    bridge_render_policy._stock_bridge_spec = compatible_stock_bridge_spec
    return cli.main()


if __name__ == "__main__":
    multiprocessing.freeze_support()
    raise SystemExit(main())
