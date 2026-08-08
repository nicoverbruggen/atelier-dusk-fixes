// SPDX-License-Identifier: MIT
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

// `DUSK_FRAME_MAP=<frame>`: the whole structure of one frame -- every render
// target bind, every pixel-shader bind, every draw, in order -- plus a PNG of
// each distinct full-screen colour surface it touched.
//
// WHY A MAP RATHER THAN ANOTHER PROBE. Finding where a pre-UI antialiasing pass
// belongs on this engine has now cost eight runs, and every one of them asked a
// narrow question and got an answer to a slightly different one:
//
//   "does the anchor fire"          -- yes, once per frame, but post-UI or not is unknown
//   "how many transitions"          -- exactly one, so the trigger is not early
//   "how many surfaces accepted"    -- one, but it counted only ACCEPTED ones and
//                                      the surface the composite reads is not
//                                      among them
//   "what does the composite read"  -- a different surface with an identical
//                                      descriptor
//   "what is in that surface"       -- two dumps, both taken at record time on a
//                                      deferred context, both worthless
//
// Each narrowed the search and none of them located the binding. A frame map
// costs one run and answers all of them at once, because the thing being looked
// for -- a surface holding the finished scene without the interface -- is
// recognisable on sight once every candidate is on disk.
//
// SURFACES ARE DUMPED AT PRESENT, NEVER AT THE BIND. This engine records on
// deferred contexts: at the moment a draw is recorded its target still holds
// whatever preceded it, and D3D11_MAP_READ is illegal on the recording context
// anyway. Both mistakes were made tonight and both produced images that looked
// like evidence. The surfaces are held with a reference and written once the
// frame's command lists have executed.
namespace atfix {

bool frameMapEnabled();

// Called from the detours that already own these slots. The map records; it
// never changes what is bound.
void frameMapNoteTargets(ID3D11DeviceContext* context, unsigned int numViews,
                         ID3D11RenderTargetView* const* views);
void frameMapNoteShader(ID3D11DeviceContext* context, void* shader,
                        bool isComposite);
void frameMapNoteDraw(ID3D11DeviceContext* context);

// Called from the hooked Present, after the frame's lists have executed: this
// is where the sequence is printed and the surfaces are written.
void frameMapFrameTick();

}  // namespace atfix
