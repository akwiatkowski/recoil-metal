"""The matrix console must keep reading the binary's lines and never emit escapes into a pipe."""
import argparse
import io
import json
import re
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import ai_matrix
from ai_matrix import command

PROGRESS_LINE = ("matrix: [tick 5400, 15s wall] army 1 (Cybran/turtle): 43 alive, "
                 "+9.8/+210.5 per s, 8 kills")
STANDING_LINE = ("matrix: standing [tick 5400] army 1 tech 2 count 12 mass 900 energy 5400 "
                 "mobile 0 commander 0 bp /units/UEB1202/UEB1202_unit.bp name Mass Extractor")
ESCAPE = re.compile(r"\x1b\[")


def standing(name, tech, count, mass, energy, blueprint=None, mobile=True, commander=False):
    return {"blueprint": blueprint or f"/units/{name.replace(' ', '')}/x_unit.bp",
            "description": name, "tech": tech, "count": count, "mass": mass,
            "energy": energy, "buildTime": 0, "mobile": mobile, "commander": commander}


def result(winner=0, finished=True, snapshots=(), standing_rows=None):
    return {"version": 1, "finished": finished, "winner": winner, "ticksPlayed": 16889,
            "wallSeconds": 13.7, "tickCap": 36000,
            "armies": [{"army": 0, "personality": "easy", "faction": "UEF", "alive": 94,
                        "generatedMass": 78054.5, "generatedEnergy": 1204908.5, "kills": 156,
                        "killMassWorth": 32565, "killEnergyWorth": 5109456,
                        "standing": (standing_rows or [None, None])[0]},
                       {"army": 1, "personality": "turtle", "faction": "Cybran", "alive": 0,
                        "generatedMass": 41200.0, "generatedEnergy": 800000.0, "kills": 98,
                        "killMassWorth": 12000, "killEnergyWorth": 900000,
                        "standing": (standing_rows or [None, None])[1]}],
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
        # Clock, two army rows, the front line, a blank, then both roster tables.
        self.assertEqual(len(lines), 10)
        self.assertIn("9:00", lines[0])  # 5400 ticks at ten a second
        self.assertIn("front line 59% : 41%", lines[3])
        self.assertEqual(lines[4], "")
        self.assertIn("units", lines[5])
        self.assertIn("nothing standing", lines[6])
        self.assertIn("buildings", lines[8])
        for line in lines:
            self.assertFalse(ESCAPE.search(line))
            self.assertLessEqual(len(line), 100)

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


class RosterTest(unittest.TestCase):
    def test_standing_lines_are_kept_per_report_and_cleared_for_a_wiped_army(self):
        state = ai_matrix.RunState(("easy", "turtle"), ("uef", "cybran"), 3600)
        self.assertFalse(state.feed(STANDING_LINE))
        self.assertEqual(state.standing()[1], [{
            "blueprint": "/units/UEB1202/UEB1202_unit.bp", "description": "Mass Extractor",
            "tech": 2, "count": 12, "mass": 900.0, "energy": 5400.0, "mobile": False,
            "commander": False}])
        # A later report replaces the roster wholesale rather than adding to it.
        state.feed("matrix: standing [tick 9000] army 1 tech 3 count 2 mass 4600 energy 31625 "
                   "mobile 1 commander 0 bp /units/UEB1302/UEB1302_unit.bp name Heavy Bot")
        self.assertEqual([row["count"] for row in state.standing()[1]], [2])
        self.assertTrue(state.standing()[1][0]["mobile"])
        # An army that sent no roster lines with its progress line has nothing standing,
        # not the units it held two reports ago.
        state.feed("matrix: [tick 12000, 40s wall] army 1 (Cybran/turtle): 0 alive, "
                   "+0.0/+0.0 per s, 8 kills")
        self.assertEqual(state.standing()[1], [])

    def test_a_nameless_type_falls_back_to_its_blueprint_id(self):
        self.assertEqual(ai_matrix.type_name({"description": "Medium Tank"}), "Medium Tank")
        self.assertEqual(ai_matrix.type_name(
            {"description": "", "blueprint": "/units/UEB1202/UEB1202_unit.bp"}), "UEB1202")
        self.assertEqual(ai_matrix.type_name({}), "unknown")

    def test_rows_key_on_the_blueprint_role_so_factions_line_up(self):
        # UEB2301 and URB2301 are one turret in two liveries; content that is not named
        # that way keys on its own id and merges nothing.
        self.assertEqual(ai_matrix.roster_key({"blueprint": "/units/UEB2301/UEB2301_unit.bp"}),
                         "B2301")
        self.assertEqual(ai_matrix.roster_key({"blueprint": "/units/URB2301/URB2301_unit.bp"}),
                         "B2301")
        self.assertEqual(ai_matrix.roster_key({"blueprint": "/units/armstump/armstump.lua"}),
                         "ARMSTUMP.LUA")

    def test_two_factions_versions_of_one_role_are_one_row(self):
        # The Seraphim rename it and the Cybran undercharge for it; the row keeps both
        # counts, the name most sides use, and the price as the range actually charged.
        rows = ai_matrix.roster_rows([
            [standing("Anti-Air Turret", 2, 5, 540, 3400,
                      "/units/UEB2304/UEB2304_unit.bp", mobile=False)],
            [standing("Anti-Air Defense", 2, 3, 480, 3400,
                      "/units/XSB2304/XSB2304_unit.bp", mobile=False)]])
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["counts"], [5, 3])
        self.assertEqual(rows[0]["mass"], (480, 540))
        self.assertEqual(ai_matrix.price_span(*rows[0]["mass"]), "480–540")
        self.assertEqual(ai_matrix.price_span(3400, 3400), "3,400")
        self.assertEqual(ai_matrix.price_span(2_520_000, 2_772_000), "2,520k–2,772k")
        # Both sides named it once, so the tie falls to the side that was read first.
        self.assertEqual(rows[0]["name"], "Anti-Air Turret")

    def test_the_dearest_variant_decides_the_order(self):
        rows = ai_matrix.roster_rows([
            [standing("Cheap", 1, 1, 100, 10, "/units/UEL0101/UEL0101_unit.bp"),
             standing("Dear", 3, 1, 900, 10, "/units/UEL0301/UEL0301_unit.bp")],
            [standing("Cheap", 1, 1, 1000, 10, "/units/URL0101/URL0101_unit.bp")]])
        self.assertEqual([row["name"] for row in rows], ["Cheap", "Dear"])
        self.assertEqual(rows[0]["mass"], (100, 1000))

    def test_the_commander_is_left_out(self):
        rows = ai_matrix.roster_rows([[
            standing("Armored Command Unit", 0, 1, 18000, 5000000,
                     "/units/UEL0001/UEL0001_unit.bp", commander=True),
            standing("Medium Tank", 1, 40, 56, 266, "/units/UEL0201/UEL0201_unit.bp")]])
        self.assertEqual([row["name"] for row in rows], ["Medium Tank"])

    def test_the_two_tables_split_what_walks_from_what_was_built(self):
        paint = ai_matrix.Palette(False)
        per_army = [[standing("Heavy Tank", 3, 4, 900, 10, "/units/UEL0303/UEL0303_unit.bp"),
                     standing("Power Generator", 2, 2, 1200, 12000,
                              "/units/UEB1201/UEB1201_unit.bp", mobile=False)], []]
        lines = ai_matrix.roster_sections(paint, per_army, ["uef", "cybran"], 90)
        text = "\n".join(lines)
        self.assertLess(text.index("Heavy Tank"), text.index("buildings"))
        self.assertGreater(text.index("Power Generator"), text.index("buildings"))
        self.assertIn("x4", lines[1])
        self.assertIn("·", lines[1])  # the second army has none of it
        for line in lines:
            self.assertFalse(ESCAPE.search(line))
            self.assertLessEqual(len(line), 90)

    def test_each_table_shows_ten_rows_and_counts_the_rest(self):
        per_army = [[standing(f"Unit {i}", 1, i, 1000 - i, 10,
                              f"/units/UEL0{i:03d}/UEL0{i:03d}_unit.bp") for i in range(1, 15)],
                    []]
        rows = ai_matrix.roster_rows(per_army)
        lines = ai_matrix.roster_table(ai_matrix.Palette(False), rows, ["uef", "cybran"], 90,
                                       "units")
        self.assertEqual(len(lines), 1 + ai_matrix.ROSTER_ROWS + 1)  # header, rows, footnote
        self.assertIn("and 4 cheaper", lines[-1])

    def test_an_empty_field_says_so_in_both_tables(self):
        lines = ai_matrix.roster_sections(ai_matrix.Palette(False), [[], []], ["uef", "uef"], 90)
        self.assertEqual(len(lines), 5)  # units, nothing, blank, buildings, nothing
        self.assertEqual(sum("nothing standing" in line for line in lines), 2)

    def test_a_mirror_match_labels_its_columns_by_seat(self):
        self.assertEqual(ai_matrix.seat_labels(["UEF", "Cybran"]), ["UEF", "Cybran"])
        self.assertEqual(ai_matrix.seat_labels(["uef", "uef"]), ["UEF 0", "UEF 1"])

    def test_the_card_carries_both_tables_and_survives_a_job_without_one(self):
        paint = ai_matrix.Palette(False)
        rows = [[standing("Mass Extractor", 2, 6, 900, 5400,
                          "/units/UEB1202/UEB1202_unit.bp", mobile=False)],
                [standing("Heavy Bot", 3, 2, 4600, 31625, "/units/URL0303/URL0303_unit.bp")]]
        text = "\n".join(ai_matrix.card(paint, result(standing_rows=rows), 100))
        self.assertIn("standing at the end", text)
        self.assertIn("Heavy Bot", text)
        self.assertIn("Mass Extractor", text)
        # A result.json written before the roster existed still renders.
        older = result()
        for army in older["armies"]:
            del army["standing"]
        lines = ai_matrix.card(paint, older, 100)
        self.assertEqual(sum("nothing standing" in line for line in lines), 2)
        for line in lines:
            self.assertEqual(ai_matrix.visible(line), 100)


class CaptureTest(unittest.TestCase):
    def test_the_last_frame_is_asked_for_in_points_times_the_backing_scale(self):
        args = argparse.Namespace(
            fa_root=Path("/fa"), map="SCMP_009", seconds=3600, interval=5,
            shot_size=[1920, 1080], shot_backing=2.0)
        argv = command(args, "deadbeef", Path("/job/run"), ("tech", "tech"), ("uef", "cybran"))
        # The capture is what lets a headless pre-run exit without opening a window, so
        # its flags are an invariant, not a preference.
        shot = argv.index("--screenshot")
        self.assertEqual(argv[shot + 1:shot + 4], ["/job/run/final.png", "1920", "1080"])
        self.assertEqual(argv[argv.index("--backing") + 1], "2.0")
        self.assertEqual(ai_matrix.shot_pixels(args), (3840, 2160))
        args.shot_backing = 1.0
        self.assertEqual(ai_matrix.shot_pixels(args), (1920, 1080))


class ActionTest(unittest.TestCase):
    @staticmethod
    def battle(tick, deaths, x=1000.0, z=1000.0, spread=40.0):
        return {"tick": tick, "deaths": deaths, "x": x, "z": z, "spread": spread}

    def test_the_busiest_moments_win_and_stay_apart(self):
        deaths = [self.battle(1000, 8), self.battle(1010, 8),      # one fight
                  self.battle(5000, 3),                            # too small alone
                  self.battle(9000, 12, x=4000.0, z=4000.0)]       # another fight
        moments = ai_matrix.action_moments(deaths, 5, played=10000)
        self.assertEqual([m["tick"] for m in moments], [1000, 9000])
        # Scored over the window, so the pair of eights is one 16-death moment.
        self.assertEqual(moments[0]["deaths"], 16)
        # And it is framed where they fell, a beat after the peak.
        self.assertAlmostEqual(moments[0]["x"], 1000.0)
        self.assertAlmostEqual(moments[0]["seconds"],
                               (1000 + ai_matrix.ACTION_LAG_TICKS) / ai_matrix.TICKS_PER_SECOND)

    def test_the_limit_and_the_floor_are_respected(self):
        deaths = [self.battle(tick, 9) for tick in range(0, 40000, 4000)]
        self.assertEqual(len(ai_matrix.action_moments(deaths, 3, played=40000)), 3)
        self.assertEqual(ai_matrix.action_moments([self.battle(500, 2)], 5, played=1000), [])
        self.assertEqual(ai_matrix.action_moments([], 5, played=1000), [])

    def test_the_cooldown_follows_the_match_length(self):
        # A ten-minute rush and an hour-long turtle game should not share a spacing.
        self.assertEqual(ai_matrix.action_cooldown(6000, 5), 600)
        self.assertEqual(ai_matrix.action_cooldown(36000, 5), 3600)
        self.assertEqual(ai_matrix.action_cooldown(100, 5), ai_matrix.ACTION_COOLDOWN_MIN_TICKS)

    def test_the_camera_holds_the_battle_without_dropping_to_glyphs(self):
        tight = ai_matrix.action_moments([self.battle(1000, 9, spread=5.0)], 1, played=2000)
        wide = ai_matrix.action_moments([self.battle(1000, 9, spread=4000.0)], 1, played=2000)
        self.assertEqual(tight[0]["radius"], ai_matrix.ACTION_RADIUS_MIN)
        self.assertEqual(wide[0]["radius"], ai_matrix.ACTION_RADIUS_MAX)

    def test_an_action_run_replays_the_match_and_writes_only_its_picture(self):
        args = argparse.Namespace(fa_root=Path("/fa"), map="SCMP_009", seconds=3600,
                                  interval=5, shot_size=[1920, 1080], shot_backing=2.0)
        moment = {"tick": 6054, "deaths": 30, "x": 5214.0, "z": 2873.0, "radius": 500.0,
                  "seconds": 605.8}
        argv = ai_matrix.action_command(args, Path("/job/run"), ("easy", "easy"),
                                        ("uef", "cybran"), moment, 2)
        self.assertEqual(argv[argv.index("--play") + 1], "605.8")
        look = argv.index("--look")
        self.assertEqual(argv[look + 1:look + 4], ["5214", "2873", "500"])
        shot = argv.index("--screenshot")
        # The pixels are asked for as POINTS at backing 1, which is what keeps models
        # above the icon threshold across a battle-wide view.
        self.assertEqual(argv[shot + 1:shot + 4], ["/job/run/action-2.png", "3840", "2160"])
        self.assertEqual(argv[argv.index("--backing") + 1], "1")
        # It must not overwrite the result of the run that measured the match.
        self.assertNotIn("--matrix-out", argv)
        self.assertNotIn("--report-interval", argv)


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
