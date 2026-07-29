import pathlib
import shutil
import subprocess

import pytest

TESTS = pathlib.Path(__file__).resolve().parent
ROOT = TESTS.parent.parent

@pytest.fixture(scope="session")
def fixtures_dir():
    # fixtures come from the c generator since committing binaries adds no value
    out = TESTS / "fixtures"
    if not (out / "empty.bin").exists():
        cc = shutil.which("cc")
        if cc is None:
            pytest.skip("fixture generation needs a C compiler")
        out.mkdir(exist_ok=True)
        gen = "/tmp/sm_gen_fixtures"
        srcs = [str(ROOT / "sm.c"), str(TESTS.parent / "ci" / "gen_fixtures.c")]
        subprocess.run([cc, "-O2", f"-I{ROOT}", *srcs, "-o", gen], check=True)
        subprocess.run([gen], check=True, cwd=TESTS.parent / "ci")
    return out
