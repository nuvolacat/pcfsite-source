/* source.pcfsite.com — lo que el Admin cambia sin republicar la web (/estado.json, servicio/partidas.py):
   el aviso de la portada y, en las tarjetas de juegos, el nombre visible, las etiquetas «Nuevo» y «Arreglado»,
   el orden y los ocultos. Sin respuesta, la página se queda como está (lo generó build.py). */
(function () {
  "use strict";
  function esc(s) { return String(s == null ? "" : s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function aviso(a) {
    if (!a || !a.texto) return;
    var main = document.getElementById("contenido");
    if (!main) return;
    var box = document.createElement("div");
    box.className = "aviso-site blueprint";
    box.setAttribute("role", "status");
    box.innerHTML = '<i class="corner tl"></i><i class="corner tr"></i><i class="corner bl"></i><i class="corner br"></i>'
      + (a.enlace ? '<a href="' + esc(a.enlace) + '">' + esc(a.texto) + "</a>" : esc(a.texto));
    main.insertBefore(box, main.firstChild);
  }
  function catalogo(c) {
    var grid = document.querySelector(".cat .grid");
    if (!grid || !c) return;
    var cards = Array.prototype.slice.call(grid.querySelectorAll(".card-g[href]"));
    var visibles = 0;
    cards.forEach(function (card, i) {
      var id = card.getAttribute("href").replace(/^\//, "");
      var v = c[id];
      card.dataset.orden = (v && v.orden) ? v.orden : 999;
      card.dataset.pos = i;
      if (!v) { visibles++; return; }
      if (v.oculto) { card.hidden = true; return; }
      visibles++;
      if (v.nombre) { var n = card.querySelector(".name"); if (n) n.textContent = v.nombre; }
      var tags = card.querySelector(".tags");
      if (tags) {
        if (v.nuevo) tags.insertAdjacentHTML("afterbegin", '<span class="tag tag-new">Nuevo</span>');
        if (v.arreglado) tags.insertAdjacentHTML("beforeend", '<span class="tag tag-fix">Arreglado</span>');
      }
    });
    cards.sort(function (a, b) { return (+a.dataset.orden - +b.dataset.orden) || (+a.dataset.pos - +b.dataset.pos); })
      .forEach(function (card) { grid.appendChild(card); });
    var cnt = document.querySelector(".cat .cat-head .cnt");
    if (cnt && visibles !== cards.length) cnt.textContent = visibles + " en el proyecto";
  }
  fetch("/estado.json", { credentials: "same-origin" })
    .then(function (r) { return r.ok ? r.json() : null; })
    .then(function (e) { if (e) { aviso(e.aviso); catalogo(e.catalogo); } })
    .catch(function () {});
})();
