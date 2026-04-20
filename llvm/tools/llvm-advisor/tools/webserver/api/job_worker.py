from __future__ import annotations

import threading
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from .service import APIService


class JobWorker:
    def __init__(self, service: "APIService") -> None:
        self._service = service
        self._wake_event = threading.Event()
        self._stop_event = threading.Event()
        self._thread = threading.Thread(
            target=self._run,
            name="llvm-advisor-job-worker",
            daemon=True,
        )
        self._thread.start()

    def wake(self) -> None:
        self._wake_event.set()

    def stop(self) -> None:
        self._stop_event.set()
        self._wake_event.set()
        self._thread.join(timeout=1.0)

    def _run(self) -> None:
        while not self._stop_event.is_set():
            self._service.requeue_stale_jobs()
            job_record = self._service.claim_next_job()
            if job_record is None:
                self._wake_event.wait(timeout=1.0)
                self._wake_event.clear()
                continue
            self._service.execute_job(job_record)
