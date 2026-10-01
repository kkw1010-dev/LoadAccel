// Target A: the engine's table of distinct source-file lists (README.md).

#pragma once

#include "PCH.h"

namespace FileLists
{
	struct Settings
	{
		int stage = 0;                   // 0 off, 1 count, 2 shadow index, 3 replace
		std::uint32_t compareFirst = 0;  // stage 3: this many index answers are all checked against the engine first
		std::uint32_t sampleEvery = 64;  // stage 3: afterwards one answer in this many is checked
		bool digest = false;             // stages 2 and 3: log a digest of the call sequence and of the table (by file name)
	};

	// False: something about the image is not what the analysis was done on; nothing was installed.
	bool Install(const Settings& a_settings);

	// One summary block in the log. a_audit walks the whole table (kDataLoaded).
	void Summary(const char* a_when, bool a_audit);
}
