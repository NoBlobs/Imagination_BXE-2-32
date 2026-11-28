# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
# Sphinx configuration. Build the HTML with `make` in this folder.
import re
import subprocess
from pathlib import Path

from docutils import nodes

project = "Imagination Rogue BXE-2-32 free GPU firmware"
author = "Thalles Campagnani"
copyright = "2026, Thalles Campagnani"
language = "en"

# sphinx_rtd_dark_mode adds a light/dark toggle button to the RTD theme;
# the choice is remembered by the browser.
extensions = [
    "sphinx_rtd_theme",
    "sphinx_rtd_dark_mode",
    "sphinx.ext.napoleon",
    "hawkmoth",
    "hawkmoth.ext.napoleon",
]
default_dark_mode = False
exclude_patterns = [
    "_build",
    ".venv",
    "README.md",
    "questions.md",
    "requirements.txt",
    "logRefs",
]

# ---------------------------------------------------------------------------
# Hawkmoth: the "Firmware (Source)" pages are generated from the /** */
# documentation comments in src/ (Google style: Args:/Returns:, through napoleon),
# parsed with libclang as the firmware is built: freestanding C23 for MIPS.
# LIBCLANG=/path/to/libclang.so overrides the system library.
# ---------------------------------------------------------------------------
import os

import clang.cindex

for _lib in (os.environ.get("LIBCLANG"), "/usr/lib/libclang.so",
             "/usr/lib64/libclang.so"):
    if _lib and Path(_lib).exists():
        clang.cindex.Config.set_library_file(_lib)
        break

hawkmoth_root = str(Path(__file__).resolve().parent.parent / "src")
hawkmoth_clang = [
    "-target", "mipsel-unknown-elf",
    "-std=gnu23",
    "-ffreestanding",
    f"-I{hawkmoth_root}/include",
]
hawkmoth_transform_default = "napoleon"   # Google-style comments


# ---------------------------------------------------------------------------
# Assembly sources: libclang cannot parse boot.S and vectors.S, so their /** */
# comments are extracted here, in the same Google style.
#
#   .. asm-autodoc:: boot.S
#
# A comment followed by `.globl NAME` or `NAME:` documents that entry point
# (shown as a C function); one followed by `.macro NAME args` documents the
# macro; any other is text for the file.
# ---------------------------------------------------------------------------
_ASM_DOC = re.compile(r"/\*\*(?!/)(.*?)\*/", re.S)
_ASM_SYM = re.compile(r"^\s*(?:\.globl\s+(\w+)|\.macro\s+(\w+)(.*)|(\w+):)")


def _asm_comment_lines(body):
    lines = body.strip("\n").split("\n")
    out = []
    for ln in lines:
        ln = ln.rstrip()
        ln = re.sub(r"^\s*\* ?", "", ln) if ln.lstrip().startswith("*") else ln.strip()
        out.append(ln)
    while out and not out[-1]:
        out.pop()
    while out and not out[0]:
        out.pop(0)
    return out


def _asm_entries(path):
    text = path.read_text()
    for m in _ASM_DOC.finditer(text):
        rest = text[m.end():].lstrip("\n").split("\n", 1)[0]
        sym = _ASM_SYM.match(rest)
        lines = _asm_comment_lines(m.group(1))
        if not sym:
            yield None, lines
        elif sym.group(2):
            args = [a for a in re.split(r"[\s,]+", sym.group(3).strip()) if a]
            args = [a.split("=")[0] for a in args]
            yield f".. c:macro:: {sym.group(2)}({', '.join(args)})", lines
        else:
            yield f".. c:function:: void {sym.group(1) or sym.group(4)}(void)", lines


def _asm_setup(app):
    from docutils.parsers.rst import Directive
    from docutils.statemachine import StringList
    from sphinx.ext.napoleon.docstring import GoogleDocstring

    class AsmAutodoc(Directive):
        required_arguments = 1
        has_content = False

        def run(self):
            path = Path(hawkmoth_root) / self.arguments[0]
            env = self.state.document.settings.env
            env.note_dependency(str(path))
            rst = []
            for head, lines in _asm_entries(path):
                body = str(GoogleDocstring(lines, app.config)).split("\n")
                if head is None:
                    rst += body + [""]
                else:
                    rst += [head, ""] + ["   " + ln if ln else "" for ln in body] + [""]
            node = nodes.section()
            node.document = self.state.document
            self.state.nested_parse(StringList(rst, str(path)), 0, node)
            return node.children

    app.add_directive("asm-autodoc", AsmAutodoc)

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
    _asm_setup(app)
    return {"parallel_read_safe": True}
