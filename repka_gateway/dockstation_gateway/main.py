"""Command-line entry point for the DockStation Linux diagnostic gateway."""

from __future__ import annotations

import argparse
import os
import signal
import sys
import threading

from . import __version__
from .diagnostics import collect_diagnostics
from .server import create_server


def parse_arguments(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run the temporary DockStation diagnostic API on loopback.")
    parser.add_argument("--port", type=int, default=8080, help="HTTP port on 127.0.0.1 (default: 8080)")
    parser.add_argument(
        "--dns-name",
        default=os.environ.get("DOCKSTATION_DNS_CHECK_NAME", ""),
        help="Optional DNS name to resolve in diagnostics (or set DOCKSTATION_DNS_CHECK_NAME)",
    )
    parser.add_argument(
        "--enable-test-adapter",
        action="store_true",
        help="Explicitly enable the non-physical VM test adapter route (disabled by default)",
    )
    parser.add_argument("--version", action="version", version=f"DockStation Gateway {__version__}")
    args = parser.parse_args(argv)
    if not 1 <= args.port <= 65535:
        parser.error("--port must be between 1 and 65535")
    return args


def run_gateway(
    port: int,
    dns_name: str | None = None,
    enable_test_adapter: bool = False,
) -> int:
    provider = lambda: collect_diagnostics(dns_name or None)
    test_request_service = None
    if enable_test_adapter:
        from .adapters.test_request_adapter import TestRequestAdapter
        from .application.test_request_service import TestRequestService

        test_request_service = TestRequestService(TestRequestAdapter())

    server = create_server(
        port=port,
        diagnostics_provider=provider,
        test_request_service=test_request_service,
    )
    stop_requested = threading.Event()
    serve_thread = threading.Thread(target=server.serve_forever, name="dockstation-http", daemon=True)
    previous_handlers: dict[int, object] = {}

    def request_stop(signum: int, _frame: object) -> None:
        del signum
        stop_requested.set()

    for signum in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[signum] = signal.signal(signum, request_stop)

    try:
        serve_thread.start()
        host, bound_port = server.server_address
        print(f"DockStation Gateway {__version__} listening on http://{host}:{bound_port}", flush=True)
        print("Read-only routes: GET /api/v1/health, GET /api/v1/system/diagnostics", flush=True)
        if enable_test_adapter:
            print("VM test-only route enabled: POST /api/v1/test/adapter/request (PING only)", flush=True)
        while not stop_requested.wait(0.25):
            if not serve_thread.is_alive():
                print("Gateway HTTP thread stopped unexpectedly", file=sys.stderr, flush=True)
                return 1
        return 0
    finally:
        if serve_thread.is_alive():
            server.shutdown()
        server.server_close()
        if serve_thread.is_alive():
            serve_thread.join(timeout=3.0)
        for signum, previous in previous_handlers.items():
            signal.signal(signum, previous)  # type: ignore[arg-type]
        print("DockStation Gateway stopped", flush=True)


def main(argv: list[str] | None = None) -> int:
    args = parse_arguments(argv)
    try:
        return run_gateway(args.port, args.dns_name, args.enable_test_adapter)
    except OSError as exc:
        print(f"Cannot start DockStation Gateway: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
