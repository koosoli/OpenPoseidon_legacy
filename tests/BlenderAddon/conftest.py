"""pytest setup for the Blender add-on's pure-Python parts (no Blender needed).

--dll points at a PoseidonFormats shared library (the Makefile passes it); without one, or with an older one that
cannot write, the tests that need it are skipped.
"""

import os
import sys

import pytest

ADDON = os.path.abspath(
    os.path.join(os.path.dirname(__file__), "..", "..", "apps", "tools", "BlenderAddon", "io_import_p3d")
)
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
if ADDON not in sys.path:
    sys.path.insert(0, ADDON)


def pytest_addoption(parser):
    parser.addoption(
        "--dll",
        default=os.environ.get("POSEIDON_FORMATS_DLL", ""),
        help="PoseidonFormats shared library (for the MLOD/PAA writer tests)",
    )


@pytest.fixture(scope="session")
def p3d(request):
    dll = request.config.getoption("--dll")
    if not dll or not os.path.isfile(dll):
        pytest.skip("no PoseidonFormats library (--dll)")
    os.environ["POSEIDON_FORMATS_DLL"] = dll
    import p3d_lib

    p3d_lib.load()
    if not p3d_lib.can_write():
        pytest.skip("this PoseidonFormats library has no writers")
    return p3d_lib
