/* source.pcfsite.com — barra superior, secciones y pie comunes (los mismos que el foro y las demás zonas).
 * Sin librerías; fichero externo (sin scripts en línea). Idempotente.
 * NO EDITAR topbar.js: se genera desde este fichero con build.py (versión en el pie). */
(function () {
  "use strict";
  var D = document;

  function init() {
    if (!D.body || D.getElementById("topbar")) return;
    var path = location.pathname || "/";

    var bar = '<nav class="nav" id="topbar">'
      + '<a class="nav-brand lnk brand" href="/"><img class="brand-mark" src="/pcf/img/favicon.svg" alt="" width="24" height="24"><span><span class="gold">source</span>.pcfsite<span class="dot">.com</span></span></a>'
      + '<span class="tb-links"><a href="https://pcfsite.com/">Inicio</a><a href="https://foro.pcfsite.com/">Foro</a><a href="https://juegos.pcfsite.com/">Jugar</a><a href="https://descargas.pcfsite.com/">Descargas</a><a href="/" aria-current="page">Source</a></span>'
      + '<span class="nav-tools"><span id="tbCuenta"></span></span></nav>';

    // secciones del sitio, bajo la barra (como en el foro)
    function cur(re) { return re.test(path) ? ' aria-current="page"' : ""; }
    var secs = '<nav class="secciones" aria-label="Secciones">'
      + '<a href="/"' + cur(/^\/(index\.html)?$/) + '>Juegos</a>'
      + '<a href="/como-funciona"' + cur(/^\/como-funciona/) + '>Cómo funciona</a>'
      + '<a href="/diario"' + cur(/^\/diario/) + '>Diario</a>'
      + "</nav>";

    var frag = D.createElement("div");
    frag.innerHTML = bar;
    D.body.insertBefore(frag.firstChild, D.body.firstChild);
    var main = D.querySelector("main");
    if (main) { var sx = D.createElement("div"); sx.innerHTML = secs; main.insertBefore(sx.firstChild, main.firstChild); }

    if (!D.querySelector("footer.pie")) {
      var f = D.createElement("footer");
      f.className = "pie fijo";
      f.innerHTML = '<span>PCFSITE.COM · Source v__VERSION__<span class="pie-fans-w"> · <a class="pie-fans" href="https://pcfsite.com/legal#quien">Hecho por fans, sin afiliación con Dinamic Multimedia</a></span></span>'
        + '<span class="links"><a class="lnk" href="https://pcfsite.com/">Inicio</a><a class="lnk" href="https://foro.pcfsite.com/">Foro</a><a class="lnk" href="https://juegos.pcfsite.com/">Jugar</a><a class="lnk" href="https://descargas.pcfsite.com/">Descargas</a><a class="lnk" href="https://foro.pcfsite.com/normas">Normas</a><a class="lnk" href="https://pcfsite.com/legal">Legal</a>'
        + '<a class="nuvola" href="https://nuvola.cat" target="_blank" rel="noopener" title="nuvola"><img class="nv-claro" src="/pcf/img/nuvola-claro.png" alt="nuvola" width="48" height="14"><img class="nv-oscuro" src="/pcf/img/nuvola-oscuro.png" alt="nuvola" width="48" height="14"></a></span>';
      (D.querySelector(".wrap") || D.body).appendChild(f);
      D.body.classList.add("pie-fijo");
    }

    // cuenta única de pcfsite.com (sesión del foro: nginx pasa /cuenta/yo al foro)
    var sc = D.createElement("script"); sc.src = "/pcf/cuenta.js?v=__VERSION__"; D.head.appendChild(sc);
  }

  if (D.readyState === "loading") D.addEventListener("DOMContentLoaded", init); else init();
})();
