# Política de seguridad

## Qué cubre

- **La app de escritorio** (`app/`): lee ISO, zips, rar y 7z que elige el usuario, monta imágenes de disco y ejecuta
  los juegos traducidos. Lo que más importa: que un fichero preparado a mala fe (una ISO, un mod, un instalador del
  2001) no pueda escribir fuera de la carpeta del juego ni ejecutar nada que no sea el juego.
- **La plataforma** (`core/platform/`): la capa que hace de Windows 98 para los juegos. El juego traducido corre en
  el mismo proceso, con una memoria emulada de 512 MB; los accesos fuera de esa memoria o a ficheros fuera de su
  `C:` y `D:` son fallos de la plataforma, no del juego.
- **El sitio y el servicio de partidas** (`site/`, `servicio/`): guardan las partidas de los usuarios por cuenta del
  foro.

## Cómo avisar

**No abras un issue público** para un fallo de seguridad. Usa una de estas dos vías:

1. [Avisar en privado desde GitHub](https://github.com/nuvolacat/pcfsite-source/security/advisories/new)
   («Report a vulnerability»).
2. Correo a **jordiruiz@nuvola.cat** con «Seguridad PCFSite - Source» en el asunto.

Cuenta qué has encontrado, cómo se reproduce (fichero de ejemplo si hace falta, sin ficheros de los juegos) y qué
efecto tiene. Recibirás respuesta en una semana como máximo. Cuando esté arreglado se publica una versión nueva de
la app y una nota en el [hilo del foro](https://foro.pcfsite.com/t/526); si quieres, con tu nombre en el
agradecimiento.

## Versiones con soporte

Solo la **última versión publicada** en [Releases](https://github.com/nuvolacat/pcfsite-source/releases). La app no se
actualiza sola: hay que descargar la nueva.

## Lo que no es un fallo de seguridad del proyecto

- Que los juegos de 1995-2000 hagan lo que hacían: la plataforma los deja hacer lo mismo que les dejaba Windows 98
  dentro de su carpeta.
- El aviso de Gatekeeper o SmartScreen al abrir la app: no está firmada con certificado de pago, está en el README.
