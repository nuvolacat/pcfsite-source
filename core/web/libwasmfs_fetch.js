/**
 * @license
 * Copyright 2022 The Emscripten Authors
 * SPDX-License-Identifier: MIT
 */

// PCF7: copia de src/lib/libwasmfs_fetch.js de Emscripten 6.0.12 con un arreglo (se enlaza con --js-library y
// sustituye a la original). Fallo: si el fichero se bajó entero (pesa ≤ 2 trozos), las lecturas siguientes seguían
// calculando los trozos con el tamaño de trozo del backend; una lectura que cruzaba el primer límite volvía a pedir
// datos y recortaba el «trozo 0» (el fichero entero) a ese tamaño, y lo de detrás se leía como ceros.

addToLibrary({
  $wasmFS$JSMemoryRanges: {},

  // Fetch backend: On first access of the file (either a read or a getSize), it
  // will fetch() the data from the network asynchronously. Otherwise, after
  // that fetch it behaves just like JSFile (and it reuses the code from there).

  _wasmfs_create_fetch_backend_js__deps: [
    '$wasmFS$backends',
    '$wasmFS$JSMemoryRanges',
    '_wasmfs_fetch_get_file_url',
    '_wasmfs_fetch_get_chunk_size',
  ],
  _wasmfs_create_fetch_backend_js: async function(backend) {
    // Get a promise that fetches the data and stores it in JS memory (if it has
    // not already been fetched).
    // PCF7: la página (web/inicio.js) va guardando los ficheros enteros en la caché del navegador (Cache API,
    // «<juego>-datos-<versión>»). Si un fichero ya está allí se lee de disco (Blob) en vez de pedirlo a la red.
    var pcf7Blobs = {};
    async function pcf7Blob(url) {   // en todas las cachés del sitio: cada juego tiene la suya y las URL no se repiten
      if (pcf7Blobs[url]) return pcf7Blobs[url];
      try {
        if (typeof caches == 'undefined') return null;
        var r = await caches.match(url);
        if (!r) return null;
        return (pcf7Blobs[url] = await r.blob());
      } catch (e) {
        return null;
      }
    }

    async function getFileRange(file, offset, len) {
      var url = '';
      var fileUrl_p = __wasmfs_fetch_get_file_url(file);
      var fileUrl = UTF8ToString(fileUrl_p);
      fileUrl = fileUrl.replace(/([^:\/])\/{2,}/g, '$1/');   // PCF7: «base//ruta» → «base/ruta» (R2 no junta barras)
      var isAbs = fileUrl.indexOf('://') !== -1;
      if (isAbs) {
        url = fileUrl;
      } else {
        try {
          var u = new URL(fileUrl, self.location.origin);
          url = u.toString();
        } catch (_e) {
          throw {status: 404};
        }
      }
      var chunkSize = __wasmfs_fetch_get_chunk_size(file);
      offset ??= 0;
      len ??= chunkSize;
      // In which chunk does the seeked range start?  E.g., 5-14 with chunksize 8 will start in chunk 0.
      if (!(file in wasmFS$JSMemoryRanges)) {   // PCF7: ¿ya está en la caché del navegador?
        var blob = await pcf7Blob(url);
        if (blob && !(file in wasmFS$JSMemoryRanges)) {
          wasmFS$JSMemoryRanges[file] = {size: blob.size, chunks: [], chunkSize, blob};
          len = Math.min(len, blob.size - offset);
        }
      }
      if (!(file in wasmFS$JSMemoryRanges)) {
        // PCF7: no estaba en la caché (no está en la precarga o aún no ha llegado): el juego espera a la red
        if (!globalThis.pcf7DeRed) globalThis.pcf7DeRed = {};
        if (!pcf7DeRed[url]) { pcf7DeRed[url] = 1; out('datos: de la red, sin precargar: ' + url.replace(/^.*?\/datos\//, '').replace(/^https:\/\/datos\.pcfsite\.com\/[^\/]+\//, '')); }
        var fileInfo = await fetch(url, {method:'HEAD', headers:{'Range': 'bytes=0-'}});
        if (fileInfo.ok &&
            fileInfo.headers.has('Content-Length') &&
            fileInfo.headers.get('Accept-Ranges') == 'bytes' &&
            (parseInt(fileInfo.headers.get('Content-Length'), 10) > chunkSize*2)) {
          var size = parseInt(fileInfo.headers.get('Content-Length'), 10);
          wasmFS$JSMemoryRanges[file] = {
            size,
            chunks: [],
            chunkSize: chunkSize
          };
          len = Math.min(len, size-offset);
        } else {
          // may as well/forced to download the whole file
          var wholeFileReq = await fetch(url);
          if (!wholeFileReq.ok) {
            throw wholeFileReq;
          }
          var wholeFileData = new Uint8Array(await wholeFileReq.arrayBuffer());
          wasmFS$JSMemoryRanges[file] = {
            size: wholeFileData.byteLength,
            chunks: [wholeFileData],
            chunkSize: wholeFileData.byteLength
          };
          return;
        }
      }
      chunkSize = wasmFS$JSMemoryRanges[file].chunkSize;   // PCF7: el del fichero (ver arriba)
      var firstChunk = (offset / chunkSize) | 0;
      // In which chunk does the seeked range end?  E.g., 5-14 with chunksize 8 will end in chunk 1, as will 5-16 (since byte 16 isn't requested).
      // This will always give us a chunk >= firstChunk since len > 0.
      var lastChunk = ((offset+len-1) / chunkSize) | 0;
      var allPresent = true;
      var i;
      // Do we have all the chunks already?  If so, we don't need to do any fetches.
      for (i = firstChunk; i <= lastChunk; i++) {
        if (!wasmFS$JSMemoryRanges[file].chunks[i]) {
          allPresent = false;
          break;
        }
      }
      if (allPresent) {
        // The data is already here, so nothing to do before we continue on to
        // the actual read.
        return;
      }
      // This is the first time we want the chunks' data.  We'll make
      // one request for all the chunks we need, rather than one
      // request per chunk.
      var start = firstChunk * chunkSize;
      // We must fetch *up to* the last byte of the last chunk.
      var end = (lastChunk+1) * chunkSize;
      var info = wasmFS$JSMemoryRanges[file];
      if (!info.blob) {   // PCF7: quizá ha llegado a la caché después de abrirse
        var b2 = await pcf7Blob(url);
        if (b2 && b2.size == info.size) info.blob = b2;
      }
      var bytes;
      if (info.blob) {
        bytes = new Uint8Array(await info.blob.slice(start, Math.min(end, info.size)).arrayBuffer());
      } else {
        var response = await fetch(url, {headers:{'Range': `bytes=${start}-${end-1}`}});
        if (!response.ok) {
          throw response;
        }
        bytes = new Uint8Array(await response.arrayBuffer());
      }
      for (i = firstChunk; i <= lastChunk; i++) {
        wasmFS$JSMemoryRanges[file].chunks[i] = bytes.slice(i*chunkSize-start,(i+1)*chunkSize-start);
      }
    }

    wasmFS$backends[backend] = {
      // alloc/free operations are not actually async. Just forward to the
      // parent class, but we must return a Promise as the caller expects.
      allocFile: async (file) => { /* nop */ },
      freeFile: async (file) => {
        // free memory
        wasmFS$JSMemoryRanges[file] = undefined;
      },

      write: async (file, buffer, length, offset) => {
        console.error('TODO: file writing in fetch backend? read-only for now');
      },

      // read/getSize fetch the data, then forward to the parent class.
      read: async (file, buffer, length, offset) => {
        // This function assumes that offset is non-negative and length is positive.
        // C read() doesn't take an offset and so doesn't have to deal with the former situation,
        // and if the length is 0 or the offset is negative there's no reasonable read we can make.
        if (offset < 0 || length <= 0) {
          return 0;
        }
        try {
          await getFileRange(file, offset || 0, length);
        } catch (failedResponse) {
          return failedResponse.status === 404 ? -{{{ cDefs.ENOENT }}} : -{{{ cDefs.EBADF }}};
        }
        var fileInfo = wasmFS$JSMemoryRanges[file];
        length = Math.min(length, fileInfo.size-offset);
        // As above, we check the length just in case offset was beyond size and length is now negative.
        if (length <= 0) {
          return 0;
        }
        var chunks = fileInfo.chunks;
        var chunkSize = fileInfo.chunkSize;
        var firstChunk = (offset / chunkSize) | 0;
        // See comments in getFileRange.
        var lastChunk = ((offset+length-1) / chunkSize) | 0;
        var readLength = 0;
        for (var i = firstChunk; i <= lastChunk; i++) {
          var chunk = chunks[i];
          var start = Math.max(i*chunkSize, offset);
          var chunkStart = i*chunkSize;
          var end = Math.min(chunkStart+chunkSize, offset+length);
          HEAPU8.set(chunk.subarray(start-chunkStart, end-chunkStart), buffer+(start-offset));
          readLength = end - offset;
        }
        return readLength;
      },
      getSize: async (file) => {
        try {
          await getFileRange(file, 0, 0);
        } catch (failedResponse) {
          return 0;
        }
        return wasmFS$JSMemoryRanges[file].size;
      },
    };
  },

});
