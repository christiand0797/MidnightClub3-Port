"""Synthetic CPU-only tests for gate_receipt.py; never run audits, copies or touch live jobs."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest

HERE = Path(__file__).parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, HERE / f"{name}.py")
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


G = load("gate_receipt")
V = load("vehicle_pack")


def row(n, **over):
    r = {"movie": f"m/{n}.bik", "frames": 10, "status": "verified", "output_validated": True,
         "recorded_model": "remacri-4x", "origin": "original-bink", "historical_source_and_weights_verified": True}
    r.update(over)
    return r


def report(rows=None, **over):
    rows = [row(n) for n in range(106)] if rows is None else rows
    r = {"mode": "audit", "requested_model": "remacri-4x", "require_original_provenance": True,
         "movies": len(rows), "covers_all_sources": True, "failed": 0, "all_requested_model_validated": True,
         "all_source_movies_validated": True, "all_original_source_and_weights_verified": True, "details": rows}
    r.update(over)
    return json.dumps(r).encode()


def marker(report_bytes, **over):
    m = {"status": "verified", "manifest_sha256": "b" * 64, "files": 3, "bytes": 30, "destination": "G:/x",
         "media_gate": {"audit_report_sha256": hashlib.sha256(report_bytes).hexdigest()}}
    m.update(over)
    return json.dumps(m).encode()


class GateReceiptTest(unittest.TestCase):
    def test_receipt_passes_vehicle_pack_gate(self):
        rep = report()
        data = G.compose_receipt(rep, marker(rep))
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "gate.json"
            path.write_bytes(data)
            digest = hashlib.sha256(data).hexdigest()
            self.assertEqual(V.validate_gate(path, digest), digest)
        receipt = json.loads(data)
        self.assertEqual(receipt["strict_movies"]["final_audit_sha256"], hashlib.sha256(rep).hexdigest())

    def test_incomplete_audits_are_refused(self):
        cases = [report([row(n) for n in range(105)]),
                 report([row(n) for n in range(105)] + [row(105, recorded_model="realesrgan-x4plus")]),
                 report([row(n) for n in range(105)] + [row(105, origin="unrecorded")]),
                 report([row(n) for n in range(105)] + [row(105, status="failed")], failed=1),
                 report(requested_model="realesrgan-x4plus"),
                 report(mode="marker-inventory")]
        for rep in cases:
            with self.assertRaises(G.GateError):
                G.compose_receipt(rep, marker(rep))

    def test_backup_must_follow_this_audit(self):
        rep = report()
        with self.assertRaises(G.GateError):
            G.compose_receipt(rep, marker(report(), media_gate={"audit_report_sha256": "c" * 64}))
        with self.assertRaises(G.GateError):
            G.compose_receipt(rep, marker(rep, status="copying"))


if __name__ == "__main__":
    unittest.main()
