// PCF2001 — grafo DirectShow de la música (dshow.cpp): lo usa el FilterGraph de video.cpp cuando el juego
// mete sus propios filtros (fuente IAsyncReader y renderer PCM) y pide Render del pin de la fuente.
#pragma once
#include "plat.h"

struct DshowMusic;

// IFilterGraph::AddFilter(filter, nombre): crea el estado si hace falta y lo devuelve
DshowMusic *dshow_add_filter(DshowMusic *m, u32 graph, u32 filter, u32 name);
void dshow_remove_filter(DshowMusic *m, u32 filter);   // IFilterGraph::RemoveFilter
// IGraphBuilder::Render(pin de salida de la fuente): lee la pista (IAsyncReader), la decodifica y conecta el renderer
u32 dshow_render(DshowMusic *m, u32 pin);   // HRESULT
// IMediaControl: 0 Stop, 1 Pause, 2 Run (Run entrega las muestras al renderer hasta que este para el grafo)
void dshow_set_state(DshowMusic *m, int &state, int st);
u64 dshow_duration(const DshowMusic *m);    // 100 ns
u64 dshow_position(const DshowMusic *m);
void dshow_free(DshowMusic *m);
// CoCreateInstance de los filtros del sistema que el juego mete en el grafo (divisor MPEG-1, decodificador de
// audio MPEG): objetos vacíos; el trabajo lo hace el grafo. 0 si la clase no es de estas.
u32 dshow_create_filter(u32 clsid, u32 iid);
