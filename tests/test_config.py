"""Milestone 2: ~/.pmux/config parsing (warnings, default_cmd, default_dir)."""
import os
import re
import unittest

from helpers import PmuxTestCase
from tui import TuiCase, dialog_complete, dump

SPEC_EXAMPLE = """\
# ~/.pmux/config
default_dir = ~/work      # used when the list is empty
default_cmd = $SHELL      # command for new processes
theme       = auto        # auto | dark | light | ansi
accent      = clay        # clay | blue | purple | teal
"""


class Warnings(PmuxTestCase):
    def list_with(self, text):
        self.px.write_config(text)
        p = self.px.run("-l")
        self.assertEqual(p.returncode, 0, "config problems must not be fatal: %r" % p.stderr)
        self.assertEqual(p.stdout, b"")
        return p.stderr.decode()

    def test_no_config_no_output(self):
        p = self.px.run("-l")
        self.assertEqual((p.returncode, p.stderr), (0, b""))

    def test_spec_example_is_valid(self):
        self.assertEqual(self.list_with(SPEC_EXAMPLE), "")

    def test_comments_blank_lines_and_spacing(self):
        text = "\n# comment\n   # indented comment\n  theme   =   light  # trailing\naccent=blue\n\n"
        self.assertEqual(self.list_with(text), "")

    def test_all_values_accepted(self):
        for theme in ("auto", "dark", "light", "ansi"):
            for accent in ("clay", "blue", "purple", "teal"):
                with self.subTest(theme=theme, accent=accent):
                    self.assertEqual(self.list_with("theme = %s\naccent = %s\n" % (theme, accent)), "")

    def test_unknown_key(self):
        err = self.list_with("# header\n\ntheme = dark\nbogus_key = 1\n")
        self.assertIn("pmux: ~/.pmux/config:4: unknown key 'bogus_key'", err)
        self.assertEqual(len(err.strip().splitlines()), 1, "exactly one warning: %r" % err)

    def test_multiple_unknown_keys(self):
        err = self.list_with("one = 1\ntheme = dark\ntwo = 2\n")
        self.assertIn("pmux: ~/.pmux/config:1: unknown key 'one'", err)
        self.assertIn("pmux: ~/.pmux/config:3: unknown key 'two'", err)

    def test_invalid_theme(self):
        err = self.list_with("\ntheme = neon\n")
        self.assertRegex(err, r"pmux: ~/\.pmux/config:2: .*invalid value for theme")

    def test_invalid_accent(self):
        # Same message shape as the theme case (inferred from the contract).
        err = self.list_with("accent = pink\n")
        self.assertRegex(err, r"pmux: ~/\.pmux/config:1: .*invalid value for accent")

    def test_warnings_do_not_break_create(self):
        self.px.write_config("bogus = 1\n")
        p = self.px.run("-n", "w", "-d", "--", "sleep", "1000")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.px.entry("w")["command"], "sleep 1000")


class DefaultCmd(PmuxTestCase):
    def setUp(self):
        super().setUp()
        bindir = os.path.join(self.px.home, "bin")
        os.makedirs(bindir)
        self.script = os.path.join(bindir, "mysh")
        with open(self.script, "w") as f:
            f.write("#!/bin/sh\nexec sleep 1000\n")
        os.chmod(self.script, 0o755)

    def create_default(self, name):
        p = self.px.run("-n", name, "-d")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(p.stderr, b"", "no config warnings expected")
        return self.px.entry(name)

    def test_home_var_expanded(self):
        self.px.write_config("default_cmd = $HOME/bin/mysh\n")
        e = self.create_default("a")
        self.assertEqual(e["command"], self.script)
        self.assertIn(e["state"], ("running", "idle"))

    def test_tilde_expanded(self):
        self.px.write_config("default_cmd = ~/bin/mysh\n")
        self.assertEqual(self.create_default("a")["command"], self.script)

    def test_arguments_split_without_wrapper(self):
        self.px.write_config("default_cmd = sleep 1234\n")
        self.assertEqual(self.create_default("a")["command"], "sleep 1234")

    def test_explicit_command_wins(self):
        self.px.write_config("default_cmd = sleep 1234\n")
        p = self.px.run("-n", "b", "-d", "--", "sleep", "99")
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertEqual(self.px.entry("b")["command"], "sleep 99")


class DefaultDirInTui(TuiCase):
    """^N with an empty list pre-fills Dir with default_dir (expanded)."""

    def dialog_create(self, name):
        t = self.tui()
        t.wait_for("No processes yet.")
        t.keys("C-n")
        lines = t.wait_for(dialog_complete, msg="new-process dialog")
        t.type(name)
        t.keys("Enter")
        e = self.px.wait_state(name, lambda s: True)
        t.keys("C-\\")
        return lines, e

    def dir_line(self, lines):
        for l in lines:
            if re.search(r"(^|[│ ])Dir\b", l):
                return l
        self.fail("no Dir field\n" + dump(lines))

    def check(self, rel):
        target = self.mkdir(rel)
        lines, e = self.dialog_create("p1")
        line = self.dir_line(lines)
        self.assertTrue("~/" + rel in line or target in line,
                        "Dir field should show default_dir\n" + dump(lines))
        self.assertEqual(e["dir"], target)

    def test_tilde(self):
        self.px.write_config("theme = dark\ndefault_dir = ~/dd1\n")
        self.check("dd1")

    def test_home_var(self):
        self.px.write_config("theme = dark\ndefault_dir = $HOME/dd2\n")
        self.check("dd2")

    def test_default_cmd_prefills_command(self):
        self.px.write_config("theme = dark\ndefault_cmd = sleep 1234\n")
        t = self.tui()
        t.wait_for("No processes yet.")
        t.keys("C-n")
        lines = t.wait_for(dialog_complete, msg="new-process dialog")
        cmd = [l for l in lines if re.search(r"(^|[│ ])Command\b", l)]
        self.assertTrue(cmd and "sleep 1234" in cmd[0], dump(lines))
        t.keys("Escape")
        t.wait_for(lambda l: not any("New process" in x for x in l), msg="Esc closes the dialog")


if __name__ == "__main__":
    unittest.main()
