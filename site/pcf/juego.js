/* source.pcfsite.com — el juego incrustado en su ficha: al pulsar «Jugar» se carga /jugar/<id>/ en un iframe.
   La ficha va con COOP/COEP (nginx) para que el iframe quede aislado y el juego pueda usar hilos (SharedArrayBuffer). */
(function () {
  "use strict";
  var marco = document.querySelector("[data-juego]");
  if (!marco) return;
  var completa = document.querySelector(".juego-completa");
  var frame = null;
  // jugar exige la cuenta de pcfsite (las partidas van a la cuenta): sin sesión, a entrar y de vuelta a esta ficha
  var sesion = null;
  function conSesion() {
    if (sesion === null) sesion = fetch("/cuenta/yo", { credentials: "same-origin" })
      .then(function (r) { return r.ok ? r.json() : { user: null }; }).then(function (d) { return !!d.user; }, function () { return false; });
    return sesion;
  }
  function empezar() {
    if (frame) return;
    conSesion().then(function (hay) {
      if (hay) return arranca();
      sesion = null;
      location.href = "https://foro.pcfsite.com/entrar?next=" + encodeURIComponent(location.origin + location.pathname + "#jugar");
    });
  }
  function arranca() {
    if (frame) return;
    frame = document.createElement("iframe");
    frame.src = marco.getAttribute("data-juego");
    frame.title = "Juego";
    frame.allow = "cross-origin-isolated; fullscreen; autoplay";
    frame.allowFullscreen = true;
    marco.appendChild(frame);
    marco.classList.add("on");
    frame.addEventListener("load", function () { frame.focus(); });
    if (completa) completa.hidden = false;
  }
  marco.querySelector(".juego-boton").addEventListener("click", empezar);

  // versiones (base, Oro, Plus…): cambian el juego del marco; si se estaba jugando, se cierra (lo grabado no se pierde)
  var versiones = document.querySelectorAll(".juego-versiones .ver[data-jugar]");
  function elige(b, guarda) {
    if (b.classList.contains("on")) return;
    if (frame && !confirm("Cambiar de versión cierra la partida en curso. Lo que hayas grabado no se pierde. ¿Seguir?")) return;
    versiones.forEach(function (x) { x.classList.toggle("on", x === b); x.setAttribute("aria-pressed", x === b ? "true" : "false"); });
    marco.setAttribute("data-juego", b.getAttribute("data-jugar"));
    marco.setAttribute("data-partidas", b.getAttribute("data-jugar") + "partidas.js");
    marco.setAttribute("data-nombre", b.getAttribute("data-id"));
    marco.querySelector(".juego-nombre").textContent = b.getAttribute("data-nombre");
    if (frame) { frame.remove(); frame = null; marco.classList.remove("on"); if (completa) completa.hidden = true; }
    api = null;   // las partidas son de cada versión
    if (guarda) try { localStorage.setItem("version:" + location.pathname, b.getAttribute("data-id")); } catch (e) {}
  }
  versiones.forEach(function (b) { b.addEventListener("click", function () { elige(b, true); }); });
  try {   // la última versión elegida en esta ficha
    var ultima = localStorage.getItem("version:" + location.pathname);
    versiones.forEach(function (b) { if (b.getAttribute("data-id") === ultima) elige(b, false); });
  } catch (e) {}
  if (completa) completa.addEventListener("click", function () { if (frame && frame.requestFullscreen) frame.requestFullscreen(); });
  if (location.hash === "#jugar") marco.scrollIntoView({ block: "center" });

  // Copia manual de las partidas (las guarda el juego en este navegador: /jugar/<id>/partidas.js, mismo origen)
  var api = null;
  function partidas() {
    if (api) return api;
    api = new Promise(function (ok, mal) {
      var s = document.createElement("script");   // un partidas.js por versión (data-juego: de qué juego son)
      s.src = marco.getAttribute("data-partidas");
      s.setAttribute("data-juego", marco.getAttribute("data-nombre"));
      s.onload = function () { ok(window.PCF7Partidas); };
      s.onerror = function () { api = null; mal(new Error("no se ha podido cargar")); };
      document.head.appendChild(s);
    });
    return api;
  }
  var exportar = document.querySelector(".juego-exportar"), importar = document.querySelector(".juego-importar");
  var fichero = document.querySelector(".juego-fichero");
  if (exportar) exportar.addEventListener("click", function () {
    exportar.disabled = true;
    partidas()
      .then(function (P) { return P.cargar().catch(function () {}).then(function () { return P.exportar(); }); })   // con lo de la cuenta
      .then(function (r) {
        if (!r.n) { alert("Todavía no hay partidas guardadas en este navegador ni en tu cuenta."); return; }
        var a = document.createElement("a");
        a.href = URL.createObjectURL(r.blob);
        a.download = marco.getAttribute("data-nombre") + "-partidas-" + new Date().toISOString().slice(0, 10) + ".json";
        document.body.appendChild(a); a.click(); a.remove();
        setTimeout(function () { URL.revokeObjectURL(a.href); }, 10000);
      })
      .catch(function (e) { alert("No se han podido descargar las partidas: " + e.message); })
      .then(function () { exportar.disabled = false; });
  });
  if (importar) importar.addEventListener("click", function () { fichero.click(); });
  if (fichero) fichero.addEventListener("change", function () {
    var f = fichero.files[0];
    fichero.value = "";
    if (!f) return;
    if (!confirm("Las partidas del fichero sustituirán a las que tengan el mismo nombre (las de ahora quedan en una copia de respaldo del navegador). ¿Seguir?")) return;
    partidas()
      .then(function (P) { return P.importar(f); })
      .then(function (n) {
        alert(n + " ficheros de partidas cargados." + (frame ? " El juego se reinicia para usarlos." : ""));
        if (frame) frame.src = frame.src;
      })
      .catch(function (e) { alert("No se han podido cargar: " + e.message); });
  });
})();
