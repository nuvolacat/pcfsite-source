// Los PC Fútbol en el navegador (Emscripten): configuración del módulo antes de cargar juego.js.
// El juego pide los datos por trozos a la URL de pcf7Datos (ver platform/src/web.cpp). Con ?log se ve el registro.
(function () {
  const q = new URLSearchParams(location.search);
  const JUEGO = document.documentElement.dataset.juego || "pcf7";   // el juego de esta página (juegos/<id>)
  document.getElementById("carga-texto").textContent = "Cargando " + document.title + "…";
  const conLog = q.has("log");
  const registro = document.getElementById("registro");
  const carga = document.getElementById("carga");
  const aviso = document.getElementById("aviso");
  const lineas = [];
  if (conLog) registro.style.display = "block";
  function log(t) {
    console.log(t);
    if (!conLog) return;
    lineas.push(t);
    if (lineas.length > 200) lineas.shift();
    registro.textContent = lineas.slice(-12).join("\n");
  }
  function fallo(t) {
    aviso.textContent = t;
    aviso.style.display = "block";
    document.querySelector("#carga .barra").style.display = "none";
  }
  if (!window.crossOriginIsolated) fallo("Este navegador no permite ejecutar el juego en esta página (falta el aislamiento entre orígenes).");
  const canvas = document.getElementById("canvas");
  canvas.addEventListener("contextmenu", (e) => e.preventDefault());
  canvas.addEventListener("click", () => canvas.focus());
  // programa del CD que toca (el lanzador navega con ?p=<programa>&a=<argumento>; sin p, el principal)
  const PROG = (q.get("p") || "").replace(/[^a-z0-9]/g, "");
  // versión de la compilación (la pone el despliegue): juego.js y su .wasm se piden con ?v=, así el navegador
  // nunca junta un juego.js guardado de antes con un .wasm nuevo (no casan y el juego se detiene)
  const VER = document.documentElement.dataset.version || "";
  const conVer = (u) => (VER ? u + "?v=" + VER : u);
  window.Module = {
    canvas,
    locateFile: (f, pre) => (f.endsWith(".wasm") ? conVer(pre + f) : pre + f),
    arguments: q.get("a") ? [q.get("a")] : [],
    print: log,
    printErr: log,
    setStatus: (t) => { if (t) log(t); },
    pcf7Datos: document.documentElement.dataset.datos || q.get("datos") || "datos",
    pcf7Traza: q.get("traza") || "",   // diagnóstico: ver platform/src/core.cpp
    pcf7Reloj: q.has("reloj"),         // diagnóstico: reloj virtual (kernel32.cpp)
    pcf7Listo: () => { carga.classList.add("fuera"); canvas.focus(); },
    // el programa termina y el lanzador pide otro: se guardan las partidas y se carga (vuelve al principal con "")
    pcf7Lanzar: (prog, arg) => {
      carga.classList.remove("fuera");
      document.getElementById("carga-texto").textContent = "Cargando…";
      const sigue = () => {
        const u = new URLSearchParams(location.search);
        u.delete("p"); u.delete("a");
        if (prog && prog !== principal) u.set("p", prog);
        if (prog && prog !== principal && arg) u.set("a", arg);
        const s = u.toString();
        location.replace(location.pathname + (s ? "?" + s : ""));
      };
      window.PCF7Partidas.pendientes().then(sigue, sigue);
    },
    pcf7Terminado: (codigo) => {
      // 0 o 1 = salir del juego (los apartados del menú van por pcf7Lanzar)
      { carga.classList.remove("fuera"); document.getElementById("carga-texto").textContent = "Has salido de " + document.title + "."; document.querySelector("#carga .barra").style.display = "none"; }
    },
    onAbort: () => fallo("El juego se ha detenido por un error."),
    // partidas (web/partidas.js): web.cpp manda lo que escribe el juego y, al arrancar, espera a pcf7PartidasListas
    pcf7Guardar: (ruta, datos) => window.PCF7Partidas.guardar(ruta, datos).catch((e) => avisoPartida("error", e)),
    pcf7Borrar: (ruta) => window.PCF7Partidas.borrar(ruta).catch((e) => avisoPartida("error", e)),
    pcf7Restaurar: [],
    pcf7PartidasListas: false,
  };
  const TEXTOS = {
    guardando: "Guardando partida…",
    cuenta: "Partida guardada en tu cuenta de pcfsite ✓",
    navegador: "Partida guardada en este navegador. Entra con tu cuenta de pcfsite para guardarla también en el servidor.",
    pendiente: "Partida guardada en este navegador; se subirá a tu cuenta en cuanto haya conexión.",
    error: "¡No se ha podido guardar la partida en este navegador! Descarga una copia desde la ficha del juego.",
  };
  let tAviso = 0;
  function avisoPartida(que, e) {
    if (e) log("partidas: " + e);
    const el = document.getElementById("partida");
    el.textContent = TEXTOS[que] || que;
    el.classList.add("ver");
    clearTimeout(tAviso);
    if (que !== "error") tAviso = setTimeout(() => el.classList.remove("ver"), que === "guardando" ? 15000 : 6000);
  }
  window.PCF7Partidas.estado = avisoPartida;
  window.PCF7Partidas.cargar()
    .then((lista) => { window.Module.pcf7Restaurar = lista; if (lista.length) log("partidas: " + lista.length + " ficheros guardados"); })
    .catch((e) => avisoPartida("error", e))
    .finally(() => { window.Module.pcf7PartidasListas = true; });
  const principal = document.documentElement.dataset.principal || "manager";
  // estadísticas de source (Admin › Source): un aviso al arrancar el programa principal y un latido por minuto
  // mientras se juega (servicio/partidas.py: /partidas/hit y /partidas/beat; sin datos personales, solo juego e id
  // aleatorio de este navegador para no contar dos veces)
  const SID = (() => {
    try {
      let s = localStorage.getItem("pcf-sid");
      if (!s) { s = Math.random().toString(36).slice(2) + Date.now().toString(36); localStorage.setItem("pcf-sid", s); }
      return s;
    } catch (e) { return ""; }
  })();
  const marca = (que) => { try { navigator.sendBeacon("/partidas/" + que, new Blob([JUEGO + " " + SID], { type: "text/plain" })); } catch (e) {} };
  if (!PROG) marca("hit");
  setInterval(() => { if (!document.hidden) marca("beat"); }, 60000);
  const js = document.createElement("script");
  js.src = conVer(PROG ? "juego-" + PROG + ".js" : "juego.js");
  document.body.appendChild(js);
  precargar(window.Module.pcf7Datos);
  tactil();

  // Pantallas táctiles (móvil, tableta): los toques ya llegan como ratón (SDL), pero no hay teclado. Una barra con las
  // teclas que no tiene el teclado del móvil (Esc, Intro, flechas, F1-F10…) y un botón ⌨ que abre el del sistema.
  // Todo se manda como eventos de teclado a window, que es donde los escucha SDL: el texto le llega por keypress
  // (charCode) y las teclas por keydown/keyup (code). Con ?tactil se ve también en el ordenador.
  function tactil() {
    const tactilQ = q.has("tactil");
    if (!tactilQ && !(window.matchMedia && matchMedia("(pointer: coarse)").matches)) return;
    document.documentElement.classList.add("tactil");
    const barra = document.getElementById("tactil");
    const texto = document.getElementById("tactil-texto");
    const mas = document.getElementById("tactil-mas");
    const CODIGOS = { Escape: 27, Enter: 13, Backspace: 8, Tab: 9, Delete: 46, Space: 32, ArrowLeft: 37, ArrowUp: 38,
                      ArrowRight: 39, ArrowDown: 40, Home: 36, End: 35, PageUp: 33, PageDown: 34 };
    for (let i = 1; i <= 10; i++) CODIGOS["F" + i] = 111 + i;
    const enviar = (tipo, o) => window.dispatchEvent(new KeyboardEvent(tipo, Object.assign({ bubbles: true, cancelable: true }, o)));
    function tecla(code) {   // una tecla sin texto (Esc, Intro, flechas…)
      const k = CODIGOS[code] || 0, o = { code, key: code === "Space" ? " " : code, keyCode: k, which: k };
      enviar("keydown", o);
      enviar("keyup", o);
    }
    function caracter(c) {   // una letra: como en un teclado, keydown + keypress (el texto) + keyup
      const cp = c.codePointAt(0);
      if (c === "\n") return tecla("Enter");
      const may = c.toUpperCase();
      const code = /^[A-Z]$/.test(may) ? "Key" + may : /^[0-9]$/.test(c) ? "Digit" + c : c === " " ? "Space" : "";
      const k = code ? (code === "Space" ? 32 : may.charCodeAt(0)) : 0;
      enviar("keydown", { code, key: c, keyCode: k, which: k });
      enviar("keypress", { code, key: c, charCode: cp, keyCode: cp, which: cp });
      enviar("keyup", { code, key: c, keyCode: k, which: k });
    }

    barra.addEventListener("pointerdown", (e) => {
      const b = e.target.closest("button");
      if (!b || e.target === texto) return;
      e.preventDefault();   // que el botón no se quede el foco (ni cierre el teclado del sistema)
      if (b.dataset.tecla) tecla(b.dataset.tecla);
      else if (b.id === "tactil-f") mas.hidden = !mas.hidden;
      else if (b.id === "tactil-ocultar") { document.documentElement.classList.add("tactil-oculta"); texto.blur(); }
    });
    // con el teclado del sistema abierto, el juego se encaja en lo que queda visible (en iOS el teclado no achica la
    // página, solo lo visible; en Android lo hace interactive-widget=resizes-content, y esto no estorba)
    const vv = window.visualViewport;
    if (vv) {
      const encaja = () => {
        const s = document.documentElement.style;
        s.setProperty("--vv-alto", Math.round(vv.height) + "px");
        s.setProperty("--vv-arriba", Math.round(vv.offsetTop) + "px");
      };
      vv.addEventListener("resize", encaja);
      vv.addEventListener("scroll", encaja);
      encaja();
    }
    document.getElementById("tactil-ver").addEventListener("click", () => document.documentElement.classList.remove("tactil-oculta"));

    // El teclado del sistema: un campo invisible encima del botón ⌨ (iOS solo lo abre si el toque cae en el campo).
    // Lo escrito se saca comparando el contenido antes y después: así valen también el autocorrector y las
    // sugerencias de Android, que no mandan teclas sino palabras (keydown con keyCode 229). Siempre lleva un carácter
    // de relleno delante para que el borrado se note aunque no haya nada escrito.
    const RELLENO = " ";
    let antes = RELLENO, componiendo = false;
    const vacia = () => { texto.value = antes = RELLENO; texto.setSelectionRange(1, 1); };
    texto.addEventListener("pointerdown", (e) => {
      if (document.activeElement === texto) { e.preventDefault(); texto.blur(); }   // segundo toque: se cierra
    });
    texto.addEventListener("focus", () => { barra.classList.add("escribiendo"); vacia(); });
    texto.addEventListener("blur", () => { barra.classList.remove("escribiendo"); canvas.focus({ preventScroll: true }); });
    texto.addEventListener("compositionstart", () => { componiendo = true; });
    texto.addEventListener("compositionend", () => { componiendo = false; vacia(); });
    texto.addEventListener("input", () => {
      const ahora = texto.value;
      let i = 0;
      while (i < antes.length && i < ahora.length && antes[i] === ahora[i]) i++;
      for (let n = antes.length - i; n > 0; n--) tecla("Backspace");
      for (const c of ahora.slice(i)) caracter(c);
      antes = ahora;
      if (!componiendo && ahora !== RELLENO) vacia();
    });
    // las teclas del propio campo no llegan al juego tal cual (irían repetidas con lo de input); Intro sí, que no
    // escribe nada en el campo
    for (const t of ["keydown", "keypress", "keyup"]) texto.addEventListener(t, (e) => {
      if (e.isTrusted) e.stopPropagation();
      if (t === "keydown" && e.key === "Enter") { e.preventDefault(); tecla("Enter"); }
    });
  }

  // Precarga: los datos que usa el juego se descargan enteros, en segundo plano, a la caché del navegador
  // («<juego>-datos-<versión del índice>»); el juego (web/libwasmfs_fetch.js) los lee de allí en vez de esperar a la red.
  // La lista y su orden salen de datos/precarga.txt (tools/web_datos.py). Lo ya descargado sirve en las siguientes visitas.
  async function precargar(base) {
    const aviso = document.getElementById("precarga");
    try {
      if (!("caches" in window) || !crypto.subtle) return;
      base = new URL(base.replace(/\/+$/, "") + "/", location.href).href;
      const indice = await (await fetch(base + "indice.txt", { cache: "no-cache" })).text();
      const huella = Array.from(new Uint8Array(await crypto.subtle.digest("SHA-1", new TextEncoder().encode(indice))))
        .slice(0, 6).map((b) => b.toString(16).padStart(2, "0")).join("");
      const nombre = JUEGO + "-datos-" + huella;   // una caché por juego y versión de sus datos
      for (const k of await caches.keys()) if (k.startsWith(JUEGO + "-datos-") && k !== nombre) await caches.delete(k);
      const cache = await caches.open(nombre);
      const lista = (await (await fetch(base + "precarga.txt", { cache: "no-cache" })).text())
        .split("\n").filter(Boolean).map((l) => { const [r, n] = l.split("\t"); return { url: base + r, n: +n || 0 }; });
      const total = lista.reduce((a, f) => a + f.n, 0);
      let hecho = 0, i = 0, mostrado = false;
      const ya = new Set((await cache.keys()).map((r) => r.url));
      for (const f of lista) if (ya.has(f.url)) hecho += f.n;
      if (hecho >= total) return;
      if (navigator.storage && navigator.storage.persist) navigator.storage.persist().catch(() => {});
      const pinta = () => {
        if (!aviso) return;
        if (!mostrado) { aviso.style.display = "block"; mostrado = true; }
        aviso.textContent = "Descargando datos del juego: " + Math.floor(100 * hecho / total) + " %";
      };
      pinta();
      const trabajador = async () => {
        while (i < lista.length) {
          const f = lista[i++];
          if (ya.has(f.url)) continue;
          for (let intento = 0; intento < 3; intento++) {
            try {
              const r = await fetch(f.url);
              if (!r.ok) throw new Error(r.status);
              await cache.put(f.url, r);
              break;
            } catch (e) {
              if (e && e.name === "QuotaExceededError") throw e;   // sin sitio: se deja como está
              await new Promise((ok) => setTimeout(ok, 2000 * (intento + 1)));
            }
          }
          hecho += f.n;
          pinta();
        }
      };
      await Promise.all([trabajador(), trabajador(), trabajador()]);
      if (aviso) { aviso.textContent = "Datos descargados: el juego ya no tendrá esperas."; setTimeout(() => aviso.classList.add("fuera"), 4000); }
    } catch (e) {
      log("precarga: " + e);
      if (aviso) aviso.style.display = "none";
    }
  }
})();
