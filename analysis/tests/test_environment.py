"""Checks that the pinned analysis environment installs and imports.

Real tests arrive with the reference book (M2) and the run loader (M5).
"""

import sys


def test_python_version():
    assert sys.version_info >= (3, 11)


def test_pinned_dependencies_import():
    import matplotlib
    import numpy
    import pandas
    import pyarrow

    assert numpy.__version__.split(".")[0] == "2"
    assert pandas.__version__.split(".")[0] == "2"
    assert pyarrow.__version__
    assert matplotlib.__version__
