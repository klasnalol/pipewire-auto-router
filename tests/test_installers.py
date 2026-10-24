import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


class InstallerUpgradeTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="router upgrade ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.stage = self.root / "release"
        self.stage.mkdir()
        for name in (
            "install-kde-app.sh",
            "install-user-service.sh",
            "pipewire-auto-router.desktop.in",
        ):
            shutil.copy2(ROOT / name, self.stage / name)
        (self.stage / "pipewire_auto_router.py").write_text("# updated watcher\n")
        binary = self.stage / "pipewire-auto-router-qt"
        binary.write_text("#!/bin/sh\nexit 0\n")
        binary.chmod(0o755)

        self.data = self.root / "data"
        self.config = self.root / "config"
        self.home = self.root / "home"
        self.home.mkdir()
        self.installed = self.data / "pipewire-auto-router"
        self.installed.mkdir(parents=True)
        (self.installed / "pipewire_auto_router.py").write_text("# old watcher\n")
        self.commands = self.root / "systemctl.log"
        self.bin = self.root / "bin"
        self.bin.mkdir()
        self.write_command("systemctl", """#!/bin/sh
printf '%s\\n' "$*" >> "$ROUTER_TEST_COMMANDS"
if [ "$2" = "is-active" ]; then
    exit "$ROUTER_TEST_SERVICE_STATUS"
fi
if [ "$2" = "restart" ]; then
    [ "$(/bin/cat "$XDG_DATA_HOME/pipewire-auto-router/pipewire_auto_router.py")" = "# updated watcher" ] || exit 91
fi
""")
        for name in ("update-desktop-database", "kbuildsycoca6", "kbuildsycoca5"):
            self.write_command(name, "#!/bin/sh\nexit 0\n")
        self.environment = {
            **os.environ,
            "HOME": str(self.home),
            "XDG_DATA_HOME": str(self.data),
            "XDG_CONFIG_HOME": str(self.config),
            "PATH": str(self.bin) + os.pathsep + os.defpath,
            "ROUTER_TEST_COMMANDS": str(self.commands),
        }

    def write_command(self, name, script):
        path = self.bin / name
        path.write_text(script)
        path.chmod(0o755)

    def run_installer(self, name, active):
        self.commands.write_text("")
        completed = subprocess.run(
            ["/bin/bash", str(self.stage / name)],
            env={**self.environment, "ROUTER_TEST_SERVICE_STATUS": "0" if active else "3"},
            text=True,
            capture_output=True,
            timeout=10,
        )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertEqual(
            (self.installed / "pipewire_auto_router.py").read_text(),
            "# updated watcher\n",
        )
        return self.commands.read_text().splitlines()

    def test_desktop_upgrade_restarts_running_watcher_after_copying_backend(self):
        commands = self.run_installer("install-kde-app.sh", active=True)
        self.assertEqual(commands, [
            "--user is-active --quiet pipewire-auto-router.service",
            "--user restart pipewire-auto-router.service",
        ])
        desktop = self.data / "applications" / "pipewire-auto-router.desktop"
        self.assertIn(str(self.installed / "pipewire-auto-router-qt"), desktop.read_text())

    def test_desktop_upgrade_keeps_inactive_watcher_inactive(self):
        commands = self.run_installer("install-kde-app.sh", active=False)
        self.assertEqual(commands, ["--user is-active --quiet pipewire-auto-router.service"])

    def test_service_install_reloads_backend_for_active_or_inactive_watcher(self):
        for active in (True, False):
            with self.subTest(active=active):
                (self.installed / "pipewire_auto_router.py").write_text("# old watcher\n")
                commands = self.run_installer("install-user-service.sh", active=active)
                self.assertEqual(commands, [
                    "--user daemon-reload",
                    "--user enable pipewire-auto-router.service",
                    "--user restart pipewire-auto-router.service",
                    "--user status --no-pager pipewire-auto-router.service",
                ])
                service = self.config / "systemd" / "user" / "pipewire-auto-router.service"
                self.assertIn(str(self.installed / "pipewire_auto_router.py"), service.read_text())


if __name__ == "__main__":
    unittest.main()
