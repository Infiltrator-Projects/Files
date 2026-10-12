#!/usr/bin/env python3
"""Event-driven reconciler for the Files swarm.

This program never edits product source, reviews code, fixes code, merges, versions,
or releases. It only reconciles objective GitHub facts into swarm/state.json so
the scheduled role workers receive the correct baton.
"""

from __future__ import annotations

import base64
import copy
import datetime as dt
import json
import os
import re
import sys
import urllib.error
import urllib.parse
import urllib.request
from typing import Any, Dict, Iterable, List, Optional, Tuple

CONTROL_REF = "swarm/control"
STATE_PATH = "swarm/state.json"
COORDINATOR_ISSUE = 2
API = "https://api.github.com"
GRAPHQL = "https://api.github.com/graphql"

HARD_RE = re.compile(
    r"(?im)(?:\bHARD\b|\bP1\b|\bP2\b|P1 Badge|P2 Badge|REVIEWER VERDICT:\s*CHANGES REQUIRED)"
)
APPROVED_MARKER = "REVIEWER VERDICT: APPROVED FOR FIXER/INTEGRATOR"
CHANGES_MARKER = "REVIEWER VERDICT: CHANGES REQUIRED"


def utc_now() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds").replace("+00:00", "Z")


def first_nonempty_line(text: str) -> str:
    for line in (text or "").splitlines():
        value = line.strip()
        if value:
            return value[:300]
    return ""


def thread_is_hard(thread: Dict[str, Any]) -> bool:
    if thread.get("isResolved") or thread.get("isOutdated"):
        return False
    comments = ((thread.get("comments") or {}).get("nodes") or [])
    return any(HARD_RE.search(comment.get("body") or "") for comment in comments)


def hard_thread_summary(thread: Dict[str, Any]) -> Dict[str, Any]:
    comments = ((thread.get("comments") or {}).get("nodes") or [])
    body = next((c.get("body") or "" for c in comments if (c.get("body") or "").strip()), "")
    severity = (
        "P1"
        if re.search(r"(?i)\bP1\b|P1 Badge", body)
        else "P2"
        if re.search(r"(?i)\bP2\b|P2 Badge", body)
        else "HARD"
    )
    return {
        "id": thread.get("id"),
        "severity": severity,
        "path": thread.get("path"),
        "line": thread.get("line"),
        "summary": first_nonempty_line(body),
    }


def select_exact_head_review(
    reviews: Iterable[Dict[str, Any]], head_sha: str
) -> Optional[Dict[str, Any]]:
    exact = [r for r in reviews if r.get("commit_id") == head_sha]
    if not exact:
        return None
    exact.sort(key=lambda r: r.get("submitted_at") or "")
    return exact[-1]


def review_protocol_verdict(review: Optional[Dict[str, Any]]) -> Optional[str]:
    if not review:
        return None
    body = (review.get("body") or "").strip()
    state = (review.get("state") or "").upper()
    if CHANGES_MARKER in body:
        return "CHANGES_REQUIRED"
    if APPROVED_MARKER in body or state == "APPROVED":
        return "APPROVED"
    return None


def run_number(run: Dict[str, Any]) -> int:
    try:
        return int(run.get("run_number") or 0)
    except (TypeError, ValueError):
        return 0


def select_verify_run(
    runs: Iterable[Dict[str, Any]], head_sha: str
) -> Optional[Dict[str, Any]]:
    exact = [
        run
        for run in runs
        if run.get("head_sha") == head_sha and run.get("name") == "Verify"
    ]
    if not exact:
        return None
    exact.sort(key=lambda r: (run_number(r), r.get("created_at") or ""))
    return exact[-1]


def reconcile_snapshot(
    original: Dict[str, Any],
    *,
    main_sha: str,
    pr: Dict[str, Any],
    hard_threads: List[Dict[str, Any]],
    exact_review: Optional[Dict[str, Any]],
    verify_run: Optional[Dict[str, Any]],
    event_name: str,
    now: str,
) -> Tuple[Dict[str, Any], bool, Optional[Tuple[str, str]]]:
    """Return (new_state, changed, phase_transition).

    Forward product decisions remain role-owned. The coordinator only applies
    deterministic safety routing and reconstructs already-proven readiness.
    """
    state = copy.deepcopy(original)
    if state.get("mode") != "WORK":
        return state, False, None

    stroke = state.get("stroke") or {}
    candidate = stroke.get("candidate") or {}
    old_phase = stroke.get("phase")
    old_head = candidate.get("head_sha")
    head_sha = ((pr.get("head") or {}).get("sha") or pr.get("head_sha") or "")
    head_changed = bool(head_sha and old_head and head_sha != old_head)

    candidate["head_sha"] = head_sha or old_head
    candidate["draft"] = bool(pr.get("draft", candidate.get("draft", False)))
    if "commits" in pr:
        candidate["commits"] = pr.get("commits")
    if "changed_files" in pr:
        candidate["changed_files"] = pr.get("changed_files")

    ci_success = bool(
        verify_run
        and verify_run.get("status") == "completed"
        and verify_run.get("conclusion") == "success"
        and verify_run.get("head_sha") == candidate.get("head_sha")
    )
    ci_record = None
    if verify_run:
        ci_record = {
            "run_id": verify_run.get("id"),
            "workflow": verify_run.get("name"),
            "run_number": verify_run.get("run_number"),
            "status": verify_run.get("status"),
            "conclusion": verify_run.get("conclusion"),
            "head_sha": verify_run.get("head_sha"),
        }
        candidate["ci"] = ci_record

    review = stroke.setdefault("review", {})
    hard_details = [hard_thread_summary(thread) for thread in hard_threads]
    review["unresolved_threads"] = [
        item["id"] for item in hard_details if item.get("id")
    ]
    review["unresolved_thread_details"] = hard_details

    exact_review_record = None
    verdict = review_protocol_verdict(exact_review)
    if exact_review:
        exact_review_record = {
            "review_id": exact_review.get("node_id") or exact_review.get("id"),
            "database_id": (
                exact_review.get("id") if isinstance(exact_review.get("id"), int) else None
            ),
            "reviewed_sha": exact_review.get("commit_id"),
            "state": exact_review.get("state"),
            "submitted_at": exact_review.get("submitted_at"),
            "protocol_verdict": verdict,
        }
        review["exact_head_review"] = exact_review_record
    elif head_changed:
        review["exact_head_review"] = None

    if ci_record:
        review["exact_head_ci"] = ci_record
    elif head_changed:
        review["exact_head_ci"] = None

    phase = old_phase
    blocker: Optional[Dict[str, Any]] = stroke.get("blocker")

    merged = bool(pr.get("merged") or pr.get("merged_at"))
    pr_state = pr.get("state")

    if merged and old_phase != "RELEASED":
        phase = "INTEGRATING"
        blocker = {
            "kind": "merged_candidate_needs_integrator_reconciliation",
            "action": "integrate",
            "reason": (
                "The active candidate is already merged on GitHub; Integrator must "
                "reconcile merge/release/publication truth before any new stroke."
            ),
            "since": now,
            "next_action": (
                "Integrator reconciles exact merge SHA, VERSION, tag/release and "
                "publication before RELEASED."
            ),
        }
    elif hard_details or verdict == "CHANGES_REQUIRED":
        phase = "FIX_REQUIRED"
        reason_bits = []
        if hard_details:
            reason_bits.append(
                f"{len(hard_details)} unresolved candidate-local HARD review thread(s): "
                + ", ".join(
                    item["id"] for item in hard_details if item.get("id")
                )
            )
        if verdict == "CHANGES_REQUIRED":
            reason_bits.append("exact-head formal Reviewer verdict requires changes")
        blocker = {
            "kind": "candidate_local_hard_findings",
            "action": "fix",
            "reason": "; ".join(reason_bits),
            "since": now,
            "next_action": (
                "Fixer closes the complete live HARD set in the same PR, obtains "
                "exact-head Verify, then hands changed code to REVIEW_READY."
            ),
        }
        review["verdict"] = "CHANGES_REQUIRED"
        review["action"] = "FIX_REQUIRED"
        review["reviewed_sha"] = exact_review.get("commit_id") if exact_review else None
        review["review_id"] = (
            (exact_review.get("node_id") or exact_review.get("id"))
            if exact_review
            else None
        )
    elif verdict == "APPROVED" and ci_success and pr_state == "open":
        phase = "INTEGRATE_READY"
        blocker = None
        review["verdict"] = "APPROVED"
        review["action"] = "INTEGRATE"
        review["reviewed_sha"] = (
            exact_review.get("commit_id") if exact_review else candidate.get("head_sha")
        )
        review["review_id"] = (
            (exact_review.get("node_id") or exact_review.get("id"))
            if exact_review
            else None
        )
    elif head_changed:
        # A changed head cannot inherit integration readiness. We do not
        # manufacture Builder/Fixer completion; we only invalidate old evidence.
        if old_phase in ("INTEGRATE_READY", "INTEGRATING"):
            phase = "REVIEW_READY" if ci_success else "REVIEWING"
        review["verdict"] = "PENDING_PROTOCOL_REVIEW"
        review["action"] = "REVIEW_REQUIRED"
        review["reviewed_sha"] = None
        review["review_id"] = None
        blocker = {
            "kind": "head_changed_requires_requalification",
            "action": "review" if ci_success else "verify",
            "reason": (
                f"Candidate head changed from {old_head} to {candidate.get('head_sha')}; "
                "old review/CI readiness cannot carry forward."
            ),
            "since": now,
            "next_action": (
                "Reviewer performs a fresh exact-head formal review."
                if ci_success
                else "Wait for exact-head Verify, then require a fresh formal review."
            ),
        }

    stroke["phase"] = phase
    stroke["candidate"] = candidate
    stroke["blocker"] = blocker
    stroke["reconciliation"] = {
        "last_reconciled_at": now,
        "source": "github-actions",
        "event": event_name,
        "main_sha": main_sha,
        "candidate_head": candidate.get("head_sha"),
        "candidate_pr_state": pr_state,
        "candidate_merged": merged,
        "exact_head_ci_run": ci_record,
        "exact_head_review": exact_review_record,
        "unresolved_hard_threads": hard_details,
    }
    state["stroke"] = stroke

    candidate_compare = copy.deepcopy(state)
    original_compare = copy.deepcopy(original)
    for item in (candidate_compare, original_compare):
        item.pop("revision", None)
        item.pop("updated_at", None)
        recon = ((item.get("stroke") or {}).get("reconciliation"))
        if recon:
            recon.pop("last_reconciled_at", None)
            recon.pop("event", None)

    changed = candidate_compare != original_compare
    transition = (old_phase, phase) if phase != old_phase else None
    if changed:
        state["revision"] = int(original.get("revision") or 0) + 1
        state["updated_at"] = now
    return state, changed, transition


class GitHub:
    def __init__(self, token: str, repository: str) -> None:
        self.token = token
        self.repository = repository
        self.owner, self.name = repository.split("/", 1)

    def request(
        self, method: str, url: str, payload: Optional[Dict[str, Any]] = None
    ) -> Any:
        data = None if payload is None else json.dumps(payload).encode("utf-8")
        request = urllib.request.Request(
            url,
            data=data,
            method=method,
            headers={
                "Authorization": f"Bearer {self.token}",
                "Accept": "application/vnd.github+json",
                "X-GitHub-Api-Version": "2022-11-28",
                "User-Agent": "files-swarm-coordinator",
            },
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read()
                return json.loads(body.decode("utf-8")) if body else None
        except urllib.error.HTTPError as error:
            detail = error.read().decode("utf-8", errors="replace")
            raise RuntimeError(
                f"GitHub {method} {url} failed: HTTP {error.code}: {detail}"
            ) from error

    def get(self, path: str) -> Any:
        return self.request("GET", f"{API}{path}")

    def post(self, path: str, payload: Dict[str, Any]) -> Any:
        return self.request("POST", f"{API}{path}", payload)

    def put(self, path: str, payload: Dict[str, Any]) -> Any:
        return self.request("PUT", f"{API}{path}", payload)

    def graphql(self, query: str, variables: Dict[str, Any]) -> Any:
        result = self.request("POST", GRAPHQL, {"query": query, "variables": variables})
        if result.get("errors"):
            raise RuntimeError(f"GitHub GraphQL failed: {json.dumps(result['errors'])}")
        return result["data"]

    def read_state(self) -> Tuple[Dict[str, Any], str]:
        encoded_ref = urllib.parse.quote(CONTROL_REF, safe="")
        content = self.get(
            f"/repos/{self.repository}/contents/{STATE_PATH}?ref={encoded_ref}"
        )
        raw = base64.b64decode(content["content"]).decode("utf-8")
        return json.loads(raw), content["sha"]

    def write_state(self, state: Dict[str, Any], blob_sha: str) -> None:
        content = json.dumps(state, indent=2, ensure_ascii=False) + "\n"
        self.put(
            f"/repos/{self.repository}/contents/{STATE_PATH}",
            {
                "message": f"Swarm coordinator reconcile revision {state['revision']}",
                "content": base64.b64encode(content.encode("utf-8")).decode("ascii"),
                "sha": blob_sha,
                "branch": CONTROL_REF,
            },
        )

    def review_threads(self, pr_number: int) -> List[Dict[str, Any]]:
        query = """
        query($owner:String!, $name:String!, $number:Int!, $cursor:String) {
          repository(owner:$owner, name:$name) {
            pullRequest(number:$number) {
              reviewThreads(first:100, after:$cursor) {
                pageInfo { hasNextPage endCursor }
                nodes {
                  id isResolved isOutdated path line
                  comments(first:100) {
                    nodes { body createdAt author { login } }
                  }
                }
              }
            }
          }
        }
        """
        cursor = None
        nodes: List[Dict[str, Any]] = []
        while True:
            data = self.graphql(
                query,
                {
                    "owner": self.owner,
                    "name": self.name,
                    "number": pr_number,
                    "cursor": cursor,
                },
            )
            threads = data["repository"]["pullRequest"]["reviewThreads"]
            nodes.extend(threads["nodes"])
            if not threads["pageInfo"]["hasNextPage"]:
                return nodes
            cursor = threads["pageInfo"]["endCursor"]

    def comment_issue(self, issue_number: int, body: str) -> None:
        self.post(
            f"/repos/{self.repository}/issues/{issue_number}/comments", {"body": body}
        )


def fetch_live_facts(gh: GitHub, state: Dict[str, Any]) -> Dict[str, Any]:
    stroke = state.get("stroke") or {}
    candidate = stroke.get("candidate") or {}
    pr_number = int(candidate.get("pr") or 0)
    if not pr_number:
        raise RuntimeError("Active swarm state has no candidate PR number")

    pr = gh.get(f"/repos/{gh.repository}/pulls/{pr_number}")
    head_sha = pr["head"]["sha"]
    main = gh.get(f"/repos/{gh.repository}/branches/main")
    reviews = gh.get(f"/repos/{gh.repository}/pulls/{pr_number}/reviews?per_page=100")
    threads = gh.review_threads(pr_number)
    runs = gh.get(
        f"/repos/{gh.repository}/actions/runs?"
        + urllib.parse.urlencode(
            {"head_sha": head_sha, "event": "pull_request", "per_page": 100}
        )
    )
    return {
        "pr_number": pr_number,
        "pr": pr,
        "main_sha": main["commit"]["sha"],
        "hard_threads": [thread for thread in threads if thread_is_hard(thread)],
        "exact_review": select_exact_head_review(reviews, head_sha),
        "verify_run": select_verify_run(runs.get("workflow_runs") or [], head_sha),
    }


def phase_comment(
    *,
    stroke_id: str,
    pr_number: int,
    head_sha: str,
    transition: Tuple[str, str],
    hard_threads: List[Dict[str, Any]],
    verify_run: Optional[Dict[str, Any]],
    event_name: str,
) -> str:
    old_phase, new_phase = transition
    thread_ids = [thread.get("id") for thread in hard_threads if thread.get("id")]
    ci = "none"
    if verify_run:
        ci = (
            f"Verify #{verify_run.get('run_number')} "
            f"{verify_run.get('conclusion') or verify_run.get('status')}"
        )
    next_role = {
        "FIX_REQUIRED": "Fixer",
        "REVIEW_READY": "Reviewer",
        "INTEGRATE_READY": "Integrator",
        "INTEGRATING": "Integrator",
    }.get(new_phase, "current phase owner")
    return (
        "SWARM COORDINATOR RECONCILIATION\n\n"
        f"stroke: `{stroke_id}`\n"
        f"phase: `{old_phase}` → `{new_phase}`\n"
        f"candidate: PR #{pr_number} @ `{head_sha}`\n"
        f"trigger: `{event_name}`\n"
        f"exact-head CI: {ci}\n"
        f"unresolved HARD threads: {', '.join(thread_ids) if thread_ids else 'none'}\n"
        f"next: {next_role} owns the baton under protocol revision 3.\n\n"
        "This is objective GitHub-state reconciliation only; it does not claim "
        "implementation, review, merge, release or publication work that has not occurred."
    )


def main() -> int:
    token = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN")
    repository = os.environ.get("GITHUB_REPOSITORY")
    event_name = os.environ.get("GITHUB_EVENT_NAME", "manual")
    if not token or not repository:
        print(
            "GH_TOKEN/GITHUB_TOKEN and GITHUB_REPOSITORY are required", file=sys.stderr
        )
        return 2

    gh = GitHub(token, repository)
    for attempt in range(3):
        state, blob_sha = gh.read_state()
        if state.get("mode") != "WORK":
            print(f"Swarm mode is {state.get('mode')}; coordinator made no changes.")
            return 0

        facts = fetch_live_facts(gh, state)
        now = utc_now()
        updated, changed, transition = reconcile_snapshot(
            state,
            main_sha=facts["main_sha"],
            pr=facts["pr"],
            hard_threads=facts["hard_threads"],
            exact_review=facts["exact_review"],
            verify_run=facts["verify_run"],
            event_name=event_name,
            now=now,
        )
        if not changed:
            print(
                f"Reconciled PR #{facts['pr_number']} @ {facts['pr']['head']['sha']}: "
                "state already matches live GitHub."
            )
            return 0

        try:
            gh.write_state(updated, blob_sha)
        except RuntimeError as error:
            text = str(error)
            if ("HTTP 409" in text or "HTTP 422" in text) and attempt < 2:
                print(f"State raced with another writer; refetching ({attempt + 1}/3).")
                continue
            raise

        confirmed, _ = gh.read_state()
        if confirmed.get("revision") != updated.get("revision"):
            raise RuntimeError(
                f"State read-back revision mismatch: expected {updated.get('revision')}, "
                f"found {confirmed.get('revision')}"
            )
        recon = ((confirmed.get("stroke") or {}).get("reconciliation") or {})
        if recon.get("candidate_head") != facts["pr"]["head"]["sha"]:
            raise RuntimeError(
                "State read-back candidate head does not match reconciled GitHub head"
            )

        print(
            f"Reconciled revision {confirmed['revision']}: "
            f"{state.get('stroke', {}).get('phase')} -> "
            f"{confirmed.get('stroke', {}).get('phase')}"
        )
        if transition:
            gh.comment_issue(
                COORDINATOR_ISSUE,
                phase_comment(
                    stroke_id=(confirmed.get("stroke") or {}).get("id", "unknown"),
                    pr_number=facts["pr_number"],
                    head_sha=facts["pr"]["head"]["sha"],
                    transition=transition,
                    hard_threads=facts["hard_threads"],
                    verify_run=facts["verify_run"],
                    event_name=event_name,
                ),
            )
        return 0

    raise RuntimeError(
        "Failed to update swarm state after three expected-SHA reconciliation attempts"
    )


if __name__ == "__main__":
    raise SystemExit(main())
