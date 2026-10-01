// Offline test of src/LargeRefCore.h against a model of the engine functions.
//
// The model is written from the disassembly of SkyrimSE.exe 1.6.1170: ID 18242 (walk one cell -> FormID[]
// map and overwrite every occurrence of a FormID with the list's last element, zeroing that last slot; the
// stored count is not changed), ID 18216 (18242 on the filtered map, then on the full map) and ID 18215
// (append a FormID to one cell's list in the full map, reallocating the list). Two copies of the model
// receive the same operations, the reference copy directly, the other through loadaccel::LargeRefCore;
// after every operation all lists of both copies must be identical.
//
// Build and run: tools\Test.ps1. Exit code 0 = every check passed.

#include "../src/LargeRefCore.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>

using namespace loadaccel;

namespace
{
	int failures = 0;
	int checks = 0;

#define CHECK(cond, ...) \
	do { \
		++checks; \
		if (!(cond)) { \
			++failures; \
			std::printf("FAIL %s:%d: %s | ", __FILE__, __LINE__, #cond); \
			std::printf(__VA_ARGS__); \
			std::printf("\n"); \
		} \
	} while (0)

	constexpr std::uint32_t kCapacity = 64;

	// One worldspace's BGSLargeRefData with the memory behind it.
	struct Model
	{
		LargeRefData data{};
		std::vector<LargeRefMap::Entry> fullEntries = std::vector<LargeRefMap::Entry>(kCapacity);
		std::vector<LargeRefMap::Entry> filteredEntries = std::vector<LargeRefMap::Entry>(kCapacity);
		std::vector<std::unique_ptr<std::uint32_t[]>> arrays;
		std::uint64_t visited = 0; // list elements the engine's walks looked at

		Model()
		{
			data.full.capacity = kCapacity;
			data.full.entries = fullEntries.data();
			data.filtered.capacity = kCapacity;
			data.filtered.entries = filteredEntries.data();
		}

		Model(const Model&) = delete;
		Model& operator=(const Model&) = delete;

		std::uint32_t* NewList(std::uint32_t a_count)
		{
			arrays.push_back(std::make_unique<std::uint32_t[]>(a_count + 1));
			arrays.back()[0] = a_count;
			return arrays.back().get();
		}

		LargeRefMap::Entry& Slot(LargeRefMap& a_map, std::uint32_t a_cell)
		{
			for (std::uint32_t i = 0;; ++i) {
				auto& entry = a_map.entries[(a_cell * 2654435761u + i) % a_map.capacity];
				if (!entry.next || entry.key == a_cell) {
					return entry;
				}
			}
		}

		// ID 18215, the part that touches a cell -> FormID[] map: append a_formID to a_cell's list.
		void Append(LargeRefMap& a_map, std::uint32_t a_formID, std::uint32_t a_cell)
		{
			auto& entry = Slot(a_map, a_cell);
			if (entry.next && entry.value) {
				const auto old = entry.value[0];
				auto* list = NewList(old + 1);
				std::memcpy(list + 1, entry.value + 1, old * sizeof(std::uint32_t));
				list[old + 1] = a_formID;
				entry.value = list;
			} else {
				auto* list = NewList(1);
				list[1] = a_formID;
				entry.key = a_cell;
				entry.value = list;
				entry.next = &entry; // in use (the engine stores a chain pointer or its end sentinel)
			}
		}

		// A whole list at once (what the RNAM data looks like after loading), without the per-append reallocation.
		void SetList(LargeRefMap& a_map, std::uint32_t a_cell, const std::vector<std::uint32_t>& a_formIDs)
		{
			auto& entry = Slot(a_map, a_cell);
			auto* list = NewList(static_cast<std::uint32_t>(a_formIDs.size()));
			std::memcpy(list + 1, a_formIDs.data(), a_formIDs.size() * sizeof(std::uint32_t));
			entry.key = a_cell;
			entry.value = list;
			entry.next = &entry;
		}

		// ID 18242.
		void RemoveFromMap(LargeRefMap& a_map, std::uint32_t a_formID)
		{
			for (std::uint32_t e = 0; e < a_map.capacity; ++e) {
				auto& entry = a_map.entries[e];
				if (!entry.next || !entry.value) {
					continue;
				}
				auto* list = entry.value;
				std::uint32_t count = list[0];
				std::uint32_t index = 0;
				std::uint32_t at = 1;
				if (count == 0) {
					continue;
				}
				do {
					++visited;
					if (list[at] == a_formID) {
						--index;
						--at;
						const auto last = list[count];
						list[at + 1] = last;
						list[count] = 0;
						--count;
					}
					++index;
					++at;
				} while (index < count);
			}
		}

		// ID 18216.
		void Remove(std::uint32_t a_formID)
		{
			RemoveFromMap(data.filtered, a_formID);
			RemoveFromMap(data.full, a_formID);
		}
	};

	bool SameLists(const LargeRefMap& a_left, const LargeRefMap& a_right)
	{
		for (std::uint32_t e = 0; e < kCapacity; ++e) {
			const auto& left = a_left.entries[e];
			const auto& right = a_right.entries[e];
			if (!left.next != !right.next) {
				return false;
			}
			if (!left.next) {
				continue;
			}
			if (left.key != right.key || left.value[0] != right.value[0] ||
				std::memcmp(left.value, right.value, (left.value[0] + 1) * sizeof(std::uint32_t)) != 0) {
				return false;
			}
		}
		return true;
	}

	std::int64_t Clock()
	{
		return std::chrono::steady_clock::now().time_since_epoch().count();
	}

	std::vector<LargeRefMismatch> reported;

	std::size_t Missing(LargeRefCore& a_core)
	{
		std::size_t missing = 0;
		for (const auto& world : a_core.Inspect()) {
			missing += world.missing;
		}
		return missing;
	}

	void Report(const LargeRefMismatch& a_mismatch)
	{
		reported.push_back(a_mismatch);
	}

	constexpr std::size_t kWorlds = 2;

	struct Pair
	{
		std::array<Model, kWorlds> reference;
		std::array<Model, kWorlds> engine;
		LargeRefCore core;

		Pair(LargeRefMode a_mode, std::uint32_t a_compareFirst, std::uint32_t a_sampleEvery) :
			core(a_mode, a_compareFirst, a_sampleEvery, Clock, Report)
		{
			reported.clear();
		}

		void Append(std::size_t a_world, std::uint32_t a_formID, std::uint32_t a_cell, bool a_filtered, bool a_tell = true)
		{
			auto& left = reference[a_world];
			auto& right = engine[a_world];
			left.Append(a_filtered ? left.data.filtered : left.data.full, a_formID, a_cell);
			if (a_tell) {
				core.NoteAppend(&right.data, a_formID);
			}
			right.Append(a_filtered ? right.data.filtered : right.data.full, a_formID, a_cell);
		}

		void Remove(std::size_t a_world, std::uint32_t a_formID)
		{
			reference[a_world].Remove(a_formID);
			auto& right = engine[a_world];
			core.Remove(&right.data, a_formID, [&] { right.Remove(a_formID); });
		}

		[[nodiscard]] bool Same() const
		{
			for (std::size_t w = 0; w < kWorlds; ++w) {
				if (!SameLists(reference[w].data.full, engine[w].data.full) || !SameLists(reference[w].data.filtered, engine[w].data.filtered)) {
					return false;
				}
			}
			return true;
		}

		// The RNAM data as it is after the plugins are loaded: FormIDs 1..a_listed spread over cells, some of
		// them under two cells, some twice in one list, a part of them in the filtered map as well.
		void Load(std::mt19937_64& a_random, std::uint32_t a_listed)
		{
			for (std::size_t w = 0; w < kWorlds; ++w) {
				for (std::uint32_t formID = 1; formID <= a_listed; ++formID) {
					const auto cell = static_cast<std::uint32_t>(a_random() % 40);
					Append(w, formID, cell, false);
					if (a_random() % 4 == 0) {
						Append(w, formID, static_cast<std::uint32_t>(a_random() % 40), false);
					}
					if (a_random() % 8 == 0) {
						Append(w, formID, cell, false);
					}
					if (a_random() % 2 == 0) {
						Append(w, formID, cell, true);
					}
				}
			}
		}
	};

	void RandomRuns()
	{
		struct Setup
		{
			LargeRefMode mode;
			std::uint32_t compareFirst;
			std::uint32_t sampleEvery;
			const char* name;
		};
		const Setup setups[] = {
			{ LargeRefMode::kCount, 0, 1, "count" },
			{ LargeRefMode::kShadow, 0, 1, "shadow" },
			{ LargeRefMode::kReplace, 0, 1, "replace, every skip checked" },
			{ LargeRefMode::kReplace, 16, 64, "replace, 16 first then 1 in 64" },
			{ LargeRefMode::kReplace, 0, 1u << 30, "replace, nothing checked" },
		};
		for (const auto& setup : setups) {
			for (std::uint64_t seed = 1; seed <= 6; ++seed) {
				std::mt19937_64 random(seed * 104729);
				Pair pair(setup.mode, setup.compareFirst, setup.sampleEvery);
				const std::uint32_t listed = seed % 2 ? 300 : 60;
				// References initialised: most FormIDs are not listed anywhere (references added by plugins).
				const std::uint32_t pool = listed * 6;
				pair.Load(random, listed);
				const int before = failures;
				for (int i = 0; i < 20000 && failures == before; ++i) {
					const auto world = static_cast<std::size_t>(random() % kWorlds);
					const auto pick = random() % 100;
					if (pick < 80) {
						pair.Remove(world, static_cast<std::uint32_t>(1 + random() % pool));
					} else if (pick < 99) {
						// What ID 18218 does after the removal: the reference is listed again under the cells it overlaps.
						pair.Append(world, static_cast<std::uint32_t>(1 + random() % pool), static_cast<std::uint32_t>(random() % 40), false);
					} else {
						pair.Remove(world, 0);
					}
					CHECK(pair.Same(), "%s seed %llu: lists differ after operation %d", setup.name, seed, i);
				}
				CHECK(reported.empty() && !pair.core.FellBack(), "%s seed %llu: %zu mismatches on healthy data", setup.name, seed, reported.size());
				const auto audit = pair.core.Inspect();
				const auto& stats = pair.core.Stats();
				CHECK(Missing(pair.core) == 0 && audit.size() == kWorlds, "%s seed %llu: audit missing %zu, worldspaces %zu", setup.name, seed, Missing(pair.core),
					audit.size());
				if (setup.mode == LargeRefMode::kCount) {
					CHECK(stats.walked.load() > 0 && stats.occurred.load() > 0 && stats.occurred.load() < stats.walked.load(),
						"%s seed %llu: the sampled walks say %llu of %llu listed", setup.name, seed, stats.occurred.load(), stats.walked.load());
				}
				if (setup.mode == LargeRefMode::kReplace) {
					CHECK(stats.skipped.load() > 0 && pair.reference[0].visited + pair.reference[1].visited > pair.engine[0].visited + pair.engine[1].visited,
						"%s seed %llu: nothing was saved", setup.name, seed);
				}
				if (seed == 1) {
					std::printf("  %-32s calls %llu, engine walks skipped %llu, list elements visited: reference %llu, behind the core %llu\n", setup.name,
						stats.calls.load(), stats.skipped.load(), pair.reference[0].visited + pair.reference[1].visited,
						pair.engine[0].visited + pair.engine[1].visited);
				}
			}
		}
	}

	// A FormID appended behind the core's back (a writer that is not ID 18215).
	void UnseenAppend()
	{
		std::mt19937_64 random(3);
		{
			Pair pair(LargeRefMode::kShadow, 0, 1);
			pair.Load(random, 50);
			pair.Remove(0, 999);                   // builds the set
			pair.Append(0, 777, 5, false, false);  // not announced
			pair.Remove(0, 777);
			CHECK(reported.size() == 1 && reported[0].formID == 777, "unseen append, shadow: %zu mismatches reported", reported.size());
			CHECK(pair.Same(), "unseen append, shadow: lists differ");
			pair.Append(0, 778, 5, false, false);
			CHECK(Missing(pair.core) == 1, "unseen append: the audit does not see it");
			CHECK(Missing(pair.core) == 0, "unseen append: the audit did not add the FormID it found");
			pair.Remove(0, 778);
			CHECK(reported.size() == 1 && pair.Same(), "unseen append: a FormID the audit added was reported or mishandled");
		}
		{
			Pair pair(LargeRefMode::kReplace, 1u << 30, 1);
			pair.Load(random, 50);
			pair.Remove(0, 999);
			pair.Append(0, 777, 5, false, false);
			pair.Remove(0, 777);
			CHECK(reported.size() == 1 && pair.core.FellBack(), "unseen append, replace with checks: not noticed");
			CHECK(pair.Same(), "unseen append, replace with checks: lists differ");
			for (int i = 0; i < 2000; ++i) {
				pair.Remove(static_cast<std::size_t>(random() % kWorlds), static_cast<std::uint32_t>(1 + random() % 300));
			}
			CHECK(pair.Same(), "unseen append, after the fallback: lists differ");
		}
		{
			// The failure this whole design guards against, shown once: no check, an unseen append, a wrong skip.
			Pair pair(LargeRefMode::kReplace, 0, 1u << 30);
			pair.Load(random, 50);
			pair.Remove(0, 999);
			pair.Append(0, 777, 5, false, false);
			pair.Remove(0, 777);
			CHECK(!pair.Same(), "unseen append, replace without checks: expected the lists to differ (the test itself is wrong)");
			CHECK(Missing(pair.core) == 1, "unseen append, replace without checks: the audit does not see it");
		}
	}

	// An append that IS announced after the set was built must make the next removal run the engine.
	void SeenAppend()
	{
		std::mt19937_64 random(4);
		Pair pair(LargeRefMode::kReplace, 0, 1u << 30);
		pair.Load(random, 50);
		pair.Remove(1, 999);
		pair.Append(1, 777, 5, false);
		pair.Append(1, 778, 6, true);
		pair.Remove(1, 777);
		pair.Remove(1, 778);
		CHECK(pair.Same() && reported.empty(), "announced append: lists differ or mismatch reported");
		CHECK(pair.core.Stats().appendsNoted.load() == 2, "announced append: %llu noted", pair.core.Stats().appendsNoted.load());
	}

	// RNAM data read into a worldspace after its set was built (a later plugin's WRLD record): the lists are
	// walked again, so the new FormIDs are not skipped; what was in the set before stays in it.
	void LoadAfterBuild()
	{
		std::mt19937_64 random(6);
		Pair pair(LargeRefMode::kReplace, 0, 1u << 30);
		pair.Load(random, 50);
		pair.Remove(0, 999); // builds the set of world 0
		for (std::uint32_t formID = 5000; formID < 5040; ++formID) {
			pair.Append(0, formID, formID % 40, formID % 2 == 0, false); // the bulk loader does not announce single FormIDs
		}
		pair.core.NoteBulkLoad(&pair.engine[0].data);
		for (std::uint32_t formID = 4990; formID < 5050; ++formID) {
			pair.Remove(0, formID);
		}
		CHECK(pair.Same() && reported.empty() && Missing(pair.core) == 0, "load after build: lists differ, mismatch or audit not clean");
		CHECK(pair.core.Stats().rewalks.load() == 1 && pair.core.Stats().builds.load() == 2, "load after build: rewalks %llu, builds %llu",
			pair.core.Stats().rewalks.load(), pair.core.Stats().builds.load());

		// A FormID that was listed, removed by the engine, and put back by code the core does not see (DynDOLOD
		// DLL NG restores large references this way), with an RNAM load in between: it must still not be skipped.
		pair.Append(0, 6000, 7, true);
		pair.Remove(0, 6000);
		pair.core.NoteBulkLoad(&pair.engine[0].data);
		pair.Remove(0, 999);
		pair.Append(0, 6000, 7, true, false);
		pair.Remove(0, 6000);
		CHECK(pair.Same() && reported.empty(), "restored FormID: skipped although it was listed before");

		pair.core.Forget(&pair.engine[0].data);
		CHECK(pair.core.Inspect().empty(), "forget: the destroyed worldspace is still walked");
	}

	// After the data load nothing is skipped any more; a sample of calls still tests the set.
	void AfterDataLoad()
	{
		std::mt19937_64 random(8);
		Pair pair(LargeRefMode::kReplace, 0, 1);
		pair.Load(random, 50);
		for (int i = 0; i < 500; ++i) {
			pair.Remove(0, static_cast<std::uint32_t>(1 + random() % 300));
		}
		const auto skipped = pair.core.Stats().skipped.load();
		CHECK(skipped > 0, "after data load: nothing was skipped before it");
		pair.core.EndOfDataLoad();
		pair.Append(0, 777, 5, false, false); // unseen
		for (int i = 0; i < 500; ++i) {
			pair.Remove(0, static_cast<std::uint32_t>(1 + random() % 300));
		}
		pair.Remove(0, 777);
		const auto& stats = pair.core.Stats();
		CHECK(stats.skipped.load() == skipped && pair.Same(), "after data load: a walk was skipped or the lists differ");
		CHECK(stats.lateCalls.load() == 501 && stats.lateSampled.load() == 501 && stats.lateWrong.load() == 1 && !pair.core.FellBack(),
			"after data load: late calls %llu, sampled %llu, wrong %llu", stats.lateCalls.load(), stats.lateSampled.load(), stats.lateWrong.load());
	}

	// The size of the real thing, roughly: one worldspace with 200,000 listed FormIDs, 300,000 references
	// initialised of which 5 % are listed. Prints what a call costs with and without the set.
	void Scale()
	{
		std::mt19937_64 random(2026);
		Model model;
		// One big list per cell; the model's 64 slots are enough to make the walk long.
		std::array<std::vector<std::uint32_t>, 60> lists;
		for (std::uint32_t formID = 1; formID <= 200000; ++formID) {
			lists[formID % 60].push_back(formID);
		}
		for (std::uint32_t cell = 0; cell < 60; ++cell) {
			model.SetList(model.data.full, cell, lists[cell]);
		}
		LargeRefCore core(LargeRefMode::kReplace, 0, 1u << 30, Clock, Report);
		reported.clear();
		const auto start = std::chrono::steady_clock::now();
		const int calls = 300000;
		for (int i = 0; i < calls; ++i) {
			const auto formID = static_cast<std::uint32_t>(i % 20 == 0 ? 1 + random() % 200000 : 1000000 + i);
			core.Remove(&model.data, formID, [&] { model.Remove(formID); });
		}
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		const auto& stats = core.Stats();
		const auto engineSeconds = static_cast<double>(stats.engineTicks.load()) / 1e9;
		CHECK(reported.empty() && Missing(core) == 0, "scale: mismatch or audit not clean");
		std::printf("  scale: %d calls in %.2f s: %llu passed to the model's walk (%.2f s, %.0f us each), %llu skipped (%.0f ns each), set built in %.3f s\n", calls,
			seconds, stats.inSet.load(), engineSeconds, engineSeconds / static_cast<double>(stats.inSet.load()) * 1e6, stats.skipped.load(),
			(seconds - engineSeconds) / static_cast<double>(stats.skipped.load()) * 1e9, static_cast<double>(stats.buildTicks.load()) / 1e9);
	}
}

int main()
{
	std::printf("random runs against the reference model\n");
	RandomRuns();
	std::printf("fault injection\n");
	UnseenAppend();
	SeenAppend();
	LoadAfterBuild();
	AfterDataLoad();
	Scale();
	std::printf("%d checks, %d failures\n", checks, failures);
	std::printf(failures ? "TEST FAILED\n" : "TEST PASSED\n");
	return failures ? 1 : 0;
}
