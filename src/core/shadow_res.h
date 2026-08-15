// SPDX-License-Identifier: MIT
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// Higher-resolution shadow maps on Ayesha, by allocating a mod-owned twin
// beside the engine's own map rather than by resizing anything the engine
// created.
//
// THE DEFECT. Ayesha draws every shadow in a scene into one 1024x1024 depth
// map with no cascades -- the renderer is `PShadowMapRendererSingle` -- so at
// the resolutions this mod renders at, shadow edges are visibly blocky. Nothing
// downstream absorbs the extra detail, which is what makes a bump worth the
// memory: the full factor reaches the screen.
//
// WHY A TWIN AND NOT A RESIZE. The engine's map stays exactly as it allocated
// it, so every engine-side assumption about its size, its memory and its
// lifetime stays true. The mod creates a second, larger texture, hangs it off
// the host by private data so it dies with the host, and redirects three things
// onto it: the clear, the caster's depth bind, the receiver's sample, and any
// copy the engine makes between two maps that both have twins. A host that
// declines the twin keeps the vanilla path with nothing else changed.
//
// THE DESCRIPTOR RULE IS MEASURED, not inherited. A censused Ayesha session
// settles at 15 distinct render-target shapes over 29 targets, and exactly one
// is the shadow map:
//
//     1024x1024 format=44 samples=1 mips=1 arraySize=1 usage=0 bindFlags=0x48
//
// Format 44 is R24G8_TYPELESS and bind 0x48 is DEPTH_STENCIL|SHADER_RESOURCE.
// Five other targets share format 44 and every one differs in size or sample
// count, so size with format and sample count separates it cleanly. The
// remaining conditions below are not needed to identify it; they are there so
// that anything ambiguous declines rather than being guessed at.
//
// THERE ARE TWO MAPS, A CASTER AND A RECEIVER, and the engine transfers one to
// the other every frame with CopySubresourceRegion. A verbose run reports both
// allocated in the same millisecond:
//
//     twin created for host=... bind=0x48   caster A, DEPTH_STENCIL|SHADER_RESOURCE
//     twin created for host=... bind=0x8    receiver B, SHADER_RESOURCE only
//
// That transfer is why the copy is mirrored and not just the clear, the bind and
// the sample. Redirect those three alone and the caster's depth lands in twin A
// while the receiver samples twin B, which then holds nothing but the clear
// value: far depth everywhere, so nothing is ever occluded and the game renders
// with no shadows at all. That was observed before the mirror existed, together
// with the reading that proves the diagnosis -- clearing the twins to 0.0
// instead of 1.0 turned the whole scene shadowed, which is what a receiver
// sampling a map with no caster content in it does.
//
// THE CENSUS CANNOT SEE THE RECEIVER MAP, and an earlier version of this file
// concluded from it that there was one map rather than two. Its `isTarget`
// predicate reports only textures binding RENDER_TARGET or DEPTH_STENCIL, and B
// binds neither -- it is SHADER_RESOURCE only. B still matches the descriptor
// rule above, which tests no bind flags, so it always had a twin; it was the
// measurement that could not show it. `count=1` did not help either: the census
// keys on (shape, call site) and counts how often that row was seen, so a
// second map from another call site is a new row rather than a larger count.
//
// Count the `SHADOWRES twin created` lines to see the pair. They are per-twin
// for exactly this reason -- the once-only announcement above cannot show it.
//
// THE MAPS ARE CREATED ON DEMAND, when a 3D scene first needs them and not at
// renderer init. Anything that looks for them during boot concludes they do not
// exist, and a run that stops at the title screen never allocates them at all.
//
// THE VIEWPORT COMES FREE, and that is measured too. The caster pass submits
//
//     viewport=1024x1024@0,0 scissor=1024x1024 boundTarget=1024x1024
//
// which is origin-anchored and full-surface, so highres.cpp's raster correction
// enlarges it to the twin with no code here. MinDepth and MaxDepth survive
// because that correction reads the struct back from RSGetViewports and writes
// only the width and height, so the caster's depth remap is untouched.
//
// THAT MAKES THE HIGH-RESOLUTION FIX A PREREQUISITE, not a companion. With it
// off and twins on, the caster would render into the top-left 1024x1024 corner
// of the enlarged map and the receiver would sample the rest as undefined
// depth. That is a broken picture rather than a degraded one, so this declines
// and says so in the log instead.
//
// THE PCF TAP RESCALE IS NOT HERE, AND IT IS NOT OPTIONAL. Enlarging the map
// alone changes almost nothing on screen: `calculateShadow2` offsets its taps
// by a `tapScale` stated in UV, so the filter keeps covering the same world
// area however many texels sit under it. Measured indoors, multiplier 8 against
// multiplier 1 gave a shadow edge of 11 px against 9 px -- no change. The
// sibling Arland project rescales the value whenever its multiplier is on,
// which is why the same bump is obvious there.
//
// Correcting it needs the engine's own code rather than a D3D interception, so
// it lives in `engines/phyre/shadow_tap.h` where this module's build-specific
// addresses belong. This file allocates and redirects; that one makes the
// result visible. Neither is much use without the other.

namespace atfix {

// The shadow map's edge length: 2048, 4096 or 8192 when asked for, otherwise
// 1024, which means the vanilla path with nothing created and nothing
// redirected. `[Rendering] ShadowMultiplier` selects the factor.
unsigned int shadowMapResolution();

// Whether this session wants twins at all: a supported game, the feature on,
// a factor above one, and the high-resolution fix available to carry the
// viewport. Answered once and cached.
bool shadowResWanted();

// Called from highres.cpp's CreateTexture2D detour after the host is created,
// with the descriptor as the GAME asked for it rather than as the resolution
// fix may have rewritten it. Creates the twin when the host is the shadow map
// and every safety condition holds; does nothing otherwise.
void shadowResNoteCreation(ID3D11Device* device,
                           const D3D11_TEXTURE2D_DESC* originalDesc,
                           const D3D11_SUBRESOURCE_DATA* initialData,
                           ID3D11Texture2D* created);

// Mirror a depth clear onto the twin, so the enlarged caster map starts each
// shadow pass in the same state the engine gave its own. No-op when the view's
// texture has no twin.
void shadowResMirrorClear(ID3D11DeviceContext* context,
                          ID3D11DepthStencilView* dsv, UINT flags, FLOAT depth,
                          UINT8 stencil);

// The twin DSV to bind in place of `dsv`, or null to bind what the engine
// asked for. Returns a retained view the caller releases after forwarding.
//
// Depth-only binds only. A colour target alongside the shadow map cannot match
// the twin's size, so that case declines and keeps the vanilla pass.
ID3D11DepthStencilView* shadowResRedirectDsv(
  ID3D11DepthStencilView* dsv, UINT rtvCount,
  ID3D11RenderTargetView* const* rtvs);

// Substitute twin SRVs into `out`, returning true when at least one view was
// replaced and `out` should be forwarded instead of `views`. Replacements are
// retained; the caller releases every entry of `out` that differs from `views`
// after the forwarded call has taken its own reference.
bool shadowResSubstituteSrvs(UINT numViews,
                             ID3D11ShaderResourceView* const* views,
                             ID3D11ShaderResourceView** out, UINT outCapacity);

// Set the raster state for a caster pass that has just been redirected onto the
// twin, sized to the twin rather than to the engine's map.
//
// The raster correction cannot be relied on for this. It settles at the first
// draw after a viewport or scissor submission marks the context stale, and the
// engine submits its 1024 viewport for a 1024 map -- correctly, as far as it
// knows. Whether that mark survives to the caster draws depends on ordering
// this module does not control, and when it does not the casters fill the
// top-left quarter of the twin while the receiver samples the whole of it.
// Setting it here makes the pass depend on nothing but this bind.
void shadowResApplyCasterViewport(ID3D11DeviceContext* context);

// ---- wiring for d3d11_hooks.cpp -------------------------------------------
//
// This module owns this detour and everything it decides; it does not own the
// vtable it is installed into. See d3d11_hooks.h. Nothing outside
// d3d11_hooks.cpp should reference this section.
//
// The remaining interception points are calls into the functions above from
// detours scene_pass.cpp and highres.cpp already own, because a vtable slot
// takes one hook and those slots are taken. Adding a second row for a slot
// already claimed declines the entire install, this feature and the resolution
// fix together. The three below are slots nothing else in this repository
// claims, so this module installs them itself.
void STDMETHODCALLTYPE hookedClearDepthStencilView(
  ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
void STDMETHODCALLTYPE hookedCopyResource(
  ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
void STDMETHODCALLTYPE hookedCopySubresourceRegion(
  ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT,
  ID3D11Resource*, UINT, const D3D11_BOX*);

}  // namespace atfix
