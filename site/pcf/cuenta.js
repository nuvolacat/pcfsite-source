/* Cuenta única de pcfsite.com en la barra (online y descargas). La sesión es la del foro:
 * /cuenta/yo lo pasa nginx al foro (mismo origen). Mismo aspecto que la barra del foro. */
(function () {
  "use strict";
  var C = '<i class="corner tl"></i><i class="corner tr"></i><i class="corner bl"></i><i class="corner br"></i>';
  function esc(s) { return String(s == null ? "" : s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  var CARET = '<svg class="caret" width="12" height="12" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2" aria-hidden="true"><path d="m6 9 6 6 6-6"/></svg>';
  function pinta(box, u) {
    var aqui = location.href;
    if (!u) {
      box.innerHTML = '<a class="btn btn-primary blueprint" href="https://foro.pcfsite.com/entrar?next=' + encodeURIComponent(aqui) + '">' + C + "Entrar</a>";
      return;
    }
    // «Cerrar sesión» va directo al foro: lo acepta por el Origin (*.pcfsite.com)
    var n = +u.avisos || 0;
    // con foto de avatar (9-oct-2026), la foto; si no, las iniciales con los colores del equipo
    var av = u.foto && /^https:\/\/foro\.pcfsite\.com\/img\//.test(u.foto)
      ? '<img class="av av-foto" src="' + esc(u.foto) + '" alt="" width="28" height="28">'
      : '<span class="av" style="background:' + esc(u.avatar_bg) + '">' + esc(u.initials) + "</span>";
    box.innerHTML = '<details class="cuenta"><summary aria-label="Cuenta' + (n ? " · " + n + " notificaciones" : "") + '">' + av
      + (n ? '<span class="cuenta-aviso" aria-hidden="true">' + (n < 100 ? n : "99+") + "</span>" : "")
      + '<span class="nm">' + esc(u.name) + "</span>" + CARET + "</summary>"
      + '<div class="cuenta-menu blueprint">' + C
      + '<a href="https://foro.pcfsite.com/ucp">Panel de usuario</a>'
      + '<a href="' + esc(u.url) + '">Ver perfil</a>'
      + '<a href="https://foro.pcfsite.com/notificaciones">Notificaciones' + (n ? '<span class="tag tag-accent badge">' + n + "</span>" : "") + "</a>"
      + ((u.mod || u.admin) ? '<div class="sep" role="separator"></div>'
        + (u.mod ? '<a href="https://foro.pcfsite.com/mcp">Moderación</a>' : "")
        + (u.admin ? '<a href="https://foro.pcfsite.com/acp">Administrar foro</a>' : "") : "")
      + (u.super ? '<div class="sep" role="separator"></div><a href="https://admin.pcfsite.com/">Panel de control</a>' : "")
      + '<form method="post" action="https://foro.pcfsite.com/salir"><input type="hidden" name="next" value="' + esc(aqui) + '"><button type="submit">Cerrar sesión</button></form>'
      + "</div></details>";
  }
  document.addEventListener("click", function (e) {
    document.querySelectorAll("details.cuenta[open]").forEach(function (d) { if (!d.contains(e.target)) d.removeAttribute("open"); });
  });
  // selector de tema común: cookie para todo pcfsite.com; con sesión, el foro lo guarda en la cuenta
  function marcaTema() {
    var t = window.pcfTemaActual ? window.pcfTemaActual() : "system";
    document.querySelectorAll("[data-tema]").forEach(function (x) { x.classList.toggle("on", x.dataset.tema === t); });
  }
  document.addEventListener("click", function (e) {
    var tg = e.target.closest("[data-tema]");
    if (!tg) return;
    e.preventDefault();
    var n = tg.dataset.tema;
    document.cookie = "tema=" + n + "; domain=.pcfsite.com; path=/; max-age=34560000; secure; samesite=lax";
    try { localStorage.removeItem("tema"); } catch (x) {}
    if (window.pcfTema) window.pcfTema(n);
    marcaTema();
    fetch("https://foro.pcfsite.com/tema?to=" + n + "&next=/salud", { mode: "no-cors", credentials: "include" }).catch(function () {});
  });
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", marcaTema); else marcaTema();
  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") document.querySelectorAll("details.cuenta[open]").forEach(function (d) { d.removeAttribute("open"); });
  });
  // Móvil (≤900 px, 9-oct-2026): los enlaces de la barra pasan a un cajón bajo la barra con el botón ☰
  // (mismo aspecto y comportamiento que en el foro). Se construye a partir de .tb-links, sin duplicar textos.
  function cajon() {
    var bar = document.getElementById("topbar"), links = bar && bar.querySelector(".tb-links"), tools = bar && bar.querySelector(".nav-tools");
    if (!links || !tools || document.getElementById("cajon")) return;
    var b = document.createElement("button");
    b.className = "btn btn-secondary btn-icon burger"; b.type = "button";
    b.setAttribute("aria-label", "Menú"); b.setAttribute("aria-expanded", "false"); b.setAttribute("aria-controls", "cajon");
    b.innerHTML = '<svg width="18" height="18" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.6" aria-hidden="true"><path class="b-abre" d="M4 6h16M4 12h16M4 18h16"/><path class="b-cierra" d="M6 6l12 12M18 6 6 18"/></svg>';
    tools.appendChild(b);
    var c = document.createElement("nav");
    c.className = "cajon"; c.id = "cajon"; c.hidden = true; c.setAttribute("aria-label", "Menú");
    links.querySelectorAll("a").forEach(function (a) {
      var x = document.createElement("a");
      x.href = a.href;
      if (a.getAttribute("aria-current")) x.setAttribute("aria-current", a.getAttribute("aria-current"));
      x.innerHTML = "<span>" + esc(a.textContent) + '</span><span class="fl">→</span>';
      c.appendChild(x);
    });
    bar.parentNode.insertBefore(c, bar.nextSibling);
    function poner(abierto) { c.hidden = !abierto; b.setAttribute("aria-expanded", abierto ? "true" : "false"); }
    b.addEventListener("click", function () { poner(c.hidden); });
    document.addEventListener("keydown", function (e) { if (e.key === "Escape" && !c.hidden) { poner(false); b.focus(); } });
    if (window.matchMedia) matchMedia("(min-width:901px)").addEventListener("change", function (m) { if (m.matches) poner(false); });
  }
  function init() {
    var box = document.getElementById("tbCuenta");
    if (!box) return;
    cajon();
    fetch("/cuenta/yo", { credentials: "same-origin" }).then(function (r) { return r.ok ? r.json() : { user: null }; })
      .then(function (d) {
        pinta(box, d.user);
        // con sesión, manda el tema de la cuenta: si la cookie común no coincide, se corrige y se aplica ya
        var t = d.user && d.user.tema;
        if (t && /^(light|dark|system)$/.test(t) && (window.pcfTemaActual ? window.pcfTemaActual() : "") !== t) {
          document.cookie = "tema=" + t + "; domain=.pcfsite.com; path=/; max-age=34560000; secure; samesite=lax";
          try { localStorage.removeItem("tema"); } catch (x) {}
          if (window.pcfTema) window.pcfTema(t);
        }
      }).catch(function () { pinta(box, null); });
  }
  if (document.readyState === "loading") document.addEventListener("DOMContentLoaded", init); else init();
  window.pcfCuenta = init;
})();
