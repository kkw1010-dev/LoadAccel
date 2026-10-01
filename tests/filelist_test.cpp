// Offline test of src/FileListCore.h against a model of the engine functions.
//
// The model is written from the disassembly of SkyrimSE.exe 1.6.1170 (IDs 14580, 14569, 14570): a table of
// list pointers searched linearly, first match wins, a new entry appended on a miss. Two copies of the
// model receive the same operations: the reference copy directly, the other through loadaccel::Core. After
// every operation both must have returned the entry at the same table position and the two tables must
// hold the same lists. Faults are then injected (entries appended behind the index's back, changed in
// place, duplicated, removed) and the test checks that the core either stays right or notices and falls
// back, and that what it returns is still the reference answer.
//
// Build and run: tools\Test.ps1 (plain cl, no CommonLib). Exit code 0 = every check passed.

#include "../src/FileListCore.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>

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

	void* File(std::uint64_t a_number)
	{
		return reinterpret_cast<void*>(0x1000 + a_number * 0x10);
	}

	// The engine side: BSTArray<TESFileArray*> plus the three functions that search it.
	struct Model
	{
		std::vector<FileArray*> table;
		FileArray** data = nullptr;
		std::uint32_t count = 0;
		std::vector<std::unique_ptr<FileArray>> lists;
		std::vector<std::unique_ptr<void*[]>> arrays;
		std::uint64_t visited = 0; // table entries looked at by the linear searches

		FileArray* New(std::uint32_t a_size)
		{
			auto list = std::make_unique<FileArray>();
			list->size = a_size;
			list->data = nullptr;
			if (a_size) {
				arrays.push_back(std::make_unique<void*[]>(a_size));
				list->data = arrays.back().get();
			}
			lists.push_back(std::move(list));
			return lists.back().get();
		}

		void Push(FileArray* a_entry)
		{
			table.push_back(a_entry);
			data = table.data();
			count = static_cast<std::uint32_t>(table.size());
		}

		void Pop()
		{
			table.pop_back();
			data = table.data();
			count = static_cast<std::uint32_t>(table.size());
		}

		// ID 14580.
		FileArray* FindOrAdd(void** a_files, std::uint32_t a_count)
		{
			for (std::uint32_t i = 0; i < count; ++i) {
				++visited;
				auto* entry = data[i];
				if (entry->size != a_count) {
					continue;
				}
				std::uint32_t k = 0;
				while (k < a_count && entry->data[k] == a_files[k]) {
					++k;
				}
				if (k == a_count) {
					return entry;
				}
			}
			auto* entry = New(a_count);
			for (std::uint32_t k = 0; k < a_count; ++k) {
				entry->data[k] = a_files[k];
			}
			Push(entry);
			return entry;
		}

		// ID 14569.
		FileArray* WithFileAdded(FileArray* a_list, void* a_file)
		{
			const auto size = a_list->size;
			for (std::uint32_t k = 0; k < size; ++k) {
				if (a_list->data[k] == a_file) {
					return a_list;
				}
			}
			for (std::uint32_t i = 0; i < count; ++i) {
				++visited;
				auto* entry = data[i];
				if (entry->size - 1 != size) {
					continue;
				}
				std::uint32_t k = 0;
				while (k < size && entry->data[k] == a_list->data[k]) {
					++k;
				}
				if (k == size && entry->data[size] == a_file) {
					return entry;
				}
			}
			auto* entry = New(size + 1);
			for (std::uint32_t k = 0; k < size; ++k) {
				entry->data[k] = a_list->data[k];
			}
			entry->data[size] = a_file;
			Push(entry);
			return entry;
		}

		// ID 14570: the list without one file. When the file is the last element the function searches and
		// appends by itself (inlined); otherwise it builds the shorter list and calls ID 14580 through a_find.
		template <class Find>
		FileArray* WithoutFile(FileArray* a_list, void* a_file, Find&& a_find)
		{
			const auto size = a_list->size;
			std::uint32_t at = 0;
			while (at < size && a_list->data[at] != a_file) {
				++at;
			}
			if (size - at == 0) {
				return a_list;
			}
			if (size - at == 1) {
				for (std::uint32_t i = 0; i < count; ++i) {
					++visited;
					auto* entry = data[i];
					if (entry->size != size - 1) {
						continue;
					}
					std::uint32_t k = 0;
					while (k < entry->size && entry->data[k] == a_list->data[k]) {
						++k;
					}
					if (k == entry->size) {
						return entry;
					}
				}
				auto* entry = New(size - 1);
				for (std::uint32_t k = 0; k + 1 < size; ++k) {
					entry->data[k] = a_list->data[k];
				}
				Push(entry);
				return entry;
			}
			std::vector<void*> shorter;
			for (std::uint32_t k = 0; k < size; ++k) {
				if (k != at) {
					shorter.push_back(a_list->data[k]);
				}
			}
			return a_find(shorter.data(), static_cast<std::uint32_t>(shorter.size()));
		}

		[[nodiscard]] long Position(const FileArray* a_entry) const
		{
			for (std::uint32_t i = 0; i < count; ++i) {
				if (data[i] == a_entry) {
					return static_cast<long>(i);
				}
			}
			return -1;
		}
	};

	std::int64_t Clock()
	{
		return std::chrono::steady_clock::now().time_since_epoch().count();
	}

	std::vector<Mismatch> reported;

	void Report(const Mismatch& a_mismatch)
	{
		reported.push_back(a_mismatch);
	}

	constexpr std::size_t kSites = 7;       // five lookups, two calls of ID 14570
	constexpr std::size_t kLookupSites = 5;
	constexpr std::size_t kWithoutSite = 5;
	using TestCore = Core<kSites>;

	// One reference model, one model behind the core, the same operations on both.
	struct Pair
	{
		Model reference;
		Model engine;
		TestCore core;
		std::vector<FileArray*> looseReference; // lists that are not in the table (a form may hold one)
		std::vector<FileArray*> looseEngine;

		Pair(Mode a_mode, std::uint32_t a_compareFirst, std::uint32_t a_sampleEvery) :
			core(&engine.data, &engine.count, a_mode, a_compareFirst, a_sampleEvery, Clock, Report)
		{
			reported.clear();
		}

		FileArray* CoreFind(std::size_t a_site, void** a_files, std::uint32_t a_count)
		{
			return core.FindOrAdd(a_site, a_files, a_count, [&] { return engine.FindOrAdd(a_files, a_count); });
		}

		void Same(const FileArray* a_reference, const FileArray* a_engine, const char* a_what)
		{
			const auto left = reference.Position(a_reference);
			const auto right = engine.Position(a_engine);
			CHECK(left == right, "%s: reference returned table position %ld, core %ld", a_what, left, right);
			CHECK(a_reference->size == a_engine->size &&
					  (a_reference->size == 0 || std::memcmp(a_reference->data, a_engine->data, a_reference->size * sizeof(void*)) == 0),
				"%s: returned lists differ in content", a_what);
			CHECK(reference.count == engine.count, "%s: table sizes differ: reference %u, core %u", a_what, reference.count, engine.count);
		}

		void Find(std::size_t a_site, std::vector<void*> a_files)
		{
			auto* left = reference.FindOrAdd(a_files.data(), static_cast<std::uint32_t>(a_files.size()));
			auto* right = CoreFind(a_site, a_files.data(), static_cast<std::uint32_t>(a_files.size()));
			Same(left, right, "FindOrAdd");
		}

		void Added(std::size_t a_site, FileArray* a_referenceList, FileArray* a_engineList, void* a_file)
		{
			auto* left = reference.WithFileAdded(a_referenceList, a_file);
			auto* right = core.WithFileAdded(a_site, a_engineList, a_file, [&] { return engine.WithFileAdded(a_engineList, a_file); });
			if (left == a_referenceList || right == a_engineList) {
				CHECK(left == a_referenceList && right == a_engineList, "WithFileAdded: only one side returned its own list");
				CHECK(reference.count == engine.count, "WithFileAdded: table sizes differ");
			} else {
				Same(left, right, "WithFileAdded");
			}
		}

		void Without(FileArray* a_referenceList, FileArray* a_engineList, void* a_file)
		{
			auto* left = reference.WithoutFile(a_referenceList, a_file, [&](void** f, std::uint32_t n) { return reference.FindOrAdd(f, n); });
			auto* right = core.Guarded(kWithoutSite,
				[&] { return engine.WithoutFile(a_engineList, a_file, [&](void** f, std::uint32_t n) { return CoreFind(0, f, n); }); });
			if (left == a_referenceList || right == a_engineList) {
				CHECK(left == a_referenceList && right == a_engineList, "WithoutFile: only one side returned its own list");
			} else {
				Same(left, right, "WithoutFile");
			}
		}

		void Loose(const std::vector<void*>& a_files)
		{
			for (auto* model : { &reference, &engine }) {
				auto* list = model->New(static_cast<std::uint32_t>(a_files.size()));
				for (std::size_t k = 0; k < a_files.size(); ++k) {
					list->data[k] = a_files[k];
				}
				(model == &reference ? looseReference : looseEngine).push_back(list);
			}
		}

		void TablesEqual(const char* a_when)
		{
			CHECK(reference.count == engine.count, "%s: table sizes differ: %u / %u", a_when, reference.count, engine.count);
			for (std::uint32_t i = 0; i < reference.count && i < engine.count; ++i) {
				const auto& left = *reference.data[i];
				const auto& right = *engine.data[i];
				if (!(left.size == right.size && (left.size == 0 || std::memcmp(left.data, right.data, left.size * sizeof(void*)) == 0))) {
					CHECK(false, "%s: table entry %u differs", a_when, i);
					break;
				}
			}
		}

		// A random operation in the shape of the load: mostly "this list plus the file being loaded".
		void Random(std::mt19937_64& a_random, std::uint32_t a_files, std::uint32_t a_maxLength)
		{
			const auto pick = a_random() % 100;
			const auto site = static_cast<std::size_t>(a_random() % kLookupSites);
			if (pick < 30 || reference.count == 0) {
				std::vector<void*> files(a_random() % (a_maxLength + 1));
				for (auto& file : files) {
					file = File(a_random() % a_files);
				}
				Find(site, std::move(files));
			} else if (pick < 80) {
				const auto at = a_random() % reference.count;
				Added(site, reference.data[at], engine.data[at], File(a_random() % a_files));
			} else if (pick < 90) {
				const auto at = a_random() % reference.count;
				const auto& list = *reference.data[at];
				// Half of the time a file that is in the list (often the last one: the inlined path).
				void* file = list.size && (a_random() & 1) ? list.data[(a_random() & 1) ? list.size - 1 : a_random() % list.size] : File(a_random() % a_files);
				Without(reference.data[at], engine.data[at], file);
			} else if (pick < 95 && !looseReference.empty()) {
				const auto at = a_random() % looseReference.size();
				Added(site, looseReference[at], looseEngine[at], File(a_random() % a_files));
			} else {
				std::vector<void*> files(a_random() % (a_maxLength + 1));
				for (auto& file : files) {
					file = File(a_random() % a_files);
				}
				Loose(files);
			}
		}
	};

	std::uint64_t Sum(const TestCore& a_core, std::atomic<std::uint64_t> SiteStats::*a_field)
	{
		std::uint64_t total = 0;
		for (std::size_t i = 0; i < kSites; ++i) {
			total += (a_core.Site(i).*a_field).load();
		}
		return total;
	}

	void RandomRuns()
	{
		struct Setup
		{
			Mode mode;
			std::uint32_t compareFirst;
			std::uint32_t sampleEvery;
			const char* name;
		};
		const Setup setups[] = {
			{ Mode::kCount, 0, 1, "count" },
			{ Mode::kShadow, 0, 1, "shadow" },
			{ Mode::kReplace, 0, 1, "replace, every answer compared" },
			{ Mode::kReplace, 16, 64, "replace, 16 first then 1 in 64" },
			{ Mode::kReplace, 0, 1u << 30, "replace, nothing compared" },
		};
		for (const auto& setup : setups) {
			for (std::uint64_t seed = 1; seed <= 6; ++seed) {
				std::mt19937_64 random(seed * 7919);
				Pair pair(setup.mode, setup.compareFirst, setup.sampleEvery);
				// Few files and short lists give many repeats; more files give many distinct lists.
				const std::uint32_t files = seed % 2 ? 6 : 40;
				const std::uint32_t maxLength = seed % 3 ? 5 : 12;
				const int before = failures;
				for (int i = 0; i < 20000 && failures == before; ++i) {
					pair.Random(random, files, maxLength);
				}
				pair.TablesEqual(setup.name);
				CHECK(reported.empty(), "%s seed %llu: %zu mismatches reported on a healthy table", setup.name, seed, reported.size());
				CHECK(!pair.core.FellBack(), "%s seed %llu: fell back on a healthy table", setup.name, seed);
				const auto snapshot = pair.core.Inspect(true);
				if (setup.mode != Mode::kCount) {
					CHECK(snapshot.audited && snapshot.audit.Clean(), "%s seed %llu: audit not clean (dup %u, wrongFirst %u, missing %u, stale %u)", setup.name, seed,
						snapshot.audit.duplicates, snapshot.audit.wrongFirst, snapshot.audit.missing, snapshot.audit.stale);
					CHECK(snapshot.synced == pair.engine.count && snapshot.distinct == pair.engine.count, "%s seed %llu: index holds %zu of %u entries", setup.name,
						seed, snapshot.distinct, pair.engine.count);
					CHECK(snapshot.foreignEntries > 0, "%s seed %llu: the inlined append of ID 14570 was never exercised", setup.name, seed);
				}
				if (setup.mode == Mode::kReplace && setup.sampleEvery > 1) {
					// The point of the exercise: the engine's search runs only for misses and samples.
					const auto calls = Sum(pair.core, &SiteStats::calls);
					const auto engineCalls = Sum(pair.core, &SiteStats::engineCalls);
					const auto misses = Sum(pair.core, &SiteStats::misses);
					CHECK(engineCalls < calls && engineCalls >= misses, "%s seed %llu: engine calls %llu of %llu, misses %llu", setup.name, seed, engineCalls, calls,
						misses);
					CHECK(pair.reference.visited > pair.engine.visited, "%s seed %llu: the linear search did not get cheaper", setup.name, seed);
				}
				if (seed == 1) {
					std::printf("  %-32s table %u, calls %llu, engine calls %llu, entries visited: reference %llu, behind the core %llu\n", setup.name,
						pair.engine.count, Sum(pair.core, &SiteStats::calls), Sum(pair.core, &SiteStats::engineCalls), pair.reference.visited, pair.engine.visited);
				}
			}
		}
	}

	std::vector<void*> Files(std::initializer_list<int> a_numbers)
	{
		std::vector<void*> files;
		for (const auto number : a_numbers) {
			files.push_back(File(static_cast<std::uint64_t>(number)));
		}
		return files;
	}

	void Seed(Pair& a_pair)
	{
		a_pair.Find(0, Files({ 1 }));
		a_pair.Find(1, Files({ 1, 2 }));
		a_pair.Find(2, Files({ 1, 2, 3 }));
		a_pair.Find(0, Files({ 4 }));
		a_pair.Find(0, Files({}));
		a_pair.Find(0, Files({ 1, 2 }));
	}

	// Entries appended by code that does not pass through the core are indexed before the next lookup.
	void ForeignAppend()
	{
		Pair pair(Mode::kReplace, 0, 1u << 30);
		Seed(pair);
		for (auto* model : { &pair.reference, &pair.engine }) {
			auto* entry = model->New(2);
			entry->data[0] = File(7);
			entry->data[1] = File(8);
			model->Push(entry);
		}
		const auto engineCalls = Sum(pair.core, &SiteStats::engineCalls);
		pair.Find(0, Files({ 7, 8 }));
		CHECK(Sum(pair.core, &SiteStats::engineCalls) == engineCalls, "foreign entry: the engine was called although the entry is in the table");
		CHECK(reported.empty() && !pair.core.FellBack(), "foreign entry: reported as a mismatch");
		CHECK(pair.core.Inspect(true).foreignEntries == 1, "foreign entry: not counted");
	}

	// An entry changed in place: the engine now finds it under its new content, the index does not.
	void ChangedInPlace()
	{
		for (const auto mode : { Mode::kShadow, Mode::kReplace }) {
			Pair pair(mode, 0, 1u << 30);
			Seed(pair);
			pair.reference.data[1]->data[1] = File(9); // [1, 2] becomes [1, 9]
			pair.engine.data[1]->data[1] = File(9);
			const auto audit = pair.core.Inspect(true).audit;
			CHECK(audit.missing == 1 && audit.stale == 1, "changed in place: audit says missing %u, stale %u", audit.missing, audit.stale);

			pair.Find(0, Files({ 1, 2 })); // the old content: nobody has it any more, both sides append
			CHECK(reported.empty(), "changed in place: looking up the old content was reported");
			pair.Find(0, Files({ 1, 9 })); // the new content: the engine finds entry 1, the index cannot
			CHECK(reported.size() == 1 && reported[0].kind == MismatchKind::kMiss, "changed in place: %zu mismatches reported", reported.size());
			CHECK(pair.core.FellBack() == (mode == Mode::kReplace), "changed in place: fallback state wrong for mode %d", static_cast<int>(mode));
			std::mt19937_64 random(5);
			for (int i = 0; i < 3000; ++i) {
				pair.Random(random, 6, 4);
			}
			pair.TablesEqual("changed in place");
		}
	}

	// Two table entries with the same content: the engine returns the first, and so must the core.
	void Duplicate()
	{
		Pair pair(Mode::kReplace, 0, 1u << 30);
		Seed(pair);
		for (auto* model : { &pair.reference, &pair.engine }) {
			auto* entry = model->New(2);
			entry->data[0] = File(1);
			entry->data[1] = File(2);
			model->Push(entry);
		}
		pair.Find(0, Files({ 1, 2 }));
		pair.Added(3, pair.reference.data[0], pair.engine.data[0], File(2));
		const auto snapshot = pair.core.Inspect(true);
		CHECK(snapshot.audit.duplicates == 1 && snapshot.syncDuplicates == 1, "duplicate: audit %u, sync %u", snapshot.audit.duplicates, snapshot.syncDuplicates);
		CHECK(reported.empty(), "duplicate: reported as a mismatch");
	}

	void Shrunk()
	{
		Pair pair(Mode::kReplace, 0, 1u << 30);
		Seed(pair);
		pair.reference.Pop();
		pair.engine.Pop();
		pair.Find(0, Files({ 1 }));
		CHECK(reported.size() == 1 && reported[0].kind == MismatchKind::kTableShrank && pair.core.FellBack(), "shrunk table: not noticed");
		std::mt19937_64 random(11);
		for (int i = 0; i < 3000; ++i) {
			pair.Random(random, 6, 4);
		}
		pair.TablesEqual("shrunk table");
	}

	void SlotReplaced()
	{
		Pair pair(Mode::kReplace, 0, 1u << 30);
		Seed(pair);
		for (auto* model : { &pair.reference, &pair.engine }) {
			auto* entry = model->New(1);
			entry->data[0] = File(30);
			model->data[model->count - 1] = entry;
		}
		pair.Find(0, Files({ 30 }));
		CHECK(reported.size() == 1 && reported[0].kind == MismatchKind::kPrefixChanged && pair.core.FellBack(), "replaced slot: not noticed");
		pair.TablesEqual("replaced slot");
	}

	// The engine's function coming back into a wrapped call on the same thread must not hang.
	void Reentrant()
	{
		Pair pair(Mode::kReplace, 0, 1u << 30);
		Seed(pair);
		auto inner = Files({ 20, 21 });
		auto outer = Files({ 22 });
		pair.reference.FindOrAdd(inner.data(), 2);
		auto* left = pair.reference.FindOrAdd(outer.data(), 1);
		auto* right = pair.core.FindOrAdd(0, outer.data(), 1, [&] {
			pair.CoreFind(1, inner.data(), 2);
			return pair.engine.FindOrAdd(outer.data(), 1);
		});
		pair.Same(left, right, "reentrant");
		CHECK(reported.size() == 1 && pair.core.FellBack(), "reentrant: the outer call should report the table change (%zu reported)", reported.size());
	}

	// Several threads through the core: the lock serialises them, so the (unlocked) model stays whole.
	void Threads()
	{
		Pair pair(Mode::kShadow, 0, 1);
		std::vector<std::thread> threads;
		std::atomic<int> wrong{ 0 };
		for (int t = 0; t < 4; ++t) {
			threads.emplace_back([&, t] {
				std::mt19937_64 random(100 + t);
				for (int i = 0; i < 20000; ++i) {
					std::vector<void*> files(random() % 5);
					for (auto& file : files) {
						file = File(random() % 8);
					}
					auto* entry = pair.CoreFind(static_cast<std::size_t>(t), files.data(), static_cast<std::uint32_t>(files.size()));
					Key key;
					key.head = files.data();
					key.headCount = static_cast<std::uint32_t>(files.size());
					if (!Equal(*entry, key)) {
						++wrong;
					}
				}
			});
		}
		for (auto& thread : threads) {
			thread.join();
		}
		const auto snapshot = pair.core.Inspect(true);
		CHECK(wrong == 0, "threads: %d answers with the wrong content", wrong.load());
		CHECK(reported.empty() && snapshot.audit.Clean(), "threads: %zu mismatches, audit duplicates %u", reported.size(), snapshot.audit.duplicates);
		std::printf("  threads: 4 x 20000 calls, table %u, calls that waited for the lock %llu\n", pair.engine.count, pair.core.Contended());
	}

	// The size of the real thing: about 38,754 distinct lists. Prints what the index costs per call.
	void Scale()
	{
		Model engine;
		TestCore core(&engine.data, &engine.count, Mode::kReplace, 0, 1u << 30, Clock, Report);
		reported.clear();
		std::mt19937_64 random(2026);
		std::vector<std::vector<void*>> keys;
		for (int i = 0; i < 38754; ++i) {
			std::vector<void*> files(1 + random() % 12);
			for (auto& file : files) {
				file = File(random() % 3713);
			}
			keys.push_back(std::move(files));
		}
		const auto start = std::chrono::steady_clock::now();
		std::uint64_t calls = 0;
		for (int round = 0; round < 12; ++round) {
			for (auto& files : keys) {
				auto* entry = core.FindOrAdd(0, files.data(), static_cast<std::uint32_t>(files.size()),
					[&] { return engine.FindOrAdd(files.data(), static_cast<std::uint32_t>(files.size())); });
				++calls;
				Key key;
				key.head = files.data();
				key.headCount = static_cast<std::uint32_t>(files.size());
				if (!Equal(*entry, key)) {
					CHECK(false, "scale: wrong content returned");
					return;
				}
			}
		}
		const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
		const auto snapshot = core.Inspect(true);
		CHECK(snapshot.audit.Clean() && reported.empty() && !core.FellBack(), "scale: audit not clean or mismatches reported");
		const auto misses = Sum(core, &SiteStats::misses);
		const auto engineSeconds = static_cast<double>(Sum(core, &SiteStats::engineTicks)) / 1e9;
		std::printf("  scale: table %u, %llu calls in %.2f s, of which %.2f s inside the model's linear search for %llu misses;\n"
					"         the index itself: %.0f ns per call\n",
			engine.count, calls, seconds, engineSeconds, misses, (seconds - engineSeconds) / static_cast<double>(calls) * 1e9);
	}
}

int main()
{
	std::printf("random runs against the reference model\n");
	RandomRuns();
	std::printf("fault injection\n");
	ForeignAppend();
	ChangedInPlace();
	Duplicate();
	Shrunk();
	SlotReplaced();
	Reentrant();
	Threads();
	Scale();
	std::printf("%d checks, %d failures\n", checks, failures);
	std::printf(failures ? "TEST FAILED\n" : "TEST PASSED\n");
	return failures ? 1 : 0;
}
