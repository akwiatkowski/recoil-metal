"""The matrix console must keep reading the binary's lines and never emit escapes into a pipe."""
import io
import json
import re
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import ai_matrix

PROGRESS_LINE = ("matrix: [tick 5400, 15s wall] army 1 (Cybran/turtle): 43 alive, "
                 "+9.8/+210.5 per s, 8 kills")
ESCAPE = re.compile(r"\x1b\[")


def result(winner=0, finished=True, snapshots=()):
    return {"version": 1, "finished": finished, "winner": winner, "ticksPlayed": 16889,
            "wallSeconds": 13.7, "tickCap": 36000,
            "armies": [{"army": 0, "personality": "easy", "faction": "UEF", "alive": 94,
                        "generatedMass": 78054.5, "generatedEnergy": 1204908.5, "kills": 156,
                        "killMassWorth": 32565, "killEnergyWorth": 5109456},
                       {"army": 1, "personality": "turtle", "faction": "Cybran", "alive": 0,
                        "generatedMass": 41200.0, "generatedEnergy": 800000.0, "kills": 98,
                        "killMassWorth": 12000, "killEnergyWorth": 900000}],
            "snapshots": list(snapshots)}


class ParsingTest(unittest.TestCase):
    def test_progress_line_matches_the_binary_format_exactly(self):
        state = ai_matrix.RunState(("easy", "turtle"), ("uef", "cybran"), 3600)
        self.assertFalse(state.feed("matrix: [tick 5400, 15s wall] army 0 (UEF/easy): 61 alive, "
                                    "+12.3/+340.0 per s, 12 kills"))
        self.assertTrue(state.feed(PROGRESS_LINE))  # the last army's row completes a report
        self.assertEqual((state.tick, state.wall), (5400, 15))
        self.assertEqual(state.rows[1].alive, 43)
        self.assertAlmostEqual(state.rows[1].mass, 9.8)
        self.assertAlmostEqual(state.rows[1].energy, 210.5)
        self.assertEqual(state.rows[1].kills, 8)
        self.assertEqual(state.rows[1].faction, "Cybran")

    def test_templates_attacks_and_the_verdict_are_kept(self):
        state = ai_matrix.RunState(("easy", "turtle"), ("uef", "uef"), 3600)
        self.assertTrue(state.feed("faf: army 0 selected NormalMain"))
        self.assertEqual(state.rows[0].template, "NormalMain")
        self.assertFalse(state.feed("  [  96.1s] army 1 ATTACKS with 41 of 41 tanks"))
        self.assertEqual(state.latest_attack, (96.1, 1, 41, 41, "tanks"))
        self.assertFalse(state.feed("  [  98.0s] army 0 completes UEL0103"))
        self.assertFalse(state.feed("  prop albedo Pine06_V1_albedo.dds: 512x512, 10 mips"))
        self.assertTrue(state.feed("  [1688.9s] team 0 WINS"))
        self.assertEqual(state.decided, (1688.9, 0))
        self.assertTrue(state.feed("  [1700.0s] a DRAW: every army lost its commander"))
        self.assertEqual(state.decided, (1700.0, None))


class RenderingTest(unittest.TestCase):
    def test_a_pipe_gets_no_escapes_and_blocks_stay_within_the_width(self):
        paint = ai_matrix.Palette(False)
        state = ai_matrix.RunState(("easy", "turtle"), ("uef", "cybran"), 3600)
        state.feed("matrix: [tick 5400, 15s wall] army 0 (UEF/easy): 61 alive, "
                   "+12.3/+340.0 per s, 12 kills")
        state.feed(PROGRESS_LINE)
        lines = state.render(paint, 100)
        self.assertEqual(len(lines), 4)
        for line in lines:
            self.assertFalse(ESCAPE.search(line))
            self.assertLessEqual(len(line), 100)
        self.assertIn("9:00", lines[0])  # 5400 ticks at ten a second
        self.assertIn("front line 59% : 41%", lines[3])

    def test_the_front_line_splits_by_standing_units(self):
        paint = ai_matrix.Palette(False)
        bar, share = ai_matrix.front_line(paint, 3, 1, 41, ["uef", "cybran"])
        self.assertEqual(share, 0.75)
        self.assertEqual(len(bar), 41)
        self.assertEqual(bar.index("◆"), 30)
        _, even = ai_matrix.front_line(paint, 0, 0, 41, ["uef", "uef"])
        self.assertEqual(even, 0.5)

    def test_colour_mode_paints_factions_and_resets(self):
        paint = ai_matrix.Palette(True)
        painted = paint.faction("Cybran", "Cybran turtle", bold=True)
        self.assertTrue(painted.startswith("\x1b[1m\x1b[38;2;225;90;90m"))
        self.assertTrue(painted.endswith("\x1b[0m"))
        self.assertEqual(ai_matrix.visible(painted), len("Cybran turtle"))
        self.assertEqual(ai_matrix.pad(painted, 20), painted + " " * 7)

    def test_sparkline_and_clock(self):
        self.assertEqual(ai_matrix.sparkline([]), "")
        self.assertEqual(ai_matrix.sparkline([0, 4, 8]), "▁▄█")
        self.assertEqual(ai_matrix.clock(0), "0:00")
        self.assertEqual(ai_matrix.clock(1688.9), "28:09")
        self.assertEqual(ai_matrix.clock(3600), "1:00:00")

    def test_card_crowns_the_winner_and_draws_the_snapshots(self):
        paint = ai_matrix.Palette(False)
        snapshots = [{"tick": 100 * i, "wallSeconds": 15 * i,
                      "armies": [{"alive": 10 * i}, {"alive": 40 - 10 * i}]} for i in range(1, 5)]
        lines = ai_matrix.card(paint, result(snapshots=snapshots), 100)
        text = "\n".join(lines)
        self.assertIn("♛ EASY wins", text)
        self.assertIn("UEF easy ♛", text)
        self.assertIn("▂▄▆█", text)  # 10..40 standing, scaled to the peak
        self.assertIn("█▆▃▁", text)
        for line in lines:
            self.assertEqual(ai_matrix.visible(line), 100)
        capped = "\n".join(ai_matrix.card(paint, result(winner=None, finished=False), 100))
        self.assertIn("sim cap reached", capped)
        self.assertIn("no snapshots", capped)

    def test_live_console_redraws_in_place_and_a_pipe_appends(self):
        live = io.StringIO()
        console = ai_matrix.Console(live, live=True)
        console.block(["a", "b"])
        console.block(["c", "d", "e"])
        self.assertIn("\x1b[2F\x1b[J", live.getvalue())
        pipe = io.StringIO()
        console = ai_matrix.Console(pipe, live=False)
        console.block(["a", "b"])
        console.block(["c"])
        self.assertEqual(pipe.getvalue(), "a\nb\n\nc\n")


class GridTest(unittest.TestCase):
    def test_cells_read_from_the_row_personality_and_mirror_across(self):
        results = [(("easy", "easy"), result(winner=1)),
                   (("turtle", "turtle"), None),
                   (("easy", "turtle"), result(winner=0)),
                   (("easy", "tech"), result(winner=None, finished=False)),
                   (("turtle", "tech"), result(winner=None, finished=True))]
        cell = lambda row, col: ai_matrix.grid_cell(results, row, col)  # noqa: E731
        self.assertEqual(cell("easy", "turtle"), "W")
        self.assertEqual(cell("turtle", "easy"), "L")
        self.assertEqual(cell("easy", "easy"), "1>0")
        self.assertEqual(cell("turtle", "turtle"), "✗")
        self.assertEqual(cell("easy", "tech"), "cap")
        self.assertEqual(cell("turtle", "tech"), "D")
        self.assertEqual(cell("tech", "tech"), "·")

    def test_report_counts_missing_runs_as_failures(self):
        with TemporaryDirectory() as directory:
            job = Path(directory)
            run = job / "easy-vs-easy_uef-uef"
            run.mkdir()
            (run / "result.json").write_text(json.dumps(result()))
            pairs = ai_matrix.run_pairs(["easy", "turtle"])
            out = io.StringIO()
            failures = ai_matrix.report(ai_matrix.Palette(False), ai_matrix.Console(out, False),
                                        job, pairs, ["uef", "uef"], ["easy", "turtle"], 100)
        self.assertEqual(failures, 2)
        self.assertIn("1/3 run(s) produced JSON", out.getvalue())
        self.assertFalse(ESCAPE.search(out.getvalue()))


if __name__ == "__main__":
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    unittest.main()
