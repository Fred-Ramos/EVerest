# SPDX-License-Identifier: Apache-2.0
# Copyright Pionix GmbH and Contributors to EVerest

import asyncio
import io
import logging
import os
import tarfile
import time
from copy import deepcopy
from datetime import datetime, timedelta, timezone
from pathlib import Path

import pytest
from everest.testing.core_utils._configuration.everest_configuration_strategies.everest_configuration_strategy import (
    EverestConfigAdjustmentStrategy,
)
from everest.testing.ocpp_utils.charge_point_utils import (
    TestUtility,
    wait_for_and_validate,
)
from everest.testing.ocpp_utils.charge_point_v201 import ChargePoint201
from everest.testing.ocpp_utils.fixtures import central_system_v201, charge_point_v201
from ocpp.messages import MessageType, unpack
from ocpp.v201.datatypes import LogParametersType
from ocpp.v201.enums import LogEnumType, LogStatusEnumType

STATUS_TIMEOUT_S = 10

UPLOAD_OK = b"HTTP/1.1 200 OK\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"

# Written before EVerest starts, as {name: (content, age of the last write in minutes)}
DIAGNOSTICS_LOGS = {
    "everest.log.1": (b"older entries\n", 120),
    "everest.log": (b"newer entries\n", 60),
    "unrelated.txt": (b"not a log\n", 60),
}
SECURITY_LOGS = {
    "libocpp_201.security.log": (b"security events\n", 30),
    "libocpp_201.log": (b"ocpp messages\n", 30),
}


class SystemLogDirectoriesStrategy(EverestConfigAdjustmentStrategy):
    """Fills a diagnostics and a security log directory next to the test's OCPP message logs and points the System
    module at them."""

    def adjust_everest_configuration(self, everest_config: dict) -> dict:
        adjusted = deepcopy(everest_config)
        modules = adjusted["active_modules"]
        # everest-testing sets a per-test MessageLogPath before everest_config_adaptions
        message_log_path = next(
            module["config_module"]["MessageLogPath"]
            for module in modules.values()
            if module.get("module") == "OCPPmulti"
        )
        base = Path(message_log_path).parent.parent
        now = time.time()
        directories = {}
        for name, files in (("diagnostics_logs", DIAGNOSTICS_LOGS), ("security_logs", SECURITY_LOGS)):
            directory = base / name
            directory.mkdir(exist_ok=True)
            for file_name, (content, age_minutes) in files.items():
                (directory / file_name).write_bytes(content)
                written = now - age_minutes * 60
                os.utime(directory / file_name, (written, written))
            directories[name] = str(directory)
        modules["system"].setdefault("config_module", {}).update(
            {
                "DiagnosticsLogDirectory": directories["diagnostics_logs"],
                "DiagnosticsLogFileFilter": "everest.log",
                "SecurityLogDirectory": directories["security_logs"],
            }
        )
        return adjusted


async def start_upload_server(uploads: asyncio.Queue):
    """An HTTP server that answers every upload with 200 OK and queues (path, body) of each."""

    async def handle_upload(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        head = await reader.readuntil(b"\r\n\r\n")
        request_line, *header_lines = head.decode().split("\r\n")
        headers = {k.strip().lower(): v.strip() for k, _, v in (line.partition(":") for line in header_lines if line)}
        if headers.get("expect", "").lower() == "100-continue":
            writer.write(b"HTTP/1.1 100 Continue\r\n\r\n")
            await writer.drain()
        body = await reader.readexactly(int(headers.get("content-length", "0")))
        await uploads.put((request_line.split(" ")[1], body))
        writer.write(UPLOAD_OK)
        await writer.drain()
        writer.close()

    return await asyncio.start_server(handle_upload, "127.0.0.1", 0)


def archive_contents(body: bytes) -> dict:
    with tarfile.open(fileobj=io.BytesIO(body), mode="r:gz") as archive:
        return {member.name: archive.extractfile(member).read() for member in archive.getmembers()}


def minutes_ago(minutes: int) -> str:
    return (datetime.now(timezone.utc) - timedelta(minutes=minutes)).isoformat()


@pytest.mark.asyncio
@pytest.mark.ocpp_version("ocpp2.0.1")
@pytest.mark.everest_core_config("everest-config-ocpp201.yaml")
async def test_N01_get_log_cancels_running_upload(
    charge_point_v201: ChargePoint201,
    test_utility: TestUtility,
):
    """N01.FR.20: a second GetLog cancels the running upload, which then reports only AcceptedCanceled."""
    logging.info("######### test_N01_get_log_cancels_running_upload #########")

    first_upload_connected = asyncio.Event()
    release_first_upload = asyncio.Event()
    connections = 0

    async def handle_upload(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        nonlocal connections
        connections += 1
        is_first = connections == 1
        await reader.read(65536)
        if is_first:
            first_upload_connected.set()
            await release_first_upload.wait()
        writer.write(UPLOAD_OK)
        await writer.drain()
        writer.close()

    server = await asyncio.start_server(handle_upload, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    log = LogParametersType(remote_location=f"http://127.0.0.1:{port}/diagnostics")

    try:
        r = await charge_point_v201.get_log_req(log=log, log_type=LogEnumType.diagnostics_log, request_id=1)
        assert LogStatusEnumType(r.status) == LogStatusEnumType.accepted
        assert await wait_for_and_validate(
            test_utility, charge_point_v201, "LogStatusNotification", {"status": "Uploading", "requestId": 1}
        )
        # Uploading is reported before the uploader's curl connects. Cancel only once it holds the first
        # connection, otherwise the second upload would take it and be held instead.
        await asyncio.wait_for(first_upload_connected.wait(), timeout=STATUS_TIMEOUT_S)

        history = charge_point_v201.message_history.messages
        cancel_index = len(history)
        r = await charge_point_v201.get_log_req(log=log, log_type=LogEnumType.diagnostics_log, request_id=2)
        assert LogStatusEnumType(r.status) == LogStatusEnumType.accepted_canceled

        assert await wait_for_and_validate(
            test_utility,
            charge_point_v201,
            "LogStatusNotification",
            {"status": "AcceptedCanceled", "requestId": 1},
            timeout=STATUS_TIMEOUT_S,
        )
        assert await wait_for_and_validate(
            test_utility,
            charge_point_v201,
            "LogStatusNotification",
            {"status": "Uploading", "requestId": 2},
            timeout=STATUS_TIMEOUT_S,
        )

        received = [unpack(m.message) for m in history[cancel_index:] if m.initiator == "Chargepoint"]
        cancelled_request_statuses = [
            msg.payload["status"]
            for msg in received
            if msg.message_type_id == MessageType.Call
            and msg.action == "LogStatusNotification"
            and msg.payload["requestId"] == 1
        ]
        assert cancelled_request_statuses == ["AcceptedCanceled"]

        assert await wait_for_and_validate(
            test_utility,
            charge_point_v201,
            "LogStatusNotification",
            {"status": "Uploaded", "requestId": 2},
            timeout=STATUS_TIMEOUT_S,
        )
    finally:
        release_first_upload.set()
        server.close()
        await server.wait_closed()


@pytest.mark.asyncio
@pytest.mark.ocpp_version("ocpp2.0.1")
@pytest.mark.everest_core_config("everest-config-ocpp201.yaml")
async def test_N01_get_log_rejects_option_like_location(
    charge_point_v201: ChargePoint201,
    test_utility: TestUtility,
):
    """A remote location starting with '-' would be parsed as an option by the uploader, so it is rejected."""
    logging.info("######### test_N01_get_log_rejects_option_like_location #########")

    log = LogParametersType(remote_location="-K/dev/null")
    r = await charge_point_v201.get_log_req(log=log, log_type=LogEnumType.diagnostics_log, request_id=1)
    assert LogStatusEnumType(r.status) == LogStatusEnumType.rejected

    assert not await wait_for_and_validate(
        test_utility, charge_point_v201, "LogStatusNotification", {"requestId": 1}, timeout=5
    )


async def get_log_and_wait_for_upload(
    charge_point_v201, test_utility, uploads, port, log_type, request_id, **log_params
):
    """Sends a GetLog, checks it is accepted and uploaded, and returns the file name and the uploaded archive."""
    log = LogParametersType(remote_location=f"http://127.0.0.1:{port}/logs/", **log_params)
    r = await charge_point_v201.get_log_req(log=log, log_type=log_type, request_id=request_id)
    assert LogStatusEnumType(r.status) == LogStatusEnumType.accepted
    assert await wait_for_and_validate(
        test_utility,
        charge_point_v201,
        "LogStatusNotification",
        {"status": "Uploaded", "requestId": request_id},
        timeout=STATUS_TIMEOUT_S,
    )
    path, body = await asyncio.wait_for(uploads.get(), timeout=STATUS_TIMEOUT_S)
    # The URL ends with "/", so the file goes there under the name given in the response
    assert path == f"/logs/{r.filename}"
    return r.filename, archive_contents(body)


@pytest.mark.asyncio
@pytest.mark.ocpp_version("ocpp2.0.1")
@pytest.mark.everest_core_config("everest-config-ocpp201.yaml")
@pytest.mark.everest_config_adaptions(SystemLogDirectoriesStrategy())
async def test_N01_get_log_uploads_the_diagnostics_log_files_of_the_requested_range(
    charge_point_v201: ChargePoint201,
    test_utility: TestUtility,
):
    """N01.FR.04: the diagnostics log files whose entries fall into the requested range are uploaded as one archive."""
    logging.info("######### test_N01_get_log_uploads_the_diagnostics_log_files_of_the_requested_range #########")

    uploads = asyncio.Queue()
    server = await start_upload_server(uploads)
    port = server.sockets[0].getsockname()[1]
    try:
        file_name, contents = await get_log_and_wait_for_upload(
            charge_point_v201, test_utility, uploads, port, LogEnumType.diagnostics_log, 1
        )
        assert file_name.startswith("diagnostics-") and file_name.endswith(".tar.gz")
        assert contents == {"everest.log.1": b"older entries\n", "everest.log": b"newer entries\n"}

        # everest.log.1 was last written 120 minutes ago, so it holds nothing from the last 90 minutes
        _, contents = await get_log_and_wait_for_upload(
            charge_point_v201,
            test_utility,
            uploads,
            port,
            LogEnumType.diagnostics_log,
            2,
            oldest_timestamp=minutes_ago(90),
        )
        assert contents == {"everest.log": b"newer entries\n"}
    finally:
        server.close()
        await server.wait_closed()


@pytest.mark.asyncio
@pytest.mark.ocpp_version("ocpp2.0.1")
@pytest.mark.everest_core_config("everest-config-ocpp201.yaml")
@pytest.mark.everest_config_adaptions(SystemLogDirectoriesStrategy())
async def test_N01_get_log_uploads_only_the_security_log(
    charge_point_v201: ChargePoint201,
    test_utility: TestUtility,
):
    """N01.FR.03: a SecurityLog request uploads the security log, not the other logs in its directory."""
    logging.info("######### test_N01_get_log_uploads_only_the_security_log #########")

    uploads = asyncio.Queue()
    server = await start_upload_server(uploads)
    port = server.sockets[0].getsockname()[1]
    try:
        file_name, contents = await get_log_and_wait_for_upload(
            charge_point_v201, test_utility, uploads, port, LogEnumType.security_log, 1
        )
        assert file_name.startswith("security-") and file_name.endswith(".tar.gz")
        assert contents == {"libocpp_201.security.log": b"security events\n"}
    finally:
        server.close()
        await server.wait_closed()


@pytest.mark.asyncio
@pytest.mark.ocpp_version("ocpp2.0.1")
@pytest.mark.everest_core_config("everest-config-ocpp201.yaml")
@pytest.mark.everest_config_adaptions(SystemLogDirectoriesStrategy())
async def test_N01_get_log_rejects_a_range_without_log_files(
    charge_point_v201: ChargePoint201,
    test_utility: TestUtility,
):
    """N01.FR.05: with no log file holding entries in the requested range, GetLog is rejected, nothing uploaded."""
    logging.info("######### test_N01_get_log_rejects_a_range_without_log_files #########")

    log = LogParametersType(remote_location="http://127.0.0.1:9/logs/", oldest_timestamp=minutes_ago(-60))
    r = await charge_point_v201.get_log_req(log=log, log_type=LogEnumType.diagnostics_log, request_id=1)
    assert LogStatusEnumType(r.status) == LogStatusEnumType.rejected
    assert r.filename is None

    assert not await wait_for_and_validate(
        test_utility, charge_point_v201, "LogStatusNotification", {"requestId": 1}, timeout=5
    )
