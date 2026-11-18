"""Errors the CLI and MCP server turn into exit codes and messages."""


class X4Error(Exception):
    """Base error. `exit_code` is what the CLI exits with."""

    exit_code = 1
    code = "error"

    def __init__(self, message: str, details: list[str] | None = None):
        super().__init__(message)
        self.message = message
        self.details = details or []

    def __str__(self) -> str:
        return "\n".join([self.message, *self.details])

    def to_json(self) -> dict:
        out: dict = {"code": self.code, "message": self.message}
        if self.details:
            out["details"] = self.details
        return out


class DeviceError(X4Error):
    """The device replied `err`, timed out, or the port failed."""

    def __init__(self, code: str, message: str, replied: bool = False):
        super().__init__(message)
        self.code = code
        self.replied = replied  # the device's own `err` reply, so the request is over on its side


class Refused(X4Error):
    """The command would change a file the device writes."""

    code = "refused"


class InvalidData(X4Error):
    """Local data failed validation. Each detail names file, line and field."""

    exit_code = 2
    code = "invalid"
    shown = 30

    def __init__(self, message: str, details: list[str] | None = None):
        details = details or []
        if len(details) > self.shown:
            details = details[: self.shown] + [f"... and {len(details) - self.shown} more"]
        super().__init__(message, details)
