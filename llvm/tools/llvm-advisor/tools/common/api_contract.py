from __future__ import annotations

from dataclasses import asdict, dataclass, field
from datetime import datetime, timezone
from typing import Any, Dict, Optional


def utc_timestamp() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds").replace(
        "+00:00", "Z"
    )


@dataclass(frozen=True)
class APIErrorDetails:
    code: str
    message: str
    details: Dict[str, Any] = field(default_factory=dict)


@dataclass(frozen=True)
class ResponsePage:
    limit: Optional[int] = None
    next_cursor: Optional[str] = None
    total_estimate: Optional[int] = None


@dataclass(frozen=True)
class ResponseMetadata:
    request_id: str
    timestamp: str
    page: Optional[ResponsePage] = None

    def to_dict(self) -> Dict[str, Any]:
        payload = {
            "request_id": self.request_id,
            "timestamp": self.timestamp,
            "page": asdict(self.page) if self.page is not None else None,
        }
        return payload


def build_success_envelope(
    *,
    request_id: str,
    data: Any,
    page: Optional[ResponsePage] = None,
    status: int = 200,
) -> Dict[str, Any]:
    return {
        "success": True,
        "data": data,
        "error": None,
        "meta": ResponseMetadata(
            request_id=request_id, timestamp=utc_timestamp(), page=page
        ).to_dict(),
        "status": status,
    }


def build_error_envelope(
    *,
    request_id: str,
    code: str,
    message: str,
    details: Optional[Dict[str, Any]] = None,
    status: int = 400,
) -> Dict[str, Any]:
    error = APIErrorDetails(code=code, message=message, details=details or {})
    return {
        "success": False,
        "data": None,
        "error": asdict(error),
        "meta": ResponseMetadata(
            request_id=request_id,
            timestamp=utc_timestamp(),
        ).to_dict(),
        "status": status,
    }
