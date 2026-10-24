<div class="texto">
<h1>Cómo funciona</h1>
<p>Los PC Fútbol se publicaron entre 1995 y 2000 como programas de MS-DOS y de Windows 95/98. Su código fuente nunca
se publicó. Lo que sí existe es el programa en sí: millones de bytes de código máquina para los procesadores de la
época. Este proyecto convierte ese código máquina, entero y sin cambios, en código C que se compila y ejecuta en los
ordenadores de hoy y en el navegador. Esta página explica exactamente cómo, paso a paso, y qué pasa con tus
partidas y tus datos.</p>

<h2>No es un emulador</h2>
<p>Un emulador (DOSBox, una máquina virtual con Windows 98) imita un PC entero, instrucción a instrucción, mientras
juegas. Aquí la traducción se hace <em>una sola vez</em>, antes de publicar el juego: el resultado es un programa
normal, compilado con el mismo compilador que cualquier aplicación moderna, que no interpreta nada en tiempo de
ejecución. Por eso arranca al instante y el simulador 3D va fluido incluso en el navegador.</p>

<h2>1. Localizar las funciones del programa original</h2>
<p>Cada programa (el <code>MANAGER.EXE</code> de cada juego y las bibliotecas propias de Dinamic que lo acompañan:
<code>gfxwin.dll</code>, <code>d2s2.dll</code>, <code>DINCOMM.DLL</code>…) se analiza con Ghidra, la herramienta
de ingeniería inversa de la NSA, que recorre el código y encuentra dónde empieza y acaba cada función. Un programa
como el del PC Fútbol 2001 tiene unas 91.000. Las bibliotecas de Microsoft que venían en el CD (MFC42, MSVCRT) se
tratan igual, porque el juego las llama de verdad y tienen que comportarse exactamente igual.</p>

<h2>2. Traducir cada instrucción a C</h2>
<p>Un traductor propio desensambla cada función y escribe, por cada instrucción del procesador x86, las líneas de C
que hacen lo mismo. Los registros del procesador (<code>eax</code>, <code>ebx</code>, las banderas, la pila de coma
flotante) pasan a ser variables globales; la memoria del programa es una tabla de 512 MB en la que una dirección de
memoria del original es simplemente un índice. Una instrucción como <code>mov eax, [esp+8]</code> se convierte en
<code>eax = rd32(esp + 8);</code>, y cada línea generada lleva al lado, como comentario, la instrucción original de la
que sale.</p>
<p>Las llamadas entre funciones del juego son llamadas de C. Las llamadas a Windows (abrir un fichero, crear una
ventana, dibujar) se resuelven en el momento de traducir: cada una apunta a una función nuestra que hace lo que haría
Windows. Las excepciones de C++, las tablas de saltos, los hilos y las llamadas a través de punteros (que es como
funcionan las clases de C++) se traducen también.</p>
<p>No se reescribe nada a mano ni se «mejora» la lógica. La táctica, el mercado, el simulador de partidos, la
economía del club, los sorteos y hasta los errores del original son exactamente los del juego de 1998, porque son su
mismo código. Lo único que se cambia en el programa es un parche de seis bytes en el PC Fútbol 4.0: la comprobación
anticopia (que pedía un escudo de una página del manual) se deja siempre correcta, como ya hacían las reediciones.
En los demás juegos no se toca ni un byte.</p>

<h2>3. Volver a escribir lo que ponía Windows</h2>
<p>El juego no hace todo solo: le pide muchas cosas al sistema. Todas esas piezas se han vuelto a escribir desde cero,
en C++ sobre SDL (una biblioteca libre que funciona en macOS, Windows, Linux y el navegador), imitando el
comportamiento de Windows 98 y de DirectX con el detalle que el juego necesita:</p>
<ul>
  <li><strong>Ventanas y controles</strong>: ventanas, menús, botones, listas, cajas de texto, cajas de mensaje, con el
      aspecto y el comportamiento de Windows 98.</li>
  <li><strong>Gráficos</strong>: GDI (texto, bitmaps, paletas de 256 colores), DirectDraw (las superficies de pantalla)
      y Direct3D de la 3 a la 7, con un rasterizador por software propio. Es el que dibuja los partidos 3D del 6.0
      al 2001, y en el 2000 y el 2001, donde todo pasa por Direct3D, también los menús.</li>
  <li><strong>Sonido</strong>: DirectSound (efectos, sonido 3D posicional), DirectMusic (la música MIDI del 2000, con un
      sintetizador propio y el banco de instrumentos General MIDI), DirectShow (los vídeos MPEG de la intro y de los
      goles, y la música MP3 del 2001) y, en el 4.0, la Sound Blaster entera: sus puertos, su DMA y su interrupción.</li>
  <li><strong>MS-DOS y VGA</strong> para el 4.0: las interrupciones del DOS, la tarjeta VGA con sus cuatro planos de
      memoria y el extensor DOS/4GW.</li>
  <li><strong>Sistema</strong>: ficheros y directorios, el CD, el registro de Windows, hilos, temporizadores,
      excepciones, COM/OLE (los controles ActiveX del panel de ayuda, por ejemplo).</li>
</ul>
<p>Lo que el juego ve es un Windows 98 con un disco <code>C:</code> en el que está instalado y un CD en
<code>D:</code>. Cuando abre <code>C:\FUTBOL7\DBDAT\…</code>, una capa nuestra convierte esa ruta en la carpeta
real donde están los datos (en el ordenador) o en una petición al servidor (en el navegador).</p>

<h2>4. Comprobar que es el mismo juego</h2>
<p>Cada juego se recorre con guiones automáticos que mueven el ratón y el teclado solos, sacan capturas de pantalla y
vigilan el registro del programa: menú, Euromanager, oficina, alineación, partido, grabar y cargar. Todas las
capturas de este sitio salen de ahí, de la versión traducida, no del original. Cuando algo falla, se compara con lo
que hace el programa original, instrucción a instrucción, hasta encontrar la diferencia, y se arregla en nuestra
capa, no en el juego. Cada arreglo queda anotado en el Diario de cada ficha.</p>

<h2>5. El navegador</h2>
<p>El mismo código C se compila a WebAssembly, con la misma capa de sistema. El juego corre en un hilo aparte de la
página (un Web Worker), dibuja en un <code>canvas</code> y usa el audio del navegador. Hace falta un navegador
reciente (Chrome, Edge, Firefox o Safari) y unos 2 GB de memoria libre: es la memoria que el programa tenía en un PC
de la época más la tabla donde vive.</p>
<p><strong>Qué se descarga.</strong> El programa traducido (<code>juego.wasm</code>: de 14 MB en el 4.0 a 220 MB en el 2000, que
viajan comprimidos con gzip, unas doce veces menos) y, de los datos, solo lo que el juego va leyendo: cuando abre un fichero, el
navegador pide al servidor de datos el trozo que necesita (peticiones de 512 KB), no el CD entero. En segundo plano,
la página va descargando a la caché del navegador los ficheros que el juego usa más, para que no haya esperas en
medio de una pantalla; en la siguiente visita ya están.</p>
<p><strong>Qué no se descarga.</strong> No hay instalación: nada se escribe en tu ordenador fuera de la caché y del
almacenamiento del propio navegador, y se borra con los datos del sitio como cualquier otra web.</p>

<h2>6. Las partidas guardadas, con exactitud</h2>
<p>El juego graba sus partidas como siempre: escribe ficheros en su carpeta (<code>C:\FUTBOL7\save\…</code>,
<code>C:\PCF2001\Save\…</code>, más algún <code>.ini</code> con las opciones). En el navegador, ese disco
<code>C:</code> está en memoria, así que hay que guardarlo en algún sitio para que sobreviva al cerrar la pestaña. Se
hace en tres capas, por este orden:</p>
<ol>
  <li><strong>En tu navegador.</strong> Cada fichero que el juego escribe se guarda en el almacenamiento local del
      navegador (IndexedDB, bajo el dominio <code>source.pcfsite.com</code>) con su ruta, su contenido y la fecha,
      en cuanto pasan cuatro décimas de segundo sin que cambie y sin que el juego lo tenga abierto
      para escribir. Al volver a abrir el juego, esos ficheros se
      ponen en su sitio antes de que arranque. Esta capa funciona sin cuenta y sin conexión con nuestro servidor:
      si no has entrado con tu cuenta, tus partidas no salen de tu navegador.</li>
  <li><strong>En tu cuenta de pcfsite.</strong> Si has entrado con tu cuenta, cada fichero se sube además, comprimido, a
      un servicio nuestro (<code>/partidas/&lt;juego&gt;/…</code>), y al arrancar se juntan las dos copias: de cada
      fichero gana la más reciente, y lo que falte en un lado se copia al otro. Así puedes seguir la misma liga desde
      otro ordenador. El servicio guarda, por usuario y por juego, el fichero actual y sus 10 últimas versiones (por si
      una partida se estropea y hay que recuperar la anterior). Para saber quién eres, el servicio le pasa al foro la
      cookie de tu sesión y el foro responde con tu identificador; no guarda tu contraseña, tu correo ni ningún dato
      personal: solo los ficheros que escribe el juego, con su nombre y su fecha.</li>
  <li><strong>En un fichero tuyo.</strong> Desde la ficha de cada juego puedes descargar todas tus partidas en un solo
      fichero (<em>Descargar mis partidas</em>) y volver a cargarlas en cualquier navegador (<em>Cargar partidas</em>).
      Es el mismo contenido que las dos capas anteriores, en texto, sin cifrar.</li>
</ol>
<p>Se hace una copia diaria de las partidas guardadas en la cuenta, cifrada, y se conservan 14 días. Si borras tu
cuenta del foro, tus partidas se borran con ella (lo explica pcfsite.com/legal).</p>
<p>Lo que se graba es exactamente lo que grababa el juego original: los mismos ficheros, en el mismo formato. Una
partida guardada aquí se puede abrir en el juego original en un PC de 1998, y al revés.</p>

<h2>7. Qué datos tuyos ve este sitio</h2>
<ul>
  <li>El servidor web guarda los registros de acceso habituales (dirección IP, página pedida, navegador) durante el
      tiempo que marca la política de pcfsite.com.</li>
  <li>Los datos de los juegos se sirven desde un almacén aparte (<code>datos.pcfsite.com</code>, en Cloudflare), que
      solo ve qué trozos de qué ficheros pide tu navegador.</li>
  <li>No hay cuentas propias de este sitio, ni anuncios, ni rastreadores. La cuenta es la del foro de pcfsite.com y solo
      se usa para las partidas y, mientras el sitio esté en pruebas, para el control de acceso.</li>
</ul>

<h2>¿Y los derechos?</h2>
<p>Los juegos se publican con permiso de los titulares de los derechos de Dinamic Multimedia. Este sitio es un
proyecto de preservación hecho por aficionados, sin afiliación con Dinamic ni con sus sucesores. Las marcas y los
contenidos de los juegos son de sus dueños.</p>

<h2>Y después</h2>
<p>El orden es «fiel primero, mejoras después». Con los ocho juegos traducidos y jugables, el trabajo que sigue es
recorrerlos a fondo (temporadas enteras, cada apartado) e ir arreglando lo que aparezca; cada arreglo se anota en el
Diario. Más adelante, con el código ya legible función a función, se podrán añadir cosas que entonces no existían:
resoluciones modernas, pantalla completa nítida, editores.</p>
</div>
