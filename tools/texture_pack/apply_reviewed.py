#!/usr/bin/env python3
"""Windows-only, journaled two-PNG apply/recovery for an accepted San Diego correction plan.

Nothing runs on import. Apply requires idle games/media/coordinator, both OS locks, exact old anchors and the
immutable planner's accepted native evidence. Recovery restores only our exact newly installed bytes.
"""
from __future__ import annotations

import argparse
from contextlib import contextmanager
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys
import uuid

import install_reviewed as planner
import reviewed_fixes as fixes
import upscale

ROOT = Path(__file__).resolve().parents[2]
FORMAT = "mc3-reviewed-texture-transaction-v1"
COORDINATOR_LOCK = ".local/overnight_pipeline/live/coordinator.lock"
INSTALL_LOCK = ".local/reviewed_texture_install/install.lock"
ACTIVE_MARKER = ".local/reviewed_texture_install/active.json"
ACTIVE_FORMAT = "mc3-reviewed-texture-install-active-v1"


class TransactionFailure(RuntimeError):
    pass


def windows_only():
    if os.name != "nt":
        raise RuntimeError("transactional publication is implemented and tested only on Windows")


def digest_json(value):
    return hashlib.sha256(json.dumps(value, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()).hexdigest()


def json_bytes(value):
    return (json.dumps(value, indent=2, sort_keys=True, allow_nan=False)+"\n").encode("utf-8")


def write_new_bytes(path, value):
    with Path(path).open("xb") as stream:
        stream.write(value)
        stream.flush()
        os.fsync(stream.fileno())


def move_entry(source, destination, *, replace=False):
    """Same-volume directory-entry move, never a write through a target's possibly shared inode."""
    windows_only()
    source, destination = Path(source), Path(destination)
    for path in (source, destination):
        if path.is_symlink() or (hasattr(path, "is_junction") and path.is_junction()):
            raise ValueError("atomic publication cannot move through a symlink/junction")
    if source.stat().st_dev != destination.parent.stat().st_dev:
        raise ValueError("atomic publication/recovery cannot cross filesystem volumes")
    call = ctypes.WinDLL("kernel32", use_last_error=True).MoveFileExW
    call.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, wintypes.DWORD]
    call.restype = wintypes.BOOL
    # No COPY_ALLOWED or reboot operations. Archive/ledger publication never requests replacement.
    if not call(str(source.resolve()), str(destination.resolve()), 8 | (1 if replace else 0)):
        raise ctypes.WinError(ctypes.get_last_error())


def durable_json(path, value):
    path = Path(path)
    temporary = path.parent/("."+path.name+"."+uuid.uuid4().hex+".tmp")
    encoded = json_bytes(value)
    write_new_bytes(temporary, encoded)
    verify_temporary(temporary, path, hashlib.sha256(encoded).hexdigest())
    move_entry(temporary, path, replace=path.exists())


@contextmanager
def exclusion(root):
    # This is the exact byte-lock path used by Coordinator.run(), not a PID-file approximation.
    sys.path.insert(0, str(ROOT/"tools/maintenance"))
    import overnight_pipeline
    with overnight_pipeline.exclusive_lock(checked_path(root, COORDINATOR_LOCK)), \
            overnight_pipeline.exclusive_lock(checked_path(root, INSTALL_LOCK)):
        yield


def idle():
    workers = planner.installation_workers()
    if workers:
        raise RuntimeError("game/media/coordinator workers must close before apply or recovery: "+json.dumps(workers))


def local_directory(root, path):
    path = planner.within(root, path)
    if Path(root)/".local" not in path.parents:
        raise ValueError("transaction evidence must use a dedicated project .local directory")
    return path


def checked_path(root, relative):
    root = Path(root).resolve()
    lexical = root/fixes.relative_path(relative)
    cursor = lexical
    while cursor != root:
        if cursor.is_symlink() or (hasattr(cursor, "is_junction") and cursor.is_junction()):
            raise ValueError("transaction paths cannot traverse symlinks/junctions: "+relative)
        cursor = cursor.parent
    return planner.within(root, lexical)


class Journal:
    def __init__(self, path, value):
        self.path, self.value = Path(path), value

    def save(self):
        durable_json(self.path, self.value)

    def event(self, phase, action, **details):
        self.value["events"].append({"sequence": len(self.value["events"]), "phase": phase, "action": action, **details})
        self.save()


def marker_value(journal):
    intent = journal.value["intent"]
    return {"format": ACTIVE_FORMAT, "nonce": intent["marker_nonce"], "root": intent["root"],
            "journal": intent["transaction_directory"]+"/journal.json", "intent_sha256": journal.value["intent_sha256"]}


def marker_state(journal, root, *, require=False):
    path = checked_path(root, ACTIVE_MARKER)
    if path.exists():
        if not path.is_file() or upscale.file_digest(path) != journal.value["active_marker_sha256"]:
            raise RuntimeError("foreign/changed active transaction marker preserved")
        return path
    if require:
        raise RuntimeError("own active transaction marker disappeared; canonical mutation refused")
    return None


def retire_marker(journal, root, fault=None):
    marker = marker_state(journal, root)
    if marker is None:
        return
    retired = journal.path.parent/("retired-active-"+uuid.uuid4().hex+".json")
    journal.event("intent", "marker-retire", marker=ACTIVE_MARKER, retired=planner.rel(root, retired))
    hook(fault, "before-marker-retire", journal)
    idle()
    if marker_state(journal, root) is not None:
        move_entry(marker, retired)
        hook(fault, "after-marker-retire", journal)
        journal.event("done", "marker-retire", retired=planner.rel(root, retired))


def register_marker(journal, root, fault=None):
    path = checked_path(root, ACTIVE_MARKER)
    temporary = temporary_for(path)
    journal.event("intent", "marker-create", marker=ACTIVE_MARKER, temporary=planner.rel(root, temporary),
                  expected_sha256=journal.value["active_marker_sha256"])
    write_new_bytes(temporary, json_bytes(marker_value(journal)))
    hook(fault, "before-marker-create", journal)
    idle()
    publish_temporary(temporary, path, journal.value["active_marker_sha256"])
    hook(fault, "after-marker-create", journal)
    journal.event("done", "marker-create", marker=ACTIVE_MARKER)


def load_journal(path, root):
    path = planner.within(root, path)
    local_directory(root, path.parent)
    value = json.loads(path.read_text(encoding="utf-8"))
    payload = value.get("intent")
    if (value.get("format") != FORMAT or not isinstance(payload, dict)
            or value.get("intent_sha256") != digest_json(payload)
            or payload.get("root") != str(Path(root).resolve())
            or payload.get("transaction_directory") != planner.rel(root, path.parent) or path.name != "journal.json"
            or payload.get("active_marker") != ACTIVE_MARKER
            or not isinstance(payload.get("marker_nonce"), str) or len(payload["marker_nonce"]) != 32
            or any(c not in "0123456789abcdef" for c in payload["marker_nonce"])):
        raise ValueError("transaction journal identity/immutable intent changed")
    plan, ledger = payload["plan"], payload["ledger"]
    operations = plan["operations"]
    textures = plan["textures"]
    if (plan.get("format") != planner.FORMAT or plan.get("city") != "s" or plan.get("operation") != "dry-run-only"
            or len(operations) != 2 or len({op["name"] for op in operations}) != 2
            or set(ledger.get("entries", {})) != {op["name"] for op in operations}
            or payload.get("ledger_target") != textures+"/reviewed_fixes.json"
            or payload.get("ledger_sha256") != plan.get("proposed_ledger_sha256")):
        raise ValueError("transaction must retain exactly its accepted two-PNG first-adoption intent")
    for operation in operations:
        name = operation["name"]
        entry = ledger["entries"][name]
        if (not fixes.NAME.fullmatch(name) or operation["target"] != textures+"/replace/"+name
                or operation["archive_target"] != entry["base_archive"]
                or not operation["archive_target"].startswith(textures+"/reviewed_archive/")
                or operation["expected_before_sha256"] != entry["before_sha256"]
                or operation["expected_after_sha256"] != entry["after_sha256"]
                or any(not fixes.SHA.fullmatch(operation[key]) for key in
                       ("expected_before_sha256", "expected_after_sha256"))):
            raise ValueError("transaction paths/old/new anchors disagree")
        for key in ("target", "archive_target", "candidate_source", "archive_source"):
            checked_path(root, operation[key])
    checked_path(root, payload["ledger_target"])
    journal = Journal(path, value)
    if value.get("active_marker_sha256") != hashlib.sha256(json_bytes(marker_value(journal))).hexdigest():
        raise ValueError("active marker identity does not match immutable transaction intent")
    return journal


def boundary(journal, root, states, published_archives, ledger_published):
    idle()
    marker_state(journal, root, require=True)
    payload = journal.value["intent"]
    if upscale.file_digest(Path(__file__)) != journal.value["transaction_tool_sha256"]:
        raise ValueError("transaction tool changed during execution")
    if upscale.file_digest(checked_path(root, payload["plan_source"])) != payload["plan_source_sha256"]:
        raise ValueError("accepted plan changed during execution")
    plan, ledger = payload["plan"], payload["ledger"]
    if upscale.file_digest(checked_path(root, plan["proposed_ledger"])) != payload["ledger_sha256"]:
        raise ValueError("proposed ledger changed during execution")
    planner.ProjectedFiles(root, ledger["dependencies"], plan["future_bindings"]).verify_all()
    for operation in plan["operations"]:
        name = operation["name"]
        if upscale.file_digest(checked_path(root, operation["target"])) != states[name]:
            raise ValueError("active target changed at mutation boundary: "+name)
        archive = checked_path(root, operation["archive_target"])
        if name in published_archives:
            if (upscale.file_digest(archive) != operation["expected_before_sha256"]
                    or archive.stat().st_nlink != 1):
                raise ValueError("published independent archive changed")
        elif archive.exists():
            raise ValueError("another archive appeared; never overwrite it")
    path = checked_path(root, payload["ledger_target"])
    if ledger_published:
        if not path.is_file() or upscale.file_digest(path) != payload["ledger_sha256"]:
            raise ValueError("new ledger changed at mutation boundary")
    elif path.exists():
        raise ValueError("unrelated canonical ledger appeared; preserve it")


def temporary_for(target):
    return Path(target).parent/("."+Path(target).name+".reviewed-"+uuid.uuid4().hex+".part")


def verify_temporary(temporary, target, expected):
    temporary, target = Path(temporary), Path(target)
    if (temporary.is_symlink() or (hasattr(temporary, "is_junction") and temporary.is_junction())
            or not temporary.is_file() or temporary.stat().st_nlink != 1
            or upscale.file_digest(temporary) != expected
            or (target.exists() and os.path.samefile(temporary, target))):
        raise ValueError("prepared temporary bytes/link identity changed before publication")


def publish_temporary(temporary, target, expected, *, replace=False, target_sha256=None):
    # This check is deliberately repeated after mutation-boundary hooks. The earlier copy verification alone
    # cannot establish that the exact file about to be renamed still contains our verified independent bytes.
    verify_temporary(temporary, target, expected)
    if replace:
        if not Path(target).is_file() or upscale.file_digest(target) != target_sha256:
            raise ValueError("target anchor changed immediately before directory-entry replacement")
    elif Path(target).exists():
        raise ValueError("publication target already exists; it will not be overwritten")
    move_entry(temporary, target, replace=replace)


def prepare_copy(journal, source, destination, expected, root, label):
    journal.event("intent", "prepare-copy", label=label, source=planner.rel(root, source),
                  temporary=planner.rel(root, destination), expected_sha256=expected)
    planner.independent_copy(source, destination, expected)
    journal.event("done", "prepare-copy", label=label, temporary=planner.rel(root, destination))


def hook(fault, point, journal):
    if fault:
        fault(point, journal)


def recover_locked(journal, root, fault=None):
    idle()
    active = marker_state(journal, root)  # A foreign marker forbids all recovery mutations, even terminal cleanup.
    if journal.value.get("status") == "rolled-back":
        retire_marker(journal, root, fault)
        return journal.value["recovery"]
    if active is None:
        # Explicit rollback after a successful marker retirement also needs a durable crash gate. Otherwise
        # a crash halfway through that rollback would release the OS lock with no marker for the coordinator.
        register_marker(journal, root, fault)
    payload = journal.value["intent"]
    plan = payload["plan"]
    result = {"restored": [], "already_old": [], "conflicts": [], "retired_ledgers": [], "archives_preserved": True}
    journal.value["status"] = "rolling-back"
    journal.save()
    ledger = checked_path(root, payload["ledger_target"])
    if ledger.exists():
        if ledger.is_file() and upscale.file_digest(ledger) == payload["ledger_sha256"]:
            retired = journal.path.parent/("retired-ledger-"+uuid.uuid4().hex+".json")
            journal.event("intent", "retire-ledger", target=payload["ledger_target"], retired=planner.rel(root, retired))
            idle()
            marker_state(journal, root, require=True)
            if upscale.file_digest(ledger) != payload["ledger_sha256"]:
                result["conflicts"].append({"path": payload["ledger_target"], "reason": "ledger changed before retirement"})
            else:
                move_entry(ledger, retired)
                hook(fault, "after-ledger-retire", journal)
                journal.event("done", "retire-ledger", retired=planner.rel(root, retired))
                result["retired_ledgers"].append(planner.rel(root, retired))
        else:
            result["conflicts"].append({"path": payload["ledger_target"], "reason": "unrelated ledger preserved"})
    for operation in reversed(plan["operations"]):
        name = operation["name"]
        target = checked_path(root, operation["target"])
        current = upscale.file_digest(target) if target.is_file() else None
        before, after = operation["expected_before_sha256"], operation["expected_after_sha256"]
        if current == before:
            result["already_old"].append(name)
            continue
        if current != after:
            result["conflicts"].append({"path": operation["target"], "reason": "unrelated replacement/deletion preserved"})
            continue
        archive = checked_path(root, operation["archive_target"])
        staged = checked_path(root, operation["archive_source"])
        source = next((path for path in (archive, staged) if path.is_file() and upscale.file_digest(path) == before), None)
        if source is None:
            result["conflicts"].append({"path": operation["target"], "reason": "no verified old-byte archive; candidate preserved"})
            continue
        idle()
        marker_state(journal, root, require=True)
        temporary = temporary_for(target)
        prepare_copy(journal, source, temporary, before, root, "rollback "+name)
        journal.event("intent", "png-restore", name=name, target=operation["target"], temporary=planner.rel(root, temporary))
        hook(fault, "before-png-restore:"+name, journal)
        idle()
        marker_state(journal, root, require=True)
        if not target.is_file() or upscale.file_digest(target) != after:
            result["conflicts"].append({"path": operation["target"], "reason": "replacement changed before rollback; preserved"})
            continue
        publish_temporary(temporary, target, before, replace=True, target_sha256=after)
        hook(fault, "after-png-restore:"+name, journal)
        if upscale.file_digest(target) != before:
            raise RuntimeError("restored PNG failed its old-byte hash")
        journal.event("done", "png-restore", name=name)
        result["restored"].append(name)
    journal.value["status"] = "recovery-conflicts" if result["conflicts"] else "rolled-back"
    journal.value["recovery"] = result
    journal.save()
    if not result["conflicts"]:
        hook(fault, "after-rollback-status", journal)
        retire_marker(journal, root, fault)
    return result


def recover(journal_path, *, root=ROOT, fault=None):
    windows_only()
    root = Path(root).resolve()
    with exclusion(root):
        idle()
        return recover_locked(load_journal(journal_path, root), root, fault)


def apply(plan_path, transaction_directory, *, root=ROOT, fault=None):
    windows_only()
    root = Path(root).resolve()
    plan_path = planner.within(root, plan_path)
    directory = local_directory(root, transaction_directory)
    if directory.exists():
        raise ValueError("transaction directory must be NEW; use recover for an existing journal")
    with exclusion(root):
        idle()
        if checked_path(root, ACTIVE_MARKER).exists():
            raise RuntimeError("existing active transaction marker preserved; recover its own journal first")
        source_sha = upscale.file_digest(plan_path)
        ready = planner.validate_plan(plan_path, root=root, workers=planner.installation_workers())
        if not ready["install_ready"] or not ready["native_review_accepted"]:
            raise ValueError("accepted native proof and closed workers are required")
        if upscale.file_digest(plan_path) != source_sha:
            raise ValueError("plan changed during accepted evidence validation")
        plan = json.loads(plan_path.read_text(encoding="utf-8"))
        ledger_source = checked_path(root, plan["proposed_ledger"])
        ledger = json.loads(ledger_source.read_text(encoding="utf-8"))
        payload = {"root": str(root), "transaction_directory": planner.rel(root, directory),
                   "plan_source": planner.rel(root, plan_path), "plan_source_sha256": source_sha,
                   "plan": plan, "ledger": ledger, "ledger_target": plan["textures"]+"/reviewed_fixes.json",
                   "ledger_sha256": plan["proposed_ledger_sha256"], "expected_eligible": ready["base_pack_counts"]["eligible"],
                   "active_marker": ACTIVE_MARKER, "marker_nonce": uuid.uuid4().hex}
        directory.mkdir(parents=True)
        journal = Journal(directory/"journal.json", {"format": FORMAT, "intent": payload, "intent_sha256": digest_json(payload),
            "transaction_tool_sha256": upscale.file_digest(Path(__file__)), "status": "applying", "events": []})
        journal.value["active_marker_sha256"] = hashlib.sha256(json_bytes(marker_value(journal))).hexdigest()
        journal.save()
        load_journal(journal.path, root)  # Validate the persisted recovery envelope before any canonical mutation.
        states = {op["name"]: op["expected_before_sha256"] for op in plan["operations"]}
        archives, published = set(), False
        try:
            register_marker(journal, root, fault)
            for operation in plan["operations"]:
                name = operation["name"]
                boundary(journal, root, states, archives, published)
                target = checked_path(root, operation["archive_target"])
                journal.event("intent", "archive-directory", directory=planner.rel(root, target.parent))
                target.parent.mkdir(parents=True, exist_ok=True)
                journal.event("done", "archive-directory", directory=planner.rel(root, target.parent))
                temporary = temporary_for(target)
                prepare_copy(journal, checked_path(root, operation["archive_source"]), temporary,
                             operation["expected_before_sha256"], root, "archive "+name)
                journal.event("intent", "archive-publish", name=name, target=operation["archive_target"], temporary=planner.rel(root, temporary))
                hook(fault, "before-archive-publish:"+name, journal)
                boundary(journal, root, states, archives, published)
                publish_temporary(temporary, target, operation["expected_before_sha256"])
                archives.add(name)
                if upscale.file_digest(target) != operation["expected_before_sha256"] or target.stat().st_nlink != 1:
                    raise RuntimeError("canonical independent archive failed verification")
                journal.event("done", "archive-publish", name=name)
            for operation in plan["operations"]:
                name = operation["name"]
                boundary(journal, root, states, archives, published)
                target = checked_path(root, operation["target"])
                temporary = temporary_for(target)
                prepare_copy(journal, checked_path(root, operation["candidate_source"]), temporary,
                             operation["expected_after_sha256"], root, "candidate "+name)
                journal.event("intent", "png-swap", name=name, target=operation["target"], temporary=planner.rel(root, temporary))
                hook(fault, "before-png-swap:"+name, journal)
                boundary(journal, root, states, archives, published)
                publish_temporary(temporary, target, operation["expected_after_sha256"], replace=True,
                                  target_sha256=states[name])
                states[name] = operation["expected_after_sha256"]
                hook(fault, "after-png-swap:"+name, journal)  # A crash here leaves an intent with no completion.
                journal.event("done", "png-swap", name=name)
            boundary(journal, root, states, archives, published)
            target = checked_path(root, payload["ledger_target"])
            temporary = temporary_for(target)
            prepare_copy(journal, ledger_source, temporary, payload["ledger_sha256"], root, "exact proposed ledger")
            journal.event("intent", "ledger-publish", target=payload["ledger_target"], temporary=planner.rel(root, temporary))
            hook(fault, "before-ledger-publish", journal)
            boundary(journal, root, states, archives, published)
            publish_temporary(temporary, target, payload["ledger_sha256"])
            published = True
            hook(fault, "after-ledger-publish", journal)
            journal.event("done", "ledger-publish")
            boundary(journal, root, states, archives, published)
            report = fixes.audit(checked_path(root, plan["textures"]), ledger["base_model"], ledger["base_pass_id"], root=root)
            counts = report.get("counts", {})
            if (report.get("complete") is not True or report.get("base_complete") is not True
                    or counts.get("base_named_pass_verified") != payload["expected_eligible"]
                    or counts.get("current_original_named_pass") != payload["expected_eligible"]-2
                    or counts.get("reviewed_fixes") != 2 or counts.get("accounted_changed_pass_records") != 2
                    or set(report.get("reviewed_fixes", {})) != set(states)):
                raise RuntimeError("post-apply full reviewed audit did not verify the exact original-plus-two pack")
            boundary(journal, root, states, archives, published)
            durable_json(directory/"post-apply-audit.json", report)
            journal.value["status"] = "committed"
            journal.value["post_apply_audit_sha256"] = upscale.file_digest(directory/"post-apply-audit.json")
            journal.save()
            hook(fault, "after-commit-status", journal)
            retire_marker(journal, root, fault)
            return {"status": "committed", "journal": str(journal.path), "reviewed_fixes": 2,
                    "eligible": payload["expected_eligible"], "archive_bytes_preserved": True}
        except Exception as error:
            journal.value["error"] = str(error)
            journal.value["status"] = "failed"
            journal.save()
            try:
                result = recover_locked(journal, root)
            except Exception as rollback_error:
                journal.value["status"] = "recovery-pending"
                journal.value["recovery_error"] = str(rollback_error)
                journal.save()
                raise TransactionFailure("apply failed; recovery remains pending at "+str(journal.path)) from error
            raise TransactionFailure("apply failed; "+journal.value["status"]+" at "+str(journal.path)+": "+json.dumps(result)) from error


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument("--apply", type=Path, help="Accepted immutable install-plan.json; changes exactly two canonical PNGs and publishes its ledger")
    group.add_argument("--recover", type=Path, help="Own transaction journal; conditionally restores old PNGs and retires only its exact ledger")
    parser.add_argument("--transaction-directory", type=Path, help="NEW dedicated .local evidence directory, required with --apply")
    args = parser.parse_args()
    if args.apply and not args.transaction_directory:
        parser.error("--apply requires a NEW --transaction-directory")
    result = apply(args.apply, args.transaction_directory) if args.apply else recover(args.recover)
    print(json.dumps(result, indent=2))
    return 2 if result.get("conflicts") else 0


if __name__ == "__main__":
    raise SystemExit(main())
