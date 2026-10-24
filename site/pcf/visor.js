/* source.pcfsite.com — capturas a tamaño completo (clic en una imagen con data-visor; Esc o clic para cerrar) */
(function () {
  "use strict";
  var v = document.getElementById("visor");
  if (!v) return;
  var img = v.querySelector("img");
  document.addEventListener("click", function (e) {
    var t = e.target.closest("img[data-visor]");
    if (t) { img.src = t.src; img.alt = t.alt; v.classList.add("show"); return; }
    if (e.target.closest("#visor")) v.classList.remove("show");
  });
  document.addEventListener("keydown", function (e) { if (e.key === "Escape") v.classList.remove("show"); });
})();
