// SPDX-License-Identifier: MIT
#pragma once

// Register this engine's scene test with core's scene-pass boundary. See
// scene_target.cpp for the rule and for why the Glow anchor was abandoned.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

namespace atfix {

void installKtglSceneTarget();

// The pre-UI anchor, found by mapping a frame. Its target/run/armed tuple lives
// on the recording context as retained D3D private data, so replacement
// deferred contexts neither overwrite nor inherit one another. See
// scene_target.cpp.
void ktglPreUiNoteTargets(ID3D11DeviceContext* context, unsigned int numViews,
                          struct ID3D11RenderTargetView* const* views);
// Returns an owned texture reference; the caller must Release it.
struct ID3D11Texture2D* ktglPreUiNoteDraw(ID3D11DeviceContext* context);

// The anchor itself, called after a forwarded draw. Exposed because the raster
// correction owns the draw slots whenever supersampling is on, and it reaches
// this through ScenePolicy::afterDraw. See scene_target.cpp.
void ktglPreUiAfterDraw(ID3D11DeviceContext* context);
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
