# SPDX-License-Identifier: GPL-2.0-only
"""Ambient scene state machine, priorities, effects and sink behavior."""

from __future__ import annotations

import json
from typing import cast

import pytest

from tests import ROOT
from tests.c_harness import c_build_directory, compile_c
from tests.process import run_process


@pytest.fixture(scope="module")
def ambient(tmp_path_factory: pytest.TempPathFactory) -> dict[str, object]:
    with c_build_directory(tmp_path_factory, "ui-ambient") as directory:
        executable = directory / "ui-ambient"
        compile_c(
            executable,
            [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'include/fplinux'}",
                str(ROOT / "tests/host_tool/gui/fplinux-ui-ambient.c"),
                str(ROOT / "lib/fplinux/fplinux-ui-ambient.c"),
            ],
            name="compile ambient engine consumer",
        )
        result = run_process(
            [str(executable)], name="exercise ambient engine", timeout=10, check=True
        )
        return cast("dict[str, object]", json.loads(result.stdout))


def test_scene_colour_and_breathing_effect(ambient: dict[str, object]) -> None:
    idle = ambient["idle"]
    assert isinstance(idle, dict)
    assert idle["colour"] == {"r": 0x18, "g": 0x5A, "b": 0x8C}
    assert 115 <= idle["intensity"] <= 116
    assert ambient["idle_late_intensity"] == 255


def test_trigger_priority_override_and_expiry(ambient: dict[str, object]) -> None:
    notification = ambient["notification"]
    low = ambient["low_battery"]
    ignored = ambient["lower_ignored"]
    expired = ambient["expired"]
    assert isinstance(notification, dict)
    assert isinstance(low, dict)
    assert isinstance(ignored, dict)
    assert isinstance(expired, dict)
    assert notification["colour"] == {"r": 0x36, "g": 0xB6, "b": 0xC8}
    assert notification["intensity"] == 110
    assert low["colour"] == {"r": 0xE8, "g": 0x98, "b": 0x1C}
    assert low["intensity"] == 110
    # The lower-priority notification cannot replace the active low-battery
    # scene; only its pulse phase is different at this instant.
    assert ignored["colour"] == low["colour"]
    assert expired["colour"] == {"r": 0x18, "g": 0x5A, "b": 0x8C}


def test_zone_override_and_disabled_frame(ambient: dict[str, object]) -> None:
    override = ambient["override"]
    disabled = ambient["disabled"]
    assert isinstance(override, dict)
    assert isinstance(disabled, dict)
    assert override["colour"] == {"r": 1, "g": 2, "b": 3}
    assert disabled["colour"] == {"r": 0, "g": 0, "b": 0}
    assert disabled["intensity"] == 0
    assert ambient["disabled_opacity"] == 0


def test_invalid_arguments_and_sink_calls(ambient: dict[str, object]) -> None:
    assert ambient["invalid"] == [True, True, True]
    assert ambient["applies"] == ambient["capture_applies"]
    assert isinstance(ambient["applies"], int)
    assert ambient["applies"] > 0
