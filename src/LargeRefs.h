// Target B: the walk over every large-reference list that TESObjectREFR::InitItemImpl runs for each
// exterior reference of a non-master file (README.md).

#pragma once

#include "PCH.h"

#include "Runtime.h"

namespace LargeRefs
{
	struct Settings
	{
		int stage = 0;                  // 0 off, 1 count, 2 shadow set, 3 replace
		std::uint32_t compareFirst = 0; // stage 3: this many "in no list" answers are all checked by a full walk first
		std::uint32_t sampleEvery = 64; // stage 3: afterwards one in this many; stage 1: one call in this many is walked for the statistics
		bool dump = false;              // at every summary, write all lists of the worldspaces seen to SKSE\LoadAccel-largerefs-<when>.txt
	};

	// False: something about the image is not what the analysis was done on; nothing was installed.
	bool Install(const Settings& a_settings, loadaccel::Runtime a_runtime);

	// One summary block in the log. Walks every list of the worldspaces seen so far.
	void Summary(const char* a_when);

	// kDataLoaded, after the summary: from here on the engine walks on every call (stage 3 skips only during the
	// data load; other plugins change the lists directly during gameplay).
	void EndOfDataLoad();
}
