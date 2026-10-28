(function () {
  'use strict';
  var root = document.documentElement;
  var theme;
  try {
    theme = localStorage.getItem('morph-share-theme');
  } catch (error) {
    /* Local-file browser policies may disable persistent storage. */
  }
  if (theme !== 'light' && theme !== 'dark') {
    theme = matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  }
  root.dataset.theme = theme;
  document.addEventListener('DOMContentLoaded', function () {
    var buttons = document.querySelectorAll('[data-set-theme]');
    function apply(value) {
      root.dataset.theme = value;
      buttons.forEach(function (button) {
        button.setAttribute('aria-pressed', String(button.dataset.setTheme === value));
      });
    }
    apply(theme);
    buttons.forEach(function (button) {
      button.addEventListener('click', function () {
        apply(button.dataset.setTheme);
        try {
          localStorage.setItem('morph-share-theme', root.dataset.theme);
        } catch (error) {
          /* Switching still works when persistence is unavailable. */
        }
      });
    });
  });
})();
