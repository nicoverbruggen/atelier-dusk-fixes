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
//
// Verbose logging is the one diagnostic with an ini key, and it has one in the
// Arland project for the same reason: it writes more lines and changes nothing
// else, so a user asked for it in a bug report can turn it on from the launcher
// instead of being talked through an environment variable.
namespace atfix {

// Absolute path to dusk-fix.ini beside the game executable, creating the file
// with its default keys on first use. Null if the path cannot be resolved, in
// which case every reader below falls back to its built-in default.
const char* configPath();

// Read a boolean from dusk-fix.ini, seeding the default key when it is absent
// so the option is discoverable in the file. Accepts true/false, 1/0, yes/no.
bool duskConfigBool(const char* section, const char* key, bool def);

// Numeric option, for the settings that are a value rather than a switch (the
// supersampling multiplier). Seeds `def` when the key is absent, exactly as
// duskConfigBool does.
int duskConfigInt(const char* section, const char* key, int def);

// Whether extra diagnostic logging is enabled: [Diagnostics] VerboseLogging
// (default false), or DUSK_VERBOSE_LOG. Gates the opt-in diagnostic lines so
// the default log stays short enough for a user to read and attach. Crash
// reports are written either way.
//
// A line belongs behind this gate when it repeats, samples, or counts. A line
// that says what the mod decided once per run does not: that is what makes the
// default log worth having.
bool verboseLogging();

// Write the settings actually in force to the log once at startup: the ini path
// and the values read from it, so a log tells you what the run was configured
// with rather than what the reporter believes it was.
void logConfiguration();

}  // namespace atfix
