// tiramisu/src/trt/bridge_dstorage.h
// DSTORAGE.dll (DirectStorage) for the guest (bridge_dstorage.cpp).
//
// DirectStorage is the GDK fast-io stack: games batch file reads into a
// DSTORAGE_QUEUE and the OS issues NVMe-depth DMA. UnityPlayer 6 imports
// DSTORAGE.dll!DStorageGetFactory unconditionally (measured: Go Next
// corpus demo failed LOAD until this surface existed).
//
// console presence of dstorage.dll is UNPROVEN (SYSTEM(C) -> the
// boot-determinism gate flagged it), so the bridge takes over the whole
// module: the factory probe succeeds (S_OK) and every per-call io surface
// fails visibly with E_NOTIMPL + a named log, so optional-path games
// degrade to stdio and required-path games name the gap in the boot
// report instead of crashing on a null vtable.
#ifndef TIRAMISU_BRIDGE_DSTORAGE_H
#define TIRAMISU_BRIDGE_DSTORAGE_H

#include "trt.h"

int trt_bridge_register_dstorage(void);

#endif  // TIRAMISU_BRIDGE_DSTORAGE_H
