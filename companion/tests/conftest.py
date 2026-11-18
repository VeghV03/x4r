import pytest
from click.testing import CliRunner

from x4.cli import main
from x4.device import connect
from x4.home import home
from x4.sim import Sim, serve_pty


@pytest.fixture
def sim(tmp_path):
    s = Sim(tmp_path / "sd")
    port, stop = serve_pty(s)
    s.port = port
    yield s
    stop.set()


@pytest.fixture
def h(tmp_path, monkeypatch):
    monkeypatch.setenv("X4_HOME", str(tmp_path / "home"))
    return home()


@pytest.fixture
def dev(sim):
    with connect(sim.port) as d:
        yield d


@pytest.fixture
def x4(sim, h, monkeypatch):
    """Run the CLI against the simulator: x4("status", "--json") -> click Result."""
    monkeypatch.setenv("X4_PORT", sim.port)
    runner = CliRunner()

    def run(*args: str, input: str | None = None):
        return runner.invoke(main, list(args), input=input, catch_exceptions=False)

    return run
