# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Sphinx configuration. Build the HTML with `make` in this folder.
import re
import subprocess
from pathlib import Path

from docutils import nodes

project = "PowerVR Rogue BXE-2-32 free GPU firmware"
author = "Thalles Campagnani"
copyright = "2026, Thalles Campagnani"
language = "en"

# sphinx_rtd_dark_mode adds a light/dark toggle button to the RTD theme;
# the choice is remembered by the browser.
extensions = ["sphinx_rtd_theme", "sphinx_rtd_dark_mode"]
default_dark_mode = False
exclude_patterns = [
    "_build",
    ".venv",
    "README.md",
    "questions.md",
    "requirements.txt",
    "logRefs",
]

# Read the Docs theme (same as SpacemiT_K1_DDR and SpacemiT_K3_DDR)
html_theme = "sphinx_rtd_theme"
html_title = "Imagination BXE-2-32 — free GPU firmware"
html_show_sourcelink = False
html_theme_options = {
    "navigation_depth": 3,
    "collapse_navigation": False,
    "sticky_navigation": True,
    "prev_next_buttons_location": "both",
}
pygments_style = "sphinx"

# _static/tables.css: let table cells wrap (the theme keeps them on one line).
# _static/theme_persist.js: keep the light/dark choice across local files.
html_static_path = ["_static"]
html_css_files = ["tables.css"]
html_js_files = ["theme_persist.js"]

# ---------------------------------------------------------------------------
# Links to the repository, experiment logs, and docsClean submodule on GitHub.
#
#   :dc:`boot-gpu-init`             docsClean/boot-gpu-init.md
#   :dc:`docsClean/ <README>`       explicit title, links to submodule root
#   :exp:`E013`                     "E013" -> docs/logRefs/E013-*.md
#   :lr:`data/survey-0-2000.txt`    file or directory under docs/logRefs/
# ---------------------------------------------------------------------------
REPO_URL = "https://github.com/NoBlobs/Imagination_BXE-2-32"
REPO_BRANCH = "main"
DOCSCLEAN_URL = "https://github.com/NoBlobs-docsClean/Imagination_BXE-2-32"

ROOT = Path(__file__).resolve().parent.parent
LOGREFS_DIR = Path(__file__).resolve().parent / "logRefs"


def _docsclean_commit():
    """Commit of docsClean pinned by this repository (gitlink), so links show
    exactly the revision the documentation was written against."""
    try:
        out = subprocess.run(
            ["git", "-C", str(ROOT), "ls-files", "-s", "docsClean"],
            capture_output=True,
            text=True,
            check=True,
        )
        fields = out.stdout.split()
        if len(fields) >= 2 and fields[0] == "160000":
            return fields[1]
    except (OSError, subprocess.CalledProcessError):
        pass
    return "main"


DOCSCLEAN_REV = _docsclean_commit()
_TITLED = re.compile(r"^(.*?)\s*<(.*)>$", re.S)


def _split(text):
    m = _TITLED.match(text)
    return (m.group(1), m.group(2)) if m else (None, text)


def _link(rawtext, title, url):
    return [nodes.reference(rawtext, title, refuri=url)], []


def dc_role(name, rawtext, text, lineno, inliner, options=None, content=None):
    title, target = _split(text)
    target = target.strip()
    if target in ("", "/", "README"):
        url = f"{DOCSCLEAN_URL}/tree/{DOCSCLEAN_REV}"
        return _link(rawtext, title or "docsClean/", url)
    fname = target if target.endswith(".md") else target + ".md"
    url = f"{DOCSCLEAN_URL}/blob/{DOCSCLEAN_REV}/{fname}"
    return _link(rawtext, title or f"docsClean/{target}", url)


def lr_role(name, rawtext, text, lineno, inliner, options=None, content=None):
    title, target = _split(text)
    target = target.strip().strip("/")
    if not target:
        url = f"{REPO_URL}/tree/{REPO_BRANCH}/docs/logRefs"
        return _link(rawtext, title or "docs/logRefs/", url)
    kind = "blob" if "." in target.rsplit("/", 1)[-1] else "tree"
    url = f"{REPO_URL}/{kind}/{REPO_BRANCH}/docs/logRefs/{target}"
    return _link(rawtext, title or f"docs/logRefs/{target}", url)


def exp_role(name, rawtext, text, lineno, inliner, options=None, content=None):
    title, eid = _split(text)
    eid = eid.strip()
    matches = sorted(LOGREFS_DIR.glob(f"{eid}-*.md"))
    if not matches:
        msg = inliner.reporter.warning(
            f"no docs/logRefs/{eid}-*.md experiment", line=lineno
        )
        return [nodes.literal(rawtext, eid)], [msg]
    url = f"{REPO_URL}/blob/{REPO_BRANCH}/docs/logRefs/{matches[0].name}"
    return _link(rawtext, title or eid, url)


def setup(app):
    app.add_role("dc", dc_role)
    app.add_role("lr", lr_role)
    app.add_role("exp", exp_role)
    return {"parallel_read_safe": True}
