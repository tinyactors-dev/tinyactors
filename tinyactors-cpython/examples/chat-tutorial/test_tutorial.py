"""Every step of the chat tutorial: each step's main.py runs and prints
what its README shows."""

import asyncio
import importlib.util
import pathlib
import unittest

HERE = pathlib.Path(__file__).parent


def step(directory):
    """A step's main.py, loaded under a name of its own."""
    spec = importlib.util.spec_from_file_location("step_" + directory.replace("-", "_"), HERE / directory / "main.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def phases(lines):
    """Actors run in no fixed order relative to each other: each phase, the
    lines between two "--" lines, as a sorted list."""
    result = [[]]
    for line in lines:
        if line.startswith("-- "):
            result.append([])
        result[-1].append(line)
    return [sorted(phase) for phase in result]


class Tutorial(unittest.TestCase):
    def test_1_room_a_post_reaches_every_member(self):
        self.assertEqual(phases(step("1-room").run()), phases(["ada sees ada: hi", "bob sees ada: hi"]))

    def test_2_presence_users_turn_away_and_offline_users_see_nothing(self):
        self.assertEqual(phases(step("2-presence").run()), phases([
            "ada is offline", "bob is offline", "ada is active", "bob is active",
            "-- 300 seconds pass",
            "ada is away", "bob is away",
            "-- bob types, ada closes the app",
            "bob is active", "ada is offline", "bob sees bob: anyone?",
        ]))

    def test_3_workspace_makes_rooms_and_a_join_of_a_missing_room_fails(self):
        self.assertEqual(phases(asyncio.run(step("3-workspace").run())), phases([
            "create general: room.created",
            "create general: room.refused (exists)",
            "create Bad Name: room.refused (invalid name)",
            "rooms: general",
            "ada joined general",
            "bob gets an error: no such room",
            "bob joined general",
            "ada sees ada in general: hi",
            "bob sees ada in general: hi",
        ]))

    def test_4_roster_a_user_who_comes_online_gets_it_and_others_get_each_change(self):
        self.assertEqual(phases(asyncio.run(step("4-roster").run())), phases([
            "-- ada connects",
            "ada sees the roster: ada active, bob offline, cat offline",
            "-- bob connects",
            "bob sees the roster: ada active, bob active, cat offline",
            "ada sees bob is active",
            "-- 300 seconds pass",
            "bob sees ada is away",
            "ada sees bob is away",
            "-- bob closes the app",
            "ada sees bob is offline",
            "the workspace answers: {'ada': 'away', 'bob': 'offline', 'cat': 'offline'}",
        ]))

    def test_5_hibernation_keeps_the_bound_and_delivers_everything(self):
        lines = asyncio.run(step("5-hibernation").run())
        self.assertIn("100 messages reached the screens", lines)
        # 100 users connect one after another (0 + 1 + ... + 99 updates), then each turns away (100 * 99)
        self.assertIn("14850 roster updates reached the screens", lines)
        self.assertIn("at most 50 actors were in memory at rest", lines)


if __name__ == "__main__":
    unittest.main()
