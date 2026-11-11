"""Runs the report notebook's code against a real run, without needing Jupyter.

The notebook is the deliverable, so its cells are executed as they are: a cell that breaks
fails this test. Figures are drawn off-screen.
"""

from __future__ import annotations

import json
import os
from pathlib import Path

import matplotlib

matplotlib.use("Agg")

NOTEBOOK = Path(__file__).resolve().parent.parent / "report.ipynb"


def test_notebook_has_no_stored_outputs():
    # Outputs would put run results (and derived market data) into the repository.
    notebook = json.loads(NOTEBOOK.read_text())
    for cell in notebook["cells"]:
        if cell["cell_type"] == "code":
            assert cell["outputs"] == []
            assert cell["execution_count"] is None


def test_notebook_cells_run_against_a_real_run(synthetic_run, monkeypatch):
    import matplotlib.pyplot as plt

    monkeypatch.setenv("ITCHBT_RUN", str(synthetic_run["run"]))
    monkeypatch.chdir(NOTEBOOK.parent)
    shown = []
    displayed = []
    monkeypatch.setattr(plt, "show", lambda *a, **k: shown.append(plt.gcf().number))
    namespace = {"display": displayed.append, "__name__": "__notebook__"}

    notebook = json.loads(NOTEBOOK.read_text())
    code_cells = [c for c in notebook["cells"] if c["cell_type"] == "code"]
    assert len(code_cells) >= 6
    for index, cell in enumerate(code_cells):
        source = "".join(cell["source"])
        try:
            exec(compile(source, f"report.ipynb cell {index}", "exec"), namespace)  # noqa: S102
        finally:
            plt.close("all")

    # Every section drew something: total PnL, per-symbol PnL, time to fill, markouts,
    # queue position, and rejects (the synthetic run is configured to produce rate-limit
    # rejects).
    assert len(shown) >= 6
    assert len(displayed) >= 4
    assert namespace["run"].meta["config"]["strategy"] == "passive_maker"
    assert os.environ["ITCHBT_RUN"] == str(synthetic_run["run"])
