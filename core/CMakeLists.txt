cmake_minimum_required(VERSION 3.24)
project(pcf7 C CXX)

# Línea base: el MANAGER.EXE de PC Fútbol 7 traducido a C (build/gen, lo genera recomp/build.py) +
# la capa de plataforma en C++ sobre SDL3 (platform/).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# El juego (juegos/<id>/juego.json): -DPCF_JUEGO=<id>, por defecto pcf7. Un ejecutable por programa del CD
# traducido: <id> (el programa principal, en build/gen o build/<id>/gen) y <id>-<nombre> para los demás
# (DBASEWIN, INFOSURF, ACTWIN, PLAYVID…, en gen-<nombre>), los que existan.
set(PCF_JUEGO "pcf7" CACHE STRING "juego que se compila (juegos/<id>)")
if(PCF_JUEGO STREQUAL "pcf7")
  set(PCF7_BUILD "${CMAKE_SOURCE_DIR}/build")
else()
  set(PCF7_BUILD "${CMAKE_SOURCE_DIR}/build/${PCF_JUEGO}")
endif()
# juegos de MS-DOS («dos»: true en juego.json): VGA, interrupciones y puertos (platform/src/dos.cpp, vga.cpp)
file(READ "${CMAKE_SOURCE_DIR}/juegos/${PCF_JUEGO}/juego.json" PCF_JUEGO_JSON)
string(JSON PCF_JUEGO_DOS ERROR_VARIABLE _sin_dos GET "${PCF_JUEGO_JSON}" dos)
if(PCF_JUEGO_DOS)
  add_compile_definitions(PCF_DOS=1)
endif()
if(NOT EXISTS "${PCF7_BUILD}/gen/funcs.h")
  message(FATAL_ERROR "Falta ${PCF7_BUILD}/gen: ejecuta antes `PCF_JUEGO=${PCF_JUEGO} .venv/bin/python recomp/build.py`")
endif()

file(GLOB PLATFORM_SRC CONFIGURE_DEPENDS platform/src/*.cpp platform/src/*.c)

# Versión para el navegador (emcmake cmake -B build/web): Emscripten + su SDL3, hilos (Web Workers con
# SharedArrayBuffer: la página necesita COOP/COEP), longjmp de WebAssembly y llamadas de cola. Ver web/README.md.
if(EMSCRIPTEN)
  set(PCF7_WEB_FLAGS -pthread -sUSE_SDL=3 -fwasm-exceptions -sSUPPORT_LONGJMP=wasm -mtail-call)
  add_library(pcf7_sdl INTERFACE)
  target_compile_options(pcf7_sdl INTERFACE ${PCF7_WEB_FLAGS})
  target_link_options(pcf7_sdl INTERFACE ${PCF7_WEB_FLAGS}
    -sWASMFS --js-library=${CMAKE_SOURCE_DIR}/web/libwasmfs_fetch.js -sFORCE_FILESYSTEM=1 -sINITIAL_MEMORY=2048MB -sALLOW_MEMORY_GROWTH=0
    -sPTHREAD_POOL_SIZE=8 -sSTACK_SIZE=8MB -sDEFAULT_PTHREAD_STACK_SIZE=4MB
    -sEXPORTED_RUNTIME_METHODS=ccall -sEXPORTED_FUNCTIONS=_main,_pcf7_volcado,_pcf7_mem ${PCF7_WEB_DEBUG} -sENVIRONMENT=web,worker -sEXIT_RUNTIME=0)
  add_library(SDL3::SDL3 ALIAS pcf7_sdl)
else()
  find_package(SDL3 REQUIRED CONFIG)
endif()

function(pcf7_program exe gendir)
  file(GLOB chunks CONFIGURE_DEPENDS "${gendir}/chunk*.c")
  set(gen ${chunks} "${gendir}/ftab.c" "${gendir}/gp.c" "${gendir}/stubs_default.c" "${gendir}/modinfo.c"
          "${gendir}/juego.c")
  # la receta de la imagen de memoria (imagen.cpp la aplica cuando no hay mem.bin: la app); en la web no hace falta
  if(NOT EMSCRIPTEN)
    if(EXISTS "${gendir}/imagen.c")
      list(APPEND gen "${gendir}/imagen.c")
    else()
      message(WARNING "Falta ${gendir}/imagen.c (vuelve a traducir con recomp/build.py): ${exe} no enlazará")
    endif()
  endif()
  # El código traducido: -fno-inline es OBLIGATORIO (cada función x86 es una función de C y su `ret` es
  # un `return`; si clang las fusiona, un retorno calculado volvería de la función equivocada).
  add_library(${exe}_gen OBJECT ${gen})
  target_include_directories(${exe}_gen PRIVATE platform/include "${gendir}")
  target_compile_options(${exe}_gen PRIVATE -O1 -fno-inline -fno-inline-functions -fno-strict-aliasing
                         -fwrapv -w -g0)
  if(CMAKE_C_COMPILER_ID MATCHES "Clang")   # (GCC no tiene ese límite de anidamiento)
    target_compile_options(${exe}_gen PRIVATE -fbracket-depth=4096)
  endif()
  if(EMSCRIPTEN)
    target_compile_options(${exe}_gen PRIVATE ${PCF7_WEB_FLAGS} ${PCF7_WEB_GEN_OPT})
    # sin la comprobación de pila tras los saltos de cola, `f_x(); return;` es una llamada de cola de verdad
    # (return_call): sin ella las cadenas de saltos entre funciones agotarían la pila del navegador
    target_compile_definitions(${exe}_gen PRIVATE PCF7_WEB)
  endif()
  if(PCF7_WATCH_WRITES)   # diagnóstico: vigilancia de escrituras (ver rt.h)
    target_compile_definitions(${exe}_gen PRIVATE PCF7_WATCH_WRITES)
  endif()
  if(WIN32)   # en Windows cada programa es una DLL (programas/<exe>.dll) que carga el lanzador: un solo .exe a la vista
    add_library(${exe} SHARED ${PLATFORM_SRC} $<TARGET_OBJECTS:${exe}_gen>)
    set_target_properties(${exe} PROPERTIES PREFIX "" SUFFIX ".dll")
    target_compile_definitions(${exe} PRIVATE PCF_DLL)
  else()
    add_executable(${exe} ${PLATFORM_SRC} $<TARGET_OBJECTS:${exe}_gen>)
  endif()
  # mem.bin se busca en <carpeta del ejecutable>/../<gendir relativo a build/>
  file(RELATIVE_PATH gname "${CMAKE_SOURCE_DIR}/build" "${gendir}")
  target_compile_definitions(${exe} PRIVATE PCF7_GENDIR="${gname}")
  target_include_directories(${exe} PRIVATE platform/include platform/src "${gendir}")
  target_compile_options(${exe} PRIVATE -Wall -Wno-unused-function)
  target_link_libraries(${exe} PRIVATE SDL3::SDL3)
endfunction()

pcf7_program(${PCF_JUEGO} "${PCF7_BUILD}/gen")
if(EMSCRIPTEN)
  # en la web el programa se llama juego.js / juego.wasm en todos los juegos (la página es la misma)
  set_target_properties(${PCF_JUEGO} PROPERTIES OUTPUT_NAME juego SUFFIX ".js")
endif()
file(GLOB other_gens LIST_DIRECTORIES true "${PCF7_BUILD}/gen-*")
foreach(g ${other_gens})
  if(EXISTS "${g}/funcs.h")
    get_filename_component(n "${g}" NAME)
    string(REPLACE "gen-" "${PCF_JUEGO}-" exe "${n}")
    pcf7_program(${exe} "${g}")
    if(EMSCRIPTEN)   # juego-<programa>.js: la página lo carga cuando el lanzador lo pide (web/inicio.js)
      string(REPLACE "gen-" "juego-" web "${n}")
      set_target_properties(${exe} PROPERTIES OUTPUT_NAME ${web} SUFFIX ".js")
    endif()
  endif()
endforeach()
