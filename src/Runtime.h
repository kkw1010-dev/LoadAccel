// The game versions LoadAccel was analysed on. Every engine address, call offset and code hash in FileLists.cpp
// and LargeRefs.cpp exists once per runtime (docs/analysis.md for 1.6.1170, docs/se-1.5.97-analysis.md for 1.5.97,
// docs/ae-1.6.640-analysis.md for 1.6.640, docs/ae-1.7.104-analysis.md for 1.7.104); main.cpp picks the set at load
// and installs nothing on any other runtime.

#pragma once

namespace loadaccel
{
	enum class Runtime
	{
		k1597,   // SkyrimSE.exe 1.5.97.0 (Address Library version-1-5-97-0.bin)
		k640,    // SkyrimSE.exe 1.6.640.0 (Address Library versionlib-1-6-640-0.bin)
		k1170,   // SkyrimSE.exe 1.6.1170.0 (Address Library versionlib-1-6-1170-0.bin)
		k17104,  // SkyrimSE.exe 1.7.104.0 (Address Library versionlib-1-7-104-0.bin, format 5)
	};
}
