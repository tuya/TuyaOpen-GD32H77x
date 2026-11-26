#!/usr/bin/env python3
"""Prepare the pinned host compiler, or resolve it without network access."""

import argparse
from contextlib import contextmanager
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import subprocess
import sys
import tarfile
import tempfile
import time
import urllib.request
import zipfile


PLATFORM_ROOT = Path(__file__).resolve().parent
OPEN_ROOT = PLATFORM_ROOT.parents[1]
MANIFEST = json.loads((PLATFORM_ROOT / "toolchain-manifest.json").read_text(encoding="utf-8"))
TOOLS = ("gcc", "g++", "ar", "ranlib", "objcopy", "size", "ld", "as")


def host_key():
    machine = platform.machine().lower()
    if sys.platform == "win32" and machine in ("amd64", "x86_64", "x86", "i386", "i686"):
        return "windows-x86"
    if machine == "amd64":
        machine = "x86_64"
    if machine == "arm64":
        machine = "aarch64"
    key = f"{sys.platform}-{machine}"
    if key not in MANIFEST:
        raise ValueError(f"No GNU Arm Embedded {MANIFEST['version']} download for host {key}; "
                         "set GD32H77X_TOOLCHAIN to a compatible installation")
    return key


def host_artifact():
    artifact = dict(MANIFEST[host_key()])
    artifact.setdefault("urls", {
        "China": "https://images.tuyacn.com/smart/embed/package/tuyaopen/" + artifact["archive"],
        "Other": ("https://armkeil.blob.core.windows.net/developer/Files/downloads/gnu-rm/"
                  + MANIFEST["version"] + "/" + artifact["archive"]),
    })
    return artifact


def install_dir(root=OPEN_ROOT):
    return Path(root) / ".tools/arm-none-eabi-gcc" / MANIFEST["version"] / host_key()


def compiler_at(root, windows=None):
    if windows is None:
        windows = sys.platform == "win32"
    return Path(root) / "bin" / ("arm-none-eabi-gcc.exe" if windows else "arm-none-eabi-gcc")


def validate_compiler(compiler):
    """Reject incomplete installations and compilers from another release."""
    compiler = Path(compiler).resolve()
    if not compiler.is_file():
        raise ValueError(f"ARM GCC does not exist: {compiler}")
    suffix = compiler.suffix
    for tool in TOOLS:
        candidate = compiler.parent / f"arm-none-eabi-{tool}{suffix}"
        if not candidate.is_file():
            raise ValueError(f"Incomplete ARM toolchain; missing {candidate}")
    try:
        version = subprocess.check_output(
            [str(compiler), "-dumpfullversion"], text=True, stderr=subprocess.STDOUT).strip()
        banner = subprocess.check_output(
            [str(compiler), "--version"], text=True, stderr=subprocess.STDOUT)
    except (OSError, subprocess.CalledProcessError) as exc:
        raise ValueError(f"Cannot run ARM GCC {compiler}: {exc}") from exc
    if version != MANIFEST["gcc_version"] or MANIFEST["version"] not in banner:
        raise ValueError(
            f"Unvalidated ARM GCC at {compiler}: expected GNU Arm Embedded "
            f"{MANIFEST['version']} ({MANIFEST['gcc_version']}), got {version}: {banner.strip()}")
    return compiler


def resolve_compiler(root=OPEN_ROOT, repair_managed=False):
    """Shared prepare/CMake discovery; never downloads or changes the environment."""
    override = os.environ.get("GD32H77X_TOOLCHAIN", "").strip()
    if override:
        return validate_compiler(compiler_at(Path(override).expanduser()))
    def cached_compiler():
        try:
            cached = compiler_at(install_dir(root))
        except ValueError:
            return None  # A user-provided toolchain may work on other hosts.
        if (cached.parent.parent / ".installing").exists():
            if not repair_managed:
                raise ValueError("ARM GCC installation is incomplete; run toolchain_prepare.py to repair")
            return None
        if cached.is_file():
            try:
                return validate_compiler(cached)
            except ValueError:
                if not repair_managed:
                    raise
        return None

    if sys.platform == "win32":
        cached = cached_compiler()
        if cached:
            return cached
    found = shutil.which("arm-none-eabi-gcc")
    if found:
        return validate_compiler(found)
    if sys.platform != "win32":
        for base in sorted(Path("/opt").glob("gcc-arm-none-eabi-*")):
            candidate = compiler_at(base)
            if candidate.is_file():
                return validate_compiler(candidate)
    return cached_compiler() if sys.platform != "win32" else None


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_archive(path, artifact):
    return (Path(path).is_file() and Path(path).stat().st_size == artifact["size"]
            and sha256(path) == artifact["sha256"])


def download_archive(destination, artifact):
    """Publish only verified downloads; try both mirrors with bounded retries."""
    destination = Path(destination)
    if valid_archive(destination, artifact):
        return
    region = "China" if os.environ.get("OPEN_COUNTRY_CODE", "").strip() == "China" else "Other"
    regions = (region, "Other" if region == "China" else "China")
    partial = destination.with_suffix(destination.suffix + ".part")
    errors = []
    for _ in range(2):
        for key in regions:
            url = artifact["urls"][key]
            print(f"GD32H77X: downloading {artifact['archive']} from {url}", file=sys.stderr)
            try:
                with urllib.request.urlopen(url, timeout=60) as source, partial.open("wb") as output:
                    shutil.copyfileobj(source, output)
                if not valid_archive(partial, artifact):
                    raise ValueError("archive size or SHA256 mismatch")
                os.replace(partial, destination)
                return
            except (OSError, ValueError) as exc:
                errors.append(f"{url}: {exc}")
            finally:
                partial.unlink(missing_ok=True)
    raise ValueError("Cannot download verified ARM GCC archive:\n" + "\n".join(errors))


def extract_archive(archive, destination):
    """Reject Windows path escapes, special files and ambiguous ZIP members."""
    if not str(archive).endswith(".zip"):
        # Python 3.12's data filter confines symlinks/hardlinks to the staging
        # tree and rejects special files while preserving executable modes.
        with tarfile.open(archive, "r:bz2") as source:
            source.extractall(destination, filter="data")
        return
    seen = set()
    with zipfile.ZipFile(archive) as source:
        for member in source.infolist():
            name = member.filename.replace("\\", "/")
            path = PurePosixPath(name)
            mode = member.external_attr >> 16
            if (path.is_absolute() or ".." in path.parts or ":" in name
                    or any(part.endswith((" ", ".")) for part in path.parts)
                    or stat.S_ISLNK(mode)
                    or (stat.S_IFMT(mode) and not (stat.S_ISREG(mode) or stat.S_ISDIR(mode)))):
                raise ValueError(f"Unsafe toolchain ZIP member: {member.filename}")
            key = name.rstrip("/").casefold()
            if key in seen:
                raise ValueError(f"Duplicate toolchain ZIP member: {member.filename}")
            seen.add(key)
        source.extractall(destination)


@contextmanager
def install_lock(path, timeout=300):
    """OS-owned lock is released even if a preparing process crashes."""
    with Path(path).open("a+b") as lock:
        lock.seek(0, os.SEEK_END)
        if lock.tell() == 0:
            lock.write(b"0")
            lock.flush()
        deadline = time.monotonic() + timeout
        while True:
            try:
                lock.seek(0)
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise ValueError(f"Timed out waiting for toolchain lock: {path}")
                time.sleep(0.2)
        try:
            yield
        finally:
            lock.seek(0)
            if os.name == "nt":
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)


def ensure_compiler(root=OPEN_ROOT):
    compiler = resolve_compiler(root, repair_managed=True)
    if compiler:
        return compiler
    artifact = host_artifact()
    target = install_dir(root)
    target.parent.mkdir(parents=True, exist_ok=True)
    with install_lock(target.parent / ".prepare.lock"):
        compiler = resolve_compiler(root, repair_managed=True)
        if compiler:
            return compiler
        cache = (Path(root) / ".tools/archives/arm-none-eabi-gcc"
                 / MANIFEST["version"] / host_key())
        cache.mkdir(parents=True, exist_ok=True)
        archive = cache / artifact["archive"]
        download_archive(archive, artifact)
        with tempfile.TemporaryDirectory(prefix=".gcc-", dir=target.parent) as temporary:
            staging = Path(temporary)
            extract_archive(archive, staging)
            candidates = list(staging.rglob(compiler_at(staging).name))
            if len(candidates) != 1 or candidates[0].parent.name != "bin":
                raise ValueError("Toolchain archive must contain exactly one bin/arm-none-eabi-gcc executable")
            compiler = validate_compiler(candidates[0])
            if sys.platform == "win32":
                # Some Windows directory watchers deny renaming a tree that
                # contains recently executed EXEs. Publish files under the
                # prepare lock instead; discovery rejects interrupted copies.
                if target.exists():
                    shutil.rmtree(target)
                target.mkdir()
                pending = target / ".installing"
                pending.touch()
                shutil.copytree(compiler.parent.parent, target, dirs_exist_ok=True)
                installed = validate_compiler(compiler_at(target))
                pending.unlink()
                return installed
            previous = staging / "previous-install"
            if target.exists():
                # Keep the old tree available for rollback until publication.
                # Windows can briefly retain handles while scanning new EXEs.
                rename_with_retry(target, previous)
            try:
                rename_with_retry(compiler.parent.parent, target)
            except OSError:
                if previous.exists():
                    rename_with_retry(previous, target)
                raise
        return validate_compiler(compiler_at(target))


def rename_with_retry(source, destination):
    for attempt in range(60):
        try:
            Path(source).rename(destination)
            return
        except PermissionError:
            if sys.platform != "win32" or attempt == 59:
                raise
            time.sleep(0.5)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resolve", action="store_true", help="resolve only; never download")
    args = parser.parse_args()
    try:
        compiler = resolve_compiler() if args.resolve else ensure_compiler()
        if compiler is None:
            raise ValueError("ARM GCC is not prepared; run the GD32H77X platform_prepare.py first")
        print(compiler.parent.as_posix())
    except (OSError, ValueError, zipfile.BadZipFile, tarfile.TarError) as exc:
        parser.exit(1, f"GD32H77X: {exc}\n")


if __name__ == "__main__":
    main()
