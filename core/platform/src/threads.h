// PCF7 — hilos cooperativos del invitado (threads.cpp).
#pragma once
#include "plat.h"

void threads_init_main();
u32 current_thread_id();
bool other_threads();                 // ¿hay otros hilos vivos?
void thread_yield();                  // cede el testigo a otro hilo (si lo hay) y vuelve cuando le toca
void cooperative_sleep(u32 ms);       // espera ms dejando correr a los demás hilos
[[noreturn]] void thread_exit(u32 code);
bool thread_done(u32 handle, u32 *exit_code);
bool is_thread_handle(u32 handle);
void threads_dump();                  // diagnóstico: dónde está cada hilo en espera
// otro programa en un hilo (hace de proceso aparte): su módulo y su línea de órdenes
u32 thread_spawn_process(u32 entry, u32 exe_base, const std::string &cmdline);
u32 thread_exe_base();                // módulo principal del "proceso" del hilo (0x400000 si es el juego)
u32 thread_cmdline();                 // su línea de órdenes (0: la del juego)
bool thread_is_process();
