// Target B core: "is this FormID in any large-reference list of this worldspace?" answered without the
// engine's walk over every list.
//
// No engine or SKSE dependency: the plugin (LargeRefs.cpp) and the offline test (tests/largeref_test.cpp)
// both drive this file, the test against a model of the engine functions written from the disassembly.
//
// What the engine does (SkyrimSE.exe 1.6.1170 and 1.5.97 alike; the IDs here are 1.6.1170's, LargeRefs.cpp has
// both sets): TESObjectREFR::InitItemImpl (ID 19507) calls ID 18216 for
// every exterior reference whose file is not a master-type file. 18216 walks both cell -> FormID[] maps
// of the worldspace's BGSLargeRefData (cellFormIDMap at +0x00, cellFormIDMapFiltered at +0x60) through
// ID 18242 and overwrites every occurrence of the reference's FormID. When the FormID occurs nowhere,
// the walk reads everything and writes nothing.
//
// What this does instead: per BGSLargeRefData, a set of the FormIDs that occur in its lists, filled by one
// walk the first time it is needed. A FormID that is not in the set occurs in no list, so the engine's
// walk would write nothing and is skipped. A FormID in the set is handed to the ENGINE's function, which
// does the removal; nothing here ever writes engine memory.
//
// The set must never miss a FormID that is in a list. It only grows: it starts as everything listed before
// the first removal of the session, every FormID the engine appends through ID 18215 is added (NoteAppend)
// before the engine appends it, and after the engine reads RNAM data into the worldspace (ID 18214,
// NoteBulkLoad) the lists are walked again and added. So it holds every FormID that was in a list at any
// time, which also covers code that puts a removed FormID back (DynDOLOD DLL NG restores large references
// in the filtered map directly).
//
// Skipping is limited to the data load: EndOfDataLoad() (kDataLoaded) makes every later call go to the
// engine. From then on one call in N only tests whether a skip would still have been right, as evidence
// for or against extending the skip to gameplay.

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace loadaccel
{
	// BSTHashMap<CellID, FormID*> exactly as ID 18242 reads it: capacity at +0x0C, entries at +0x28, each
	// entry 24 bytes { key, value, next }, an entry is in use when next != nullptr. The value points at
	// { count, FormID[count] }.
	struct LargeRefMap
	{
		struct Entry
		{
			std::uint32_t key;
			std::uint32_t pad;
			std::uint32_t* value;
			Entry* next;
		};

		std::uint8_t pad00[0x0C];
		std::uint32_t capacity; // 0x0C
		std::uint8_t pad10[0x18];
		Entry* entries; // 0x28
	};
	static_assert(sizeof(LargeRefMap::Entry) == 0x18);
	static_assert(offsetof(LargeRefMap, capacity) == 0x0C);
	static_assert(offsetof(LargeRefMap, entries) == 0x28);
	static_assert(sizeof(LargeRefMap) == 0x30);

	// BGSLargeRefData: the two maps ID 18216 walks. (+0x30 is formIDCellMap; both runtimes have two more maps behind
	// these, at +0x98 and +0xC8: the constructors ID 18212 / 17800 are the same code. ID 18216 touches none of the three.
	// 1.5.97's Remove hands the walk a pointer 8 bytes into each map, and its walk reads capacity and entries 8 bytes
	// lower: the same fields.)
	struct LargeRefData
	{
		LargeRefMap full;         // 0x00 cellFormIDMap
		LargeRefMap formIDToCell; // 0x30 (layout only)
		LargeRefMap filtered;     // 0x60 cellFormIDMapFiltered
	};
	static_assert(offsetof(LargeRefData, filtered) == 0x60);
	static_assert(sizeof(LargeRefData) == 0x90);

	// Calls a_visit(formID) for every list element of one map, in the engine's order. Stops when a_visit returns true.
	template <class Visit>
	bool WalkMap(const LargeRefMap& a_map, Visit&& a_visit)
	{
		if (!a_map.entries) {
			return false;
		}
		const auto* end = a_map.entries + a_map.capacity;
		for (const auto* entry = a_map.entries; entry < end; ++entry) {
			if (!entry->next || !entry->value) {
				continue;
			}
			const auto count = entry->value[0];
			for (std::uint32_t i = 1; i <= count; ++i) {
				if (a_visit(entry->value[i])) {
					return true;
				}
			}
		}
		return false;
	}

	// The ground truth, at the engine's cost: does a_formID occur in any list of either map?
	[[nodiscard]] inline bool Occurs(const LargeRefData& a_data, std::uint32_t a_formID)
	{
		const auto same = [=](std::uint32_t a_listed) { return a_listed == a_formID; };
		return WalkMap(a_data.filtered, same) || WalkMap(a_data.full, same);
	}

	enum class LargeRefMode
	{
		kCount = 1,  // call the engine, count; one call in N is checked by a full walk to learn how many FormIDs are listed at all
		kShadow = 2, // call the engine on every call; before it, compare the set's answer with a full walk
		kReplace = 3 // skip the engine when the set says the FormID is in no list; check a sample by a full walk
	};

	struct LargeRefMismatch
	{
		const LargeRefData* data;
		std::uint32_t formID;
		std::uint64_t call;
	};

	struct LargeRefStats
	{
		std::atomic<std::uint64_t> calls{ 0 };
		std::atomic<std::uint64_t> inSet{ 0 };          // the set said "may be in a list": the engine ran
		std::atomic<std::uint64_t> notInSet{ 0 };       // the set said "in no list"
		std::atomic<std::uint64_t> skipped{ 0 };        // kReplace: engine walk not run
		std::atomic<std::uint64_t> walked{ 0 };         // ground-truth walks done
		std::atomic<std::uint64_t> occurred{ 0 };       // ... of which the FormID was in a list
		std::atomic<std::uint64_t> setWasSuperset{ 0 }; // kShadow: the set said "may be", the walk said "is not" (allowed: costs one engine walk)
		std::atomic<std::uint64_t> appends{ 0 };        // ID 18215 calls seen
		std::atomic<std::uint64_t> appendsNoted{ 0 };   // ... for a worldspace whose set already exists
		std::atomic<std::uint64_t> loads{ 0 };          // ID 18214 calls seen (RNAM data read into a worldspace)
		std::atomic<std::uint64_t> rewalks{ 0 };        // ... after the worldspace's set existed: its lists were walked again
		std::atomic<std::uint64_t> builds{ 0 };         // sets built
		std::atomic<std::uint64_t> healed{ 0 };         // listed FormIDs the audit found missing from a set (and added)
		std::atomic<std::uint64_t> lateCalls{ 0 };      // calls after the data load (the engine walks on all of them)
		std::atomic<std::uint64_t> lateSampled{ 0 };    // ... of which tested: would a skip have been right?
		std::atomic<std::uint64_t> lateWouldSkip{ 0 };  // ... the set said "in no list"
		std::atomic<std::uint64_t> lateWrong{ 0 };      // ... and the FormID was in a list: a skip would have been wrong
		std::atomic<std::uint64_t> skippableTicks{ 0 }; // kShadow: engine time of the calls the set would have skipped
		std::atomic<std::uint64_t> ticks{ 0 };          // whole wrapper
		std::atomic<std::uint64_t> engineTicks{ 0 };    // inside the engine's function
		std::atomic<std::uint64_t> walkTicks{ 0 };      // ground-truth walks
		std::atomic<std::uint64_t> buildTicks{ 0 };     // building sets
		std::atomic<std::uint64_t> sampledTicks{ 0 };   // kReplace: time of the sampled walks, the basis of the saving estimate
		std::atomic<std::uint64_t> sampled{ 0 };        // kReplace: skips checked one in N (the check-first stretch is not counted here)
	};

	class LargeRefCore
	{
	public:
		using Clock = std::int64_t (*)();
		using Report = void (*)(const LargeRefMismatch&);

		// a_compareFirst / a_sampleEvery: in kReplace, the first so many "in no list" answers are all checked by a
		// full walk, afterwards one in a_sampleEvery. In kCount one call in a_sampleEvery is walked for the statistics.
		LargeRefCore(LargeRefMode a_mode, std::uint32_t a_compareFirst, std::uint32_t a_sampleEvery, Clock a_clock, Report a_report) :
			mode_(a_mode), compareFirst_(a_compareFirst), sampleEvery_(a_sampleEvery ? a_sampleEvery : 1), clock_(a_clock), report_(a_report)
		{}

		// ID 18216 (this = BGSLargeRefData*, FormID). a_original() must call the engine's function with the same arguments.
		template <class Original>
		void Remove(const LargeRefData* a_data, std::uint32_t a_formID, Original&& a_original)
		{
			const auto start = clock_();
			stats_.calls.fetch_add(1, std::memory_order_relaxed);
			// FormID 0 is what the engine leaves in emptied list slots, so its walk for 0 is not a no-op: never filtered.
			if (FellBack() || a_formID == 0) {
				CallEngine(a_original);
			} else if (dataLoaded_.load(std::memory_order_relaxed) && mode_ == LargeRefMode::kReplace) {
				Observed(a_data, a_formID);
				CallEngine(a_original);
			} else if (mode_ == LargeRefMode::kCount) {
				Counted(a_data, a_formID);
				CallEngine(a_original);
			} else {
				Filtered(a_data, a_formID, a_original);
			}
			stats_.ticks.fetch_add(static_cast<std::uint64_t>(clock_() - start), std::memory_order_relaxed);
		}

		// Before the engine appends a_formID to a list of a_data (ID 18215).
		void NoteAppend(const LargeRefData* a_data, std::uint32_t a_formID)
		{
			stats_.appends.fetch_add(1, std::memory_order_relaxed);
			if (mode_ == LargeRefMode::kCount) {
				return;
			}
			std::lock_guard lock(mutex_);
			const auto it = worlds_.find(a_data);
			if (it != worlds_.end() && it->second.built) {
				it->second.set.insert(a_formID);
				stats_.appendsNoted.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// After the engine has read RNAM data into a_data (ID 18214): the lists changed wholesale, walk them again.
		void NoteBulkLoad(const LargeRefData* a_data)
		{
			stats_.loads.fetch_add(1, std::memory_order_relaxed);
			std::lock_guard lock(mutex_);
			const auto it = worlds_.find(a_data);
			if (it != worlds_.end() && it->second.built) {
				it->second.built = false; // the set is kept; Built() adds what is listed now
				stats_.rewalks.fetch_add(1, std::memory_order_relaxed);
			}
		}

		// kDataLoaded: from here on the engine walks on every call.
		void EndOfDataLoad() { dataLoaded_.store(true, std::memory_order_relaxed); }

		// Before the engine destroys a_data (ID 18213): the pointer must not be walked again.
		void Forget(const LargeRefData* a_data)
		{
			std::lock_guard lock(mutex_);
			worlds_.erase(a_data);
		}

		struct WorldReport
		{
			const LargeRefData* data = nullptr;
			std::uint64_t calls = 0;    // removal calls for this worldspace
			std::uint64_t notInSet = 0; // ... answered "in no list"
			std::size_t setSize = 0;    // FormIDs in the set (0 in kCount)
			std::size_t cells = 0;      // lists in the full map
			std::size_t cellsFiltered = 0;
			std::size_t listed = 0;  // list elements in both maps right now (zeros not counted)
			std::size_t zeros = 0;   // emptied slots
			std::size_t missing = 0; // list elements whose FormID was not in the set: the set was NOT a superset
		};

		// Walks every list of every worldspace seen so far; with a set, checks that each listed FormID is in it.
		// A FormID found missing is reported and then added, so one unseen writer does not stay a hole for good.
		[[nodiscard]] std::vector<WorldReport> Inspect()
		{
			std::vector<WorldReport> reports;
			std::lock_guard lock(mutex_);
			for (auto& [data, world] : worlds_) {
				WorldReport report;
				report.data = data;
				report.calls = world.calls;
				report.notInSet = world.notInSet;
				report.setSize = world.set.size();
				const auto check = [&](std::uint32_t a_listed) {
					if (a_listed == 0) {
						++report.zeros;
					} else {
						++report.listed;
						if (world.built && world.set.insert(a_listed).second) {
							++report.missing;
							stats_.healed.fetch_add(1, std::memory_order_relaxed);
						}
					}
					return false;
				};
				WalkMap(data->filtered, check);
				WalkMap(data->full, check);
				report.cells = Lists(data->full);
				report.cellsFiltered = Lists(data->filtered);
				reports.push_back(report);
			}
			return reports;
		}

		[[nodiscard]] LargeRefMode GetMode() const { return mode_; }
		[[nodiscard]] bool FellBack() const { return fellBack_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t Mismatches() const { return mismatches_.load(std::memory_order_relaxed); }
		[[nodiscard]] std::uint64_t Contended() const { return contended_.load(std::memory_order_relaxed); }
		[[nodiscard]] const LargeRefStats& Stats() const { return stats_; }

	private:
		struct World
		{
			std::unordered_set<std::uint32_t> set;
			bool built = false;
			std::uint64_t calls = 0;
			std::uint64_t notInSet = 0;
		};

		[[nodiscard]] static std::size_t Lists(const LargeRefMap& a_map)
		{
			std::size_t lists = 0;
			if (a_map.entries) {
				for (std::uint32_t i = 0; i < a_map.capacity; ++i) {
					if (a_map.entries[i].next && a_map.entries[i].value) {
						++lists;
					}
				}
			}
			return lists;
		}

		template <class Original>
		void CallEngine(Original& a_original, bool a_skippable = false)
		{
			const auto start = clock_();
			a_original();
			const auto ticks = static_cast<std::uint64_t>(clock_() - start);
			stats_.engineTicks.fetch_add(ticks, std::memory_order_relaxed);
			if (a_skippable) {
				stats_.skippableTicks.fetch_add(ticks, std::memory_order_relaxed);
			}
		}

		bool Walk(const LargeRefData& a_data, std::uint32_t a_formID, std::uint64_t* a_ticks = nullptr)
		{
			const auto start = clock_();
			const bool occurs = Occurs(a_data, a_formID);
			const auto ticks = static_cast<std::uint64_t>(clock_() - start);
			stats_.walkTicks.fetch_add(ticks, std::memory_order_relaxed);
			stats_.walked.fetch_add(1, std::memory_order_relaxed);
			if (occurs) {
				stats_.occurred.fetch_add(1, std::memory_order_relaxed);
			}
			if (a_ticks) {
				*a_ticks = ticks;
			}
			return occurs;
		}

		[[nodiscard]] std::unique_lock<std::recursive_mutex> Lock()
		{
			std::unique_lock lock(mutex_, std::try_to_lock);
			if (!lock.owns_lock()) {
				contended_.fetch_add(1, std::memory_order_relaxed);
				lock.lock();
			}
			return lock;
		}

		// After the data load in kReplace: the engine walks; one call in sampleEvery_ tests the set against a walk.
		void Observed(const LargeRefData* a_data, std::uint32_t a_formID)
		{
			stats_.lateCalls.fetch_add(1, std::memory_order_relaxed);
			const auto lock = Lock();
			auto& world = Built(a_data);
			++world.calls;
			if (++calls_ % sampleEvery_ != 0) {
				return;
			}
			stats_.lateSampled.fetch_add(1, std::memory_order_relaxed);
			if (!world.set.contains(a_formID)) {
				stats_.lateWouldSkip.fetch_add(1, std::memory_order_relaxed);
				if (Walk(*a_data, a_formID)) {
					stats_.lateWrong.fetch_add(1, std::memory_order_relaxed);
					if (report_) {
						report_({ a_data, a_formID, calls_ });
					}
				}
			}
		}

		// kCount: which worldspaces, and for one call in sampleEvery_ whether the FormID is listed at all.
		void Counted(const LargeRefData* a_data, std::uint32_t a_formID)
		{
			bool sample;
			{
				const auto lock = Lock();
				++worlds_[a_data].calls;
				sample = ++calls_ % sampleEvery_ == 0;
			}
			if (sample) {
				(void)Walk(*a_data, a_formID);
			}
		}

		// With the lock held. Walks the lists when the set does not exist yet or RNAM data arrived since the last
		// walk; what is in the set stays in it.
		World& Built(const LargeRefData* a_data)
		{
			auto& world = worlds_[a_data];
			if (!world.built) {
				const auto start = clock_();
				const auto add = [&](std::uint32_t a_listed) {
					if (a_listed != 0) {
						world.set.insert(a_listed);
					}
					return false;
				};
				WalkMap(a_data->filtered, add);
				WalkMap(a_data->full, add);
				world.built = true;
				stats_.builds.fetch_add(1, std::memory_order_relaxed);
				stats_.buildTicks.fetch_add(static_cast<std::uint64_t>(clock_() - start), std::memory_order_relaxed);
			}
			return world;
		}

		template <class Original>
		void Filtered(const LargeRefData* a_data, std::uint32_t a_formID, Original& a_original)
		{
			const auto lock = Lock();
			const auto call = ++calls_;
			auto& world = Built(a_data);
			++world.calls;

			if (world.set.contains(a_formID)) {
				stats_.inSet.fetch_add(1, std::memory_order_relaxed);
				if (mode_ == LargeRefMode::kShadow && !Walk(*a_data, a_formID)) {
					stats_.setWasSuperset.fetch_add(1, std::memory_order_relaxed);
				}
				CallEngine(a_original);
				// The FormID stays in the set although the engine has just overwritten its occurrences: the set
				// only has to be a superset, and keeping it means nothing here depends on how the removal works.
				return;
			}

			stats_.notInSet.fetch_add(1, std::memory_order_relaxed);
			++world.notInSet;
			const auto answered = ++answered_;
			const bool first = answered <= compareFirst_;
			const bool sample = !first && (answered - compareFirst_) % sampleEvery_ == 0;
			if (mode_ == LargeRefMode::kShadow || first || sample) {
				std::uint64_t ticks = 0;
				const bool occurs = Walk(*a_data, a_formID, &ticks);
				if (mode_ == LargeRefMode::kReplace && sample) {
					stats_.sampled.fetch_add(1, std::memory_order_relaxed);
					stats_.sampledTicks.fetch_add(ticks, std::memory_order_relaxed);
				}
				if (occurs) {
					// The set said "in no list" and the FormID is in one: the skip would have been wrong.
					mismatches_.fetch_add(1, std::memory_order_relaxed);
					if (mode_ == LargeRefMode::kReplace) {
						fellBack_.store(true, std::memory_order_relaxed);
					}
					if (report_) {
						report_({ a_data, a_formID, call });
					}
					CallEngine(a_original);
					return;
				}
			}
			if (mode_ == LargeRefMode::kShadow) {
				CallEngine(a_original, true);
				return;
			}
			stats_.skipped.fetch_add(1, std::memory_order_relaxed);
		}

		const LargeRefMode mode_;
		const std::uint32_t compareFirst_;
		const std::uint32_t sampleEvery_;
		const Clock clock_;
		const Report report_;

		std::recursive_mutex mutex_;
		std::unordered_map<const LargeRefData*, World> worlds_;
		std::uint64_t calls_ = 0;
		std::uint64_t answered_ = 0;
		std::atomic<bool> fellBack_{ false };
		std::atomic<bool> dataLoaded_{ false };
		std::atomic<std::uint64_t> mismatches_{ 0 };
		std::atomic<std::uint64_t> contended_{ 0 };
		LargeRefStats stats_;
	};
}
