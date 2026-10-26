"""Stage an owned GitHub draft prerelease; this tool deliberately has no publish operation.

Resume at asset boundaries using GitHub's server SHA-256 digests. Authentication stays inside gh/keyring.
Optional --auxiliary-plan pins separately reviewed proof archives; this tool neither builds nor reviews them.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import stat
import subprocess
import threading
import time
import uuid
from datetime import datetime, timezone
from pathlib import Path

AUXILIARY_FORMAT = "mc3-release-auxiliary-assets-v1"
AUXILIARY_RECEIPT = "auxiliary-assets.json"


def regular_unlinked(path):
    """Check lexical ancestors before resolving; resolve() alone would conceal a junction."""
    path = Path(path).absolute()
    for item in (path, *path.parents):
        info = item.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, "st_file_attributes", 0) & 0x400:
            raise RuntimeError("Auxiliary input crosses a link or junction.")
    if not stat.S_ISREG(path.stat().st_mode):
        raise RuntimeError("Auxiliary input must be a regular file.")
    return path.resolve()


class ControlLock:
    """The OS releases this byte lock after interruption; no stale lock-file deletion is needed."""
    def __init__(self, folder):
        folder.mkdir(parents=True, exist_ok=True)
        self.file = (folder / "uploader.lock").open("a+b")
        if not self.file.seek(0, 2):
            self.file.write(b"\0"); self.file.flush()
        self.file.seek(0)
        try:
            import msvcrt
            msvcrt.locking(self.file.fileno(), msvcrt.LK_NBLCK, 1)
        except Exception:
            self.file.close()
            raise RuntimeError("Another uploader owns this control directory.")

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.file.close()


def utc() -> str:
    return datetime.now(timezone.utc).isoformat()


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(4 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def gh_json(endpoint: str, *, method: str = "GET", body=None):
    command = ["gh", "api", "--method", method, "-H", "Accept: application/vnd.github+json", endpoint]
    if body is not None:
        command += ["--input", "-"]
    result = subprocess.run(command, input=None if body is None else json.dumps(body),
                            capture_output=True, text=True, encoding="utf-8")
    if result.returncode:
        raise RuntimeError(f"GitHub {method} {endpoint}: {result.stderr.strip()}")
    return json.loads(result.stdout) if result.stdout.strip() else None


class DraftUploader:
    def __init__(self, args):
        self.args = args
        self.control = args.control.resolve()
        self.control.mkdir(parents=True, exist_ok=True)
        self.lock = threading.RLock()
        self.state_path = self.control / "release-state.json"
        self.log_path = self.control / "upload.log"
        self.prefix = f"repos/{args.repo}"
        if self.state_path.exists():
            self.state = json.loads(self.state_path.read_text(encoding="utf-8"))
            if (self.state["repo"], self.state["tag"]) != (args.repo, args.tag):
                raise RuntimeError("Control directory belongs to another repository/tag.")
        else:
            self.state = {"format": 1, "repo": args.repo, "tag": args.tag,
                          "owner_marker": str(uuid.uuid4()), "created_utc": utc(), "assets": {}}
            self.save()
        self.marker = f"<!-- mc3-portable-release:{self.state['owner_marker']} -->"

    def log(self, message):
        line = f"{utc()} {message}"
        with self.lock:
            with self.log_path.open("a", encoding="utf-8") as stream:
                stream.write(line + "\n")
            print(line, flush=True)

    def save(self):
        with self.lock:
            self.state["updated_utc"] = utc()
            temporary = self.state_path.with_suffix(".json.tmp")
            temporary.write_text(json.dumps(self.state, indent=2) + "\n", encoding="utf-8")
            # Windows scanners/readers can briefly deny delete-sharing after inspecting the last state.
            # Retain the complete temporary file and retry only that transient publication failure.
            for attempt in range(8):
                try:
                    temporary.replace(self.state_path)
                    break
                except PermissionError:
                    if attempt == 7:
                        raise
                    time.sleep(min(0.05 * 2 ** attempt, 1.0))

    def verify_repository(self):
        repo = gh_json(self.prefix)
        if repo["full_name"] != self.args.repo or repo["private"] or repo.get("visibility") != "public":
            raise RuntimeError("Destination is not the exact approved public repository.")
        if not repo.get("permissions", {}).get("push"):
            raise RuntimeError("GitHub account cannot upload to the approved repository.")
        commit = gh_json(f"{self.prefix}/commits/{self.args.target}")
        if commit["sha"] != self.args.target:
            raise RuntimeError("Target is not the exact approved source commit.")
        self.account = gh_json("user")["login"]
        self.log(f"Verified public destination {self.args.repo}, target {self.args.target}, account {self.account}")

    def prepare_assets(self):
        folder = self.args.content.resolve()
        manifest = folder / "content-manifest.tsv"
        inventory = folder / "content-files.tsv"
        lines = manifest.read_text(encoding="utf-8").splitlines()
        if not lines or lines[0] != "MC3-CONTENT\t1":
            raise RuntimeError("Invalid content manifest header.")
        archive = lines[1].split("\t")
        if len(archive) != 3 or archive[0] != "archive" or not re.fullmatch(r"[0-9a-f]{64}", archive[2]):
            raise RuntimeError("Invalid whole-archive metadata.")
        assets = []
        total = 0
        for line in lines[2:]:
            row = line.split("\t")
            if (len(row) != 4 or row[0] != "chunk" or
                    not re.fullmatch(r"mc3-content-[A-Za-z0-9._-]+\.\d{3}\.mc3c", row[1]) or
                    not re.fullmatch(r"[0-9a-f]{64}", row[3])):
                raise RuntimeError("Invalid content chunk metadata.")
            path = folder / row[1]
            size = int(row[2])
            if not 0 < size <= 1900 << 20 or not path.is_file() or path.is_symlink() or path.stat().st_size != size:
                raise RuntimeError(f"Content chunk size/type mismatch: {row[1]}")
            assets.append({"path": str(path), "name": path.name, "size": size, "sha256": row[3]})
            total += size
        if not assets or len(assets) > 512 or total != int(archive[1]):
            raise RuntimeError("Content chunks do not match the archive size.")
        for path in [manifest, inventory]:
            if not path.is_file() or path.is_symlink():
                raise RuntimeError("Missing/nonregular content metadata.")
            assets.append({"path": str(path), "name": path.name, "size": path.stat().st_size, "sha256": sha256(path)})
        if self.args.runtime_zip:
            path = self.args.runtime_zip.resolve()
            version = self.args.tag.removeprefix("v")
            if path.name != f"MidnightClub3-Native-{version}-win64.zip" or not path.is_file():
                raise RuntimeError("Runtime ZIP name does not match the approved release tag.")
            if not self.args.runtime_sha256 or sha256(path) != self.args.runtime_sha256:
                raise RuntimeError("Runtime ZIP differs from its explicitly approved SHA-256.")
            sidecar = Path(str(path) + ".sha256")
            if sidecar.read_text(encoding="utf-8") != f"{self.args.runtime_sha256}  {path.name}\n":
                raise RuntimeError("Runtime checksum sidecar is not exact.")
            for file in [path, sidecar]:
                assets.append({"path": str(file), "name": file.name, "size": file.stat().st_size, "sha256": sha256(file)})
        auxiliary, receipt = self.auxiliary_assets()
        assets.extend(auxiliary)
        names = [asset["name"].casefold() for asset in assets]
        if len(set(names)) != len(names) or "sha256sums.txt" in names:
            raise RuntimeError("Duplicate release asset name.")
        if receipt is not None:
            self.pin_auxiliary(receipt)
        # SHA256SUMS deliberately excludes itself; it covers every approved content/runtime/auxiliary asset.
        sums = self.control / "SHA256SUMS.txt"
        sums.write_text("".join(f"{asset['sha256']}  {asset['name']}\n" for asset in sorted(assets, key=lambda a: a["name"])), encoding="utf-8", newline="\n")
        assets.append({"path": str(sums), "name": sums.name, "size": sums.stat().st_size, "sha256": sha256(sums)})
        self.state["plan"] = assets
        self.state["archive"] = {"size": total, "sha256": archive[2]}
        self.state["runtime_included"] = bool(self.args.runtime_zip)
        self.save()
        return assets

    def auxiliary_assets(self):
        request = getattr(self.args, "auxiliary_plan", None)
        pinned = self.state.get("auxiliary_plan")
        receipt_path = self.control / AUXILIARY_RECEIPT
        if request is None:
            if pinned or os.path.lexists(receipt_path):
                raise RuntimeError("Pinned auxiliary plan cannot be omitted on resume.")
            return [], None
        path = regular_unlinked(request)
        if path.stat().st_size > 1 << 20:
            raise RuntimeError("Auxiliary plan is oversized.")
        raw = path.read_bytes()
        document = json.loads(raw)
        if (not isinstance(document, dict) or set(document) != {"format", "assets"}
                or document["format"] != AUXILIARY_FORMAT or not isinstance(document["assets"], list)
                or not 1 <= len(document["assets"]) <= 32):
            raise RuntimeError("Invalid auxiliary asset plan.")
        assets, names = [], set()
        controls = {"release-state.json", "release-state.json.tmp", "upload.log", "uploader.lock",
                    "sha256sums.txt", AUXILIARY_RECEIPT}
        for row in document["assets"]:
            if not isinstance(row, dict) or set(row) != {"name", "path", "size", "sha256"}:
                raise RuntimeError("Invalid auxiliary asset record.")
            name, relative, size, digest = (row[key] for key in ("name", "path", "size", "sha256"))
            if (not isinstance(name, str) or name.casefold() in controls
                    or not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,179}\.(?:zip|7z|tar\.gz|tar\.zst)", name)
                    or re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", name.split(".")[0], re.I)):
                raise RuntimeError("Unsafe auxiliary archive basename or control-file collision.")
            if name.casefold() in names:
                raise RuntimeError("Duplicate auxiliary archive name.")
            names.add(name.casefold())
            if (not isinstance(relative, str) or re.search(r"[\x00-\x1f\\:]", relative)
                    or any(part in ("", ".", "..") for part in relative.split("/"))
                    or any(part.endswith((".", " ")) or re.fullmatch(r"CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9]", part.split(".")[0], re.I)
                           for part in relative.split("/"))
                    or Path(relative).is_absolute() or Path(relative).name != name):
                raise RuntimeError("Unsafe auxiliary artifact path.")
            if (type(size) is not int or not 0 < size <= 1900 << 20 or not isinstance(digest, str)
                    or not re.fullmatch(r"[0-9a-f]{64}", digest)):
                raise RuntimeError("Invalid auxiliary size or SHA-256.")
            artifact = regular_unlinked(path.parent / relative)
            if path.parent not in artifact.parents or artifact == self.control or self.control in artifact.parents:
                raise RuntimeError("Auxiliary artifact escapes its input root or collides with control files.")
            if artifact.stat().st_size != size or sha256(artifact) != digest:
                raise RuntimeError("Auxiliary artifact bytes changed.")
            assets.append({"path": str(artifact), "name": name, "size": size, "sha256": digest})
        if path.read_bytes() != raw:
            raise RuntimeError("Auxiliary plan changed during validation.")
        receipt = {"format": AUXILIARY_FORMAT, "input_path": str(path),
                   "input_sha256": hashlib.sha256(raw).hexdigest(), "assets": assets}
        if pinned:
            stored = regular_unlinked(receipt_path)
            stored_bytes = stored.read_bytes()
            if (hashlib.sha256(stored_bytes).hexdigest() != pinned.get("receipt_sha256")
                    or json.loads(stored_bytes) != receipt):
                raise RuntimeError("Immutable auxiliary plan changed; use a new reviewed release/control.")
        elif os.path.lexists(receipt_path):
            raise RuntimeError("Foreign/unregistered auxiliary control receipt; preserve it.")
        return assets, receipt

    def pin_auxiliary(self, receipt):
        if self.state.get("auxiliary_plan"):
            return
        path = self.control / AUXILIARY_RECEIPT
        # Exclusive creation deliberately fails closed after an incomplete first registration; never adopt it.
        data = (json.dumps(receipt, indent=2) + "\n").encode("utf-8")
        digest = hashlib.sha256(data).hexdigest()
        with path.open("xb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        if sha256(path) != digest:
            raise RuntimeError("Auxiliary control receipt changed during registration; preserve it.")
        self.state["auxiliary_plan"] = {"receipt_sha256": digest, "names": [a["name"] for a in receipt["assets"]]}
        self.save()

    def check_auxiliary_upload(self, local):
        pinned = self.state.get("auxiliary_plan")
        if not pinned or local["name"].casefold() not in {n.casefold() for n in pinned["names"]}:
            return False
        assets, _receipt = self.auxiliary_assets()
        if local not in assets:
            raise RuntimeError("Auxiliary upload differs from its immutable plan.")
        return True

    def ensure_draft(self):
        notes = self.args.notes.read_text(encoding="utf-8").rstrip() + "\n\n" + self.marker + "\n"
        if "release_id" in self.state:
            release = self.owned_draft()
            if release["target_commitish"] != self.args.target:
                if not self.args.update_owned_target:
                    raise RuntimeError("Owned draft target changed; explicit --update-owned-target is required.")
                release = gh_json(f"{self.prefix}/releases/{release['id']}", method="PATCH",
                                  body={"tag_name": self.args.tag, "target_commitish": self.args.target, "body": notes, "draft": True, "prerelease": True})
            elif release["body"] != notes:
                release = gh_json(f"{self.prefix}/releases/{release['id']}", method="PATCH", body={"body": notes})
        else:
            releases = gh_json(f"{self.prefix}/releases?per_page=100")
            matches = [release for release in releases if release["tag_name"] == self.args.tag]
            if matches:
                release = matches[0]
                if self.marker not in (release.get("body") or "") or not release["draft"]:
                    raise RuntimeError("Existing release is not this control directory's owned draft.")
            else:
                # Refuse an existing tag from unrelated work; a new draft may create its own tag when published.
                check = subprocess.run(["gh", "api", f"{self.prefix}/git/ref/tags/{self.args.tag}"], capture_output=True, text=True)
                if check.returncode == 0:
                    raise RuntimeError("An existing tag has no owned draft; refusing to adopt it.")
                if "404" not in check.stderr:
                    raise RuntimeError("Cannot establish that the release tag is unused.")
                release = gh_json(f"{self.prefix}/releases", method="POST", body={
                    "tag_name": self.args.tag, "target_commitish": self.args.target,
                    "name": self.args.title, "body": notes, "draft": True, "prerelease": True})
            self.state["release_id"] = release["id"]
            self.state["release_url"] = release["html_url"]
        if release["target_commitish"] != self.args.target:
            if not self.args.update_owned_target:
                raise RuntimeError("Recovered owned draft has another target; explicit --update-owned-target is required.")
            release = gh_json(f"{self.prefix}/releases/{release['id']}", method="PATCH",
                              body={"tag_name": self.args.tag, "target_commitish": self.args.target, "body": notes, "draft": True, "prerelease": True})
        self.state["target"] = self.args.target
        self.save()
        self.owned_draft()
        self.log(f"Owned draft ready: release {release['id']}, {release['html_url']}")

    def owned_draft(self):
        release = gh_json(f"{self.prefix}/releases/{self.state['release_id']}")
        if (release["tag_name"] != self.args.tag or not release["draft"] or not release["prerelease"] or
                self.marker not in (release.get("body") or "")):
            raise RuntimeError("Release is published, unexpected, or no longer carries this run's ownership marker.")
        return release

    def assets(self):
        return gh_json(f"{self.prefix}/releases/{self.state['release_id']}/assets?per_page=100")

    def remote_matches(self, remote, local):
        if remote.get("state") != "uploaded" or remote.get("size") != local["size"]:
            return False
        digest = remote.get("digest")
        if digest:
            return digest == "sha256:" + local["sha256"]
        # Older assets can lack a server digest. Authenticate a streamed download without saving another copy.
        command = ["gh", "api", "-H", "Accept: application/octet-stream", f"{self.prefix}/releases/assets/{remote['id']}"]
        with subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE) as process:
            hasher, size = hashlib.sha256(), 0
            for block in iter(lambda: process.stdout.read(4 << 20), b""):
                hasher.update(block); size += len(block)
            errors = process.stderr.read().decode("utf-8", "replace")
            if process.wait():
                raise RuntimeError(f"Cannot verify remote asset: {errors.strip()}")
        return size == local["size"] and hasher.hexdigest() == local["sha256"]

    def upload_one(self, local):
        name = local["name"]
        auxiliary = self.check_auxiliary_upload(local)
        self.owned_draft()
        matches = [asset for asset in self.assets() if asset["name"].casefold() == name.casefold()]
        if len(matches) > 1:
            raise RuntimeError(f"Duplicate remote assets named {name}.")
        if matches:
            remote = matches[0]
            if remote["name"] != name:
                raise RuntimeError(f"Remote asset case collision: {name}; preserve it.")
            if self.remote_matches(remote, local):
                if auxiliary:
                    self.check_auxiliary_upload(local)
                self.record(local, remote, "verified-existing")
                return
            previous = self.state["assets"].get(name, {})
            replace_sums = (name == "SHA256SUMS.txt" and self.args.replace_owned_checksums and
                            previous.get("remote_id") == remote["id"] and
                            remote.get("size") == previous.get("size") and
                            remote.get("digest") == "sha256:" + previous.get("sha256", ""))
            attempt = previous.get("attempt_started_utc")
            created = remote.get("created_at")
            retry_starter = (remote.get("state") == "starter" and remote.get("size") == 0 and attempt and created and
                             datetime.fromisoformat(created.replace("Z", "+00:00")) >= datetime.fromisoformat(attempt).replace(microsecond=0) and
                             remote.get("uploader", {}).get("login") == self.account)
            if auxiliary:
                retry_starter = retry_starter and previous.get("size") == local["size"] and previous.get("sha256") == local["sha256"]
            if not (replace_sums or retry_starter):
                raise RuntimeError(f"Existing remote asset differs: {name}; refusing to clobber it.")
            self.owned_draft()
            if auxiliary:
                self.check_auxiliary_upload(local)
            self.log(f"Removing owned {'checksum revision' if replace_sums else 'empty failed upload'} {name}, asset {remote['id']}")
            gh_json(f"{self.prefix}/releases/assets/{remote['id']}", method="DELETE")
        path = Path(local["path"])
        if not path.is_file() or path.stat().st_size != local["size"] or sha256(path) != local["sha256"]:
            raise RuntimeError(f"Local release asset changed before upload: {name}")
        if auxiliary:
            self.check_auxiliary_upload(local)
        with self.lock:
            self.state["assets"].setdefault(name, {}).update({"attempt_started_utc": utc(), "size": local["size"], "sha256": local["sha256"]})
            self.save()
        self.log(f"Uploading {name} ({local['size']:,} bytes)")
        command = ["gh", "release", "upload", self.args.tag, str(path), "--repo", self.args.repo]
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8")
        # A lost response can still leave a complete asset. Always inspect it before considering another attempt.
        self.owned_draft()
        matches = [asset for asset in self.assets() if asset["name"].casefold() == name.casefold()]
        if len(matches) != 1 or matches[0]["name"] != name or not self.remote_matches(matches[0], local):
            raise RuntimeError(f"Upload did not authenticate: {name}; gh exit={result.returncode}, {result.stderr.strip()}")
        if auxiliary:
            self.check_auxiliary_upload(local)
        self.record(local, matches[0], "verified-upload")

    def record(self, local, remote, status):
        with self.lock:
            self.state["assets"][local["name"]] = {
                "remote_id": remote["id"], "size": local["size"], "sha256": local["sha256"],
                "server_digest": remote.get("digest"), "status": status, "verified_utc": utc(),
                "download_url": remote["browser_download_url"]}
            self.save()
        self.log(f"{status}: {local['name']} (asset {remote['id']}, sha256 {local['sha256']})")

    def run(self):
        self.verify_repository()
        plan = self.prepare_assets()
        if not self.args.upload:
            self.log(f"Prepared {len(plan)} local assets; no GitHub writes (--upload omitted).")
            return
        self.ensure_draft()
        # Small metadata finishes first; two independent large uploads keep progress recoverable per asset.
        ordered = sorted(plan, key=lambda asset: asset["size"])
        with concurrent.futures.ThreadPoolExecutor(max_workers=self.args.workers) as executor:
            futures = [executor.submit(self.upload_one, local) for local in ordered]
            for future in concurrent.futures.as_completed(futures):
                future.result()
        release = self.owned_draft()
        if release["target_commitish"] != self.args.target:
            raise RuntimeError("Draft source target changed during upload; publication remains blocked.")
        actual = self.assets()
        planned = {asset["name"] for asset in plan}
        if {asset["name"] for asset in actual} != planned:
            raise RuntimeError("Draft contains missing or unplanned assets; publication remains blocked.")
        for local in plan:
            remote = next(asset for asset in actual if asset["name"] == local["name"])
            if not self.remote_matches(remote, local):
                raise RuntimeError(f"Final remote authentication failed: {local['name']}")
        self.state["draft_assets_verified_utc"] = utc()
        self.state["publication_status"] = "held-for-final-QA"
        self.save()
        self.log(f"ALL {len(plan)} REMOTE ASSETS VERIFIED. Release remains DRAFT; final publication is a separate QA decision.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", default="christiand0797/MidnightClub3-Port")
    parser.add_argument("--tag", required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--title", default="Midnight Club 3 Native — Windows x64 development prerelease")
    parser.add_argument("--content", type=Path, required=True)
    parser.add_argument("--control", type=Path, required=True)
    parser.add_argument("--notes", type=Path, required=True)
    parser.add_argument("--runtime-zip", type=Path)
    parser.add_argument("--runtime-sha256")
    parser.add_argument("--auxiliary-plan", type=Path,
                        help="immutable JSON proof-archive allowlist: format mc3-release-auxiliary-assets-v1; assets name/path/size/sha256, paths relative to the plan")
    parser.add_argument("--workers", type=int, choices=range(1, 4), default=2)
    parser.add_argument("--upload", action="store_true")
    parser.add_argument("--update-owned-target", action="store_true")
    parser.add_argument("--replace-owned-checksums", action="store_true")
    args = parser.parse_args()
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repo) or not re.fullmatch(r"v?[A-Za-z0-9][A-Za-z0-9._-]{0,79}", args.tag):
        parser.error("Invalid repository or fixed release tag.")
    if not re.fullmatch(r"[0-9a-f]{40}", args.target):
        parser.error("--target must be the full approved lowercase commit SHA.")
    if bool(args.runtime_zip) != bool(args.runtime_sha256) or (args.runtime_sha256 and not re.fullmatch(r"[0-9a-f]{64}", args.runtime_sha256)):
        parser.error("Runtime upload requires its exact explicitly approved SHA-256.")
    try:
        with ControlLock(args.control.resolve()):
            DraftUploader(args).run()
    except Exception as error:
        print(f"DRAFT UPLOAD STOPPED: {error}", flush=True)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
