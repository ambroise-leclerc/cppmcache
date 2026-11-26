#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ambroise Leclerc
"""Checks the attribution rule of CONTRIBUTING.md over a range of commits.

Every author, committer and co-author must be a human account that can be contacted, and neither
the commit messages nor the pull request description may attribute the work to an AI tool. The
error messages are in English: they are displayed in CI.

    tools/check_attribution.py <base>..<head>
    PR_BODY="…" tools/check_attribution.py origin/main..HEAD
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

# Addresses that do not designate a contactable person.
NON_HUMAN_EMAIL = re.compile(
    r"(\[bot\]@|@anthropic\.com$|@openai\.com$|^noreply@|^no-reply@|^bot@|^copilot@|@cursor\.(com|sh)$|"
    r"@devin\.ai$|@aider\.chat$|^actions@github\.com$|^github-actions@)",
    re.IGNORECASE,
)
# The noreply addresses that GitHub assigns to a user account are still those of a person.
GITHUB_USER_NOREPLY = re.compile(r"^(\d+\+)?[A-Za-z0-9-]+@users\.noreply\.github\.com$")
# Commits created by the GitHub web interface (merging a pull request): technical committer.
GITHUB_WEB_COMMITTER = ("GitHub", "noreply@github.com")

NON_HUMAN_NAME = re.compile(
    r"(\[bot\]|\bClaude (Code|Opus|Sonnet|Haiku|Fable)\b|\bGitHub Copilot\b|\bChatGPT\b|\bOpenAI Codex\b|"
    r"\bCursor Agent\b|\bGemini (CLI|Code)\b|^Claude$|^Copilot$|^Codex$|^Devin$|^aider\b)",
    re.IGNORECASE,
)
ATTRIBUTION_TEXT = re.compile(
    r"(generated (with|by) \[?(claude|chatgpt|copilot|cursor|codex|gemini|an? (ai|llm))|"
    r"\bclaude-session:|claude\.ai/code/session|\U0001F916|"
    r"\b(written|authored|created|produced) (with|by) (claude|chatgpt|copilot|cursor|codex|gemini|an? (ai|llm))\b)",
    re.IGNORECASE,
)
CO_AUTHOR = re.compile(r"^co-authored-by:\s*(.*?)\s*<([^>]*)>\s*$", re.IGNORECASE | re.MULTILINE)


def identity_problem(name: str, email: str, role: str) -> str | None:
    if role == "committer" and (name, email) == GITHUB_WEB_COMMITTER:
        return None
    if GITHUB_USER_NOREPLY.match(email):
        return None if not NON_HUMAN_NAME.search(name) else f"{role} name '{name}' names a tool, not a person"
    if not email or "@" not in email:
        return f"{role} '{name}' has no contact address"
    if NON_HUMAN_EMAIL.search(email):
        return f"{role} '{name} <{email}>' is not a contactable human account"
    if NON_HUMAN_NAME.search(name):
        return f"{role} name '{name}' names a tool, not a person"
    return None


def text_problems(text: str, where: str) -> list[str]:
    problems = []
    for match in ATTRIBUTION_TEXT.finditer(text):
        problems.append(f"{where} attributes the work to an AI tool: '{match.group(0).strip()}'")
    for match in CO_AUTHOR.finditer(text):
        problem = identity_problem(match.group(1), match.group(2), "co-author")
        if problem:
            problems.append(f"{where}: {problem}")
    return problems


def main() -> int:
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    separator = "\x1e"
    fields = "%H%x1f%an%x1f%ae%x1f%cn%x1f%ce%x1f%B"
    log = subprocess.run(["git", "log", f"--format={fields}{separator}", sys.argv[1]],
                         capture_output=True, text=True, check=True).stdout
    problems: list[str] = []
    commits = 0
    for record in log.split(separator):
        record = record.strip("\n")
        if not record:
            continue
        commit, author, author_email, committer, committer_email, message = record.split("\x1f", 5)
        commits += 1
        where = f"commit {commit[:10]}"
        for problem in (identity_problem(author, author_email, "author"),
                        identity_problem(committer, committer_email, "committer")):
            if problem:
                problems.append(f"{where}: {problem}")
        problems += text_problems(message, where)
    problems += text_problems(os.environ.get("PR_BODY", ""), "pull request description")

    if problems:
        print("Attribution rule violated (see CONTRIBUTING.md, “Authorship and attribution”):", file=sys.stderr)
        for problem in problems:
            print(f"  - {problem}", file=sys.stderr)
        return 1
    print(f"{commits} commit(s) checked: every author, committer and co-author is a contactable human account.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
