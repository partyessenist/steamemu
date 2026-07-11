//====== steamemu ============================================================
// ISteamUserStats leaderboards test.
//
// Leaderboards persist locally under <storage>/leaderboards/. Two phases run in
// separate processes with DISTINCT identities but a shared save_path, so we
// prove (a) the async call-result flow (CCallResult), (b) upload/keep-best/
// ranking, and (c) persistence across runs + multiple users:
//   phase A (id 1000001): create "HighScores" (descending), upload score 500.
//   phase B (id 1000002): upload score 900, download the global table, and
//                         verify B ranks 1st (900) and A 2nd (500).
//
// POSIX-only (fork/exec); this host is Linux.
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "steam/steam_api.h"

static const char* kBoard = "HighScores";

static void sleep_ms(int ms) {
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// Minimal CCallResult waiter, exactly as a game maps an async return to a member.
template <typename T>
struct Waiter {
	bool done = false;
	bool ioFailure = false;
	T result{};
	CCallResult<Waiter<T>, T> cr;
	void OnResult(T* r, bool bIOFailure) { result = *r; ioFailure = bIOFailure; done = true; }
	bool Wait(SteamAPICall_t h) {
		if (h == k_uAPICallInvalid) return false;
		cr.Set(h, this, &Waiter<T>::OnResult);
		auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!done && std::chrono::steady_clock::now() < deadline) {
			SteamAPI_RunCallbacks();
			if (!done) sleep_ms(10);
		}
		return done && !ioFailure;
	}
};

static int fail(const char* msg) { printf("FAIL: %s\n", msg); return 1; }

static int phase_a() {
	if (!SteamAPI_Init()) return fail("A: init");
	ISteamUserStats* us = SteamUserStats();

	Waiter<LeaderboardFindResult_t> find;
	if (!find.Wait(us->FindOrCreateLeaderboard(
	        kBoard, k_ELeaderboardSortMethodDescending, k_ELeaderboardDisplayTypeNumeric)))
		return fail("A: FindOrCreateLeaderboard did not complete");
	SteamLeaderboard_t h = find.result.m_hSteamLeaderboard;
	if (h == 0 || !find.result.m_bLeaderboardFound) return fail("A: no leaderboard handle");

	int32 details[1] = {7};
	Waiter<LeaderboardScoreUploaded_t> up;
	if (!up.Wait(us->UploadLeaderboardScore(h, k_ELeaderboardUploadScoreMethodForceUpdate,
	                                        500, details, 1)))
		return fail("A: upload did not complete");
	if (!up.result.m_bSuccess) return fail("A: upload not successful");
	printf("A: uploaded 500, rank=%d\n", up.result.m_nGlobalRankNew);

	SteamAPI_Shutdown();
	return 0;
}

static int phase_b() {
	int fails = 0;
	if (!SteamAPI_Init()) return fail("B: init");
	ISteamUserStats* us = SteamUserStats();

	// FindLeaderboard must locate the board phase A created (persistence).
	Waiter<LeaderboardFindResult_t> find;
	if (!find.Wait(us->FindLeaderboard(kBoard))) return fail("B: FindLeaderboard did not complete");
	if (!find.result.m_bLeaderboardFound) return fail("B: existing leaderboard not found");
	SteamLeaderboard_t h = find.result.m_hSteamLeaderboard;
	printf("B: found board, existing entries=%d\n", us->GetLeaderboardEntryCount(h));
	if (us->GetLeaderboardSortMethod(h) != k_ELeaderboardSortMethodDescending)
		++fails, printf("FAIL: B: sort method not persisted\n");

	int32 details[1] = {3};
	Waiter<LeaderboardScoreUploaded_t> up;
	if (!up.Wait(us->UploadLeaderboardScore(h, k_ELeaderboardUploadScoreMethodForceUpdate,
	                                        900, details, 1)))
		return fail("B: upload did not complete");
	printf("B: uploaded 900, rank=%d (prev=%d)\n",
	       up.result.m_nGlobalRankNew, up.result.m_nGlobalRankPrevious);
	if (up.result.m_nGlobalRankNew != 1) ++fails, printf("FAIL: B: 900 should rank 1st\n");

	// Download the global table; expect A(500) and B(900), B first (descending).
	Waiter<LeaderboardScoresDownloaded_t> dl;
	if (!dl.Wait(us->DownloadLeaderboardEntries(h, k_ELeaderboardDataRequestGlobal, 1, 10)))
		return fail("B: download did not complete");
	int n = dl.result.m_cEntryCount;
	printf("B: downloaded %d entries\n", n);
	if (n != 2) ++fails, printf("FAIL: B: expected 2 entries, got %d\n", n);

	uint64 me = SteamUser()->GetSteamID().ConvertToUint64();
	bool sawMeFirst = false, sawOtherSecond = false;
	for (int i = 0; i < n; ++i) {
		LeaderboardEntry_t e{};
		int32 det[8] = {0};
		if (!us->GetDownloadedLeaderboardEntry(dl.result.m_hSteamLeaderboardEntries, i, &e, det, 8)) {
			++fails, printf("FAIL: B: GetDownloadedLeaderboardEntry(%d)\n", i);
			continue;
		}
		printf("  rank=%d score=%d user=%llu details0=%d ndet=%d\n",
		       e.m_nGlobalRank, e.m_nScore, (unsigned long long)e.m_steamIDUser.ConvertToUint64(),
		       det[0], e.m_cDetails);
		if (e.m_nGlobalRank == 1) {
			if (e.m_nScore == 900 && e.m_steamIDUser.ConvertToUint64() == me && det[0] == 3)
				sawMeFirst = true;
		} else if (e.m_nGlobalRank == 2) {
			if (e.m_nScore == 500 && det[0] == 7) sawOtherSecond = true;
		}
	}
	if (!sawMeFirst) ++fails, printf("FAIL: B: self not ranked 1st with score 900/details\n");
	if (!sawOtherSecond) ++fails, printf("FAIL: B: peer not ranked 2nd with score 500\n");

	SteamAPI_Shutdown();
	return fails ? 1 : 0;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "a") == 0) return phase_a();
	if (argc > 1 && std::strcmp(argv[1], "b") == 0) return phase_b();

	// Parent: isolate storage in a temp dir via a config file's save_path so the
	// test is hermetic and both phases share one leaderboards store.
	char tmpl[] = "/tmp/steamemu_lb_XXXXXX";
	char* dir = mkdtemp(tmpl);
	if (!dir) { perror("mkdtemp"); printf("FAILED: leaderboard_test\n"); return 1; }
	std::string ini = std::string(dir) + "/steamemu.ini";
	FILE* f = std::fopen(ini.c_str(), "w");
	std::fprintf(f, "save_path = %s/store\n", dir);
	std::fclose(f);

	setenv("SteamAppId", "480", 1);
	setenv("STEAMEMU_CONFIG", ini.c_str(), 1);

	auto run = [&](const char* phase, const char* id) {
		pid_t pid = fork();
		if (pid == 0) {
			setenv("STEAMEMU_STEAMID", id, 1);
			execl(argv[0], argv[0], phase, (char*)nullptr);
			perror("execl");
			_exit(127);
		}
		int st = 0; waitpid(pid, &st, 0);
		return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	};

	int a = run("a", "1000001");
	int b = run("b", "1000002");
	printf("phase A=%d phase B=%d\n", a, b);
	if (a == 0 && b == 0) { printf("PASS: leaderboard_test upload/rank/persist worked\n"); return 0; }
	printf("FAILED: leaderboard_test\n");
	return 1;
}
