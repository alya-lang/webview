#!/usr/bin/env python3
"""Shared plumbing for the alya AI review scripts (index + code).

Everything here is deliberately side-effect-free except where noted:
pure helpers plus thin `gh`/`opencode` wrappers. No model text ever
reaches a shell from these helpers — callers validate before rendering.
"""
import json
import os
import subprocess
import sys


def run(cmd, **kw):
    kw.setdefault("capture_output", True)
    kw.setdefault("text", True)
    kw.setdefault("timeout", 180)
    return subprocess.run(cmd, **kw)


def fail(msg):
    print(f"ai-review error: {msg}", file=sys.stderr)
    sys.exit(1)


def extract_json(text):
    """Best-effort JSON recovery from model output (fences/prose)."""
    try:
        return json.loads(text)
    except ValueError:
        pass
    t = text.replace("```json", "```").replace("```JSON", "```")
    start_f = t.find("```")
    while start_f != -1:
        end_f = t.find("```", start_f + 3)
        if end_f == -1:
            break
        try:
            return json.loads(t[start_f + 3 : end_f])
        except ValueError:
            pass
        start_f = t.find("```", end_f + 3)
    start, end = text.find("{"), text.rfind("}")
    if start != -1 and end > start:
        try:
            return json.loads(text[start : end + 1])
        except ValueError:
            pass
    return None


def scrubbed_env():
    """Minimal environment for the model subprocess: no repo secrets.

    PATH locates `opencode`, HOME its config dir. No API key is passed
    (the free model tier needs none) and crucially no GH_TOKEN.
    """
    keep = ("PATH", "HOME", "SystemRoot", "TMPDIR", "TMP", "TEMP",
            "LANG", "LC_ALL")
    return {k: v for k, v in os.environ.items() if k in keep}


def run_model(models, prompt, workdir, env, ok=None, timeout=600,
              tag="ai-review"):
    """Try each model in order; first answer passing `ok` wins.

    The chain — not any single id — is the reliability strategy: dead
    or degraded models fail fast to the next. Every attempt is logged.
    """
    check = ok or (lambda text: bool(text))
    for model in models:
        model = model.strip()
        if not model:
            continue
        try:
            proc = subprocess.run(
                ["opencode", "run", "--model", model, "--format", "json"],
                input=prompt, capture_output=True, text=True,
                timeout=timeout, cwd=workdir, env=env,
            )
        except subprocess.TimeoutExpired:
            print(f"{tag}: model {model} timed out, trying next.")
            continue
        except FileNotFoundError:
            print(f"{tag}: opencode CLI not on PATH.")
            return "", ""
        texts = []
        for line in proc.stdout.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                event = json.loads(line)
            except ValueError:
                continue
            part = event.get("part") or {}
            if event.get("type") == "text" and isinstance(part.get("text"), str):
                texts.append(part["text"])
        text = "".join(texts).strip()
        if text and check(text):
            print(f"{tag}: answered by {model}.")
            return text, model
        print(f"{tag}: model {model} unusable, trying next.")
    return "", ""


def _i18n_path(script_file, language):
    return os.path.join(os.path.dirname(os.path.abspath(script_file)),
                        "i18n", f"{language}.json")


def load_strings(script_file, language):
    """UI strings for `language`, English fallback per missing key.

    Languages are data, not code: adding one means dropping
    `scripts/i18n/<lang>.json` next to the caller — no code change.
    Unknown languages silently fall back to English.
    """
    cache = load_strings.__dict__.setdefault("cache", {})
    key = (os.path.abspath(script_file), language)
    if key not in cache:
        merged = {}
        for lang in ("en", language) if language != "en" else ("en",):
            try:
                with open(_i18n_path(script_file, lang), encoding="utf-8") as fh:
                    doc = json.load(fh)
                if isinstance(doc, dict):
                    merged.update({k: v for k, v in doc.items()
                                   if isinstance(v, str)})
            except (OSError, ValueError):
                pass
        cache[key] = merged
    return cache[key]


def lang_name(script_file, language):
    if language != "en" and not os.path.isfile(_i18n_path(script_file, language)):
        return f"ISO language code '{language}'"
    name = load_strings(script_file, language).get("language_name", "")
    return name if name else f"ISO language code '{language}'"


def read_json_doc(path):
    """JSON object from `path`, or {} when missing/invalid (fail-open)."""
    try:
        with open(path, encoding="utf-8") as fh:
            doc = json.load(fh)
        return doc if isinstance(doc, dict) else {}
    except (OSError, ValueError):
        return {}


def clamp_int(value, default, lo, hi):
    try:
        return max(lo, min(hi, int(value)))
    except (TypeError, ValueError):
        return default


def str_list(value, allowed, limit):
    """Strings from `value` filtered by `allowed` (None = any string)."""
    if not isinstance(value, list):
        return []
    if allowed is None:
        out = [v for v in value if isinstance(v, str)]
    else:
        out = [v for v in value if v in allowed]
    return out[:limit]


def upsert_issue_comment(owner, name, pr_number, marker, body):
    """Create or update the single marker-tagged issue comment."""
    lst = run(["gh", "api", f"repos/{owner}/{name}/issues/{pr_number}/comments",
               "--paginate"])
    existing = None
    if lst.returncode == 0:
        try:
            for c in json.loads(lst.stdout):
                if isinstance(c, dict) and marker in str(c.get("body", "")):
                    existing = c.get("id")
                    break
        except ValueError:
            existing = None
    if existing:
        up = run(["gh", "api", "--method", "PATCH",
                  f"repos/{owner}/{name}/issues/comments/{existing}",
                  "-f", f"body={body}"])
    else:
        up = run(["gh", "api", "--method", "POST",
                  f"repos/{owner}/{name}/issues/{pr_number}/comments",
                  "-f", f"body={body}"])
    if up.returncode != 0:
        fail(f"summary comment failed: {up.stderr.strip()[:200]}")
