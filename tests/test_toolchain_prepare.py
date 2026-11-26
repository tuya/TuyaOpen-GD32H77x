"""Offline tests: no real compiler download, installation, or USB access."""

import hashlib
import importlib.util
import io
import os
from pathlib import Path
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch
import zipfile


PLATFORM = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("toolchain_prepare", PLATFORM / "toolchain_prepare.py")
toolchain = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(toolchain)


class ToolchainTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="gcc test ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        env = patch.dict(os.environ, {"GD32H77X_TOOLCHAIN": "", "OPEN_COUNTRY_CODE": "Other"})
        env.start()
        self.addCleanup(env.stop)

    def compiler(self, root, windows=True):
        compiler = toolchain.compiler_at(root, windows)
        compiler.parent.mkdir(parents=True, exist_ok=True)
        for name in toolchain.TOOLS:
            (compiler.parent / f"arm-none-eabi-{name}{compiler.suffix}").write_bytes(b"fake")
        return compiler

    def artifact(self, data):
        return {"archive": "gcc.zip", "size": len(data),
                "sha256": hashlib.sha256(data).hexdigest(),
                "urls": {"China": "https://cn.invalid/gcc.zip", "Other": "https://arm.invalid/gcc.zip"}}

    def banner(self, command, **kwargs):
        return "10.3.1\n" if command[-1] == "-dumpfullversion" else "GNU Arm Embedded 10.3-2021.10\n"

    def test_validate_windows_executable_and_release(self):
        compiler = self.compiler(self.root)
        with patch.object(toolchain.subprocess, "check_output", side_effect=self.banner):
            self.assertEqual(toolchain.validate_compiler(compiler), compiler.resolve())
        with patch.object(toolchain.subprocess, "check_output", return_value="12.2.1"):
            with self.assertRaisesRegex(ValueError, "Unvalidated"):
                toolchain.validate_compiler(compiler)
        (compiler.parent / "arm-none-eabi-objcopy.exe").unlink()
        with self.assertRaisesRegex(ValueError, "Incomplete"):
            toolchain.validate_compiler(compiler)

    def test_override_is_authoritative_and_cache_precedes_path(self):
        cached = self.compiler(toolchain.install_dir(self.root))
        with patch.object(toolchain.sys, "platform", "win32"), patch.object(
                toolchain.subprocess, "check_output", side_effect=self.banner), patch.object(
                toolchain.shutil, "which", return_value="wrong.exe") as which:
            self.assertEqual(toolchain.resolve_compiler(self.root), cached.resolve())
            which.assert_not_called()
            with patch.dict(os.environ, {"GD32H77X_TOOLCHAIN": str(self.root / "missing")}):
                with self.assertRaisesRegex(ValueError, "does not exist"):
                    toolchain.resolve_compiler(self.root)

    def test_linux_path_remains_supported(self):
        compiler = self.compiler(self.root, windows=False)
        with patch.object(toolchain.sys, "platform", "linux"), patch.object(
                toolchain.shutil, "which", return_value=str(compiler)), patch.object(
                toolchain.subprocess, "check_output", side_effect=self.banner):
            self.assertEqual(toolchain.ensure_compiler(self.root), compiler.resolve())

    def test_host_detection_selects_matching_package(self):
        for system, machine, expected in (
                ("win32", "AMD64", "windows-x86"),
                ("win32", "x86", "windows-x86"),
                ("linux", "x86_64", "linux-x86_64"),
                ("linux", "aarch64", "linux-aarch64"),
                ("linux", "arm64", "linux-aarch64"),
                ("darwin", "x86_64", "darwin-x86_64")):
            with self.subTest(system=system, machine=machine), patch.object(
                    toolchain.sys, "platform", system), patch.object(
                    toolchain.platform, "machine", return_value=machine):
                self.assertEqual(toolchain.host_key(), expected)
                self.assertEqual(toolchain.host_artifact()["archive"], toolchain.MANIFEST[expected]["archive"])
                self.assertEqual(toolchain.install_dir(self.root).name, expected)
        for system, machine in (("darwin", "arm64"), ("win32", "ARM64"), ("linux", "armv7l")):
            with self.subTest(system=system, machine=machine), patch.object(
                    toolchain.sys, "platform", system), patch.object(
                    toolchain.platform, "machine", return_value=machine):
                with self.assertRaisesRegex(ValueError, "No GNU Arm Embedded"):
                    toolchain.host_artifact()

    def test_cached_archive_is_verified_and_reused_offline(self):
        archive = self.root / "gcc.zip"
        archive.write_bytes(b"good")
        with patch.object(toolchain.urllib.request, "urlopen") as download:
            toolchain.download_archive(archive, self.artifact(b"good"))
            download.assert_not_called()

    def test_corrupt_cache_and_mirror_fallback(self):
        archive = self.root / "gcc.zip"
        archive.write_bytes(b"bad!")
        with patch.dict(os.environ, {"OPEN_COUNTRY_CODE": "China"}), patch.object(
                toolchain.urllib.request, "urlopen", side_effect=[io.BytesIO(b"bad!"), io.BytesIO(b"good")]) as download:
            toolchain.download_archive(archive, self.artifact(b"good"))
        self.assertEqual(archive.read_bytes(), b"good")
        self.assertIn("cn.invalid", download.call_args_list[0].args[0])
        self.assertIn("arm.invalid", download.call_args_list[1].args[0])
        self.assertFalse(archive.with_suffix(".zip.part").exists())

    def test_failed_download_never_publishes(self):
        archive = self.root / "gcc.zip"
        with patch.object(toolchain.urllib.request, "urlopen", side_effect=OSError("offline")) as download:
            with self.assertRaisesRegex(ValueError, "Cannot download verified"):
                toolchain.download_archive(archive, self.artifact(b"good"))
        self.assertEqual(download.call_count, 4)
        self.assertFalse(archive.exists())
        self.assertFalse(archive.with_suffix(".zip.part").exists())

    def test_zip_path_escape_rejected(self):
        for name in ("../escape", "C:/escape", "/escape", "x/../../escape", "x./file"):
            with self.subTest(name=name):
                archive = self.root / "gcc.zip"
                with zipfile.ZipFile(archive, "w") as output:
                    output.writestr(name, b"bad")
                with self.assertRaisesRegex(ValueError, "Unsafe"):
                    toolchain.extract_archive(archive, self.root / "extract")

    def test_tar_extraction_and_escape_rejection(self):
        archive = self.root / "gcc.tar.bz2"
        with tarfile.open(archive, "w:bz2") as output:
            member = tarfile.TarInfo("gcc/bin/arm-none-eabi-gcc")
            member.size = 4
            member.mode = 0o755
            output.addfile(member, io.BytesIO(b"fake"))
        toolchain.extract_archive(archive, self.root / "extract")
        self.assertEqual((self.root / "extract/gcc/bin/arm-none-eabi-gcc").read_bytes(), b"fake")
        with tarfile.open(archive, "w:bz2") as output:
            member = tarfile.TarInfo("../../escape")
            output.addfile(member)
        with self.assertRaises(tarfile.TarError):
            toolchain.extract_archive(archive, self.root / "extract")

    def test_cmake_resolves_real_host_suffix_and_keeps_tools_together(self):
        cmake = toolchain.shutil.which("cmake", path=str(Path(sys.executable).parent))
        if not cmake:
            self.skipTest("SDK cmake is not available")
        compiler = self.compiler(self.root / "tool chain", windows=sys.platform == "win32")
        fixture = self.root / "cmake fixture"
        fixture.mkdir()
        (fixture / "toolchain_file.cmake").write_text(
            (PLATFORM / "toolchain_file.cmake").read_text(encoding="utf-8"), encoding="utf-8")
        resolver = fixture / "toolchain_prepare.py"
        resolver.write_text(f"print({compiler.parent.as_posix()!r})\n", encoding="utf-8")
        script = fixture / "check.cmake"
        lines = [f'set(TOS_PYTHON "{Path(sys.executable).as_posix()}")',
                 f'include("{fixture.as_posix()}/toolchain_file.cmake")']
        for variable, name in (("CMAKE_C_COMPILER", "gcc"), ("CMAKE_CXX_COMPILER", "g++"),
                               ("CMAKE_ASM_COMPILER", "gcc"), ("CMAKE_AR", "ar"),
                               ("CMAKE_RANLIB", "ranlib"), ("CMAKE_OBJCOPY", "objcopy"),
                               ("CMAKE_SIZE", "size"), ("CMAKE_LINKER", "ld")):
            expected = (compiler.parent / f"arm-none-eabi-{name}{compiler.suffix}").as_posix()
            lines.extend([f'if(NOT "${{{variable}}}" STREQUAL "{expected}")',
                          f'  message(FATAL_ERROR "Wrong {variable}: ${{{variable}}}")', "endif()"])
        script.write_text("\n".join(lines), encoding="utf-8")
        result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        resolver.write_text("raise SystemExit('compiler unavailable')\n", encoding="utf-8")
        result = subprocess.run([cmake, "-P", str(script)], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("compiler unavailable", result.stderr)

    def test_lock_is_exclusive_and_released(self):
        lock = self.root / "prepare.lock"
        code = ("import sys; sys.path.insert(0, sys.argv[1]); "
                "from pathlib import Path; from toolchain_prepare import install_lock; "
                "\nwith install_lock(Path(sys.argv[2]), timeout=0): pass\n")
        command = [sys.executable, "-B", "-c", code, str(PLATFORM), str(lock)]
        with toolchain.install_lock(lock):
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("Timed out", result.stderr)
        result = subprocess.run(command, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_install_then_resolve_without_parent_environment_change(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as output:
            for name in toolchain.TOOLS:
                output.writestr(f"gcc/bin/arm-none-eabi-{name}.exe", b"fake")
        content = data.getvalue()
        manifest = dict(toolchain.MANIFEST, **{"windows-x86": self.artifact(content)})
        with patch.object(toolchain, "MANIFEST", manifest), patch.object(
                toolchain.sys, "platform", "win32"), patch.object(
                toolchain.platform, "machine", return_value="AMD64"), patch.object(
                toolchain.shutil, "which", return_value=None), patch.object(
                toolchain.subprocess, "check_output", side_effect=self.banner), patch.object(
                toolchain.urllib.request, "urlopen", return_value=io.BytesIO(content)) as download:
            compiler = toolchain.ensure_compiler(self.root)
            self.assertTrue(compiler.is_file())
            self.assertEqual(toolchain.resolve_compiler(self.root), compiler)
            self.assertEqual(toolchain.ensure_compiler(self.root), compiler)
            self.assertEqual(download.call_count, 1)
            self.assertEqual(os.environ["GD32H77X_TOOLCHAIN"], "")

    def test_incomplete_managed_install_is_repaired_from_archive_cache(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as output:
            for name in toolchain.TOOLS:
                output.writestr(f"gcc/bin/arm-none-eabi-{name}.exe", b"fake")
        content = data.getvalue()
        manifest = dict(toolchain.MANIFEST, **{"windows-x86": self.artifact(content)})
        with patch.object(toolchain, "MANIFEST", manifest), patch.object(
                toolchain.sys, "platform", "win32"), patch.object(
                toolchain.platform, "machine", return_value="AMD64"), patch.object(
                toolchain.shutil, "which", return_value=None), patch.object(
                toolchain.subprocess, "check_output", side_effect=self.banner), patch.object(
                toolchain.urllib.request, "urlopen") as download:
            incomplete = self.compiler(toolchain.install_dir(self.root))
            (incomplete.parent / "arm-none-eabi-g++.exe").unlink()
            with self.assertRaisesRegex(ValueError, "Incomplete"):
                toolchain.resolve_compiler(self.root)
            cache = (self.root / ".tools/archives/arm-none-eabi-gcc" / manifest["version"]
                     / toolchain.host_key() / "gcc.zip")
            cache.parent.mkdir(parents=True)
            cache.write_bytes(content)
            self.assertEqual(toolchain.ensure_compiler(self.root), incomplete.resolve())
            self.assertTrue((incomplete.parent / "arm-none-eabi-g++.exe").is_file())
            download.assert_not_called()

    def test_interrupted_publication_is_rejected_and_repaired(self):
        data = io.BytesIO()
        with zipfile.ZipFile(data, "w") as output:
            for name in toolchain.TOOLS:
                output.writestr(f"gcc/bin/arm-none-eabi-{name}.exe", b"fake")
        content = data.getvalue()
        manifest = dict(toolchain.MANIFEST, **{"windows-x86": self.artifact(content)})
        with patch.object(toolchain, "MANIFEST", manifest), patch.object(
                toolchain.sys, "platform", "win32"), patch.object(
                toolchain.platform, "machine", return_value="AMD64"), patch.object(
                toolchain.shutil, "which", return_value=None), patch.object(
                toolchain.subprocess, "check_output", side_effect=self.banner), patch.object(
                toolchain.urllib.request, "urlopen", return_value=io.BytesIO(content)) as download:
            with patch.object(toolchain.shutil, "copytree", side_effect=OSError("interrupted")):
                with self.assertRaisesRegex(OSError, "interrupted"):
                    toolchain.ensure_compiler(self.root)
            with self.assertRaisesRegex(ValueError, "installation is incomplete"):
                toolchain.resolve_compiler(self.root)
            compiler = toolchain.ensure_compiler(self.root)
            self.assertTrue(compiler.is_file())
            self.assertFalse((compiler.parent.parent / ".installing").exists())
            self.assertEqual(download.call_count, 1)


if __name__ == "__main__":
    unittest.main()
