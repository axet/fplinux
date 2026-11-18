# SPDX-License-Identifier: GPL-2.0-only
"""Stage fitted LTE calibration records from fixed NV copies.

The stock CP reaches these records through a runtime logical-index table whose
contents are not present in the firmware image. This module stages the physical
NV records by identifier instead of guessing that table. DownloadedNV and
ProtectNV are published as separate containers because a few fitted bytes
differ between them.
"""

from __future__ import annotations

import struct
from typing import TYPE_CHECKING

from .formats import PreparedGroup

if TYPE_CHECKING:
    from collections.abc import Mapping

_HEADER = struct.Struct("<4sHHI")
_ENTRY = struct.Struct("<HHI")
_MAGIC = b"LTEC"
_VERSION = 1
SOURCE_DOWNLOADED = 0
SOURCE_PROTECT = 1

# Physical NV identifiers and exact sizes observed in the fitted backup. The
# logical indices are documented separately because several sizes have multiple
# candidates and the CP's mapping table is built at runtime.
LTE_CALIBRATION_RECORD_SIZES = {
    0x17D: 60,
    0x1AF: 64,
    0x514: 7108,
    0x519: 200,
    0x51A: 1608,
    0x51C: 4004,
    0x51E: 64,
    0x51F: 8004,
    0x521: 4004,
    0x522: 4004,
    0x524: 1608,
    0x528: 1500,
    0x841: 168,
    0x9C3: 4,
    0x9C4: 11696,
    0x9C6: 168,
    0x9C7: 24,
    0x9C8: 11556,
    0x9C9: 9656,
    0x9CA: 15116,
    0x9CB: 27288,
    0x9CC: 10180,
    0x9CD: 8056,
    0x9CE: 6316,
    0x9CF: 7020,
    0x9D0: 5312,
    0x9D1: 6284,
    0x9D2: 6284,
    0x9D3: 6284,
    0x9D4: 11624,
    0x9D5: 9404,
    0x9D6: 9376,
    0x9D7: 11108,
    0xA01: 27316,
    0xA02: 27316,
    0xA04: 11620,
    0xA05: 60,
    0xA08: 11624,
    0xA09: 15116,
    0xA0A: 27332,
    0xAA2: 3336,
    0xAA3: 64,
    0xA21: 736,
    0xA22: 11396,
    0xA23: 9016,
    0xA24: 10660,
    0xA25: 14540,
    0xA26: 19968,
    0xA27: 404,
    0xA28: 200,
    0xA29: 200,
    0xA2A: 12420,
    0xA2B: 19968,
    0xA2C: 10400,
    0xA2D: 100,
    0xA2E: 100,
    0xA2F: 924,
    0xA30: 20824,
    0xA31: 3864,
    0xA32: 460,
    0xA33: 400,
    0xA34: 184,
    0xA35: 400,
}


def lte_calibration_container(records: Mapping[int, bytes], *, source: int) -> bytes:
    """Serialize one fixed-NV copy's records into a deterministic container."""
    if source not in (SOURCE_DOWNLOADED, SOURCE_PROTECT):
        message = "unsupported LTE calibration source"
        raise ValueError(message)
    expected = dict(LTE_CALIBRATION_RECORD_SIZES)
    if set(records) != set(expected):
        message = "LTE calibration has an unexpected physical NV record set"
        raise ValueError(message)
    parts = [_HEADER.pack(_MAGIC, _VERSION, source, len(records))]
    for identifier, payload in sorted(records.items()):
        size = expected[identifier]
        if len(payload) != size:
            message = (
                f"LTE calibration NV{identifier:#x} has the wrong size: "
                f"{len(payload)} bytes; expected {size}"
            )
            raise ValueError(message)
        parts.append(_ENTRY.pack(identifier, 0, len(payload)))
        parts.append(payload)
        parts.append(b"\0" * (-len(payload) % 4))
    return b"".join(parts)


def read_lte_calibration_container(contents: bytes, *, source: int) -> dict[int, bytes]:
    """Read one fixed-NV copy's deterministic container back into its records."""
    if source not in (SOURCE_DOWNLOADED, SOURCE_PROTECT):
        message = "unsupported LTE calibration source"
        raise ValueError(message)
    if len(contents) < _HEADER.size:
        message = "truncated LTE calibration container header"
        raise ValueError(message)
    magic, version, stored_source, count = _HEADER.unpack_from(contents)
    if magic != _MAGIC or version != _VERSION or stored_source != source:
        message = "unsupported LTE calibration container header"
        raise ValueError(message)
    cursor = _HEADER.size
    records: dict[int, bytes] = {}
    for _ in range(count):
        if cursor + _ENTRY.size > len(contents):
            message = "truncated LTE calibration container entry"
            raise ValueError(message)
        identifier, entry_reserved, length = _ENTRY.unpack_from(contents, cursor)
        cursor += _ENTRY.size
        if entry_reserved or identifier <= (max(records) if records else -1):
            message = "damaged LTE calibration container entry"
            raise ValueError(message)
        padded_length = (length + 3) & ~3
        if cursor + padded_length > len(contents):
            message = "truncated LTE calibration container payload"
            raise ValueError(message)
        records[identifier] = contents[cursor : cursor + length]
        cursor += padded_length
    if cursor != len(contents) or set(records) != set(LTE_CALIBRATION_RECORD_SIZES):
        message = "incomplete LTE calibration container"
        raise ValueError(message)
    for identifier, payload in records.items():
        if len(payload) != LTE_CALIBRATION_RECORD_SIZES[identifier]:
            raise ValueError(f"LTE calibration NV{identifier:#x} has the wrong size")
    return records


def _calibration_subset(records: Mapping[int, bytes], label: str) -> dict[int, bytes]:
    """Select only the staged physical records from one complete fixed-NV parse."""
    selected: dict[int, bytes] = {}
    for identifier in sorted(LTE_CALIBRATION_RECORD_SIZES):
        size = LTE_CALIBRATION_RECORD_SIZES[identifier]
        if identifier not in records:
            raise ValueError(f"{label} is missing LTE calibration record {identifier:#x}")
        payload = records[identifier]
        if len(payload) != size:
            message = f"{label} record {identifier:#x} has the wrong size"
            raise ValueError(message)
        selected[identifier] = payload
    return selected


def prepare_lte_calibration(
    downloaded: Mapping[int, bytes],
    protected: Mapping[int, bytes],
    *,
    prefix: str,
) -> PreparedGroup:
    """Publish both fixed-NV calibration copies, including any fitted differences."""
    downloaded = _calibration_subset(downloaded, "DownloadedNV")
    protected = _calibration_subset(protected, "ProtectNV")

    originals: dict[str, bytes] = {}
    for label, records in (("downloaded", downloaded), ("protect", protected)):
        originals.update(
            {
                f"{prefix}-{label}-nv{identifier:03x}.bin": payload
                for identifier, payload in records.items()
            }
        )

    differences = []
    for identifier in sorted(LTE_CALIBRATION_RECORD_SIZES):
        fixed = downloaded[identifier]
        backup = protected[identifier]
        if fixed == backup:
            continue
        offsets = [
            str(index)
            for index, pair in enumerate(zip(fixed, backup, strict=True))
            if pair[0] != pair[1]
        ]
        differences.append(f"nv{identifier:03x}: {', '.join(offsets)}")

    report = "\n".join(["# Offset of each differing byte in ProtectNV", *differences, ""])
    return PreparedGroup(
        originals=originals,
        prepared={
            f"{prefix}-lte-calibration-downloaded.bin": lte_calibration_container(
                downloaded, source=SOURCE_DOWNLOADED
            ),
            f"{prefix}-lte-calibration-protect.bin": lte_calibration_container(
                protected, source=SOURCE_PROTECT
            ),
        },
        reports={f"{prefix}-lte-calibration-differences.txt": report.encode("ascii")},
    )
