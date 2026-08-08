// SPDX-License-Identifier: MIT
#pragma once

// Register this engine's scene test with core's scene-pass boundary. See
// scene_target.cpp for the rule and for why the Glow anchor was abandoned.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace atfix {

void installKtglSceneTarget();

// The pre-UI anchor, found by mapping a frame. See scene_target.cpp.
void ktglPreUiNoteTargets(unsigned int numViews,
                          struct ID3D11RenderTargetView* const* views);
struct ID3D11Texture2D* ktglPreUiNoteDraw();
void ktglPreUiFrameTick();
bool ktglPreUiActive();

// Wiring for d3d11_hooks.cpp: the four draw slots this feature owns.
void STDMETHODCALLTYPE hookedPreUiDraw(ID3D11DeviceContext*, UINT, UINT);
void STDMETHODCALLTYPE hookedPreUiDrawIndexed(ID3D11DeviceContext*, UINT, UINT,
                                              INT);
void STDMETHODCALLTYPE hookedPreUiDrawIndexedInstanced(ID3D11DeviceContext*,
                                                       UINT, UINT, UINT, INT,
                                                       UINT);
void STDMETHODCALLTYPE hookedPreUiDrawInstanced(ID3D11DeviceContext*, UINT,
                                                UINT, UINT, UINT);

}  // namespace atfix
