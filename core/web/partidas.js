// PCF7 en el navegador: partidas guardadas, en tres capas.
//  1. Navegador: cada fichero que escribe el juego (lo manda platform/src/web.cpp) se guarda en IndexedDB.
//  2. Cuenta de pcfsite.com: si hay sesión, también en el servidor (/partidas/pcf7/, servicio/partidas.py de
//     pcfsite-source), comprimido. Al arrancar se juntan las dos copias: gana la más nueva de cada fichero.
//  3. Copia manual: exportar() / importar() (botones de la ficha del juego en source.pcfsite.com). importar() admite
//     también la partida de juegos.pcfsite.com (<juego>.pcfsave), que convierte el servidor.
// Antes de sustituir o borrar una copia local, la anterior va a «respaldo» (las 5 últimas de cada fichero).
(function () {
  "use strict";
  // el juego: el de la página (<html data-juego>) o el que diga quien carga este script (data-juego en <script>)
  const yo = document.currentScript;
  const JUEGO = (yo && yo.dataset.juego) || document.documentElement.dataset.juego || "pcf7";
  const API = "/partidas/" + JUEGO + "/";
  // ficheros de partida (PCF7 y 2001: save\…; Extensión del 2000: saveext\…; PCF 5 y 6: TACTICS\MANAGER.000,
  // PROMANAG.000, MANAG022.000, PROMA003.000…; PCF4: TACTICS\LIGA.N, PROLIGA.N)
  const ES_PARTIDA = /\/save(ext)?\/|\/tactics\/(manager|promanag|manag\d+|proma\d+|liga|proliga)\.\d/i;
  const BD = JUEGO + "-partidas";
  let bd = null, cola = [], subiendo = false, sesion = null;   // sesion: null = no se sabe, true / false
  const P = (window.PCF7Partidas = { estado: () => {} });

  function abrir() {
    if (bd) return bd;
    bd = new Promise((ok, mal) => {
      const r = indexedDB.open(BD, 1);
      r.onupgradeneeded = () => {
        r.result.createObjectStore("ficheros", { keyPath: "ruta" });
        r.result.createObjectStore("respaldo", { keyPath: "clave" });
      };
      r.onsuccess = () => ok(r.result);
      r.onerror = () => mal(r.error);
    });
    return bd;
  }
  async function tx(almacen, modo, fn) {
    const db = await abrir();
    return new Promise((ok, mal) => {
      const t = db.transaction(almacen, modo);
      let res;
      Promise.resolve(fn(t.objectStore(almacen), t)).then((v) => (res = v));
      t.oncomplete = () => ok(res);
      t.onerror = () => mal(t.error);
      t.onabort = () => mal(t.error);
    });
  }
  const pide = (req) => new Promise((ok, mal) => { req.onsuccess = () => ok(req.result); req.onerror = () => mal(req.error); });
  const todas = () => tx("ficheros", "readonly", (s) => pide(s.getAll()));
  const una = (ruta) => tx("ficheros", "readonly", (s) => pide(s.get(ruta)));

  // escribe un fichero (o su borrado: datos = null); la versión anterior, si tenía datos, pasa a «respaldo»
  async function pon(reg) {
    const db = await abrir();
    await new Promise((ok, mal) => {
      const t = db.transaction(["ficheros", "respaldo"], "readwrite");
      const f = t.objectStore("ficheros"), r = t.objectStore("respaldo");
      const g = f.get(reg.ruta);
      g.onsuccess = () => {
        const ant = g.result;
        if (ant && ant.datos && ant.fecha !== reg.fecha) {
          r.put({ clave: ant.ruta + "@" + ant.fecha, ruta: ant.ruta, fecha: ant.fecha, datos: ant.datos });
          const c = r.getAllKeys(IDBKeyRange.bound(ant.ruta + "@", ant.ruta + "@￿"));
          c.onsuccess = () => {
            const viejas = c.result.sort((a, b) => +a.split("@").pop() - +b.split("@").pop()).slice(0, -5);
            for (const k of viejas) r.delete(k);
          };
        }
        f.put(reg);
      };
      t.oncomplete = ok;
      t.onerror = () => mal(t.error);
    });
  }

  async function comprime(datos) {
    if (!window.CompressionStream) return datos;
    return new Uint8Array(await new Response(new Blob([datos]).stream().pipeThrough(new CompressionStream("gzip"))).arrayBuffer());
  }
  async function descomprime(datos) {
    if (datos[0] !== 0x1f || datos[1] !== 0x8b) return datos;
    return new Uint8Array(await new Response(new Blob([datos]).stream().pipeThrough(new DecompressionStream("gzip"))).arrayBuffer());
  }
  async function conTiempo(promesa, ms) {
    let t;
    try { return await Promise.race([promesa, new Promise((_, mal) => (t = setTimeout(() => mal(new Error("tiempo")), ms)))]); }
    finally { clearTimeout(t); }
  }

  // Al arrancar: junta navegador y cuenta. Devuelve los ficheros que hay que poner en el disco del juego.
  P.cargar = async function () {
    const local = new Map((await todas()).map((r) => [r.ruta, r]));
    try {
      const r = await conTiempo(fetch(API, { cache: "no-store" }), 8000);
      sesion = r.status === 401 || r.status === 404 ? false : r.ok ? true : null;   // 404: sin servicio de cuentas (pruebas)
      if (r.ok) {
        const servidor = (await r.json()).ficheros;
        const enServidor = new Set();
        for (const s of servidor) {
          enServidor.add(s.ruta);
          const l = local.get(s.ruta);
          if (!l || s.fecha > l.fecha) {   // la de la cuenta es más nueva (otro ordenador): se trae
            let reg = { ruta: s.ruta, fecha: s.fecha, datos: null, subido: true };
            if (!s.borrado) {
              const f = await conTiempo(fetch(API + "f/" + encodeURI(s.ruta), { cache: "no-store" }), 30000);
              if (!f.ok) continue;
              reg.datos = await descomprime(new Uint8Array(await f.arrayBuffer()));
            }
            await pon(reg);
            local.set(s.ruta, reg);
          } else if (l.fecha > s.fecha && !l.subido) cola.push(l.ruta);
          else if (l.fecha === s.fecha && !l.subido) { l.subido = true; await pon(l); }
        }
        for (const l of local.values()) if (!enServidor.has(l.ruta) && l.datos) { l.subido = false; cola.push(l.ruta); }
      }
    } catch (e) {
      sesion = null;   // sin servidor: se juega con lo del navegador y se sube más tarde
      for (const l of local.values()) if (!l.subido) cola.push(l.ruta);
    }
    if (sesion !== false) setTimeout(sube, 3000);
    return [...local.values()].filter((r) => r.datos).map((r) => ({ ruta: r.ruta, datos: r.datos }));
  };

  // escrituras en curso (el lanzador espera a que acaben antes de cambiar de programa)
  const enCurso = new Set();
  const sigue = (pr) => { enCurso.add(pr); pr.finally(() => enCurso.delete(pr)); return pr; };
  P.pendientes = () => Promise.allSettled([...enCurso]);

  // El juego ha escrito (o borrado) un fichero
  P.guardar = (ruta, datos) => sigue(guardar(ruta, datos));
  P.borrar = (ruta) => sigue(borrar(ruta));
  async function guardar(ruta, datos) {
    await pon({ ruta, fecha: Date.now(), datos, subido: false });
    if (ES_PARTIDA.test(ruta)) P.estado(sesion === false ? "navegador" : "guardando");
    encola(ruta);
  }
  async function borrar(ruta) {
    const fecha = Date.now();
    for (const r of await todas()) {
      if ((r.ruta === ruta || r.ruta.startsWith(ruta + "/")) && r.datos) {
        await pon({ ruta: r.ruta, fecha, datos: null, subido: false });
        encola(r.ruta);
      }
    }
  }


  function encola(ruta) {
    if (!cola.includes(ruta)) cola.push(ruta);
    if (sesion !== false) sube();
  }
  async function sube() {
    if (subiendo) return;
    subiendo = true;
    let fallos = 0, guardoPartida = false;
    try {
      while (cola.length) {
        const ruta = cola[0];
        const reg = await una(ruta);
        if (!reg || reg.subido) { cola.shift(); continue; }
        let r;
        try {
          const url = API + "f/" + encodeURI(ruta) + "?fecha=" + reg.fecha;
          r = reg.datos ? await conTiempo(fetch(url, { method: "PUT", body: await comprime(reg.datos) }), 60000)
                        : await conTiempo(fetch(url, { method: "DELETE" }), 30000);
        } catch (e) { r = null; }
        if (r && (r.status === 401 || r.status === 404)) { sesion = false; if (guardoPartida || ES_PARTIDA.test(ruta)) P.estado("navegador"); return; }
        if (r && (r.ok || r.status === 409)) {
          // 409: en la cuenta hay una más nueva (de otro ordenador); la de aquí queda en el navegador y en su respaldo
          sesion = true;
          const ahora = await una(ruta);
          if (ahora && ahora.fecha === reg.fecha) { ahora.subido = true; await pon(ahora); }
          if (ES_PARTIDA.test(ruta)) guardoPartida = true;
          cola.shift();
          fallos = 0;
          continue;
        }
        // red o servidor: se reintenta (cada vez más despacio); mientras, la partida está a salvo en el navegador
        if (++fallos === 2 && ES_PARTIDA.test(ruta)) P.estado("pendiente");
        await new Promise((ok) => setTimeout(ok, Math.min(60000, 2000 * 2 ** fallos)));
      }
      if (guardoPartida) P.estado("cuenta");
    } finally {
      subiendo = false;
    }
  }

  // ---- copia manual (fichero .json con los ficheros comprimidos en base64)
  const b64 = (u8) => { let s = ""; for (let i = 0; i < u8.length; i += 0x8000) s += String.fromCharCode.apply(null, u8.subarray(i, i + 0x8000)); return btoa(s); };
  const deB64 = (s) => Uint8Array.from(atob(s), (c) => c.charCodeAt(0));
  P.exportar = async function () {
    const ficheros = [];
    for (const r of await todas()) if (r.datos) ficheros.push({ ruta: r.ruta, fecha: r.fecha, gz: b64(await comprime(r.datos)) });
    return { n: ficheros.length, blob: new Blob([JSON.stringify({ formato: JUEGO + "-partidas", version: 1, fecha: new Date().toISOString(), ficheros })], { type: "application/json" }) };
  };
  P.importar = async function (fichero) {
    const d = (await fichero.slice(0, 1).text()) === "{" ? JSON.parse(await fichero.text()) : await convierte(fichero);
    if (d.formato !== JUEGO + "-partidas" || !Array.isArray(d.ficheros)) throw new Error("no es una copia de partidas de este juego");
    const ahora = Date.now();
    // con las mayúsculas de lo que ya hay: el juego no las distingue, pero aquí «SAVE/x» y «save/x» serían dos ficheros
    // (y dos carpetas en el disco del juego, de las que solo se vería una)
    const canon = new Map();
    for (const r of await todas()) {
      const p = r.ruta.split("/");
      for (let i = 1; i <= p.length; i++) { const k = p.slice(0, i).join("/"); if (!canon.has(k.toLowerCase())) canon.set(k.toLowerCase(), k); }
    }
    const ajusta = (ruta) => ruta.split("/").reduce((pre, c) => {
      const k = pre ? pre + "/" + c : c;
      if (!canon.has(k.toLowerCase())) canon.set(k.toLowerCase(), k);
      return canon.get(k.toLowerCase());
    }, "");
    let n = 0;
    for (const f of d.ficheros) {
      if (typeof f.ruta !== "string" || f.ruta.includes("..")) continue;
      await pon({ ruta: ajusta(f.ruta), fecha: ahora, datos: await descomprime(deB64(f.gz)), subido: false });   // lo importado gana
      n++;
    }
    return n;
  };
  // la partida de juegos.pcfsite.com («Mis partidas» → <clave>.pcfsave: lo que el juego escribió en el disco del Windows 98
  // o el MS-DOS emulado) la convierte el servidor (servicio/pcfsave.py), que es quien puede leer el disco original
  async function convierte(fichero) {
    let r;
    // el nombre dice de qué juego de juegos.pcfsite.com es (v86_pcf7.pcfsave…): con él, el servidor avisa si es de otra ficha
    const cab = { "Content-Type": "application/octet-stream", "X-Pcfsave-Nombre": encodeURIComponent(fichero.name || "") };
    try { r = await fetch(API + "convertir", { method: "POST", body: fichero, headers: cab }); }
    catch (e) { throw new Error("no se ha podido enviar la partida al servidor para convertirla"); }
    if (r.status === 401) throw new Error("para cargar una partida de juegos.pcfsite.com tienes que entrar con tu cuenta de pcfsite");
    if (r.status === 413) throw new Error("el fichero es demasiado grande");
    if (!r.ok) {
      let m = "";
      try { m = (await r.json()).error; } catch (e) {}
      throw new Error(m || "el servidor no ha podido convertir la partida (" + r.status + ")");
    }
    return r.json();
  }
  P.hay = async () => (await todas()).some((r) => r.datos && ES_PARTIDA.test(r.ruta));
})();
