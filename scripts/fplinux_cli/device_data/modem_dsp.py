# SPDX-License-Identifier: GPL-2.0-only
"""Extract stock CEVA DSP images from admitted physical modem partitions."""

from __future__ import annotations

from typing import TYPE_CHECKING

from fplinux_cli.device_data.formats import PreparedGroup, RequiredPartition

if TYPE_CHECKING:
    from fplinux_cli.device_data.formats import PhysicalNand

GSM_DSP_PARTITION_ID = 0x10000006
LTE_DSP_PARTITION_ID = 0x10000005

REQUIRED_MODEM_DSP_PARTITIONS = {
    GSM_DSP_PARTITION_ID: RequiredPartition("dsp", 0x100),
    LTE_DSP_PARTITION_ID: RequiredPartition("image_10000005", 0x100),
}


def _trim_partition(contents: bytes) -> bytes:
    """Drop NAND erase padding while preserving the partition image bytes."""
    trimmed = contents.rstrip(b"\xff")
    if not trimmed:
        raise ValueError("modem DSP partition is empty after removing erase padding")
    return trimmed


def prepare_modem_dsp(
    nand: PhysicalNand,
    partitions: dict[int, tuple[int, int]],
    *,
    prefix: str,
) -> PreparedGroup:
    """Prepare the stock GSM and LTE DSP firmware requested by the modem driver."""
    gsm = _trim_partition(nand.partition_bytes(partitions[GSM_DSP_PARTITION_ID]))
    lte = _trim_partition(nand.partition_bytes(partitions[LTE_DSP_PARTITION_ID]))
    gsm_name = f"{prefix}-gsm-dsp.bin"
    lte_name = f"{prefix}-lte-dsp.bin"
    return PreparedGroup(
        originals={gsm_name: gsm, lte_name: lte},
        prepared={gsm_name: gsm, lte_name: lte},
    )
