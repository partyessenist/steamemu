//====== steamemu ============================================================
// Local persistence for ISteamRemoteStorage + ISteamUserStats. See storage.h.
//============================================================================
#include "storage.h"
#include "emu_common.h"
#include "config.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(_WIN32)
#include <direct.h>
#include <windows.h>
#define EMU_MKDIR(p) _mkdir(p)
#else
#include <dirent.h>
#include <unistd.h>
#define EMU_MKDIR(p) ::mkdir(p, 0755)
#endif

namespace emu {
namespace {

void MakeDirs(const std::string& path) {
	// Create each path component in turn (mkdir -p).
	std::string cur;
	for (size_t i = 0; i < path.size(); ++i) {
		cur += path[i];
		if (path[i] == '/' || i + 1 == path.size()) {
			if (!cur.empty() && cur != "/") EMU_MKDIR(cur.c_str());
		}
	}
}

std::string RemoteDir() {
	std::string d = StorageRoot() + "/remote";
	MakeDirs(d);
	return d;
}

// Normalize a game-supplied Steam cloud file name into a SAFE RELATIVE path that
// PRESERVES its subdirectories. Steam names routinely contain '/',
// and the game re-discovers saves by ENUMERATING cloud
// storage and matching that exact path -- so flattening '/' to '_' broke read-back
// (the enumerated name no longer matched what the game looked up). We keep the
// directory structure but refuse to escape the storage dir: '\' is treated as a
// separator, '.'/'' components are dropped, and '..' is never allowed to traverse up.
std::string NormalizeRelPath(const std::string& file) {
	std::string out, comp;
	auto flush = [&]() {
		if (comp.empty() || comp == "." || comp == "..") { comp.clear(); return; }
		for (char& c : comp) if (c == ':') c = '_';  // defang drive-letter components
		if (!out.empty()) out += '/';
		out += comp;
		comp.clear();
	};
	for (char c : file) {
		if (c == '/' || c == '\\') flush();
		else comp += c;
	}
	flush();
	if (out.empty()) out = "_";
	return out;
}

std::string RemotePath(const std::string& file) { return RemoteDir() + "/" + NormalizeRelPath(file); }

// Flatten a name into a single safe path component (no subdirs). Used for leaderboard
// files, where the "name" is an identifier that should map to one file, not a tree.
std::string SafeName(const std::string& file) {
	std::string s = file;
	for (char& c : s) if (c == '/' || c == '\\' || c == ':') c = '_';
	if (s.empty() || s == "." || s == "..") s = "_";
	return s;
}

// Create the parent directory chain for a full file path (up to the last '/').
void MakeParentDirs(const std::string& fullPath) {
	std::string::size_type slash = fullPath.find_last_of('/');
	if (slash != std::string::npos) MakeDirs(fullPath.substr(0, slash));
}

} // namespace

std::string StorageRoot() {
	static std::string root = [] {
		// An explicit save_path/storage_dir in the config wins; otherwise fall
		// back to the per-app default under the cwd.
		std::string base = Config().SavePath();
		if (base.empty()) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), "steamemu_storage/%u", AppID());
			base = buf;
		}
		MakeDirs(base);
		return base;
	}();
	return root;
}

const char* UserDataDirC() {
	static std::string dir = [] {
		std::string d = StorageRoot() + "/user";
		MakeDirs(d);
		return d;
	}();
	return dir.c_str();
}

// ============================ Storage (files) ==============================
StorageBackend& StorageBackend::Get() { static StorageBackend s; return s; }

bool StorageBackend::Write(const std::string& file, const void* data, int32_t cb) {
	if (cb < 0) return false;
	std::string path = RemotePath(file);
	MakeParentDirs(path);  // Steam names may nest (out/save/x.sav); create the subdirs.
	FILE* f = std::fopen(path.c_str(), "wb");
	if (!f) return false;
	bool ok = cb == 0 || std::fwrite(data, 1, cb, f) == (size_t)cb;
	std::fclose(f);
	return ok;
}

int32_t StorageBackend::Read(const std::string& file, void* data, int32_t cbToRead) {
	if (cbToRead <= 0) return 0;
	FILE* f = std::fopen(RemotePath(file).c_str(), "rb");
	if (!f) return 0;
	size_t n = std::fread(data, 1, cbToRead, f);
	std::fclose(f);
	return (int32_t)n;
}

namespace {
std::mutex g_asyncReadMtx;
std::map<uint64_t, std::vector<uint8_t>> g_asyncReads;  // call handle -> stashed bytes
} // namespace

bool StorageBackend::ReadAsyncStart(uint64_t hCall, const std::string& file, uint32_t offset,
                                    uint32_t cbToRead, uint32_t* cbReadOut) {
	if (cbReadOut) *cbReadOut = 0;
	FILE* f = std::fopen(RemotePath(file).c_str(), "rb");
	if (!f) return false;
	std::fseek(f, 0, SEEK_END);
	long size = std::ftell(f);
	if (size < 0 || offset > (uint64_t)size) { std::fclose(f); return false; }
	uint32_t n = (uint32_t)size - offset;
	if (n > cbToRead) n = cbToRead;
	std::vector<uint8_t> buf(n);
	std::fseek(f, (long)offset, SEEK_SET);
	size_t got = n ? std::fread(buf.data(), 1, n, f) : 0;
	std::fclose(f);
	buf.resize(got);
	if (cbReadOut) *cbReadOut = (uint32_t)got;
	std::lock_guard<std::mutex> lk(g_asyncReadMtx);
	g_asyncReads[hCall] = std::move(buf);
	return true;
}

bool StorageBackend::ReadAsyncComplete(uint64_t hCall, void* dst, uint32_t cbToRead) {
	std::lock_guard<std::mutex> lk(g_asyncReadMtx);
	auto it = g_asyncReads.find(hCall);
	if (it == g_asyncReads.end()) return false;
	uint32_t n = (uint32_t)it->second.size();
	if (n > cbToRead) n = cbToRead;
	if (dst && n) std::memcpy(dst, it->second.data(), n);
	g_asyncReads.erase(it);
	return true;
}

bool StorageBackend::Exists(const std::string& file) {
	struct stat st;
	return ::stat(RemotePath(file).c_str(), &st) == 0;
}

bool StorageBackend::Delete(const std::string& file) {
	return std::remove(RemotePath(file).c_str()) == 0;
}

int32_t StorageBackend::GetSize(const std::string& file) {
	struct stat st;
	if (::stat(RemotePath(file).c_str(), &st) != 0) return 0;
	return (int32_t)st.st_size;
}

int64_t StorageBackend::GetTimestamp(const std::string& file) {
	struct stat st;
	if (::stat(RemotePath(file).c_str(), &st) != 0) return 0;
	return (int64_t)st.st_mtime;
}

namespace {
std::mutex g_listMtx;
std::vector<std::string> g_fileList;  // stable storage for FileNameByIndex

// Recursively collect files under `base`, returning paths RELATIVE to base with '/'
// separators -- i.e. exactly the names the game wrote and re-looks-up. Implemented for
// BOTH platforms: the old version was guarded #if !defined(_WIN32), so on Windows the
// list was always empty and GetFileCount()==0, which is why games never saw their
// cloud saves and could not offer a "Continue".
void CollectFiles(const std::string& base, const std::string& rel,
                  std::vector<std::string>& out) {
	std::string dir = rel.empty() ? base : base + "/" + rel;
#if defined(_WIN32)
	WIN32_FIND_DATAA fd;
	HANDLE h = ::FindFirstFileA((dir + "/*").c_str(), &fd);
	if (h == INVALID_HANDLE_VALUE) return;
	do {
		if (fd.cFileName[0] == '.') continue;
		std::string child = rel.empty() ? fd.cFileName : rel + "/" + fd.cFileName;
		if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
			CollectFiles(base, child, out);
		else
			out.push_back(child);
	} while (::FindNextFileA(h, &fd));
	::FindClose(h);
#else
	if (DIR* d = ::opendir(dir.c_str())) {
		while (dirent* e = ::readdir(d)) {
			if (e->d_name[0] == '.') continue;
			std::string child = rel.empty() ? e->d_name : rel + "/" + e->d_name;
			struct stat st;
			if (::stat((dir + "/" + e->d_name).c_str(), &st) == 0 && S_ISDIR(st.st_mode))
				CollectFiles(base, child, out);
			else
				out.push_back(child);
		}
		::closedir(d);
	}
#endif
}

void RefreshFileList() {
	g_fileList.clear();
	CollectFiles(RemoteDir(), "", g_fileList);
}
} // namespace

int32_t StorageBackend::FileCount() {
	std::lock_guard<std::mutex> lk(g_listMtx);
	RefreshFileList();
	return (int32_t)g_fileList.size();
}

const char* StorageBackend::FileNameByIndex(int i, int32_t* sizeOut) {
	std::lock_guard<std::mutex> lk(g_listMtx);
	if (g_fileList.empty()) RefreshFileList();
	if (i < 0 || i >= (int)g_fileList.size()) { if (sizeOut) *sizeOut = 0; return ""; }
	const std::string& name = g_fileList[i];
	if (sizeOut) *sizeOut = GetSize(name);
	return name.c_str();  // stable: g_fileList entries outlive the call
}

// Write streams: accumulate then flush on close.
namespace {
std::mutex g_streamMtx;
std::map<uint64_t, std::pair<std::string, std::vector<uint8_t>>> g_streams;
uint64_t g_nextStream = 1;
}

uint64_t StorageBackend::StreamOpen(const std::string& file) {
	std::lock_guard<std::mutex> lk(g_streamMtx);
	uint64_t h = g_nextStream++;
	g_streams[h] = {file, {}};
	return h;
}

bool StorageBackend::StreamWrite(uint64_t handle, const void* data, int32_t cb) {
	if (cb < 0) return false;
	std::lock_guard<std::mutex> lk(g_streamMtx);
	auto it = g_streams.find(handle);
	if (it == g_streams.end()) return false;
	const uint8_t* p = (const uint8_t*)data;
	it->second.second.insert(it->second.second.end(), p, p + cb);
	return true;
}

bool StorageBackend::StreamClose(uint64_t handle) {
	std::string file;
	std::vector<uint8_t> buf;
	{
		std::lock_guard<std::mutex> lk(g_streamMtx);
		auto it = g_streams.find(handle);
		if (it == g_streams.end()) return false;
		file = it->second.first;
		buf = std::move(it->second.second);
		g_streams.erase(it);
	}
	return Write(file, buf.data(), (int32_t)buf.size());
}

bool StorageBackend::StreamCancel(uint64_t handle) {
	std::lock_guard<std::mutex> lk(g_streamMtx);
	return g_streams.erase(handle) != 0;
}

// ============================ Stats + achievements =========================
namespace {
struct StatStore {
	std::mutex mtx;
	std::map<std::string, int32_t> ints;
	std::map<std::string, float> floats;
	std::map<std::string, uint32_t> achievements;  // name -> unlock time (0 = locked/absent)
	std::vector<std::string> achOrder;             // stable order for enumeration
	bool loaded = false;

	std::string Path() { return StorageRoot() + "/stats.txt"; }

	void Load() {
		if (loaded) return;
		loaded = true;
		FILE* f = std::fopen(Path().c_str(), "r");
		if (!f) return;
		char line[512];
		while (std::fgets(line, sizeof(line), f)) {
			char kind[8], name[256]; double val = 0; unsigned t = 0;
			// Formats:  i <name> <intval> | f <name> <floatval> | a <name> <unlocktime>
			if (std::sscanf(line, "%7s %255s %lf", kind, name, &val) >= 2) {
				if (kind[0] == 'i') ints[name] = (int32_t)val;
				else if (kind[0] == 'f') floats[name] = (float)val;
				else if (kind[0] == 'a') {
					std::sscanf(line, "%7s %255s %u", kind, name, &t);
					if (!achievements.count(name)) achOrder.push_back(name);
					achievements[name] = t;
				}
			}
		}
		std::fclose(f);
	}

	void Save() {
		FILE* f = std::fopen(Path().c_str(), "w");
		if (!f) return;
		for (auto& kv : ints) std::fprintf(f, "i %s %d\n", kv.first.c_str(), kv.second);
		for (auto& kv : floats) std::fprintf(f, "f %s %g\n", kv.first.c_str(), (double)kv.second);
		for (auto& kv : achievements) std::fprintf(f, "a %s %u\n", kv.first.c_str(), kv.second);
		std::fclose(f);
	}

	void Track(const std::string& name) {
		if (!achievements.count(name)) achOrder.push_back(name);
	}
};
StatStore& S() { static StatStore s; return s; }
}

StatsBackend& StatsBackend::Get() { static StatsBackend s; return s; }

bool StatsBackend::GetInt(const std::string& name, int32_t* out) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	auto it = s.ints.find(name);
	if (out) *out = it == s.ints.end() ? 0 : it->second;
	return it != s.ints.end();
}
bool StatsBackend::GetFloat(const std::string& name, float* out) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	auto it = s.floats.find(name);
	if (out) *out = it == s.floats.end() ? 0.f : it->second;
	return it != s.floats.end();
}
bool StatsBackend::SetInt(const std::string& name, int32_t v) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load(); s.ints[name] = v; return true;
}
bool StatsBackend::SetFloat(const std::string& name, float v) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load(); s.floats[name] = v; return true;
}

bool StatsBackend::GetAchievement(const std::string& name, bool* achieved, uint32_t* unlockTime) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	auto it = s.achievements.find(name);
	bool have = it != s.achievements.end() && it->second != 0;
	if (achieved) *achieved = have;
	if (unlockTime) *unlockTime = have ? it->second : 0;
	return true;  // querying a (possibly unknown) achievement always "succeeds"
}
bool StatsBackend::SetAchievement(const std::string& name) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	s.Track(name);
	if (s.achievements[name] == 0) s.achievements[name] = (uint32_t)std::time(nullptr);
	return true;
}
bool StatsBackend::ClearAchievement(const std::string& name) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	s.Track(name); s.achievements[name] = 0; return true;
}
uint32_t StatsBackend::NumAchievements() {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	return (uint32_t)s.achOrder.size();
}
const char* StatsBackend::AchievementNameByIndex(uint32_t i) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	return i < s.achOrder.size() ? s.achOrder[i].c_str() : "";
}
bool StatsBackend::Store() {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load(); s.Save(); return true;
}
void StatsBackend::ResetAll(bool achievementsToo) {
	auto& s = S(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	s.ints.clear(); s.floats.clear();
	if (achievementsToo) { s.achievements.clear(); s.achOrder.clear(); }
	s.Save();
}

// ============================ Leaderboards =================================
namespace {

struct LbEntry {
	uint64_t steamID = 0;
	int32_t score = 0;
	std::vector<int32_t> details;
};
struct Leaderboard {
	std::string name;
	int sort = 2;      // ELeaderboardSortMethod (2 = descending)
	int display = 1;   // ELeaderboardDisplayType (1 = numeric)
	std::vector<LbEntry> entries;   // one per user
};
struct SnapRow {
	uint64_t steamID; int32_t rank; int32_t score; std::vector<int32_t> details;
};

struct LbStore {
	std::mutex mtx;
	bool loaded = false;
	std::map<std::string, Leaderboard> boards;   // name -> board
	std::map<std::string, uint64_t> nameToHandle;
	std::map<uint64_t, std::string> handleToName;
	uint64_t nextHandle = 1;
	std::map<uint64_t, std::vector<SnapRow>> snapshots;
	uint64_t nextSnapshot = 1;
	std::string nameCache;   // backs NameOf()'s borrowed const char*

	std::string Dir() {
		std::string d = StorageRoot() + "/leaderboards";
		MakeDirs(d);
		return d;
	}
	std::string PathFor(const std::string& name) { return Dir() + "/" + SafeName(name) + ".txt"; }

	void LoadOne(const std::string& fileName) {
		// fileName is "<safename>.txt" -- we read the board's real name from meta.
		FILE* f = std::fopen((Dir() + "/" + fileName).c_str(), "r");
		if (!f) return;
		Leaderboard lb;
		char line[1024];
		while (std::fgets(line, sizeof(line), f)) {
			if (std::strncmp(line, "name ", 5) == 0) {
				std::string n = line + 5;
				while (!n.empty() && (n.back() == '\n' || n.back() == '\r')) n.pop_back();
				lb.name = n;
			} else if (std::strncmp(line, "meta ", 5) == 0) {
				std::sscanf(line, "meta %d %d", &lb.sort, &lb.display);
			} else if (std::strncmp(line, "entry ", 6) == 0) {
				LbEntry e;
				int nd = 0, off = 0;
				unsigned long long sid = 0;
				if (std::sscanf(line, "entry %llu %d %d%n", &sid, &e.score, &nd, &off) >= 3) {
					e.steamID = sid;
					const char* p = line + off;
					for (int i = 0; i < nd; ++i) {
						int v = 0, adv = 0;
						if (std::sscanf(p, " %d%n", &v, &adv) != 1) break;
						e.details.push_back(v); p += adv;
					}
					lb.entries.push_back(std::move(e));
				}
			}
		}
		std::fclose(f);
		if (!lb.name.empty()) boards[lb.name] = std::move(lb);
	}

	void Load() {
		if (loaded) return;
		loaded = true;
#if !defined(_WIN32)
		if (DIR* d = ::opendir(Dir().c_str())) {
			while (dirent* e = ::readdir(d)) {
				if (e->d_name[0] == '.') continue;
				LoadOne(e->d_name);
			}
			::closedir(d);
		}
#endif
	}

	void Save(const Leaderboard& lb) {
		FILE* f = std::fopen(PathFor(lb.name).c_str(), "w");
		if (!f) return;
		std::fprintf(f, "name %s\n", lb.name.c_str());
		std::fprintf(f, "meta %d %d\n", lb.sort, lb.display);
		for (const auto& e : lb.entries) {
			std::fprintf(f, "entry %llu %d %d", (unsigned long long)e.steamID,
			             e.score, (int)e.details.size());
			for (int32_t d : e.details) std::fprintf(f, " %d", d);
			std::fprintf(f, "\n");
		}
		std::fclose(f);
	}

	uint64_t HandleFor(const std::string& name) {
		auto it = nameToHandle.find(name);
		if (it != nameToHandle.end()) return it->second;
		uint64_t h = nextHandle++;
		nameToHandle[name] = h;
		handleToName[h] = name;
		return h;
	}
	Leaderboard* BoardOf(uint64_t h) {
		auto it = handleToName.find(h);
		if (it == handleToName.end()) return nullptr;
		auto b = boards.find(it->second);
		return b == boards.end() ? nullptr : &b->second;
	}
};
LbStore& LB() { static LbStore s; return s; }

// Sorted copy of a board's entries (best first) with ranks assigned.
std::vector<SnapRow> RankEntries(const Leaderboard& lb) {
	std::vector<LbEntry> es = lb.entries;
	bool ascending = lb.sort == 1;   // k_ELeaderboardSortMethodAscending
	std::stable_sort(es.begin(), es.end(), [&](const LbEntry& a, const LbEntry& b) {
		return ascending ? a.score < b.score : a.score > b.score;
	});
	std::vector<SnapRow> rows;
	for (size_t i = 0; i < es.size(); ++i)
		rows.push_back({es[i].steamID, (int32_t)i + 1, es[i].score, es[i].details});
	return rows;
}

} // namespace

LeaderboardBackend& LeaderboardBackend::Get() { static LeaderboardBackend s; return s; }

uint64_t LeaderboardBackend::FindOrCreate(const std::string& name, int sort, int display,
                                          bool createIfMissing, bool* found) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	auto it = s.boards.find(name);
	if (it == s.boards.end()) {
		if (!createIfMissing) { if (found) *found = false; return 0; }
		Leaderboard lb; lb.name = name; lb.sort = sort; lb.display = display;
		s.Save(lb);
		s.boards[name] = std::move(lb);
	}
	if (found) *found = true;
	return s.HandleFor(name);
}

const char* LeaderboardBackend::NameOf(uint64_t board) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx);
	auto it = s.handleToName.find(board);
	if (it == s.handleToName.end()) return "";
	s.nameCache = it->second;   // stable across the call
	return s.nameCache.c_str();
}

int LeaderboardBackend::EntryCount(uint64_t board) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx);
	Leaderboard* lb = s.BoardOf(board);
	return lb ? (int)lb->entries.size() : 0;
}
int LeaderboardBackend::SortMethod(uint64_t board) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx);
	Leaderboard* lb = s.BoardOf(board);
	return lb ? lb->sort : 0;
}
int LeaderboardBackend::DisplayType(uint64_t board) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx);
	Leaderboard* lb = s.BoardOf(board);
	return lb ? lb->display : 0;
}

bool LeaderboardBackend::Upload(uint64_t board, int method, int32_t score,
                                const int32_t* details, int cDetails,
                                bool* scoreChanged, int* newRank, int* prevRank) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	Leaderboard* lb = s.BoardOf(board);
	if (!lb) { if (scoreChanged) *scoreChanged = false; if (newRank) *newRank = 0; if (prevRank) *prevRank = 0; return false; }

	uint64_t me = LocalSteamID().ConvertToUint64();
	// Previous rank for the local user (0 if no prior entry).
	int prev = 0;
	{
		auto ranked = RankEntries(*lb);
		for (auto& r : ranked) if (r.steamID == me) { prev = r.rank; break; }
	}

	LbEntry* mine = nullptr;
	for (auto& e : lb->entries) if (e.steamID == me) { mine = &e; break; }
	bool changed = false;
	bool keepBest = method == 1;   // k_ELeaderboardUploadScoreMethodKeepBest
	bool ascending = lb->sort == 1;
	if (!mine) {
		lb->entries.push_back({me, score, {}});
		mine = &lb->entries.back();
		changed = true;
	} else if (!keepBest) {
		changed = mine->score != score; mine->score = score;
	} else {
		bool better = ascending ? score < mine->score : score > mine->score;
		if (better) { mine->score = score; changed = true; }
	}
	// Store the details alongside the score only when we actually (re)placed it,
	// so a kept better score keeps its own details.
	if (changed) {
		int nd = (details && cDetails > 0) ? cDetails : 0;
		mine->details.assign(details, details + nd);
	}
	s.Save(*lb);

	int now = 0;
	auto ranked = RankEntries(*lb);
	for (auto& r : ranked) if (r.steamID == me) { now = r.rank; break; }
	if (scoreChanged) *scoreChanged = changed;
	if (newRank) *newRank = now;
	if (prevRank) *prevRank = prev;
	return true;
}

uint64_t LeaderboardBackend::Download(uint64_t board, int request, int rangeStart,
                                      int rangeEnd, int* countOut) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx); s.Load();
	Leaderboard* lb = s.BoardOf(board);
	std::vector<SnapRow> all = lb ? RankEntries(*lb) : std::vector<SnapRow>();
	std::vector<SnapRow> slice;

	if (request == 1) {   // k_ELeaderboardDataRequestGlobalAroundUser
		uint64_t me = LocalSteamID().ConvertToUint64();
		int center = 1;
		for (auto& r : all) if (r.steamID == me) { center = r.rank; break; }
		int lo = center + rangeStart, hi = center + rangeEnd;
		for (auto& r : all) if (r.rank >= lo && r.rank <= hi) slice.push_back(r);
	} else if (request == 0) {   // k_ELeaderboardDataRequestGlobal: ranks [start,end] over [1,N]
		int lo = rangeStart < 1 ? 1 : rangeStart;
		int hi = rangeEnd <= 0 ? (int)all.size() : rangeEnd;
		for (auto& r : all) if (r.rank >= lo && r.rank <= hi) slice.push_back(r);
	} else {   // Friends / Users: we only hold local rows -- return them all
		slice = all;
	}

	uint64_t eh = s.nextSnapshot++;
	s.snapshots[eh] = slice;
	if (countOut) *countOut = (int)slice.size();
	return eh;
}

bool LeaderboardBackend::GetEntry(uint64_t entriesHandle, int index, uint64_t* steamID,
                                  int32_t* rank, int32_t* score, uint64_t* ugc,
                                  int32_t* detailsOut, int detailsMax, int32_t* detailsCountOut) {
	auto& s = LB(); std::lock_guard<std::mutex> lk(s.mtx);
	auto it = s.snapshots.find(entriesHandle);
	if (it == s.snapshots.end() || index < 0 || index >= (int)it->second.size())
		return false;
	const SnapRow& r = it->second[index];
	if (steamID) *steamID = r.steamID;
	if (rank) *rank = r.rank;
	if (score) *score = r.score;
	if (ugc) *ugc = 0xffffffffffffffffull;   // k_UGCHandleInvalid
	int nd = (int)r.details.size();
	int copy = nd < detailsMax ? nd : detailsMax;
	if (detailsOut && copy > 0)
		std::memcpy(detailsOut, r.details.data(), (size_t)copy * sizeof(int32_t));
	if (detailsCountOut) *detailsCountOut = nd;
	return true;
}

} // namespace emu
