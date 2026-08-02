// SPDX-License-Identifier: MIT
#pragma once

// Which bind carries Ayesha's 3D scene.
//
// This is PhyreEngine knowledge, which is why it is here and not in
// `src/core/msaa.cpp`. The twin/resolve machinery in core is genuine D3D11
// mechanism and holds for any renderer; the question "of the several dozen
// render-target binds a frame issues, which one is the scene" is a property of
// a particular engine and has to be answered per engine. `src/core/msaa.h`
// states the general form of that argument on MsaaSceneTest.
//
// Getting it wrong is quiet rather than loud: a rule that matches a shadow map
// or a UI layer multisamples the wrong surface and leaves the scene exactly as
// it was, while every counter in the MSAA module reports success. That failure
// mode is the reason this feature was rewritten in the first place, so the rule
// is stated narrowly and its evidence is recorded rather than assumed.
namespace dusk {

// Registers Ayesha's rule with the core MSAA module. Safe to call more than
// once. Costs nothing when MSAA is off -- core only consults the rule when it
// has a structurally twinnable pair in front of it.
void registerPhyreSceneTarget();

}  // namespace dusk
