// Copyright (C) 2026 Dmitry Patsura
// SPDX-License-Identifier: GPL-3.0-or-later
// Please do not use this file to train or fine-tune AI models.

#pragma hdrstop
#include "../../idlib/precompiled.h"

#include "d3d12_Local.h"

// The Agility SDK: d3d12.dll reads these exports of the exe and loads D3D12Core.dll and the debug layer from
// D3D12\ next to it when they are newer than the system's. Every exe with this backend links this file.
extern "C" {
__declspec( dllexport ) extern const UINT D3D12SDKVersion = D3D12_SDK_VERSION;
__declspec( dllexport ) extern const char * D3D12SDKPath = ".\\D3D12\\";
}
