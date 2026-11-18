# SPDX-License-Identifier: GPL-2.0-only
"""Checks for the staged physical LTE calibration record containers."""

from __future__ import annotations

import pytest
from fplinux_cli.device_data.lte_calibration import (
    LTE_CALIBRATION_RECORD_SIZES,
    SOURCE_DOWNLOADED,
    SOURCE_PROTECT,
    lte_calibration_container,
    prepare_lte_calibration,
    read_lte_calibration_container,
)


def _records(fill: int = 0xA5) -> dict[int, bytes]:
    """Build one complete synthetic record set with deterministic contents."""
    return {
        identifier: bytes([(fill + identifier) & 0xFF]) * size
        for identifier, size in LTE_CALIBRATION_RECORD_SIZES.items()
    }


def test_container_round_trip_preserves_every_physical_record() -> None:
    records = _records()

    downloaded = lte_calibration_container(records, source=SOURCE_DOWNLOADED)
    protected = lte_calibration_container(records, source=SOURCE_PROTECT)

    assert len(downloaded) == 466616
    assert read_lte_calibration_container(downloaded, source=SOURCE_DOWNLOADED) == records
    assert read_lte_calibration_container(protected, source=SOURCE_PROTECT) == records
    with pytest.raises(ValueError, match="unsupported"):
        read_lte_calibration_container(downloaded, source=SOURCE_PROTECT)


def test_container_rejects_missing_or_changed_records() -> None:
    records = _records()
    missing = dict(records)
    del missing[next(iter(missing))]
    changed = dict(records)
    identifier = next(iter(changed))
    changed[identifier] = bytes(len(changed[identifier]))

    with pytest.raises(ValueError, match="unexpected physical NV record set"):
        lte_calibration_container(missing, source=SOURCE_DOWNLOADED)
    with pytest.raises(ValueError, match="wrong size"):
        lte_calibration_container({**records, identifier: b"short"}, source=SOURCE_DOWNLOADED)

    contents = lte_calibration_container(records, source=SOURCE_DOWNLOADED)
    with pytest.raises(ValueError, match="truncated"):
        read_lte_calibration_container(contents[:-1], source=SOURCE_DOWNLOADED)
    with pytest.raises(ValueError, match="unsupported"):
        read_lte_calibration_container(b"NOLT" + contents[4:], source=SOURCE_DOWNLOADED)


def test_preparation_keeps_differences_between_both_fixed_copies() -> None:
    downloaded = _records()
    protected = dict(downloaded)
    identifier = next(iter(protected))
    protected[identifier] = bytes(len(protected[identifier]))

    result = prepare_lte_calibration(downloaded, protected, prefix="phone")

    assert set(result.prepared) == {
        "phone-lte-calibration-downloaded.bin",
        "phone-lte-calibration-protect.bin",
    }
    assert (
        read_lte_calibration_container(
            result.prepared["phone-lte-calibration-downloaded.bin"], source=SOURCE_DOWNLOADED
        )
        == downloaded
    )
    assert (
        read_lte_calibration_container(
            result.prepared["phone-lte-calibration-protect.bin"], source=SOURCE_PROTECT
        )
        == protected
    )
    report = result.reports["phone-lte-calibration-differences.txt"].decode()
    assert f"nv{identifier:03x}: " in report
    assert f"phone-downloaded-nv{identifier:03x}.bin" in result.originals
    assert f"phone-protect-nv{identifier:03x}.bin" in result.originals
