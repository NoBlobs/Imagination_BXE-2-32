# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Thalles Campagnani
#
# Project documentation (reStructuredText + Sphinx).
#
#   make            HTML (default) -> _build/html/index.html
#   make html       same
#   make serve      build and serve on http://127.0.0.1:8765 (PORT=)
#   make clean      remove _build/
#   make distclean  also remove the local Python venv
#
# Sphinx is installed on first use in a local venv (.venv/).

PYTHON   ?= python3
VENV     := .venv
SPHINX   := $(VENV)/bin/sphinx-build
BUILDDIR := _build
OPTS     ?= -q
PORT     ?= 8765

.PHONY: html serve clean distclean venv
html: venv
	$(SPHINX) $(OPTS) -b html . $(BUILDDIR)/html
	@echo "HTML: $(CURDIR)/$(BUILDDIR)/html/index.html"

serve: html
	@echo "Serving on http://127.0.0.1:$(PORT)/ (Ctrl-C to stop)"
	$(PYTHON) -m http.server $(PORT) --bind 127.0.0.1 --directory $(BUILDDIR)/html

venv: $(VENV)/.stamp

$(VENV)/.stamp: requirements.txt
	$(PYTHON) -m venv $(VENV)
	$(VENV)/bin/pip install -q -r requirements.txt
	@touch $@

clean:
	rm -rf _build

distclean: clean
	rm -rf .venv
