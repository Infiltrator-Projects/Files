import unittest

from swarm.coordinator import reconcile_snapshot, thread_is_hard


def base_state(phase="REVIEW_READY", head="aaa"):
    return {
        "schema_version": 2,
        "revision": 10,
        "mode": "WORK",
        "updated_at": "old",
        "stroke": {
            "id": "stroke",
            "phase": phase,
            "candidate": {"pr": 12, "head_sha": head, "draft": False},
            "review": {
                "review_id": None,
                "reviewed_sha": None,
                "verdict": "PENDING_PROTOCOL_REVIEW",
                "action": "REVIEW_REQUIRED",
                "unresolved_threads": [],
            },
            "blocker": None,
        },
    }


def pr(head="aaa", state="open", merged=False):
    return {
        "head": {"sha": head},
        "state": state,
        "draft": False,
        "merged": merged,
        "merged_at": "2026-01-01T00:00:00Z" if merged else None,
        "commits": 3,
        "changed_files": 2,
    }


def p2_thread(thread_id="T1", *, resolved=False, outdated=False):
    return {
        "id": thread_id,
        "isResolved": resolved,
        "isOutdated": outdated,
        "path": "src/x.cpp",
        "line": 42,
        "comments": {"nodes": [{"body": "**P2 Badge** HARD: race remains"}]},
    }


def green_ci(head="aaa"):
    return {
        "id": 99,
        "name": "Verify",
        "run_number": 7,
        "status": "completed",
        "conclusion": "success",
        "head_sha": head,
    }


class CoordinatorTests(unittest.TestCase):
    def test_hard_thread_filter_ignores_resolved_and_outdated(self):
        self.assertTrue(thread_is_hard(p2_thread()))
        self.assertFalse(thread_is_hard(p2_thread(resolved=True)))
        self.assertFalse(thread_is_hard(p2_thread(outdated=True)))

    def test_live_hard_thread_forces_fix_required(self):
        original = base_state("REVIEW_READY")
        updated, changed, transition = reconcile_snapshot(
            original,
            main_sha="main",
            pr=pr(),
            hard_threads=[p2_thread("T-live")],
            exact_review=None,
            verify_run=green_ci(),
            event_name="pull_request_review",
            now="2026-01-01T00:00:00.000Z",
        )
        self.assertTrue(changed)
        self.assertEqual(("REVIEW_READY", "FIX_REQUIRED"), transition)
        self.assertEqual("FIX_REQUIRED", updated["stroke"]["phase"])
        self.assertEqual(["T-live"], updated["stroke"]["review"]["unresolved_threads"])
        self.assertEqual(11, updated["revision"])

    def test_exact_head_approval_and_green_ci_reconstruct_integration_readiness(self):
        original = base_state("REVIEW_READY")
        review = {
            "id": 123,
            "node_id": "R1",
            "commit_id": "aaa",
            "state": "COMMENTED",
            "submitted_at": "2026-01-01T00:00:00Z",
            "body": "REVIEWER VERDICT: APPROVED FOR FIXER/INTEGRATOR\nall clear",
            "user": {"login": "The-First-Infiltrator"},
        }
        updated, changed, transition = reconcile_snapshot(
            original,
            main_sha="main",
            pr=pr(),
            hard_threads=[],
            exact_review=review,
            verify_run=green_ci(),
            event_name="pull_request_review",
            now="2026-01-01T00:00:00.000Z",
        )
        self.assertTrue(changed)
        self.assertEqual(("REVIEW_READY", "INTEGRATE_READY"), transition)
        self.assertEqual("INTEGRATE_READY", updated["stroke"]["phase"])
        self.assertIsNone(updated["stroke"]["blocker"])

    def test_changed_head_invalidates_integration_readiness(self):
        original = base_state("INTEGRATE_READY", head="old")
        original["stroke"]["review"]["review_id"] = "old-review"
        original["stroke"]["review"]["reviewed_sha"] = "old"
        original["stroke"]["review"]["verdict"] = "APPROVED"
        updated, changed, transition = reconcile_snapshot(
            original,
            main_sha="main",
            pr=pr(head="new"),
            hard_threads=[],
            exact_review=None,
            verify_run=green_ci(head="new"),
            event_name="pull_request",
            now="2026-01-01T00:00:00.000Z",
        )
        self.assertTrue(changed)
        self.assertEqual(("INTEGRATE_READY", "REVIEW_READY"), transition)
        self.assertEqual("new", updated["stroke"]["candidate"]["head_sha"])
        self.assertIsNone(updated["stroke"]["review"]["review_id"])
        self.assertEqual("PENDING_PROTOCOL_REVIEW", updated["stroke"]["review"]["verdict"])

    def test_merged_candidate_routes_to_integrator(self):
        original = base_state("INTEGRATE_READY")
        updated, changed, transition = reconcile_snapshot(
            original,
            main_sha="main",
            pr=pr(state="closed", merged=True),
            hard_threads=[],
            exact_review=None,
            verify_run=green_ci(),
            event_name="pull_request",
            now="2026-01-01T00:00:00.000Z",
        )
        self.assertTrue(changed)
        self.assertEqual(("INTEGRATE_READY", "INTEGRATING"), transition)
        self.assertEqual("INTEGRATING", updated["stroke"]["phase"])


if __name__ == "__main__":
    unittest.main()
