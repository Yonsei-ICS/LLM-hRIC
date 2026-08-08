import importlib.util
import json
from pathlib import Path
import sqlite3
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "scripts/build_results.py"
SPEC = importlib.util.spec_from_file_location("paper_build_results", SCRIPT)
module = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(module)


class BuildResultsTest(unittest.TestCase):
    def _run(self, root: Path, name: str, status="complete", quality="primary", end=1):
        run_dir = root / name
        run_dir.mkdir(parents=True)
        payload = {
            "run_id": name,
            "status": status,
            "scenario": "balanced",
            "arm": "llm_only",
            "seed": 1,
            "end_ts_ms": end,
            "validation": {"data_quality": quality},
        }
        (run_dir / "manifest.json").write_text(json.dumps(payload))
        (run_dir / "llm_hric.sqlite3").touch()
        return run_dir

    def test_selects_latest_complete_primary_run(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            self._run(root, "failed", status="failed", end=30)
            self._run(root, "degraded", quality="degraded", end=40)
            self._run(root, "old", end=10)
            self._run(root, "new", end=20)
            selected, excluded = module.discover_runs(root)
            self.assertEqual([row["run_id"] for row in selected], ["new"])
            reasons = {row["run_id"]: row["reason"] for row in excluded}
            self.assertEqual(reasons["failed"], "status_not_complete")
            self.assertEqual(reasons["degraded"], "data_quality_not_primary")
            self.assertTrue(reasons["old"].startswith("superseded_by:"))

    def test_campaign_completeness_requires_exact_matrix(self):
        spec = {
            "traffic_scenarios": [{"id": "balanced"}, {"id": "heavy"}],
            "arms": ["llm_only", "ddpg_only"],
            "seeds": [1, 2],
        }
        selected = [
            {"scenario": scenario, "arm": arm, "seed": seed}
            for scenario in ("balanced", "heavy")
            for arm in ("llm_only", "ddpg_only")
            for seed in (1, 2)
        ]
        self.assertTrue(module.campaign_complete(selected, spec))
        self.assertFalse(module.campaign_complete(selected[:-1], spec))

        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            module.write_status(output, selected, spec)
            self.assertIn(r"\FullCampaigntrue", (output / "results_status.tex").read_text())
            module.write_status(output, selected[:-1], spec)
            self.assertIn(r"\FullCampaignfalse", (output / "results_status.tex").read_text())

    def test_tex_escape(self):
        self.assertEqual(module.tex_escape("llm_guided&50%"), r"llm\_guided\&50\%")

    def test_ddpg_summary_keeps_sla_and_complete_intent_distinct(self):
        conn = sqlite3.connect(":memory:")
        conn.row_factory = sqlite3.Row
        conn.execute(
            """CREATE TABLE steps(
                 reward REAL,reward_components_json TEXT,action_json TEXT
               )"""
        )
        action = {"fused_action": {"prb_ratio": {"1:ffffff": 60, "1:123456": 40}}}
        conn.execute(
            "INSERT INTO steps VALUES(?,?,?)",
            (0.8, json.dumps({"sla_satisfied": True, "intent_satisfied": True}), json.dumps(action)),
        )
        conn.execute(
            "INSERT INTO steps VALUES(?,?,?)",
            (0.4, json.dumps({"sla_satisfied": True, "intent_satisfied": False}), json.dumps(action)),
        )
        rows = conn.execute("SELECT * FROM steps").fetchall()
        summary = module.summarize_ddpg_steps(rows)
        self.assertEqual(summary["steps"], 2)
        self.assertAlmostEqual(summary["sla_satisfaction"], 1.0)
        self.assertAlmostEqual(summary["intent_satisfaction"], 0.5)
        self.assertAlmostEqual(summary["mean_slice_a_ratio"], 60.0)

    def test_ddpg_details_split_priority_directions(self):
        with tempfile.TemporaryDirectory() as tmp:
            db_path = Path(tmp) / "run.sqlite3"
            conn = sqlite3.connect(db_path)
            conn.execute(
                """CREATE TABLE experiment_steps(
                     run_id TEXT,step INTEGER,phase TEXT,reward REAL,
                     reward_components_json TEXT,action_json TEXT
                   )"""
            )
            action = json.dumps({"fused_action": {"prb_ratio": {"1:ffffff": 60}}})
            for step, priority, satisfied in ((1, "1:ffffff", True), (2, "1:123456", False)):
                components = {
                    "priority_slice": priority, "priority_action_ratio": 60,
                    "priority_min_ratio": 55, "priority_slice_dl_th_mbps": 50,
                    "protected_slice_dl_th_mbps": 30, "sla_satisfied": True,
                    "intent_satisfied": satisfied,
                }
                conn.execute(
                    "INSERT INTO experiment_steps VALUES(?,?,?,?,?,?)",
                    ("run", step, "training", 0.5, json.dumps(components), action),
                )
            conn.commit()
            conn.close()
            selected = [{
                "run_id": "run", "arm": "ddpg_only", "scenario": "balanced",
                "seed": 1, "database": str(db_path),
            }]
            phases, priorities = module.collect_ddpg_details(selected)
            training = next(row for row in phases if row["phase"] == "training")
            self.assertEqual(training["steps"], 2)
            self.assertEqual({row["priority_slice"] for row in priorities}, {"1:ffffff", "1:123456"})
            by_priority = {row["priority_slice"]: row for row in priorities}
            self.assertEqual(by_priority["1:ffffff"]["intent_satisfaction"], 1.0)
            self.assertEqual(by_priority["1:123456"]["intent_satisfaction"], 0.0)

    def test_missing_ddpg_run_writes_buildable_placeholders(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp)
            module.write_ddpg_only_tables(output, [], [])
            self.assertIn("No complete primary DDPG-only", (output / "ddpg_only_phase_table.tex").read_text())
            self.assertIn("No complete primary DDPG-only", (output / "ddpg_only_priority_table.tex").read_text())

    def test_action_space_diagnostics_detect_saturation_bins_and_boundaries(self):
        conn = sqlite3.connect(":memory:")
        conn.row_factory = sqlite3.Row
        conn.execute(
            """CREATE TABLE steps(
                 reward REAL,reward_components_json TEXT,action_json TEXT,
                 apply_success INTEGER,transition_valid INTEGER,effect_coverage REAL,
                 control_latency_ms REAL
               )"""
        )
        components = {
            "normalized_total_dl_th": 0.8, "protected_slice_dl_th_mbps": 30,
            "priority_slice_dl_th_mbps": 50, "normalized_priority_slice_dl_th": 0.4,
            "normalized_sla_deficit": 0, "intent_satisfied": True,
            "mean_dl_bler": 0, "normalized_action_churn": 0,
            "scaled_reward": 1, "reward_running_std": 1,
        }
        for actor_output, slice_a_prbs in ((0.01, 11), (0.51, 53)):
            action = {
                "cell_prbs": 106,
                "ddpg_action": {"actor_output": actor_output},
                "fused_action": {"slice_a_prbs": slice_a_prbs},
                "training_context": {"low": 0.1, "high": 0.9},
            }
            conn.execute(
                "INSERT INTO steps VALUES(?,?,?,?,?,?,?)",
                (1, json.dumps(components), json.dumps(action), 1, 1, 1, 1),
            )
        rows = conn.execute("SELECT * FROM steps").fetchall()
        metrics = module.analyze_results._step_metrics(rows)
        self.assertAlmostEqual(metrics["actor_saturation_rate"], 0.5)
        self.assertAlmostEqual(metrics["action_bin_coverage"], 0.2)
        self.assertAlmostEqual(metrics["action_boundary_rate"], 0.5)


if __name__ == "__main__":
    unittest.main()
