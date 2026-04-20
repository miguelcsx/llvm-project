# ===----------------------------------------------------------------------===//
#
# Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
# See https://llvm.org/LICENSE.txt for license information.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# ===----------------------------------------------------------------------===//

#!/usr/bin/env python3

import argparse
import json
import logging
import mimetypes
import os
import sys
import uuid
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, Callable, Dict, List, Mapping, Optional, Sequence, Tuple
from urllib.parse import parse_qs, urlparse

from .api.service import APIService

LOGGER = logging.getLogger(__name__)
CURRENT_DIR = Path(__file__).resolve().parent

CORS_ALLOW_ORIGIN = "*"
CORS_ALLOW_METHODS = "GET, POST, OPTIONS"
CORS_ALLOW_HEADERS = "Content-Type"


@dataclass(frozen=True)
class FrontendPaths:
    root: Path
    static: Path
    templates: Path

    @classmethod
    def from_server_dir(cls, server_dir: Path) -> "FrontendPaths":
        frontend_root = server_dir / "frontend"
        return cls(
            root=frontend_root,
            static=frontend_root / "static",
            templates=frontend_root / "templates",
        )


@dataclass(frozen=True)
class ServerConfig:
    data_dir: Path
    host: str
    port: int
    frontend: FrontendPaths

    @classmethod
    def from_args(cls, args: argparse.Namespace) -> "ServerConfig":
        return cls(
            data_dir=Path(args.data_dir).expanduser().resolve(),
            host=args.host,
            port=args.port,
            frontend=FrontendPaths.from_server_dir(CURRENT_DIR),
        )


@dataclass(frozen=True)
class RuntimeMetadata:
    pid: int
    host: str
    port: int
    data_dir: str
    started_at: str

    def to_dict(self) -> Dict[str, Any]:
        return {
            "pid": self.pid,
            "host": self.host,
            "port": self.port,
            "data_dir": self.data_dir,
            "started_at": self.started_at,
            "url": f"http://{self.host}:{self.port}",
        }


@dataclass(frozen=True)
class RequestContext:
    raw_path: str
    path: str
    path_parts: List[str]
    query_params: Dict[str, List[str]]

    @classmethod
    def from_raw_path(cls, raw_path: str) -> "RequestContext":
        parsed_url = urlparse(raw_path)
        normalized_path = parsed_url.path.rstrip("/")
        return cls(
            raw_path=raw_path,
            path=normalized_path,
            path_parts=[part for part in normalized_path.strip("/").split("/") if part],
            query_params=parse_qs(parsed_url.query),
        )


@dataclass(frozen=True)
class Route:
    predicate: Callable[[RequestContext], bool]
    handler: Callable[["APIHandler", RequestContext], Optional[Dict[str, Any]]]


class APIServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(
        self,
        server_address: Tuple[str, int],
        request_handler_class,
        config: ServerConfig,
    ) -> None:
        super().__init__(server_address, request_handler_class)
        self.config = config
        self.api_service = APIService(
            self.config.data_dir,
            CURRENT_DIR.parent.parent / "config",
        )
        self.runtime_metadata = RuntimeMetadata(
            pid=os.getpid(),
            host=self.config.host,
            port=self.config.port,
            data_dir=str(self.config.data_dir),
            started_at=self.api_service.handle_health("bootstrap")["meta"]["timestamp"],
        )
        self._write_runtime_metadata()

    def _runtime_metadata_path(self) -> Path:
        return (
            self.config.data_dir
            / ".llvm-advisor-store"
            / "runtime"
            / "server.json"
        )

    def _write_runtime_metadata(self) -> None:
        runtime_path = self._runtime_metadata_path()
        runtime_path.parent.mkdir(parents=True, exist_ok=True)
        temp_path = runtime_path.with_suffix(".tmp")
        temp_path.write_text(
            json.dumps(self.runtime_metadata.to_dict(), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        os.replace(temp_path, runtime_path)

    def clear_runtime_metadata(self) -> None:
        runtime_path = self._runtime_metadata_path()
        if not runtime_path.exists():
            return
        try:
            payload = json.loads(runtime_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            payload = {}
        if int(payload.get("pid", -1)) != self.runtime_metadata.pid:
            return
        try:
            runtime_path.unlink()
        except OSError:
            LOGGER.warning("Failed to remove runtime metadata: %s", runtime_path)


class APIHandler(BaseHTTPRequestHandler):
    server_version = "LLVMAdvisorHTTP/1.0"

    def __init__(self, *args, **kwargs):
        self.config: ServerConfig
        self.frontend: FrontendPaths
        self.api_service: APIService
        self._request_id = ""
        super().__init__(*args, **kwargs)

    @property
    def app(self) -> APIServer:
        return self.server  # type: ignore[return-value]

    def setup(self) -> None:
        super().setup()
        self.config = self.app.config
        self.frontend = self.config.frontend
        self.api_service = self.app.api_service

    def log_message(self, format: str, *args: Any) -> None:
        LOGGER.info("%s - - %s", self.address_string(), format % args)

    def do_OPTIONS(self) -> None:
        self.send_response(200)
        self._send_cors_headers()
        self.end_headers()

    def do_GET(self) -> None:
        try:
            self._request_id = self._new_request_id()
            request = RequestContext.from_raw_path(self.path)
            response = self._dispatch(request)
            if response is not None:
                self._send_json_response(response)
        except BrokenPipeError:
            LOGGER.warning("Client disconnected while writing response")
        except ConnectionResetError:
            LOGGER.warning("Client connection reset during request handling")
        except Exception as exc:
            LOGGER.exception("Unhandled server error")
            self._send_error(f"Internal server error: {exc}", status=500)

    def do_POST(self) -> None:
        try:
            self._request_id = self._new_request_id()
            request = RequestContext.from_raw_path(self.path)
            response = self._dispatch_post(request)
            self._send_json_response(response)
        except BrokenPipeError:
            LOGGER.warning("Client disconnected while writing response")
        except ConnectionResetError:
            LOGGER.warning("Client connection reset during request handling")
        except Exception as exc:
            LOGGER.exception("Unhandled server error")
            self._send_error(f"Internal server error: {exc}", status=500)

    def _dispatch(self, request: RequestContext) -> Optional[Dict[str, Any]]:
        for route in self._routes():
            if route.predicate(request):
                return route.handler(self, request)
        return {
            "success": False,
            "error": "Endpoint not found",
            "status": 404,
            "available_endpoints": self._get_available_endpoints(),
        }

    def _dispatch_post(self, request: RequestContext) -> Dict[str, Any]:
        payload = self._read_json_payload()
        if request.path == "/api/snapshots":
            return self.api_service.handle_create_snapshot(self._request_id, payload)
        if (
            request.path.startswith("/api/snapshots/")
            and len(request.path_parts) == 4
            and request.path_parts[3] == "representation-requests"
        ):
            return self.api_service.handle_snapshot_representation_request(
                self._request_id,
                request.path_parts[2],
                payload,
            )
        if request.path == "/api/query":
            return self.api_service.handle_query(self._request_id, payload)
        if request.path == "/api/jobs":
            return self.api_service.handle_create_job(self._request_id, payload)
        if request.path.startswith("/api/jobs/") and request.path.endswith("/cancel"):
            job_id = request.path_parts[2] if len(request.path_parts) >= 4 else ""
            return self.api_service.handle_cancel_job(self._request_id, job_id)
        if request.path == "/api/compare":
            return self.api_service.handle_compare(self._request_id, payload)
        return {
            "success": False,
            "error": "Endpoint not found",
            "status": 404,
            "available_endpoints": self._get_available_endpoints(),
        }

    def _routes(self) -> Sequence[Route]:
        return (
            Route(lambda req: req.path in {"", "/"}, APIHandler._serve_index),
            Route(lambda req: req.path.startswith("/static/"), APIHandler._serve_static_asset),
            Route(lambda req: req.path in {"/api", "/api/"}, APIHandler._serve_api_docs),
            Route(lambda req: req.path == "/api/health", APIHandler._handle_api_health),
            Route(lambda req: req.path == "/api/capabilities", APIHandler._handle_api_capabilities),
            Route(
                lambda req: req.path == "/api/representation-capabilities",
                APIHandler._handle_api_representation_capabilities,
            ),
            Route(lambda req: req.path == "/api/snapshots", APIHandler._handle_api_snapshots),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 3,
                APIHandler._handle_api_snapshot_detail,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "health",
                APIHandler._handle_snapshot_health,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "summary",
                APIHandler._handle_snapshot_summary,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "units",
                APIHandler._handle_snapshot_units,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) >= 5
                and req.path_parts[3] == "units",
                APIHandler._handle_snapshot_unit_detail,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "representations",
                APIHandler._handle_snapshot_representation_types,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 5
                and req.path_parts[3] == "representations",
                APIHandler._handle_snapshot_representation_data,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "representation-query",
                APIHandler._handle_snapshot_representation_query,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) == 4
                and req.path_parts[3] == "files",
                APIHandler._handle_snapshot_files,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) >= 5
                and req.path_parts[3] == "source",
                APIHandler._handle_snapshot_source,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) >= 6
                and req.path_parts[3] == "explorer",
                APIHandler._handle_snapshot_explorer_representation,
            ),
            Route(
                lambda req: req.path.startswith("/api/snapshots/")
                and len(req.path_parts) >= 6
                and req.path_parts[3] == "analysis",
                APIHandler._handle_snapshot_analysis,
            ),
            Route(lambda req: req.path == "/api/jobs", APIHandler._handle_api_jobs),
            Route(
                lambda req: req.path.startswith("/api/jobs/")
                and len(req.path_parts) >= 3,
                APIHandler._handle_api_job_detail,
            ),
        )

    def _handle_api_health(self, request: RequestContext) -> Dict[str, Any]:
        del request
        return self.api_service.handle_health(self._request_id)

    def _handle_api_capabilities(self, request: RequestContext) -> Dict[str, Any]:
        del request
        return self.api_service.handle_capabilities(self._request_id)

    def _handle_api_representation_capabilities(
        self, request: RequestContext
    ) -> Dict[str, Any]:
        del request
        return self.api_service.handle_representation_capabilities(
            self._request_id
        )

    def _handle_api_snapshots(self, request: RequestContext) -> Dict[str, Any]:
        del request
        return self.api_service.handle_list_snapshots(self._request_id)

    def _handle_api_snapshot_detail(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_get_snapshot(self._request_id, request.path_parts[2])

    def _handle_snapshot_health(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_health(self._request_id, request.path_parts[2])

    def _handle_snapshot_summary(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_summary(self._request_id, request.path_parts[2])

    def _handle_snapshot_units(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_units(self._request_id, request.path_parts[2])

    def _handle_snapshot_unit_detail(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_unit_detail(
            self._request_id,
            request.path_parts[2],
            request.path_parts[4],
        )

    def _handle_snapshot_representation_types(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_representation_types(
            self._request_id, request.path_parts[2]
        )

    def _handle_snapshot_representation_data(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_representation_data(
            self._request_id,
            request.path_parts[2],
            request.path_parts[4],
        )

    def _handle_snapshot_representation_query(
        self, request: RequestContext
    ) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_representation_query(
            self._request_id,
            request.path_parts[2],
            request.query_params,
        )

    def _handle_snapshot_files(self, request: RequestContext) -> Dict[str, Any]:
        unit_name = request.query_params.get("unit", [None])[0]
        return self.api_service.handle_snapshot_files(
            self._request_id,
            request.path_parts[2],
            unit_name,
        )

    def _handle_snapshot_source(self, request: RequestContext) -> Dict[str, Any]:
        file_path = "/".join(request.path_parts[4:])
        return self.api_service.handle_snapshot_source(
            self._request_id,
            request.path_parts[2],
            file_path,
            request.query_params.get("unit", [None])[0],
        )

    def _handle_snapshot_explorer_representation(
        self, request: RequestContext
    ) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_explorer_representation(
            self._request_id,
            request.path_parts[2],
            request.path_parts[4],
            "/".join(request.path_parts[5:]),
            request.query_params.get("unit", [None])[0],
        )

    def _handle_snapshot_analysis(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_snapshot_analysis(
            self._request_id,
            request.path_parts[2],
            request.path_parts[4],
            request.path_parts[5] if len(request.path_parts) >= 6 else "overview",
            request.query_params,
        )

    def _handle_api_jobs(self, request: RequestContext) -> Dict[str, Any]:
        del request
        return self.api_service.handle_list_jobs(self._request_id)

    def _handle_api_job_detail(self, request: RequestContext) -> Dict[str, Any]:
        return self.api_service.handle_get_job(self._request_id, request.path_parts[2])

    def _serve_index(self, request: RequestContext) -> Optional[Dict[str, Any]]:
        del request
        self._send_file_response(
            self.frontend.templates / "index.html",
            content_type="text/html; charset=utf-8",
        )
        return None

    def _serve_static_asset(self, request: RequestContext) -> Optional[Dict[str, Any]]:
        target_path = self._resolve_safe_path(
            self.frontend.static,
            request.path[len("/static/") :],
        )
        if target_path is None:
            self._send_error("Invalid static file path", status=400)
            return None
        content_type, _ = mimetypes.guess_type(str(target_path))
        self._send_file_response(
            target_path,
            content_type=content_type or "application/octet-stream",
        )
        return None

    def _serve_api_docs(self, request: RequestContext) -> Dict[str, Any]:
        del request
        return {
            "success": True,
            "data": {
                "description": "Canonical API for LLVM Advisor snapshots, query, compare, and jobs",
                "data_directory": str(self.config.data_dir),
                "endpoints": self._get_available_endpoints(),
            },
            "status": 200,
        }

    def _send_json_response(self, payload: Mapping[str, Any]) -> None:
        status = int(payload.get("status", 200))
        body = json.dumps(payload, indent=2, default=str).encode("utf-8")
        self.send_response(status)
        self._send_cors_headers()
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def _send_error(self, message: str, status: int = 500) -> None:
        self._send_json_response({"success": False, "error": message, "status": status})

    def _read_json_payload(self) -> Dict[str, Any]:
        content_length = int(self.headers.get("Content-Length", "0"))
        if content_length <= 0:
            return {}
        raw_body = self.rfile.read(content_length)
        if not raw_body:
            return {}
        payload = json.loads(raw_body.decode("utf-8"))
        return payload if isinstance(payload, dict) else {}

    def _send_file_response(self, file_path: Path, content_type: str) -> None:
        try:
            resolved_path = file_path.resolve(strict=True)
        except FileNotFoundError:
            self._send_error("File not found", status=404)
            return
        except OSError as exc:
            self._send_error(f"Error resolving file: {exc}", status=500)
            return

        try:
            content = resolved_path.read_bytes()
        except OSError as exc:
            self._send_error(f"Error serving file: {exc}", status=500)
            return

        self.send_response(200)
        self._send_cors_headers()
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def _send_cors_headers(self) -> None:
        self.send_header("Access-Control-Allow-Origin", CORS_ALLOW_ORIGIN)
        self.send_header("Access-Control-Allow-Methods", CORS_ALLOW_METHODS)
        self.send_header("Access-Control-Allow-Headers", CORS_ALLOW_HEADERS)

    def _resolve_safe_path(self, root: Path, relative_path: str) -> Optional[Path]:
        candidate = (root / relative_path).resolve()
        try:
            candidate.relative_to(root.resolve())
        except ValueError:
            return None
        return candidate

    def _get_available_endpoints(self) -> Dict[str, Any]:
        return {
            "GET /api/health": "Service health",
            "GET /api/capabilities": "Capability registry",
            "GET /api/representation-capabilities": "Representation capability registry",
            "GET /api/snapshots": "List snapshots",
            "POST /api/snapshots": "Create snapshot metadata",
            "GET /api/snapshots/{snapshot_id}": "Snapshot detail",
            "GET /api/snapshots/{snapshot_id}/health": "Snapshot health",
            "GET /api/snapshots/{snapshot_id}/summary": "Snapshot summary",
            "GET /api/snapshots/{snapshot_id}/units": "Snapshot units",
            "GET /api/snapshots/{snapshot_id}/units/{unit_name}": "Snapshot unit detail",
            "GET /api/snapshots/{snapshot_id}/representations": "Snapshot representation kinds",
            "GET /api/snapshots/{snapshot_id}/representations/{representation_kind}": "Snapshot representation data",
            "GET /api/snapshots/{snapshot_id}/representation-query?unit={unit_name}&source={source_ref}&kind={representation_kind}": "Indexed representation catalog query",
            "POST /api/snapshots/{snapshot_id}/representation-requests": "Declarative representation request planning",
            "GET /api/snapshots/{snapshot_id}/files?unit={unit_name}": "Snapshot explorer files",
            "GET /api/snapshots/{snapshot_id}/source/{file_path}?unit={unit_name}": "Snapshot source text",
            "GET /api/snapshots/{snapshot_id}/explorer/{representation_kind}/{file_path}?unit={unit_name}": "Snapshot representation text",
            "GET /api/snapshots/{snapshot_id}/analysis/{analysis_type}/{sub_endpoint}": "Snapshot analysis views",
            "POST /api/query": "Materialize capability outputs",
            "GET /api/jobs": "List jobs",
            "POST /api/jobs": "Create job",
            "GET /api/jobs/{job_id}": "Job detail",
            "POST /api/jobs/{job_id}/cancel": "Cancel job",
            "POST /api/compare": "Compare snapshots",
        }

    def _new_request_id(self) -> str:
        return f"req_{uuid.uuid4().hex}"


def parse_args(argv: Optional[Sequence[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="LLVM Advisor API Server")
    parser.add_argument("--data-dir", required=True, help="Directory containing .llvm-advisor data")
    parser.add_argument("--port", type=int, default=8000, help="Port to listen on")
    parser.add_argument("--host", default="localhost", help="Host to bind to")
    parser.add_argument(
        "--log-level",
        default="INFO",
        choices=("DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"),
        help="Logging verbosity",
    )
    return parser.parse_args(argv)


def configure_logging(level: str) -> None:
    effective_level = getattr(logging, level.upper(), logging.INFO)
    logging.basicConfig(
        level=effective_level,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    if effective_level > logging.INFO:
        logging.getLogger("http.server").setLevel(effective_level)


def validate_data_dir(data_dir: Path) -> None:
    if not data_dir.exists():
        raise FileNotFoundError(f"Data directory does not exist: {data_dir}")
    if not data_dir.is_dir():
        raise NotADirectoryError(f"Data directory is not a directory: {data_dir}")


def build_server(config: ServerConfig) -> APIServer:
    return APIServer((config.host, config.port), APIHandler, config)


def run_server(server: APIServer) -> None:
    LOGGER.info(
        "Starting web server on http://%s:%s",
        server.config.host,
        server.config.port,
    )
    server.serve_forever()


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = parse_args(argv)
    configure_logging(args.log_level)
    config = ServerConfig.from_args(args)
    validate_data_dir(config.data_dir)

    try:
        server = build_server(config)
    except Exception as exc:
        LOGGER.exception("Failed to build web server")
        print(f"error: {exc}", file=sys.stderr)
        return 1

    try:
        run_server(server)
    except KeyboardInterrupt:
        LOGGER.info("Stopping web server")
    finally:
        server.clear_runtime_metadata()
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
