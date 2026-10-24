// PCF7 — COMCTL32 (controles comunes de Windows) implementado en el host. Las clases de ventana que ya
// tiene user32 (msctls_statusbar32) se registran con las del sistema; aquí van las funciones exportadas.
#include "ctlutil.h"

STUB(ext_comctl32_dll_ord17) { RET(0, 0); }                 // InitCommonControls
STUB(ext_comctl32_dll_InitCommonControls) { RET(0, 0); }
STUB(ext_comctl32_dll_InitCommonControlsEx) { RET(1, 1); }
