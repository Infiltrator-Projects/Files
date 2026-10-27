#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fetch and qualify prebuilt providers from their owning repositories only."""
from __future__ import annotations

import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

CATALOGUE = Path(__file__).with_name("temporal-providers.json")


def read_url(url, *, api=False):
    headers = {"User-Agent": "Infiltrator-independent-provider-test"}
    token = os.environ.get("GITHUB_TOKEN") or os.environ.get("GH_TOKEN")
    if api:
        headers["Accept"] = "application/vnd.github+json"
        if token:
            headers["Authorization"] = "Bearer " + token
    for attempt in range(4):
        try:
            with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=120) as response:
                return response.read()
        except urllib.error.HTTPError as error:
            if error.code < 500 or attempt == 3:
                raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == 3:
                raise
        time.sleep(2 ** attempt)
    raise RuntimeError("Download retries exhausted")


def catalogue():
    entries = json.loads(CATALOGUE.read_text())
    if len(entries) != 54 or len({x["package"] for x in entries}) != 54:
        raise ValueError("Expected 54 distinct independent providers")
    for entry in entries:
        if (not entry["repository"].startswith(("Calendar-", "Clock-")) or
                entry["package"] != "infiltrator-" + entry["repository"].lower()):
            raise ValueError("Invalid independent provider identity")
    return entries


def fetch_one(entry, directory):
    repository = "Infiltrator-Projects/" + entry["repository"]
    release = json.loads(read_url(f"https://api.github.com/repos/{repository}/releases/latest", api=True))
    tag = release.get("tag_name", "")
    if release.get("draft") or release.get("prerelease") or not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError(repository + " has no stable canonical release")
    version, slug = tag[1:], entry["repository"].lower()
    if subprocess.run(["dpkg", "--compare-versions", version, "ge", entry.get("minimum_version", "1.0.0")]).returncode:
        raise ValueError(repository + " is older than its independent resource baseline")
    filename = f"{entry['package']}_{version}_amd64.deb"
    names = {filename, f"{slug}-{version}-native-installer.run", f"{slug}-{version}-source.zip"}
    assets = release.get("assets", [])
    if len(assets) != 3 or {x["name"] for x in assets} != names:
        raise ValueError(repository + " does not have its three-asset release")
    asset = next(x for x in assets if x["name"] == filename)
    url = f"https://github.com/{repository}/releases/download/{tag}/{filename}"
    digest = asset.get("digest", "")
    if asset.get("browser_download_url") != url or not re.fullmatch(r"sha256:[0-9a-f]{64}", digest):
        raise ValueError(repository + " has no trusted package URL/digest")
    package = read_url(url)
    if len(package) != asset["size"] or hashlib.sha256(package).hexdigest() != digest[7:]:
        raise ValueError(filename + " does not match its published bytes")
    (directory / filename).write_bytes(package)
    print("Verified independent package: " + filename, flush=True)
    return dict(entry, version=version, filename=filename, sha256=digest[7:], size=asset["size"])


def verify(directory):
    records = json.loads((directory / "providers.json").read_text())
    known = {x["package"]: x for x in catalogue()}
    if not records or len({x["package"] for x in records}) != len(records):
        raise ValueError("No unique independent package snapshot")
    for record in records:
        entry = known[record["package"]]
        if any(record.get(key) != value for key, value in entry.items()):
            raise ValueError("Provider identity differs from the catalogue")
        filename = f"{record['package']}_{record['version']}_amd64.deb"
        if record["filename"] != filename:
            raise ValueError("Package filename differs from its identity")
        path = directory / filename
        data = path.read_bytes()
        if len(data) != record["size"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
            raise ValueError("Package snapshot checksum mismatch")
        for field, value in (("Package", record["package"]), ("Version", record["version"]), ("Architecture", "amd64")):
            actual = subprocess.check_output(["dpkg-deb", "-f", path, field], text=True).strip()
            if actual != value:
                raise ValueError("Debian " + field + " differs from its published identity")
    if {p.name for p in directory.glob("*.deb")} != {x["filename"] for x in records}:
        raise ValueError("Unexpected package in the independent snapshot")
    return records


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=("fetch", "verify", "install"))
    parser.add_argument("directory", type=Path)
    parser.add_argument("--select", action="append", help="kind:id; omitted selects all 54 providers")
    args = parser.parse_args()
    directory = args.directory.resolve()
    if args.command == "fetch":
        directory.mkdir(parents=True, exist_ok=True)
        if any(directory.iterdir()):
            raise ValueError("Fetch destination must be empty; an existing snapshot is verified without re-fetching")
        entries = catalogue()
        if args.select:
            selected = set(args.select)
            entries = [x for x in entries if x["kind"] + ":" + x["id"] in selected]
            if len(entries) != len(selected):
                raise ValueError("Unknown requested provider")
        with ThreadPoolExecutor(max_workers=4) as pool:
            records = list(pool.map(lambda entry: fetch_one(entry, directory), entries))
        (directory / "providers.json").write_text(json.dumps(records, indent=2) + "\n")
    records = verify(directory)
    if args.command == "install":
        command = ["/usr/bin/apt-get", "-o", "DPkg::Lock::Timeout=300", "install", "-y", "--no-install-recommends"]
        if os.geteuid() != 0:
            command = ["/usr/bin/sudo", "--", *command]
        subprocess.run([*command, *(str(directory / x["filename"]) for x in records)], check=True)
    print(f"Qualified independent provider snapshot: {len(records)} packages")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.SubprocessError) as error:
        print("Independent provider setup failed: " + str(error), file=sys.stderr)
        raise SystemExit(1)
