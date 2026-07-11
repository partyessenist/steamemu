//====== steamemu ============================================================
// Local persistence backing ISteamRemoteStorage (cloud files) and
// ISteamUserStats (stats + achievements).
//
// Everything lives under  <cwd>/steamemu_storage/<appid>/  so a game's cloud
// saves, stats and achievements survive across runs with no Valve backend.
// Files map 1:1 to real files in a  remote/  subdir; stats/achievements persist
// to a small text file next to them.
//============================================================================
#pragma once

#include "steam/steamclientpublic.h"

#include <cstdint>
#include <string>
#include <vector>

namespace emu {

// -------- ISteamRemoteStorage: a flat per-app file store --------------------
class StorageBackend {
public:
	static StorageBackend& Get();

	bool Write(const std::string& file, const void* data, int32_t cb);
	int32_t Read(const std::string& file, void* data, int32_t cbToRead);
	bool Exists(const std::string& file);
	bool Delete(const std::string& file);
	int32_t GetSize(const std::string& file);
	int64_t GetTimestamp(const std::string& file);

	int32_t FileCount();
	// Stable const char* to the i-th file name (borrowed, valid for process life).
	const char* FileNameByIndex(int i, int32_t* sizeOut);

	// FileReadAsync support: read [offset, offset+cbToRead) NOW and stash the
	// bytes keyed by the call handle; the game fetches them from its completion
	// handler via FileReadAsyncComplete. Returns false (nothing stashed) if the
	// file is missing or the offset is past EOF; *cbReadOut is the amount stashed
	// (clamped to EOF).
	bool ReadAsyncStart(uint64_t hCall, const std::string& file, uint32_t offset,
	                    uint32_t cbToRead, uint32_t* cbReadOut);
	// Copy up to cbToRead stashed bytes for hCall into dst and drop the stash.
	bool ReadAsyncComplete(uint64_t hCall, void* dst, uint32_t cbToRead);

	// Write-stream handles accumulate chunks then commit on Close.
	uint64_t StreamOpen(const std::string& file);
	bool StreamWrite(uint64_t handle, const void* data, int32_t cb);
	bool StreamClose(uint64_t handle);   // flushes to Write()
	bool StreamCancel(uint64_t handle);

private:
	StorageBackend() = default;
};

// -------- ISteamUserStats: typed stats + achievements -----------------------
class StatsBackend {
public:
	static StatsBackend& Get();

	bool GetInt(const std::string& name, int32_t* out);
	bool GetFloat(const std::string& name, float* out);
	bool SetInt(const std::string& name, int32_t v);
	bool SetFloat(const std::string& name, float v);

	bool GetAchievement(const std::string& name, bool* achieved, uint32_t* unlockTime = nullptr);
	bool SetAchievement(const std::string& name);
	bool ClearAchievement(const std::string& name);
	uint32_t NumAchievements();
	const char* AchievementNameByIndex(uint32_t i);  // borrowed

	bool Store();               // persist to disk; caller queues UserStatsStored_t
	void ResetAll(bool achievementsToo);

private:
	StatsBackend() = default;
};

// -------- ISteamUserStats: leaderboards -------------------------------------
// Local, persisted leaderboards. There is no Valve backend, so a board holds
// whatever scores this machine has uploaded (one entry per user), persisted
// under  <storage>/leaderboards/<name>.txt  and ranked on read. Handles are
// opaque uint64s stable for the process; downloads snapshot a ranked slice into
// a separate entries-handle that GetEntry() reads.
class LeaderboardBackend {
public:
	static LeaderboardBackend& Get();

	// Find (and optionally create) a board by name; returns its handle (0 if not
	// found and not created). *found reports whether a usable handle came back.
	uint64_t FindOrCreate(const std::string& name, int sort, int display,
	                      bool createIfMissing, bool* found);
	const char* NameOf(uint64_t board);   // borrowed, "" if unknown
	int EntryCount(uint64_t board);
	int SortMethod(uint64_t board);
	int DisplayType(uint64_t board);

	// Upload the local user's score (KeepBest/ForceUpdate). Reports whether the
	// stored score changed and the new/previous global ranks.
	bool Upload(uint64_t board, int method, int32_t score,
	            const int32_t* details, int cDetails,
	            bool* scoreChanged, int* newRank, int* prevRank);

	// Snapshot a ranked slice for a data request/range; returns an entries-handle
	// and the number of rows (via *countOut).
	uint64_t Download(uint64_t board, int request, int rangeStart, int rangeEnd, int* countOut);
	// Read the index-th row of a download snapshot into raw out-params (kept free
	// of isteamuserstats.h; the interface BODY assembles LeaderboardEntry_t).
	bool GetEntry(uint64_t entriesHandle, int index, uint64_t* steamID,
	              int32_t* rank, int32_t* score, uint64_t* ugc,
	              int32_t* detailsOut, int detailsMax, int32_t* detailsCountOut);

private:
	LeaderboardBackend() = default;
};

// Free accessors mirroring emu::Net(): the interface BODIES call emu::Storage()
// and emu::Stats().
inline StorageBackend& Storage() { return StorageBackend::Get(); }
inline StatsBackend& Stats() { return StatsBackend::Get(); }
inline LeaderboardBackend& Leaderboards() { return LeaderboardBackend::Get(); }

// Directory (created on demand) that holds this app's storage. Public so other
// modules could share it if needed.
std::string StorageRoot();

// Per-user data dir under the storage root (created on demand); backs
// ISteamUser::GetUserDataFolder. Borrowed, stable for the process.
const char* UserDataDirC();

} // namespace emu
