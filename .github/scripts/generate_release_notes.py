#!/usr/bin/env python3
"""
generate_release_notes.py
Standardized release notes generator for Alya language packages.

Responsibilities:
1. Extract package metadata from alya.toml (name, description, version, etc.).
2. Resolve current tag and determine previous tag in git history.
3. Generate commit changelog between releases.
4. Construct compare/commits links for full changelog tracking.
5. Populate .github/release_template.md placeholders and output RELEASE_NOTES.md.
6. Export outputs (tag, title, prev_tag, etc.) to $GITHUB_OUTPUT.
"""

import os
import sys
import subprocess
from pathlib import Path

# Ensure UTF-8 output on all platforms (especially Windows CP1254/CP1252)
if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
if hasattr(sys.stderr, "reconfigure"):
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")


def run_git(args, check=True):
    """Run git command and return stripped stdout."""
    try:
        res = subprocess.run(
            ["git"] + args,
            capture_output=True,
            text=True,
            check=check,
            encoding="utf-8",
            errors="replace",
        )
        return res.stdout.strip()
    except Exception:
        return ""


def parse_simple_toml(file_path):
    """Parse top-level [package] key-value pairs from alya.toml."""
    props = {}
    path = Path(file_path)
    if not path.is_file():
        return props

    current_section = ""
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith("[") and line.endswith("]"):
                current_section = line[1:-1].strip()
                continue
            if current_section == "package" and "=" in line:
                key, val = line.split("=", 1)
                key = key.strip()
                val = val.strip().strip('"').strip("'")
                props[key] = val
    return props


def format_author(name, email):
    """Format author name or GitHub username handle."""
    name = (name or "").strip()
    email = (email or "").strip()
    if "@users.noreply.github.com" in email:
        handle = email.split("@")[0]
        if "+" in handle:
            handle = handle.split("+", 1)[1]
        if handle:
            return f"@{handle}"
    if " " not in name and name:
        return f"@{name}" if not name.startswith("@") else name
    return name or "contributor"


def main():
    # 1. Resolve Release Tag
    tag = ""
    if len(sys.argv) > 1 and sys.argv[1].strip():
        tag = sys.argv[1].strip()
    elif os.environ.get("GITHUB_REF_TYPE") == "tag" and os.environ.get("GITHUB_REF_NAME"):
        tag = os.environ.get("GITHUB_REF_NAME").strip()
    elif os.environ.get("INPUT_TAG"):
        tag = os.environ.get("INPUT_TAG").strip()
    else:
        # Fallback to exact tag on HEAD or latest tag
        tag = run_git(["describe", "--tags", "--exact-match"], check=False)
        if not tag:
            tag = run_git(["describe", "--tags", "--abbrev=0"], check=False)

    if not tag:
        tag = "v0.1.0"

    version = tag.lstrip("v")

    # 2. Extract Package Metadata from alya.toml
    pkg_meta = parse_simple_toml("alya.toml")
    pkg_name = pkg_meta.get("name")
    if not pkg_name:
        repo_env = os.environ.get("GITHUB_REPOSITORY", "")
        if "/" in repo_env:
            pkg_name = repo_env.split("/")[-1]
        else:
            pkg_name = Path.cwd().name

    description = pkg_meta.get("description", "A modern package for the Alya programming language")
    alya_version = pkg_meta.get("alya-version", "0.0.19")

    # Resolve Repository Slug (e.g. alya-lang/toml)
    repo = os.environ.get("GITHUB_REPOSITORY", "")
    if not repo:
        repo_url = pkg_meta.get("repository", "")
        if "github.com/" in repo_url:
            repo = repo_url.split("github.com/")[-1].strip("/")
            if repo.endswith(".git"):
                repo = repo[:-4]
    if not repo:
        repo = f"alya-lang/{pkg_name}"

    repo_url = f"https://github.com/{repo}"

    # 3. Release Title: Library Name + Version (e.g. "toml v0.1.0")
    title_version = tag if tag.startswith("v") else f"v{tag}"
    title = f"{pkg_name} {title_version}"

    # 4. Detect Previous Tag in Git History
    # Check whether the target tag already exists as a git object (e.g. tag push).
    # On manual workflow_dispatch triggers, the tag has not been created in git yet,
    # so we compare against HEAD.
    tag_rev = run_git(["rev-parse", "--verify", f"refs/tags/{tag}"], check=False)
    tag_exists = bool(tag_rev)
    target_ref = tag if tag_exists else "HEAD"

    prev_tag = ""
    # If the tag already exists, look before it (tag^). Otherwise, look at HEAD.
    base_ref = f"{tag}^" if tag_exists else target_ref
    describe_prev = run_git(["describe", "--tags", "--abbrev=0", base_ref], check=False)
    if describe_prev and describe_prev != tag:
        prev_tag = describe_prev
    else:
        # Fallback: check all version tags sorted descending
        all_tags_raw = run_git(["tag", "-l", "v*", "--sort=-v:refname"], check=False)
        if all_tags_raw:
            all_tags = [t.strip() for t in all_tags_raw.splitlines() if t.strip() and t.strip() != tag]
            for candidate in all_tags:
                is_ancestor = subprocess.run(
                    ["git", "merge-base", "--is-ancestor", candidate, target_ref],
                    capture_output=True,
                ).returncode == 0
                if is_ancestor:
                    prev_tag = candidate
                    break

    # 5. Full Changelog Link & Commit Range
    if prev_tag:
        full_changelog_url = f"{repo_url}/compare/{prev_tag}...{tag}"
        full_changelog = f"**Full Changelog**: https://github.com/{repo}/compare/{prev_tag}...{tag}"
        commit_range = f"{prev_tag}..{target_ref}"
    else:
        full_changelog_url = f"{repo_url}/commits/{tag}"
        full_changelog = f"**Full Changelog**: https://github.com/{repo}/commits/{tag}"
        commit_range = target_ref

    # 6. Extract Commit Log for Changes Section
    commits_raw = run_git(["log", "--pretty=format:%h%x09%an%x09%ae%x09%s", commit_range], check=False)
    if commits_raw:
        lines = []
        for raw_line in commits_raw.splitlines():
            if not raw_line.strip():
                continue
            parts = raw_line.split("\t", 3)
            if len(parts) == 4:
                commit_hash, author_name, author_email, subject = parts
            elif len(parts) == 3:
                commit_hash, author_name, subject = parts
                author_email = ""
            else:
                continue

            if (
                subject.startswith(f"release {tag}")
                or subject.startswith(f"chore: release {tag}")
                or subject.startswith(f"chore(release): {tag}")
            ):
                continue

            author_tag = format_author(author_name, author_email)
            lines.append(f"* {subject} by {author_tag} ({commit_hash})")

        commits_text = "\n".join(lines) if lines else "* Initial release"
    else:
        commits_text = "* Initial release"

    # 7. Load Template or Use Built-in Default
    template_path = Path(".github/release_template.md")
    if template_path.is_file():
        template = template_path.read_text(encoding="utf-8")
    else:
        template = (
            "{{DESCRIPTION}}\n\n"
            "## 📦 Installation\n\n"
            "Using the Alya CLI:\n\n"
            "```bash\n"
            "alya add {{PACKAGE_NAME}} --git https://github.com/{{REPOSITORY}} --tag {{TAG}}\n"
            "alya install\n"
            "```\n\n"
            "Or add it directly to your project's `alya.toml`:\n\n"
            "```toml\n"
            "[dependencies]\n"
            '{{PACKAGE_NAME}} = { git = "https://github.com/{{REPOSITORY}}", tag = "{{TAG}}" }\n'
            "```\n\n"
            "## 🚀 What's Changed\n\n"
            "{{CHANGELOG_COMMITS}}\n\n"
            "## 🔗 Resources\n\n"
            "- **Documentation**: [README.md](https://github.com/{{REPOSITORY}}#readme)\n"
            "- **Examples**: [examples/](https://github.com/{{REPOSITORY}}/tree/{{TAG}}/examples)\n"
            "- **Issue Tracker**: [GitHub Issues](https://github.com/{{REPOSITORY}}/issues)\n\n"
            "---\n\n"
            "{{FULL_CHANGELOG}}\n"
        )

    # 8. Substitute Placeholders
    replacements = {
        "{{PACKAGE_NAME}}": pkg_name,
        "{{TAG}}": tag,
        "{{VERSION}}": version,
        "{{DESCRIPTION}}": description,
        "{{REPOSITORY}}": repo,
        "{{REPO_URL}}": repo_url,
        "{{MIN_ALYA_VERSION}}": alya_version,
        "{{PREV_TAG}}": prev_tag,
        "{{CHANGELOG_COMMITS}}": commits_text,
        "{{FULL_CHANGELOG}}": full_changelog,
        "{{CHANGELOG_URL}}": full_changelog_url,
    }

    body = template
    for placeholder, val in replacements.items():
        body = body.replace(placeholder, val)

    # 9. Output to File
    out_file = Path("RELEASE_NOTES.md")
    out_file.write_text(body, encoding="utf-8")
    print(f"Generated release notes in {out_file.resolve()}")
    print(f"  Title:     {title}")
    print(f"  Tag:       {tag}")
    print(f"  Prev Tag:  {prev_tag or '(None - Initial Release)'}")
    print(f"  Changelog: {full_changelog_url}")

    # 10. Export to GITHUB_OUTPUT if present
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as f:
            f.write(f"tag={tag}\n")
            f.write(f"title={title}\n")
            f.write(f"prev_tag={prev_tag}\n")
            f.write(f"pkg_name={pkg_name}\n")
            f.write(f"version={version}\n")
            f.write(f"changelog_url={full_changelog_url}\n")


if __name__ == "__main__":
    main()
