#!/usr/bin/env python3
"""Trust boundary for the event-driven Files swarm coordinator.

The core reconciler is intentionally pure about GitHub facts. This entrypoint
adds the repository's trust policy before executing it so an arbitrary public
PR reviewer cannot steer the swarm by writing "P1" or submitting an approval.
"""

from __future__ import annotations

import os

try:
    from . import coordinator as core
except ImportError:  # Executed directly as `python3 swarm/coordinator_entry.py`.
    import coordinator as core


def _configured_reviewers() -> set[str]:
    configured = os.environ.get("SWARM_TRUSTED_REVIEWERS", "The-First-Infiltrator")
    return {item.strip() for item in configured.split(",") if item.strip()}


TRUSTED_REVIEWERS = _configured_reviewers()
TRUSTED_THREAD_AUTHORS = TRUSTED_REVIEWERS | {
    "chatgpt-codex-connector",
    "chatgpt-codex-connector[bot]",
}


def trusted_thread_is_hard(thread):
    if thread.get("isResolved") or thread.get("isOutdated"):
        return False
    comments = ((thread.get("comments") or {}).get("nodes") or [])
    for comment in comments:
        author = ((comment.get("author") or {}).get("login") or "")
        if author not in TRUSTED_THREAD_AUTHORS:
            continue
        if core.HARD_RE.search(comment.get("body") or ""):
            return True
    return False


def trusted_select_exact_head_review(reviews, head_sha):
    exact = []
    for review in reviews:
        if review.get("commit_id") != head_sha:
            continue
        login = ((review.get("user") or {}).get("login") or "")
        if login not in TRUSTED_REVIEWERS:
            continue
        body = review.get("body") or ""
        state = (review.get("state") or "").upper()
        if (
            core.APPROVED_MARKER in body
            or core.CHANGES_MARKER in body
            or state == "APPROVED"
        ):
            exact.append(review)
    if not exact:
        return None
    exact.sort(key=lambda review: review.get("submitted_at") or "")
    return exact[-1]


def trusted_review_protocol_verdict(review):
    if not review:
        return None
    login = ((review.get("user") or {}).get("login") or "")
    if login not in TRUSTED_REVIEWERS:
        return None
    body = (review.get("body") or "").strip()
    state = (review.get("state") or "").upper()
    if core.CHANGES_MARKER in body:
        return "CHANGES_REQUIRED"
    if core.APPROVED_MARKER in body or state == "APPROVED":
        return "APPROVED"
    return None


# Apply policy before any live-fact reconciliation executes. fetch_live_facts
# resolves these module globals at call time, so this preserves the tested core
# logic while keeping the public-repository trust boundary explicit and small.
core.thread_is_hard = trusted_thread_is_hard
core.select_exact_head_review = trusted_select_exact_head_review
core.review_protocol_verdict = trusted_review_protocol_verdict


if __name__ == "__main__":
    raise SystemExit(core.main())
