"""RTM read/write (io_import_p3d/rtm_io.py): every RTM in the repo reads, and writes back to the same numbers."""

import glob
import os

import numpy as np
import pytest
from conftest import REPO

import rtm_io

RTMS = sorted(
    glob.glob(os.path.join(REPO, "tests", "fixtures", "rtm", "*.rtm"))
    + glob.glob(os.path.join(REPO, "content", "default-packs", "swim", "rtm", "*.rtm"))
)


@pytest.mark.parametrize("path", RTMS, ids=[os.path.basename(p) for p in RTMS])
def test_round_trip(path, tmp_path):
    a = rtm_io.read_rtm(path)
    out = tmp_path / "out.rtm"
    rtm_io.write_rtm(str(out), a["step"], a["names"], a["phases"])
    b = rtm_io.read_rtm(str(out))
    assert b["names"] == a["names"]
    assert np.allclose(b["step"], a["step"])
    assert len(b["phases"]) == len(a["phases"])
    for (ta, ma), (tb, mb) in zip(a["phases"], b["phases"]):
        assert tb == pytest.approx(ta)
        assert np.allclose(np.array(ma), np.array(mb))


def test_mat4_unmat():
    m = np.arange(12, dtype=float)
    assert np.allclose(rtm_io.unmat(rtm_io.mat4(m)), m)


def test_rejects_other_files(tmp_path):
    p = tmp_path / "x.rtm"
    p.write_bytes(b"NOT_RTM_" + bytes(32))
    with pytest.raises(ValueError):
        rtm_io.read_rtm(str(p))
