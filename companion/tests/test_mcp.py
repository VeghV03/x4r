import asyncio
import json

import pytest
from mcp.server.mcpserver.exceptions import ToolError

from x4.mcp_server import server

TOOLS = {
    "status", "pull", "push_deck", "push_glossary", "push_book", "sync", "context",
    "ls", "get", "put", "rm", "screenshot", "press", "screen", "rss_sync", "font_add", "font_install", "dict_install",
    "words_install", "words_grids", "chess_install", "excerpt", "article", "forecasts", "settle", "shelf",
    "feed", "football", "stage",
}  # fmt: skip


def test_tool_listing():
    tools = {t.name: t for t in asyncio.run(server.list_tools())}
    assert set(tools) == TOOLS
    assert not {"backup", "flash", "serve"} & set(tools)
    assert all(t.description for t in tools.values())
    press = tools["press"].input_schema["properties"]
    assert press["button"]["enum"] == ["back", "confirm", "left", "right", "up", "down", "power"]
    assert tools["pull"].input_schema["properties"]["what"]["default"] == "all"


def _text(result) -> str:
    return result.content[0].text


def test_tools_against_simulator(sim, h, monkeypatch, tmp_path):
    monkeypatch.setenv("X4_PORT", sim.port)
    status = asyncio.run(server.call_tool("status", {}))
    assert json.loads(_text(status))["battery"] == 82
    shot = asyncio.run(server.call_tool("screenshot", {}))
    assert shot.content[0].type == "image" and shot.content[0].mime_type == "image/png"
    asyncio.run(server.call_tool("press", {"button": "down"}))
    assert sim.presses == [{"button": "down", "long": False}]


def test_tool_errors_are_readable(sim, h, monkeypatch):
    monkeypatch.setenv("X4_PORT", sim.port)
    with pytest.raises(ToolError) as e:
        asyncio.run(server.call_tool("get", {"remote": "/nope", "local": "/tmp/x"}))
    assert json.loads(str(e.value).split(": ", 1)[1]) == {"code": "not_found", "message": "get /nope: not_found: no such file"}
    with pytest.raises(ToolError, match="pass an absolute path"):
        asyncio.run(server.call_tool("push_deck", {"file": "relative.jsonl"}))
