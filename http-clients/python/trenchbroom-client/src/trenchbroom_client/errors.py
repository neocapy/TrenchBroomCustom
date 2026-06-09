"""Exception hierarchy for the TrenchBroom HTTP client."""

from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .types import OpResult


class TrenchBroomError(Exception):
    """Base class for all errors raised by this library."""


class ConnectionFailed(TrenchBroomError):
    """Could not reach the TrenchBroom HTTP server."""


class ApiError(TrenchBroomError):
    """The server returned a non-2xx response.

    Attributes:
        status: HTTP status code (400 malformed, 404 unknown doc,
            409 doc missing / modal tool active, 422 operation failed).
        message: the server's ``error`` string.
    """

    def __init__(self, status: int, message: str, body: object = None) -> None:
        super().__init__(f"HTTP {status}: {message}")
        self.status = status
        self.message = message
        self.body = body  # parsed JSON response body, if any


class EditFailed(ApiError):
    """An atomic /edit batch failed and was rolled back.

    ``results`` holds one OpResult per op, in order. All handles in it
    are void (the transaction was rolled back).
    """

    def __init__(self, message: str, results: list[OpResult]) -> None:
        super().__init__(422, message, results)
        self.results = results
