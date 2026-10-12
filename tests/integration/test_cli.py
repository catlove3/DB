"""Process-level CLI contracts; these do not test database functionality."""

from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


BINARY = Path(sys.argv[1]).resolve()
# Let unittest process only its own arguments.
del sys.argv[1]


class CliIntegrationTests(unittest.TestCase):
    def run_db(self, *arguments, input_text="", cwd=None):
        return subprocess.run(
            [str(BINARY), *arguments],
            input=input_text,
            text=True,
            capture_output=True,
            cwd=cwd,
            timeout=5,
        )

    def test_help_exits_without_entering_shell(self):
        result = self.run_db("--help", input_text="SELECT * FROM missing;\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn("Usage: mydb", result.stdout)
        self.assertNotIn("mydb> ", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_batch_eof_has_no_banner_or_prompt(self):
        for argument in ("-b", "--batch"):
            with self.subTest(argument=argument):
                result = self.run_db(argument, input_text=" \n\t\n")
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, "")

    def test_exit_stops_before_later_statements(self):
        for line in ("exit\n", "  exit; \r\n"):
            with self.subTest(line=line):
                result = self.run_db("-b", input_text=line + "SELECT * FROM missing;\n")
                self.assertEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertEqual(result.stderr, "")

    def test_interactive_mode_can_start_and_exit(self):
        result = self.run_db(input_text="exit\n")
        self.assertEqual(result.returncode, 0)
        self.assertIn("mydb> ", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_sql_produces_separate_error_blocks(self):
        result = self.run_db("-b", input_text="SHOW DATABASES;\nSELECT * FROM missing;\n")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.count("!ERROR\n"), 2)
        self.assertEqual(result.stdout.count("\n@\n"), 2)
        self.assertNotIn("mydb> ", result.stdout)
        self.assertEqual(result.stderr, "")

    def test_response_is_flushed_before_more_input(self):
        # Withhold the next statement and EOF until a result arrives, as the
        # course runner does. Run in a separate process to enforce a timeout.
        probe = """
import subprocess, sys
with subprocess.Popen([sys.argv[1], '-b'], stdin=subprocess.PIPE,
                      stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                      text=True) as process:
    process.stdin.write('SHOW DATABASES;\\n')
    process.stdin.flush()
    assert process.stdout.readline().strip() == '!ERROR'
    assert 'not implemented' in process.stdout.readline()
    assert process.stdout.readline().strip() == '@'
    process.stdin.write('exit\\n')
    process.stdin.flush()
    assert process.wait(timeout=3) == 0
"""
        subprocess.run([sys.executable, "-c", probe, str(BINARY)], check=True, timeout=5)

    def test_invalid_arguments_do_not_write_to_stdout(self):
        cases = (
            ("--unknown",),
            ("--data_dir",),
            ("--data_dir", "--batch"),
            ("--file", "input.csv"),
            ("--table", "student"),
            ("--init", "--database", "campus"),
        )
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.run_db(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn("mydb:", result.stderr)

    def test_unimplemented_startup_operations_report_failure(self):
        cases = (("-d", "campus"), ("-f", "input.csv", "-t", "student"))
        for arguments in cases:
            with self.subTest(arguments=arguments):
                result = self.run_db(*arguments)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(result.stdout, "")
                self.assertIn("not implemented", result.stderr)

    def test_starter_init_is_non_destructive_and_does_not_read_sql(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            sentinel = root / "keep.txt"
            sentinel.write_text("existing data", encoding="utf-8")
            result = self.run_db(
                "-b", "--init", "--data_dir", str(root),
                input_text="SHOW DATABASES;\n", cwd=root,
            )
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, "")
            self.assertEqual(result.stderr, "")
            self.assertEqual(sentinel.read_text(encoding="utf-8"), "existing data")
            self.assertEqual(list(root.iterdir()), [sentinel])


if __name__ == "__main__":
    unittest.main(verbosity=2)
