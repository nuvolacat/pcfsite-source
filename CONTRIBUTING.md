# Cómo contribuir

Gracias por querer ayudar. Lo que más falta hace, de menor a mayor esfuerzo:

1. **Jugar y contar lo que falla.** Abre un [issue](https://github.com/nuvolacat/pcfsite-source/issues/new/choose)
   con la plantilla «Fallo en un juego»: qué juego, qué sistema y versión de la app, en qué pantalla, qué pasó. Adjunta
   el registro del juego (en la app: `Documentos/PCFSite - Source/<juego>/<juego>.log`; en la web: `?log` en la
   dirección lo muestra en pantalla). Con eso se encuentra casi todo.
2. **Recorrer a fondo un juego**: una temporada entera, los apartados que menos se usan, comparar con el original en
   un PC o máquina virtual de la época.
3. **Probar la app en máquinas distintas** (Windows x64 y ARM, Linux compilado desde el fuente, Macs antiguos) y
   contar qué pasa.
4. **Mejorar la plataforma** (`core/platform/`): cada función de Windows o de DirectX que se implementa mejor
   beneficia a todos los juegos.
5. **Portar otro juego de Dinamic** con el mismo método: [`core/juegos/README.md`](core/juegos/README.md).

Las preguntas y las conversaciones largas van en el [hilo oficial del foro](https://foro.pcfsite.com/t/526); los
fallos concretos, en issues.

## Lo que no se puede subir

**Nada de los juegos.** Ni ISO, ni carpetas instaladas, ni ejecutables, ni datos, ni el código C generado a partir
de ellos (`core/build/`, `gen*`), ni volcados de memoria (`mem.bin`). Ni en el repositorio, ni en issues, ni en
pull requests. Los registros (`.log`) y las capturas de pantalla sí. Una pull request con ficheros de los juegos se
cierra sin más.

## Cómo preparar el entorno

Está en el [README](README.md#construir): requisitos, cómo se traduce un juego de principio a fin, cómo se compila
la app en cada sistema y cómo se prueba. Para trabajar en la plataforma hace falta tener al menos un juego traducido
en `core/build/<id>/`, y para eso hace falta el CD de ese juego (el repositorio no lo lleva).

## Pull requests

- **Una cosa por pull request**, con un título que diga qué cambia y por qué. Si es un arreglo, cuenta cómo se
  reproduce el fallo y cómo se comprobó que ya no pasa (juego, pantalla, registro antes y después).
- **El criterio del proyecto: imitar lo que hace Windows, no parchear el juego.** Si un juego falla, la solución está
  en la plataforma (hacer lo que hacía Windows 98, DirectX o MS-DOS en ese caso), no en cambiar el código traducido.
  Los únicos parches sobre el juego son los que ya aplicaban las reediciones oficiales (anticopia del 4.0).
- **No se reescribe nada a mano** del código generado: sale de `core/recomp/` y se regenera. Si hace falta cambiar
  cómo se traduce algo, el cambio va en el traductor.
- **Estilo**: el que ya hay en cada carpeta (C++17 en la plataforma, Python en las herramientas, comentarios en
  castellano, nombres en castellano). Sin dependencias nuevas salvo que sea imprescindible: la plataforma solo usa
  SDL3 y la biblioteca estándar.
- Las comprobaciones automáticas (`.github/workflows/`) tienen que pasar. El lanzador de la app se compila en
  Ubuntu x86_64 y arm64 en cada cambio; los juegos no se pueden compilar ahí (su C sale de los CD).
- El historial de `main` es un solo commit por versión publicada; las pull requests se integran con *squash*.

## Licencia

Todo lo que entra en el repositorio queda bajo la [GPL-3.0](LICENSE), la misma que el proyecto. Al enviar una pull
request aceptas que tu aportación se publique con esa licencia y con tu nombre en el historial.
