#!/usr/bin/env python3
"""Collect explicitly selected, opaque MC3 files into an offline cloud proposal.

The CLI attaches a read-only handle to an existing mc3.exe, verifies its actual
image and instance, then waits for that same process to exit with code zero. It
never starts/stops a game, uploads, restores, parses a career, or changes a save.
Exit and stable file bytes are observations, not a physical-disk durability or
original-loader acceptance claim. Run this helper before closing the game.
"""
from __future__ import annotations

import argparse
import base64
from dataclasses import asdict, dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import time
import unicodedata

MAX_BYTES = 16 * 1024 * 1024
MAX_FILES = 256
MAX_REVISION = 2**53 - 1
PREFERENCES = {
    "settings.toml": "settings.toml",
    "settings.ini": "settings.ini",
    "config/mc3_enhancements.toml": "config/mc3_enhancements.toml",
    "config/tuning_profiles.toml": "config/tuning_profiles.toml",
}
RESERVED = re.compile(r"^(?:con|prn|aux|nul|clock\$|conin\$|conout\$|com[1-9¹²³]|lpt[1-9¹²³])(?:\.|$)", re.I)


class SnapshotError(ValueError):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SnapshotError(message)


def virtual_path(value: str) -> str:
    """A deliberately narrower subset of the server's portable cloud paths."""
    require(isinstance(value, str) and 0 < len(value) <= 180, "Invalid selected cloud path.")
    require(unicodedata.normalize("NFC", value) == value, "Cloud paths must use NFC Unicode.")
    try:
        value.encode("utf-8", "strict")
    except UnicodeError as error:
        raise SnapshotError("Invalid Unicode in cloud path.") from error
    require(not re.search(r'[\\:<>"|?*\x00-\x1f\x7f]', value), "Cloud path has nonportable characters.")
    parts = value.split("/")
    require(all(p and p not in (".", "..") and p[-1] not in ". " and not RESERVED.match(p)
                for p in parts), "Cloud path has an unsafe component.")
    preferred = PREFERENCES.get(value.lower())
    if preferred:
        return preferred
    require(5 <= len(parts) <= 8 and [p.lower() for p in parts[:4]] ==
            ["hdd", "e", "udata", "54540079"], "Select this title's explicit hdd/E/UDATA/54540079 files or known preferences.")
    require(parts[-1].lower().endswith((".mc3remix", ".mc3")) or
            parts[-1].lower() in {"titlemeta.xbx", "titleimage.xbx", "savemeta.xbx", "saveimage.xbx"},
            "Selected HDD file is not an identified original save or metadata file.")
    return "/".join(["hdd", "E", "UDATA", "54540079", *parts[4:]])


def instance_root(install: Path, instance: int) -> Path:
    require(type(instance) is int and 0 <= instance <= 8, "Instance must be 0 through 8.")
    return install / (f"hdd{instance}" if instance > 1 else "hdd")


def original_instance(command_line: str) -> int:
    # main.cpp Instance() uses the first literal substring, then _wtoi, clamped 0..8.
    tail = command_line.partition("--instance")[2]
    number = re.match(r"\s*([+-]?[0-9]+)", tail)
    return max(0, min(8, int(number[1]))) if number else 0


def no_links(path: Path) -> None:
    """Reject symbolic links, junctions and other Windows reparse components."""
    absolute = path.absolute()
    for current in [*reversed(absolute.parents), absolute]:
        item = current.lstat()
        require(not stat.S_ISLNK(item.st_mode) and not
                (getattr(item, "st_file_attributes", 0) & 0x400), "Reparse/symlink source paths are not supported.")


def source_for(install: Path, instance: int, selected: str) -> Path:
    name = virtual_path(selected)
    parts = name.split("/")
    result = instance_root(install, instance).joinpath(*parts[1:]) if parts[0] == "hdd" else install.joinpath(*parts)
    no_links(result)
    require(result.resolve().is_relative_to(install.resolve()), "Selected source escapes the installation.")
    return result


@dataclass(frozen=True)
class ExitObservation:
    process_id: int
    executable: str
    creation_filetime: int
    instance: int
    exit_code: int


def _cim_processes(process_id: int | None = None) -> list[dict]:
    require(os.name == "nt", "Native process verification currently supports Windows only.")
    query = f"ProcessId = {int(process_id)}" if process_id is not None else "Name = 'mc3.exe'"
    script = ("$ErrorActionPreference='Stop'; [Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); @(" +
              f"Get-CimInstance Win32_Process -Filter \"{query}\" | " +
              "Select-Object ProcessId,ExecutablePath,CommandLine,@{Name='CreationFileTime';Expression={$_.CreationDate.ToFileTimeUtc().ToString()}}" +
              ") | ConvertTo-Json -Compress")
    executable = Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/WindowsPowerShell/v1.0/powershell.exe"
    result = subprocess.run([str(executable), "-NoProfile", "-NonInteractive", "-Command", script],
                            capture_output=True, timeout=30, creationflags=subprocess.CREATE_NO_WINDOW)
    require(result.returncode == 0, "Cannot inspect native process identity.")
    try:
        rows = json.loads(result.stdout.decode("utf-8-sig") or "[]")
    except (UnicodeError, json.JSONDecodeError) as error:
        raise SnapshotError("Cannot decode native process identity.") from error
    return rows if isinstance(rows, list) else [rows]


def assert_native_closed(install: Path) -> None:
    expected = install / "mc3.exe"
    for row in _cim_processes():
        require(isinstance(row, dict), "Cannot inspect native process identity.")
        image = row.get("ExecutablePath")
        require(isinstance(image, str) and image, "Another MC3 process has an unreadable executable path.")
        require(os.path.normcase(os.path.abspath(image)) != os.path.normcase(str(expected)),
                "An MC3 process in this installation is still running; preferences are shared between instances.")


def wait_for_native_exit(install: Path, instance: int, process_id: int, timeout: float) -> ExitObservation:
    """Hold the checked process handle so PID reuse cannot satisfy this wait."""
    require(os.name == "nt", "Native process verification currently supports Windows only.")
    return _wait_windows_process(install / "mc3.exe", process_id, timeout, instance)


def _wait_windows_process(expected: Path, process_id: int, timeout: float,
                          expected_instance: int | None) -> ExitObservation:
    # expected_instance=None is a test seam for an actual isolated Python child.
    import ctypes
    from ctypes import wintypes
    require(type(process_id) is int and process_id > 0 and process_id != os.getpid(), "Invalid native process ID.")
    require(0 < timeout <= 3600, "Wait timeout must be greater than zero and at most 3600 seconds.")
    no_links(expected)
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.QueryFullProcessImageNameW.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.LPWSTR, ctypes.POINTER(wintypes.DWORD)]
    kernel.GetProcessTimes.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME), ctypes.POINTER(wintypes.FILETIME)]
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    handle = kernel.OpenProcess(0x100000 | 0x1000, False, process_id)
    require(bool(handle), "The selected process must still exist when snapshot collection starts.")
    try:
        count = wintypes.DWORD(32768)
        name = ctypes.create_unicode_buffer(count.value)
        require(bool(kernel.QueryFullProcessImageNameW(handle, 0, name, ctypes.byref(count))), "Cannot read process executable.")
        require(os.path.normcase(str(Path(name.value).resolve())) == os.path.normcase(str(expected.resolve())) and
                os.path.samefile(expected, name.value), "Process executable does not match the selected installation.")
        created, exited, kernel_time, user_time = (wintypes.FILETIME() for _ in range(4))
        require(bool(kernel.GetProcessTimes(handle, ctypes.byref(created), ctypes.byref(exited), ctypes.byref(kernel_time), ctypes.byref(user_time))), "Cannot read process creation identity.")
        creation = created.dwLowDateTime | created.dwHighDateTime << 32
        instance = 0
        if expected_instance is not None:
            rows = _cim_processes(process_id)
            # CIM's DMTF datetime has microsecond precision; the held handle's
            # FILETIME retains its full 100 ns precision and prevents PID reuse.
            require(len(rows) == 1 and int(rows[0].get("CreationFileTime", -1)) // 10 == creation // 10 and
                    isinstance(rows[0].get("CommandLine"), str), "Process command-line identity is unavailable or changed.")
            instance = original_instance(rows[0]["CommandLine"])
            require(instance == expected_instance, "Process HDD instance differs from the selected instance.")
        result = kernel.WaitForSingleObject(handle, min(int(timeout * 1000), 3600000))
        require(result == 0, "Native process has not exited within the selected wait.")
        code = wintypes.DWORD()
        require(bool(kernel.GetExitCodeProcess(handle, ctypes.byref(code))), "Cannot read process exit code.")
        require(code.value == 0, "Native process did not exit with code zero; no snapshot was collected.")
        return ExitObservation(process_id, str(expected.resolve()), creation, instance, code.value)
    finally:
        kernel.CloseHandle(handle)


def _read_file(path: Path, remaining: int) -> tuple[bytes, tuple[int, ...]]:
    no_links(path)
    before = path.stat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1, "Selected source must be a regular file without hard links.")
    require(0 <= before.st_size <= remaining, "Selected save/config files exceed 16 MiB.")
    with path.open("rb") as source:
        opened = os.fstat(source.fileno())
        require((opened.st_dev, opened.st_ino) == (before.st_dev, before.st_ino), "Source changed while opening.")
        data = source.read(remaining + 1)
        after_handle = os.fstat(source.fileno())
    after = path.stat()
    def stamp(value):
        return (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns, value.st_nlink)
    # Windows CPython 3.12 stat/fstat can expose different ctime semantics for
    # the same file. Compare ctime only between path queries, not across APIs.
    require(len(data) <= remaining and stamp(before) == stamp(opened) == stamp(after_handle) == stamp(after)
            and before.st_ctime_ns == after.st_ctime_ns and len(data) == before.st_size, "Source changed during collection.")
    no_links(path)
    return data, (*stamp(after), after.st_ctime_ns)


def collect_snapshot(install: Path, instance: int, selected: list[str], base_revision: int,
                     observation: ExitObservation, *, closed_check=assert_native_closed,
                     stability_seconds: float = 0.15) -> tuple[dict, dict]:
    require(install.is_absolute(), "Install directory must be absolute.")
    no_links(install)
    install = install.resolve()
    instance_root(install, instance)
    require(type(base_revision) is int and 0 <= base_revision <= MAX_REVISION, "Invalid cloud base revision.")
    require(isinstance(observation, ExitObservation) and observation.exit_code == 0 and observation.instance == instance and
            Path(observation.executable) == install / "mc3.exe" and observation.creation_filetime > 0,
            "A checked exited process observation for this installation/instance is required.")
    require(0 <= stability_seconds <= 5, "Invalid stability interval.")
    require(0 < len(selected) <= MAX_FILES, "Select 1 through 256 exact save/config files.")
    names = [virtual_path(name) for name in selected]
    folded = [name.casefold() for name in names]
    require(len(set(folded)) == len(folded), "Duplicate selected cloud path.")
    require(not any(a != b and b.startswith(a + "/") for a in folded for b in folded), "Cloud file/directory prefix collision.")
    closed_check(install)
    files, observed = [], []
    total = 0
    for name in names:
        source = source_for(install, instance, name)
        data, stamp = _read_file(source, MAX_BYTES - total)
        total += len(data)
        sha = hashlib.sha256(data).hexdigest()
        files.append({"path": name, "encoding": "base64", "data": base64.b64encode(data).decode("ascii"), "sha256": sha})
        observed.append((source, sha, stamp))
    time.sleep(stability_seconds)
    closed_check(install)
    for source, sha, stamp in observed:
        second, second_stamp = _read_file(source, MAX_BYTES)
        require(second_stamp == stamp and hashlib.sha256(second).hexdigest() == sha, "Selected files changed between stability passes.")
    closed_check(install)
    request = {"base_revision": base_revision, "files": files, "settings": {}}
    receipt = {"schema": "mc3-cloud-snapshot-proposal-v1", "status": "proposal", "selected_files_only": True,
               "process": asdict(observation), "physical_hdd_directory": instance_root(install, instance).name,
               "bytes": total, "files": [{"path": f["path"], "sha256": f["sha256"], "size": len(base64.b64decode(f["data"]))} for f in files],
               "stability_passes": 2, "uploaded": False, "restored": False,
               "physical_disk_durability_proved": False, "original_loader_accepted": False}
    return request, receipt


def write_proposal(output: Path, install: Path, request: dict, receipt: dict) -> Path:
    require(output.is_absolute(), "Proposal output directory must be absolute.")
    no_links(output.parent)
    require(not output.resolve().is_relative_to(install.resolve()), "Proposal output must be outside the installation.")
    output.mkdir(mode=0o700)  # exclusive; preserve any prior artifact or partial output
    payload = (json.dumps(request, ensure_ascii=True, separators=(",", ":")) + "\n").encode("utf-8")
    receipt = {**receipt, "request_sha256": hashlib.sha256(payload).hexdigest()}
    for name, data in (("cloud-request.json", payload), ("proposal.json", (json.dumps(receipt, indent=2) + "\n").encode("utf-8"))):
        with (output / name).open("xb") as destination:
            destination.write(data)
            destination.flush()
            os.fsync(destination.fileno())
    return output / "proposal.json"


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-directory", type=Path, required=True)
    parser.add_argument("--instance", type=int, required=True)
    parser.add_argument("--process-id", type=int, required=True)
    parser.add_argument("--wait-seconds", type=float, default=900)
    parser.add_argument("--file", action="append", required=True, dest="selected", help="Exact virtual hdd/E/UDATA/54540079/... or known preference path; no wildcard.")
    parser.add_argument("--base-revision", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True, help="Unused absolute directory outside the installation.")
    args = parser.parse_args(argv)
    try:
        require(args.install_directory.is_absolute(), "Install directory must be absolute.")
        no_links(args.install_directory)
        install = args.install_directory.resolve()
        instance_root(install, args.instance)
        require(type(args.base_revision) is int and 0 <= args.base_revision <= MAX_REVISION, "Invalid cloud base revision.")
        require(0 < len(args.selected) <= MAX_FILES, "Select 1 through 256 exact save/config files.")
        for name in args.selected:
            virtual_path(name)
        require(args.output.is_absolute(), "Proposal output directory must be absolute.")
        no_links(args.output.parent)
        require(not args.output.exists() and not args.output.resolve().is_relative_to(install),
                "Choose an unused proposal directory outside the installation.")
        observation = wait_for_native_exit(install, args.instance, args.process_id, args.wait_seconds)
        request, receipt = collect_snapshot(install, args.instance, args.selected, args.base_revision, observation)
        result = write_proposal(args.output, install, request, receipt)
        print(json.dumps({"status": "proposal", "receipt": str(result), "files": len(request["files"]), "uploaded": False}))
        return 0
    except (SnapshotError, OSError, subprocess.SubprocessError) as error:
        print(json.dumps({"status": "error", "message": str(error)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
