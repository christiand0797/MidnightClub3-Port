"""Package the exact pinned reviewed-texture proof graph, without rewriting history.

The caller's ledger SHA-256 is the trust anchor, not a digital signature. Only
the format-specific reference fields below create payload dependencies. Private
files and historical diagnostic observations remain inside their original JSON
receipts, but their referenced bytes are deliberately not embedded. Compiler
commands, arbitrary strings, environment paths and unpaired hashes are not file
references. Unknown mc3 proof formats fail closed; opaque data/build receipts
are copied exactly and explicitly reported as not expanded.

Use --check for a read-only graph check. Packaging requires a new output folder;
an external complete manifest appears only after independent copies, ZIP writes
and every archived member have all passed streaming SHA-256 verification.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import unicodedata
import zipfile


class ProofError(RuntimeError):
    pass


SHA = re.compile(r"[0-9a-f]{64}\Z")
RESERVED = re.compile(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])(?:\..*)?\Z", re.I)
BLOCK = 1 << 20
ZIP_NAME = "reviewed-texture-proof.zip"
MANIFEST_NAME = "reviewed-proof-manifest.json"
INVENTORY_NAME = "reviewed-proof-inventory.json"
DEFAULT_LEDGER = "templates/game/out/native/textures/reviewed_fixes.json"
PAIRS = (("path", "sha256"), ("image_path", "image_sha256"),
         ("metadata_path", "metadata_sha256"),
         ("native_run_receipt", "native_run_receipt_sha256"))
ART_PAIRS = (("source", "source_sha256"), ("output", "output_sha256"),
             ("bundle", "bundle_sha256"), ("model_path", "model_sha256"),
             ("module_path", "module_sha256"))
OPAQUE_FORMATS = {
    "mc3-photo-capture-v1", "mc3-native-gated-run-v1",
    "mc3-loading-joined-candidate-integrity-v1",
    "mc3-private-native-photo-qa-setup-v1", "mc3-private-native-photo-qa-complete-v1",
    "mc3-private-native-photo-qa-extended-preflight-v1",
    "mc3-private-native-photo-qa-shortcut-repaired-preflight-v1",
    "mc3-private-r11-overlay-v1", "mc3-qa-startup-independent-review-v1",
}


def digest(value):
    if not isinstance(value, str) or not SHA.fullmatch(value):
        raise ProofError(f"Invalid SHA-256: {value!r}")
    return value


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ProofError(f"Duplicate JSON key: {key!r}")
        result[key] = value
    return result


def _json(data):
    try:
        return json.loads(data.decode("utf-8-sig"), object_pairs_hook=_unique_object)
    except (ValueError, UnicodeError) as exc:
        raise ProofError(f"Invalid UTF-8 JSON: {exc}") from exc


def _linked(info):
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, "st_file_attributes", 0) & 0x400)


def _check_parents(path):
    """Reject symlinks/junctions at every existing ancestor, including the root."""
    for parent in reversed((path, *path.parents)):
        try:
            info = parent.lstat()
        except OSError as exc:
            raise ProofError(f"Cannot inspect {parent}: {exc}") from exc
        if _linked(info) or not stat.S_ISDIR(info.st_mode):
            raise ProofError(f"Linked or non-directory ancestor: {parent}")


def _regular(path):
    _check_parents(path.parent)
    try:
        info = path.lstat()
    except OSError as exc:
        raise ProofError(f"Missing/unreadable dependency {path}: {exc}") from exc
    if _linked(info) or not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ProofError(f"Linked or non-regular dependency: {path}")
    return info


def _identity(info):
    # Python 3.12 Windows path-stat/fstat may disagree transiently on legacy
    # st_ctime (creation time) immediately after a write. File ID, length,
    # modification time, link count and the streamed digest are the anchors.
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_nlink)


def _stream(path, destination=None, retain=False, max_retain=64 << 20):
    before = _regular(path)
    h = hashlib.sha256()
    data = bytearray() if retain else None
    total = 0
    try:
        with path.open("rb") as source:
            if _identity(os.fstat(source.fileno())) != _identity(before):
                raise ProofError(f"Dependency changed while opening: {path}")
            for block in iter(lambda: source.read(BLOCK), b""):
                h.update(block)
                total += len(block)
                if destination is not None:
                    destination.write(block)
                if data is not None:
                    if total > max_retain:
                        raise ProofError(f"JSON exceeds read limit: {path}")
                    data.extend(block)
            if _identity(os.fstat(source.fileno())) != _identity(before):
                raise ProofError(f"Dependency changed while reading: {path}")
        if _identity(_regular(path)) != _identity(before):
            raise ProofError(f"Dependency entry changed while reading: {path}")
    except OSError as exc:
        raise ProofError(f"Cannot stream {path}: {exc}") from exc
    return h.hexdigest(), total, None if data is None else bytes(data)


def _safe_name(name):
    if not name or name.startswith("/") or "\\" in name or unicodedata.normalize("NFC", name) != name:
        raise ProofError(f"Unsafe archive name: {name!r}")
    for part in name.split("/"):
        if (not part or part in (".", "..") or part[-1] in " ." or RESERVED.fullmatch(part)
                or any(unicodedata.category(c).startswith("C") or c in ':<>"|?*' for c in part)):
            raise ProofError(f"Unsafe archive component: {part!r}")
    return name


def _private(name):
    parts = name.casefold().split("/")
    return (any(re.fullmatch(r"hdd\d*", x) for x in parts)
            or any(x in {".git", ".ssh", ".aws", ".azure", "credentials", "secrets"} for x in parts)
            or any(x == ".env" or x.startswith(".env.") for x in parts)
            or parts[-1].split(".")[0] in {"settings", "credentials"}
            or parts[-1] in {"desktop-shortcut.txt", "id_rsa", "id_ed25519"}
            or parts[-1].startswith(("stop-", "child-stop-", "child-pause-"))
            or parts[-1].endswith((".release", ".lnk", ".key", ".pem", ".exe", ".dll", ".pdb", ".obj", ".lib")))


class Graph:
    def __init__(self, root, *, max_files=4096, max_bytes=2 << 30):
        self.root = Path(os.path.abspath(root))
        _check_parents(self.root)
        if max_files < 1 or max_bytes < 1:
            raise ProofError("Positive file/byte limits are required")
        self.max_files, self.max_bytes = max_files, max_bytes
        self.files, self.case_names, self.edges = {}, {}, []
        self.omissions, self.opaque = [], []
        self.total = 0

    def name(self, raw):
        if not isinstance(raw, str) or not raw:
            raise ProofError("Reference path must be a nonempty string")
        text = raw.replace("\\", "/")
        base = self.root.as_posix().rstrip("/")
        if text.startswith("/") or re.match(r"^[A-Za-z]:", text):
            if not text.casefold().startswith(base.casefold() + "/"):
                raise ProofError(f"Reference escapes project: {raw!r}")
            text = text[len(base) + 1:]
        return _safe_name(text)

    def omit(self, source, pointer, value, reason):
        self.omissions.append({"document": source, "field": pointer, "reason": reason,
                               "observed_reference": value})

    def add(self, raw, expected, source, pointer):
        expected = digest(expected)
        name = self.name(raw)
        if _private(name):
            raise ProofError(f"Private dependency cannot be payload: {name}")
        folded = name.casefold()
        if folded == INVENTORY_NAME.casefold() or folded.startswith(INVENTORY_NAME.casefold() + "/"):
            raise ProofError(f"Dependency collides with reserved inventory: {name}")
        previous = self.case_names.get(folded)
        if previous is not None and previous != name:
            raise ProofError(f"Case-colliding archive paths: {previous!r}, {name!r}")
        if any(key.startswith(folded + "/") or folded.startswith(key + "/") for key in self.case_names):
            raise ProofError(f"File/directory-colliding archive path: {name}")
        self.case_names[folded] = name
        self.edges.append({"from": source, "field": pointer, "to": name, "sha256": expected})
        if name in self.files:
            if self.files[name]["sha256"] != expected:
                raise ProofError(f"Conflicting required hashes for {name}")
            return
        actual, size, data = _stream(self.root / name, retain=name.lower().endswith(".json"))
        if actual != expected:
            raise ProofError(f"Changed dependency {name}: expected {expected}, got {actual}")
        if len(self.files) >= self.max_files or self.total + size > self.max_bytes:
            raise ProofError("Proof graph exceeds file/byte ceiling")
        self.files[name] = {"path": name, "sha256": actual, "bytes": size}
        self.total += size
        if data is not None:
            self.expand(name, _json(data))

    def pins(self, document, value, pointer, *, art=False, skip_release=False):
        """Read structured pairs only, inside a schema-selected subtree."""
        if isinstance(value, dict):
            for path_key, sha_key in PAIRS + (ART_PAIRS if art else ()):
                if path_key in value and sha_key in value:
                    self.add(value[path_key], value[sha_key], document, pointer + "/" + path_key)
            for key, child in value.items():
                if skip_release and key in {"release_file", "release_token"}:
                    self.omit(document, pointer + "/" + key, child, "historical one-shot gate marker; not payload")
                elif isinstance(child, (dict, list)):
                    self.pins(document, child, pointer + "/" + key, art=art, skip_release=skip_release)
        elif isinstance(value, list):
            for index, child in enumerate(value):
                self.pins(document, child, pointer + "/" + str(index), art=art, skip_release=skip_release)

    def mapping(self, document, value, pointer):
        if not isinstance(value, dict):
            raise ProofError(f"Expected reference hash map: {document} {pointer}")
        for raw, expected in value.items():
            self.add(raw, expected, document, pointer + "/" + raw)

    def expand(self, name, data):
        if not isinstance(data, dict):
            self.opaque.append({"document": name, "format": None, "reason": "opaque JSON data"})
            return
        fmt = data.get("format")
        if fmt == "mc3-reviewed-texture-fixes-v1":
            deps = data.get("dependencies")
            if not isinstance(deps, dict) or not isinstance(data.get("entries"), dict):
                raise ProofError("Malformed reviewed ledger")
            for entry_name, entry in data["entries"].items():
                if not isinstance(entry, dict) or "/" in _safe_name(entry_name):
                    raise ProofError("Malformed reviewed entry")
                bindings = ((entry.get("base_archive"), entry.get("before_sha256")),
                            ("templates/game/out/native/textures/replace/" + entry_name, entry.get("after_sha256")),
                            ("templates/game/out/native/textures/source/" + entry_name, entry.get("source_sha256")))
                for raw, expected in bindings:
                    if deps.get(raw) != digest(expected):
                        raise ProofError(f"Ledger entry lacks exact dependency binding: {raw}")
                if entry.get("review_proof") not in deps:
                    raise ProofError("Ledger review proof lacks an explicit dependency pin")
            self.mapping(name, deps, "/dependencies")
        elif fmt == "mc3-native-texture-review-v1":
            if data.get("decision") != "accepted":
                raise ProofError("Native review is not accepted")
            self.mapping(name, data.get("evidence_sha256"), "/evidence_sha256")
        elif fmt == "mc3-native-photo-matched-local-review-v1":
            for key in ("matched_pair", "additional_base2_visibility_frame", "candidate30_reference",
                        "historical_evidence_preserved"):
                self.pins(name, data.get(key), "/" + key)
            provenance = data.get("provenance", {})
            for key in ("runs", "source_build", "overlay", "root_run_proof"):
                self.pins(name, provenance.get(key), "/provenance/" + key)
            self.pins(name, data.get("comparison_method", {}).get("CPU_reference_actually_viewed"),
                      "/comparison_method/CPU_reference_actually_viewed")
        elif fmt == "mc3-native-photo-candidate-visibility-review-v1":
            self.mapping(name, data.get("evidence"), "/evidence")
            for key in ("frames", "early_baseline_rejection"):
                self.pins(name, data.get(key), "/" + key)
        elif fmt == "mc3-native-photo-early-baseline-visual-review-v1":
            self.pins(name, {k: data[k] for k in ("native_run_receipt", "native_run_receipt_sha256") if k in data}, "")
            self.pins(name, data.get("frames"), "/frames")
        elif fmt == "mc3-native-photo-r11-handshake-independent-review-v1":
            for key in ("helper", "runs", "source_evidence", "variant_switches", "setup_and_build_evidence"):
                self.pins(name, data.get(key), "/" + key, skip_release=True)
            for key in ("unchanged_pins", "pre_release_no_window_evidence", "media_resume", "inherited_pause_review"):
                if key in data:
                    self.omit(name, "/" + key, data[key], "diagnostic/private/build-environment observations; not live dependencies")
        elif fmt == "mc3-native-photo-r11-root-run-proof-v1":
            self.pins(name, data.get("runs"), "/runs", skip_release=True)
            self.pins(name, data.get("source_build"), "/source_build")
            for key in ("isolated_exe", "settings_after", "private_save_sha256", "media_state_at_seal"):
                if key in data:
                    self.omit(name, "/" + key, data[key], "private/runtime/environment observation; not payload")
        elif fmt in {"mc3-garage-photo-joined-plan-v1", "mc3-garage-photo-inference-v1",
                     "mc3-garage-photo-joined-candidates-v1", "mc3-garage-joined-boundary-review-v1"}:
            self.pins(name, data, "", art=True)
        elif fmt == "mc3-texture-review-v1":
            if data.get("decision") != "accepted":
                raise ProofError("Texture review is not accepted")
            # Unpaired comparison/native paths must already have ledger pins.
            self.opaque.append({"document": name, "format": fmt,
                                "reason": "exact receipt; unpaired paths do not create new dependencies"})
        elif fmt in OPAQUE_FORMATS or fmt is None or not str(fmt).startswith("mc3-"):
            self.opaque.append({"document": name, "format": fmt,
                                "reason": "exact data/build/environment receipt; referenced environment not expanded"})
        else:
            raise ProofError(f"Unsupported proof format {fmt!r} in {name}")


def build_graph(project, ledger, ledger_sha256, *, max_files=4096, max_bytes=2 << 30):
    graph = Graph(project, max_files=max_files, max_bytes=max_bytes)
    name = graph.name(str(ledger))
    graph.add(name, ledger_sha256, "<caller ledger SHA-256>", "/ledger")
    data = _json(_stream(graph.root / name, retain=True)[2])
    if not isinstance(data, dict) or data.get("format") != "mc3-reviewed-texture-fixes-v1":
        raise ProofError("Trust anchor is not a reviewed texture ledger")
    graph.ledger = name
    return graph


def _copy_verified(source, destination, expected):
    _check_parents(destination.parent)
    with destination.open("xb") as output:
        actual, size, _ = _stream(source, output)
        output.flush()
        os.fsync(output.fileno())
    if actual != expected["sha256"] or size != expected["bytes"]:
        raise ProofError(f"Source changed after planning: {source}")
    if _stream(destination)[:2] != (actual, size):
        raise ProofError(f"Independent copy verification failed: {destination}")


def _write_zip(expected_files, copies, path):
    with path.open("xb") as raw:
        with zipfile.ZipFile(raw, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, allowZip64=True) as archive:
            for name in sorted(expected_files):
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system = 3
                info.external_attr = (stat.S_IFREG | 0o644) << 16
                info.compress_type = zipfile.ZIP_DEFLATED
                with archive.open(info, "w", force_zip64=True) as member:
                    actual, size, _ = _stream(copies / name, member)
                expected = expected_files[name]
                if (actual, size) != (expected["sha256"], expected["bytes"]):
                    raise ProofError(f"Staged bytes changed: {name}")
        raw.flush()
        os.fsync(raw.fileno())


def verify_archive(path, expected):
    with zipfile.ZipFile(path, "r") as archive:
        infos = archive.infolist()
        if [x.filename for x in infos] != sorted(expected):
            raise ProofError("Archive names/order/duplicates differ from exact graph")
        for info in infos:
            want = expected[info.filename]
            if info.file_size != want["bytes"] or info.is_dir():
                raise ProofError(f"Archive member size/type mismatch: {info.filename}")
            h, size = hashlib.sha256(), 0
            with archive.open(info) as source:
                for block in iter(lambda: source.read(BLOCK), b""):
                    size += len(block)
                    h.update(block)
            if size != want["bytes"] or h.hexdigest() != want["sha256"]:
                raise ProofError(f"Archive member hash mismatch: {info.filename}")


def _inventory(graph, ledger_sha256):
    return {
        "format": "mc3-reviewed-proof-inventory-v1",
        "ledger": graph.ledger, "ledger_sha256": digest(ledger_sha256),
        "files": [graph.files[name] for name in sorted(graph.files)],
        "total_uncompressed_bytes": graph.total,
        "required_reference_edges": sorted(graph.edges, key=lambda e: (e["from"], e["field"], e["to"])),
        "metadata_only_observations": sorted(graph.omissions, key=lambda e: (e["document"], e["field"])),
        "opaque_receipts_not_expanded": sorted(graph.opaque, key=lambda e: e["document"]),
        "closure_limit": "Required schema-selected ledger/image/model/review references only; historical/private/build-environment observations, unpaired hashes, command lines and arbitrary strings are not payload. No history was rewritten or repinned.",
        "authentication_limit": "SHA-256 authenticates exact bytes against the caller's trusted ledger hash; no digital signature or neural rerun is claimed.",
    }


def _encode_json(value):
    return (json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n").encode("utf-8")


def package(project, ledger, ledger_sha256, output, *, max_files=4096, max_bytes=2 << 30):
    graph = build_graph(project, ledger, ledger_sha256, max_files=max_files, max_bytes=max_bytes)
    inventory = _inventory(graph, ledger_sha256)
    inventory_bytes = _encode_json(inventory)
    inventory_row = {"path": INVENTORY_NAME, "bytes": len(inventory_bytes),
                     "sha256": hashlib.sha256(inventory_bytes).hexdigest()}
    expected_files = graph.files | {INVENTORY_NAME: inventory_row}
    output = Path(os.path.abspath(output))
    _check_parents(output.parent)
    try:
        output.mkdir()  # Atomic reservation; even an empty existing directory is refused.
    except OSError as exc:
        raise ProofError(f"A fresh output directory is required: {output}") from exc
    copies = output / "payload"
    copies.mkdir()
    for name, expected in sorted(graph.files.items()):
        destination = copies / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        _copy_verified(graph.root / name, destination, expected)
    with (copies / INVENTORY_NAME).open("xb") as wrapper:
        wrapper.write(inventory_bytes)
        wrapper.flush()
        os.fsync(wrapper.fileno())
    zip_path = output / ZIP_NAME
    _write_zip(expected_files, copies, zip_path)
    verify_archive(zip_path, expected_files)
    archive_sha, archive_size, _ = _stream(zip_path)
    manifest = inventory | {
        "format": "mc3-reviewed-proof-package-v1", "complete": True,
        "archive": {"path": ZIP_NAME, "bytes": archive_size, "sha256": archive_sha},
        "inventory": inventory_row,
        "files": [expected_files[name] for name in sorted(expected_files)],
        "total_uncompressed_bytes": graph.total + len(inventory_bytes),
        "verification": "source hashes, independent streaming copies, ZIP writes and complete member reread (including inventory) passed",
    }
    data = _encode_json(manifest)
    with (output / MANIFEST_NAME).open("xb") as target:
        target.write(data)
        target.flush()
        os.fsync(target.fileno())
    return manifest


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--project", type=Path, default=Path(__file__).resolve().parents[2])
    parser.add_argument("--ledger", default=DEFAULT_LEDGER)
    parser.add_argument("--ledger-sha256", required=True)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--check", action="store_true", help="Verify required graph only; write nothing")
    parser.add_argument("--max-files", type=int, default=4096)
    parser.add_argument("--max-bytes", type=int, default=2 << 30)
    args = parser.parse_args(argv)
    try:
        options = {"max_files": args.max_files, "max_bytes": args.max_bytes}
        if args.check:
            if args.output:
                raise ProofError("--check does not accept --output")
            graph = build_graph(args.project, args.ledger, args.ledger_sha256, **options)
            result = {"checked": True, "required_files": len(graph.files), "bytes": graph.total,
                      "metadata_only_observations": len(graph.omissions), "opaque_receipts": len(graph.opaque),
                      "opaque_formats": sorted({str(x["format"]) for x in graph.opaque})}
        else:
            if args.output is None:
                raise ProofError("--output is required for packaging")
            manifest = package(args.project, args.ledger, args.ledger_sha256, args.output, **options)
            result = {"archive": manifest["archive"], "files": len(manifest["files"]),
                      "manifest_sha256": _stream(args.output / MANIFEST_NAME)[0]}
        print(json.dumps(result, indent=2))
        return 0
    except (ProofError, OSError, zipfile.BadZipFile) as exc:
        print(f"proof package refused: {exc}")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
