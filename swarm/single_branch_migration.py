#!/usr/bin/env python3
from __future__ import annotations

import json
import os
from pathlib import Path
import subprocess
import sys

REPO = os.environ.get("GITHUB_REPOSITORY", "Infiltrator-Projects/Infiltrator-File-Manager")

STALE = {
    "builder/retained-evidence-integrity": "65e349a1259b5440464dc14b361e62bba2d5295b",
    "feature/view-modes": "4f511915c86c6ba2249a157c244b8e22e55e67db",
    "fix/detail-sorting-temporal-policy": "0481cc1bb520875db3a7b4be2116892c1802123a",
    "fix/temporal-policy-20261010": "1b2db5e8e230279a83e77d9750408bf03419c724",
    "swarm/builder-detail-list-metadata": "8a1448d13cda840f5608cfb3cc527af1475b8d99",
    "swarm/builder-mounted-places": "ced9e615d26216e5aa2e21b7e49b3891f8b8569f",
    "swarm/destination-ownership": "4052517c8f58158a0b3e3131bdff8bed90abad42",
    "swarm/transfer-failure-residue": "cfa325b4e0d69a35f9c0b31ac6ee3518514ba53d",
}
ANCESTORS = [name for name in STALE if name != "swarm/transfer-failure-residue"]
CONTROL_BASE = "8933ff91f4d1c1bea2410a123db92a39ef80b206"


def run(*args: str, capture: bool = False) -> str:
    result = subprocess.run(args, check=True, text=True, capture_output=capture)
    return result.stdout.strip() if capture else ""


def patch_coordinator() -> None:
    path = Path("swarm/coordinator.py")
    text = path.read_text(encoding="utf-8")
    old = 'CONTROL_REF = "swarm/control"\nSTATE_PATH = "swarm/state.json"\nCOORDINATOR_ISSUE = 2\n'
    new = 'COORDINATOR_ISSUE = 2\nSTATE_COMMENT_MARKER = "FILES SWARM STATE v1"\n'
    if old not in text:
        raise RuntimeError("coordinator constants do not match expected pre-migration source")
    text = text.replace(old, new, 1)

    start = text.index("    def read_state(self) -> Tuple[Dict[str, Any], str]:")
    end = text.index("    def review_threads(", start)
    methods = '''    def _state_comment(self) -> Dict[str, Any]:
        matches: List[Dict[str, Any]] = []
        page = 1
        while True:
            comments = self.get(
                f"/repos/{self.repository}/issues/{COORDINATOR_ISSUE}/comments?per_page=100&page={page}"
            )
            matches.extend(
                comment
                for comment in comments
                if (comment.get("body") or "").startswith(STATE_COMMENT_MARKER)
            )
            if len(comments) < 100:
                break
            page += 1
        if len(matches) != 1:
            raise RuntimeError(
                f"Expected exactly one {STATE_COMMENT_MARKER} comment, found {len(matches)}"
            )
        return matches[0]

    @staticmethod
    def _decode_state_comment(comment: Dict[str, Any]) -> Dict[str, Any]:
        prefix = (
            STATE_COMMENT_MARKER
            + "\\n\\nMachine-owned coordination cache. Live GitHub remains authoritative. "
              "Human directions remain in issue #2 outside this block.\\n\\n```json\\n"
        )
        suffix = "\\n```"
        body = (comment.get("body") or "").rstrip()
        if not body.startswith(prefix) or not body.endswith(suffix):
            raise RuntimeError("Machine-state comment has an invalid envelope")
        return json.loads(body[len(prefix):-len(suffix)])

    def read_state(self) -> Tuple[Dict[str, Any], str]:
        comment = self._state_comment()
        return self._decode_state_comment(comment), str(comment.get("updated_at") or "")

    def write_state(self, state: Dict[str, Any], state_token: str) -> None:
        current = self._state_comment()
        current_token = str(current.get("updated_at") or "")
        if current_token != state_token:
            raise RuntimeError(
                f"STATE_RACE: machine-state comment changed from {state_token} to {current_token}"
            )
        body = (
            STATE_COMMENT_MARKER
            + "\\n\\nMachine-owned coordination cache. Live GitHub remains authoritative. "
              "Human directions remain in issue #2 outside this block.\\n\\n```json\\n"
            + json.dumps(state, indent=2, ensure_ascii=False)
            + "\\n```\\n"
        )
        updated = self.request(
            "PATCH",
            f"{API}/repos/{self.repository}/issues/comments/{current['id']}",
            {"body": body},
        )
        confirmed = self._decode_state_comment(updated)
        if confirmed.get("revision") != state.get("revision"):
            raise RuntimeError("Machine-state comment read-back revision mismatch")

'''
    text = text[:start] + methods + text[end:]
    retry_old = 'if ("HTTP 409" in text or "HTTP 422" in text) and attempt < 2:'
    retry_new = 'if ("STATE_RACE" in text or "HTTP 409" in text or "HTTP 422" in text) and attempt < 2:'
    if retry_old not in text:
        raise RuntimeError("coordinator retry guard does not match expected source")
    text = text.replace(retry_old, retry_new, 1)
    path.write_text(text, encoding="utf-8")


def patch_docs() -> None:
    protocol = Path("swarm/PROTOCOL.md")
    p = protocol.read_text(encoding="utf-8")
    p = p.replace(
        "This contract lives on the operational branch `swarm/control`. It and `swarm/state.json` are coordination data, never application source and never a product PR. Product branches start from current `main`, not this branch.",
        "This contract lives on `main`. Mutable coordination state lives outside Git refs in the unique `FILES SWARM STATE v1` machine-state comment on issue #2; it is a cache, never application source and never a product PR. Product branches start from current `main`.",
    )
    p = p.replace("`swarm/control`", "`main`")
    p = p.replace("swarm/control", "main")
    p = p.replace("`swarm/state.json`", "the `FILES SWARM STATE v1` machine-state comment on issue #2")
    p = p.replace("swarm/state.json", "FILES SWARM STATE v1 machine-state comment on issue #2")
    protocol.write_text(p, encoding="utf-8")

    coordinator_doc = Path("swarm/COORDINATOR.md")
    c = coordinator_doc.read_text(encoding="utf-8")
    c = c.replace("`swarm/control`", "`main`")
    c = c.replace("swarm/control", "main")
    c = c.replace("`swarm/state.json`", "the `FILES SWARM STATE v1` machine-state comment on issue #2")
    c = c.replace("swarm/state.json", "FILES SWARM STATE v1 machine-state comment on issue #2")
    c = c.replace(
        "State updates use the GitHub Contents API with the current blob SHA. A stale write is refetched and reconciled up to three times; the coordinator never force-writes an old snapshot over a newer worker update. Every successful write is read back before it is reported as complete.",
        "State updates use the dedicated `FILES SWARM STATE v1` issue comment rather than a Git ref. The coordinator refetches the comment token before writing, retries detected races up to three times, and reads back the resulting revision before reporting success. Mutable coordination therefore never advances `main`.",
    )
    c = c.replace(
        "Updating `main` does not itself retrigger the coordinator, preventing feedback loops.",
        "Updating the machine-state issue comment does not trigger source CI or release workflows, preventing feedback loops.",
    )
    coordinator_doc.write_text(c, encoding="utf-8")

    workers = Path("swarm/workers.json")
    data = json.loads(workers.read_text(encoding="utf-8"))
    for worker in data:
        prompt = worker.get("prompt", "")
        prompt = prompt.replace(
            "First fetch swarm/PROTOCOL.md and swarm/state.json with ref swarm/control in that repository, plus the newest explicit Shannon directions in issue #2.",
            "First fetch swarm/PROTOCOL.md from main and the unique issue #2 comment whose first line is FILES SWARM STATE v1, plus the newest explicit Shannon directions in issue #2.",
        )
        prompt = prompt.replace("Execute protocol revision 2;", "Execute protocol revision 3;")
        prompt = prompt.replace(
            "Obtain the current blob SHA for expected-SHA state writes, preserve others' fields and read back the result.",
            "Before a state write, refetch the marker comment and require its JSON revision unchanged; increment revision and updated_at, preserve others' fields, update that same comment and read back the result. Never use or recreate swarm/control or a checked-in swarm/state.json.",
        )
        worker["prompt"] = prompt
    workers.write_text(json.dumps(data, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def main() -> int:
    start_main = run("git", "rev-parse", "HEAD", capture=True)
    run("git", "fetch", "origin", "+refs/heads/*:refs/remotes/origin/*", "--prune")
    if run("git", "rev-parse", "origin/main", capture=True) != start_main:
        raise RuntimeError("main moved before migration began")

    for branch, expected in STALE.items():
        actual = run("git", "rev-parse", f"refs/remotes/origin/{branch}", capture=True)
        if actual != expected:
            raise RuntimeError(f"{branch} moved from {expected} to {actual}")
    for branch in ANCESTORS:
        run("git", "merge-base", "--is-ancestor", f"origin/{branch}", "HEAD")

    control_head = run("git", "rev-parse", "refs/remotes/origin/swarm/control", capture=True)
    run("git", "merge-base", "--is-ancestor", CONTROL_BASE, control_head)
    changed = run("git", "diff", "--name-only", f"{CONTROL_BASE}..{control_head}", capture=True).splitlines()
    unexpected = [item for item in changed if item and item != "swarm/state.json"]
    if unexpected:
        raise RuntimeError(f"swarm/control gained non-state changes: {unexpected}")

    # Require exactly one external machine-state cache before removing branch state.
    import urllib.request
    token = os.environ["GH_TOKEN"]
    request = urllib.request.Request(
        f"https://api.github.com/repos/{REPO}/issues/2/comments?per_page=100",
        headers={"Authorization": f"Bearer {token}", "Accept": "application/vnd.github+json"},
    )
    with urllib.request.urlopen(request, timeout=30) as response:
        comments = json.loads(response.read().decode("utf-8"))
    markers = [c for c in comments if (c.get("body") or "").startswith("FILES SWARM STATE v1")]
    if len(markers) != 1:
        raise RuntimeError(f"expected exactly one FILES SWARM STATE v1 comment, found {len(markers)}")

    patch_coordinator()
    patch_docs()

    retirement = {
        "reason": "Consolidate Infiltrator File Manager to one Git branch while preserving swarm coordination outside Git refs.",
        "machine_state_issue": 2,
        "machine_state_comment_id": markers[0]["id"],
        "machine_state_marker": "FILES SWARM STATE v1",
        "retired": dict(STALE, **{"swarm/control": control_head}),
        "note": "Seven retired branches were strict ancestors of main. swarm/transfer-failure-residue was an older superseded implementation lineage whose exact head is retained here for audit. swarm/control carried static control-plane files now on main plus mutable state now held in issue #2.",
    }
    Path("swarm/branch-retirement-20261012.json").write_text(
        json.dumps(retirement, indent=2) + "\n", encoding="utf-8"
    )

    Path("swarm/state.json").unlink()
    Path(".github/workflows/one-time-single-branch.yml").unlink()
    Path("swarm/single_branch_migration.py").unlink()

    run(sys.executable, "-m", "py_compile", "swarm/coordinator.py", "swarm/coordinator_entry.py")
    run(sys.executable, "-m", "unittest", "discover", "-v", "-s", "swarm", "-p", "test_coordinator*.py")
    json.loads(Path("swarm/workers.json").read_text(encoding="utf-8"))
    source = Path("swarm/coordinator.py").read_text(encoding="utf-8")
    if 'CONTROL_REF = "swarm/control"' in source or 'STATE_PATH = "swarm/state.json"' in source:
        raise RuntimeError("coordinator still contains branch-backed state constants")

    run("git", "config", "user.name", "Shannon Smith")
    run("git", "config", "user.email", "infiltratr@yandex.com")
    run("git", "add", "-A")
    run("git", "commit", "-m", "Consolidate swarm state outside Git branches")

    run("git", "fetch", "origin", "main")
    if run("git", "rev-parse", "origin/main", capture=True) != start_main:
        raise RuntimeError("main moved during migration; refusing push and branch cleanup")
    run("git", "push", "origin", "HEAD:main")

    for branch in [*STALE.keys(), "swarm/control"]:
        run("git", "push", "origin", "--delete", branch)

    remaining = run("git", "ls-remote", "--heads", "origin", capture=True).splitlines()
    names = [line.split()[1].removeprefix("refs/heads/") for line in remaining if line.strip()]
    if names != ["main"]:
        raise RuntimeError(f"unexpected remaining branch set: {names}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
