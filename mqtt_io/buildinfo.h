#ifndef BUILDINFO_H
#define BUILDINFO_H

// Build date/time strings captured when buildinfo.c is compiled.
// buildinfo.c is force-rebuilt on every make invocation (see makefile.targets).
extern const char g_pcBuildDate[];   // __DATE__  "Mon DD YYYY"
extern const char g_pcBuildTime[];   // __TIME__  "HH:MM:SS"

#endif // BUILDINFO_H
