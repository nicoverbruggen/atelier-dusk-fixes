// SPDX-License-Identifier: MIT
#pragma once

// dusk-fix.ini: where it lives and the values read from it. Definitions in
// config.cpp. This header deliberately pulls in no Windows or D3D headers, so
// the 32-bit launcher proxy can share the same key names without sharing the
// 64-bit DLL's include graph.
//
// The Arland project's split between the two configuration surfaces is carried
// over unchanged, because it earned its keep there: the ini holds the options a
// user is meant to set, and every environment switch is a diagnostic. A
// diagnostic that could be turned on from the ini would eventually be turned on
// by someone reading a forum post, and these diagnostics are slow on purpose.
namespace atfix {

// Absolute path to dusk-fix.ini beside the game executable, creating the file
// with its default keys on first use. Null if the path cannot be resolved, in
// which case every reader below falls back to its built-in default.
const char* configPath();

// Read a boolean from dusk-fix.ini, seeding the default key when it is absent
// so the option is discoverable in the file. Accepts true/false, 1/0, yes/no.
bool duskConfigBool(const char* section, const char* key, bool def);

// Numeric option, for the settings that are a value rather than a switch (the
// supersampling multiplier, the anisotropic level). Seeds `def` when the key is
// absent, exactly as duskConfigBool does.
int duskConfigInt(const char* section, const char* key, int def);

// Write the settings actually in force to the log once at startup: the ini path
// and the values read from it, so a log tells you what the run was configured
// with rather than what the reporter believes it was.
void logConfiguration();

}  // namespace atfix
