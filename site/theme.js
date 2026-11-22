(function () {
  // Remembers the theme preview in this browser only. Nothing is sent anywhere.
  var key = "omabook-theme", form = document.querySelector(".themes");
  if (!form) return;
  try {
    var saved = localStorage.getItem(key);
    if (saved) { var r = form.querySelector('input[value="' + saved + '"]'); if (r) r.checked = true; }
  } catch (e) {}
  form.addEventListener("change", function (e) {
    try { localStorage.setItem(key, e.target.value); } catch (x) {}
  });
})();
