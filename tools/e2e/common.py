"""JSON fetch and JSON-RPC over the UI test hooks, for the BLE setup test."""

from __future__ import annotations

from datetime import datetime, timezone
import json
from typing import Any
from urllib import request


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="milliseconds").replace(
        "+00:00", "Z"
    )


def fetch_json(url: str, timeout: float = 5.0) -> dict[str, Any]:
    with request.urlopen(url, timeout=timeout) as response:
        value = json.loads(response.read())
    if not isinstance(value, dict):
        raise RuntimeError(f"expected JSON object from {url}")
    return value


def call_rpc(client: Any, method: str, params: dict[str, Any]) -> Any:
    script = """
        const method = arguments[0];
        const params = arguments[1];
        const done = arguments[arguments.length - 1];
        const h = window.__kvmTestHooks;
        if (!h) return done({error: {message: "test hooks unavailable"}});
        h.sendJsonRpc(method, params, value => done(value), 10000);
    """
    return client.execute_async(script, [method, params])


def require_rpc_result(response: Any, method: str) -> Any:
    if not isinstance(response, dict) or "error" in response or "result" not in response:
        raise RuntimeError(f"{method} failed: {response!r}")
    return response["result"]
