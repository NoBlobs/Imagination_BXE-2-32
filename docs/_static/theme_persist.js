/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 Thalles Campagnani */
/*
 * Keep the light/dark choice when pages are opened as local files (file://).
 * Browsers such as Firefox give every local file its own origin, so the
 * localStorage used by sphinx_rtd_dark_mode is lost on each page change.
 * window.name survives navigation inside the same tab, so the choice is also
 * stored there and re-applied on load.
 */
(function () {
  var RE = /bxe232theme=(dark|light);?/;

  function fromName() {
    var m = (window.name || '').match(RE);
    return m ? m[1] : null;
  }

  function apply(theme) {
    document.documentElement.setAttribute('data-theme', theme);
    try { localStorage.setItem('theme', theme); } catch (e) { /* ignore */ }
  }

  function save() {
    var t = document.documentElement.getAttribute('data-theme') || 'light';
    window.name = (window.name || '').replace(RE, '') + 'bxe232theme=' + t + ';';
  }

  var t = fromName();
  if (t)
    apply(t);

  window.addEventListener('pagehide', save);
  document.addEventListener('click', function (e) {
    if (e.target.closest && e.target.closest('#themeSwitcher'))
      setTimeout(save, 0);
  });
})();
