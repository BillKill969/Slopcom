import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


class BuildSelectionTest(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory(prefix="slopcom-build-selection-")
        self.fixture = Path(self.temp_dir.name)
        repository = Path(__file__).resolve().parents[1]
        shutil.copyfile(repository / "Makefile", self.fixture / "Makefile")

        sources = [
            "main.c",
            "x11.c",
            "egl.c",
            "render.c",
            "stats.c",
            "background.c",
            "damage.c",
            "x11_errors.c",
        ]
        source_time = 1_700_000_000_000_000_000
        object_time = source_time + 1_000_000_000
        release_time = source_time + 2_000_000_000
        debug_time = source_time + 3_000_000_000

        source_dir = self.fixture / "src"
        source_dir.mkdir()
        for source in sources:
            path = source_dir / source
            path.touch()
            os.utime(path, ns=(source_time, source_time))

        for mode in ("release", "debug"):
            build_dir = self.fixture / "build" / mode
            build_dir.mkdir(parents=True)
            for source in sources:
                object_path = build_dir / source.replace(".c", ".o")
                object_path.touch()
                os.utime(object_path, ns=(object_time, object_time))

            binary = build_dir / "slopcom"
            binary.write_text(f"dummy {mode} executable\n")
            timestamp = release_time if mode == "release" else debug_time
            os.utime(binary, ns=(timestamp, timestamp))

        tools_dir = self.fixture / "tools"
        tools_dir.mkdir()
        pkg_config = tools_dir / "pkg-config"
        pkg_config.write_text(f"#!{sys.executable}\nraise SystemExit(0)\n")
        pkg_config.chmod(0o755)

        compiler = tools_dir / "cc"
        compiler.write_text(
            f"#!{sys.executable}\n"
            "import pathlib, sys\n"
            "args = sys.argv[1:]\n"
            "output = pathlib.Path(args[args.index('-o') + 1])\n"
            "output.write_text('dummy linked executable\\n')\n"
        )
        compiler.chmod(0o755)

        self.env = os.environ.copy()
        self.env["PATH"] = str(tools_dir) + os.pathsep + self.env.get("PATH", "")
        self.env["CC"] = str(compiler)

    def tearDown(self):
        self.temp_dir.cleanup()

    def make(self, build, target):
        return subprocess.run(
            ["make", f"BUILD={build}", target],
            cwd=self.fixture,
            env=self.env,
            check=True,
            text=True,
            capture_output=True,
        )

    def selected_binary(self):
        return os.readlink(self.fixture / "slopcom")

    def test_requested_mode_always_selects_its_binary(self):
        # Direct build-directory targets build binaries without selecting the
        # top-level ./slopcom link.
        self.make("release", "build/release/slopcom")
        self.make("debug", "build/debug/slopcom")
        self.assertFalse((self.fixture / "slopcom").exists())

        debug_first = self.make("debug", "slopcom")
        self.assertEqual(self.selected_binary(), "build/debug/slopcom")
        self.assertIn("ln -sf build/debug/slopcom slopcom", debug_first.stdout)

        debug_again = self.make("debug", "slopcom")
        self.assertEqual(self.selected_binary(), "build/debug/slopcom")
        self.assertIn("ln -sf build/debug/slopcom slopcom", debug_again.stdout)

        # The debug binary is newer, reproducing the timestamp case where a
        # normal symlink target incorrectly remains selected for release.
        release_first = self.make("release", "slopcom")
        self.assertEqual(self.selected_binary(), "build/release/slopcom")
        self.assertIn("ln -sf build/release/slopcom slopcom", release_first.stdout)

        release_again = self.make("release", "slopcom")
        self.assertEqual(self.selected_binary(), "build/release/slopcom")
        self.assertIn("ln -sf build/release/slopcom slopcom", release_again.stdout)

        self.make("debug", "slopcom")
        self.assertEqual(self.selected_binary(), "build/debug/slopcom")

        # Direct binary targets remain independent of the selected link.
        self.make("release", "build/release/slopcom")
        self.assertEqual(self.selected_binary(), "build/debug/slopcom")


if __name__ == "__main__":
    unittest.main()
