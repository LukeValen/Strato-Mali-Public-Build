#!/usr/bin/env python3
"""Bounded, NieR-only Vulkan command-recording crash breadcrumbs."""
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
CPP = ROOT / "app/src/main/cpp/skyline"
class RecordTraceTests(unittest.TestCase):
    def test_no_other_game_changes(self):
        state = (CPP / "gpu/interconnect/common/state_updater.h").read_text()
        self.assertIn("if (!gpu.nierDiagnosticsEnabled)", state)
        self.assertIn("cmd->record(gpu, commandBuffer, cmd);", state)
        self.assertIn("gpu.nierStateCommandIndex.store(++index", state)

    def test_captures_pipeline_and_draw_boundaries(self):
        state = (CPP / "gpu/interconnect/common/state_updater.h").read_text()
        draw = (CPP / "gpu/interconnect/maxwell_3d/maxwell_3d.cpp").read_text()
        rec = (CPP / "gpu/interconnect/command_executor.cpp").read_text()
        self.assertIn("gpu.nierRecordPhase.store(11", state)
        self.assertIn("gpu.nierRecordPhase.store(12", state)
        self.assertIn("gpu.nierRecordPhase.store(drawParams->indexed ? 30 : 31", draw)
        self.assertIn("gpu.nierRecordPhase.store(drawParams->indexed ? 32 : 33", draw)
        self.assertIn("NIER_DIAG draw fault: phase=", rec)

if __name__ == "__main__":
    unittest.main()
