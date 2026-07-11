//====== steamemu ============================================================
// ISteamRemoteStorage + ISteamUserStats behavioral test.
//
// Two phases in separate processes so we also prove PERSISTENCE across runs:
//   phase 1 (write): write a cloud file + stats/achievement, StoreStats, exit.
//   phase 2 (read) : a fresh process reads them back from disk.
// The parent runs both and checks the round-trip.
//============================================================================
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>

#include "steam/steam_api.h"

static const char* kFile = "save.dat";
static const char* kPayload = "steamemu-cloud-save-v1";

static int phase_write() {
	if (!SteamAPI_Init()) return 2;
	ISteamRemoteStorage* rs = SteamRemoteStorage();
	ISteamUserStats* us = SteamUserStats();
	if (!rs->FileWrite(kFile, kPayload, (int)std::strlen(kPayload) + 1)) return 3;
	us->SetStat("kills", (int32)42);
	us->SetStat("accuracy", 0.75f);
	us->SetAchievement("FIRST_BLOOD");
	if (!us->StoreStats()) return 4;
	SteamAPI_RunCallbacks();
	SteamAPI_Shutdown();
	return 0;
}

static int phase_read() {
	int fails = 0;
	if (!SteamAPI_Init()) return 2;
	ISteamRemoteStorage* rs = SteamRemoteStorage();
	ISteamUserStats* us = SteamUserStats();

	if (!rs->FileExists(kFile)) { printf("FAIL: file not persisted\n"); ++fails; }
	int sz = rs->GetFileSize(kFile);
	char buf[64] = {0};
	int rd = rs->FileRead(kFile, buf, sizeof(buf));
	printf("read %d bytes (size=%d): '%s'\n", rd, sz, buf);
	if (std::strcmp(buf, kPayload) != 0) { printf("FAIL: payload mismatch\n"); ++fails; }
	if (rs->GetFileCount() < 1) { printf("FAIL: file count zero\n"); ++fails; }

	int32 kills = 0; float acc = 0;
	us->GetStat("kills", &kills);
	us->GetStat("accuracy", &acc);
	printf("kills=%d accuracy=%.2f\n", kills, acc);
	if (kills != 42) { printf("FAIL: int stat not persisted\n"); ++fails; }
	if (acc < 0.74f || acc > 0.76f) { printf("FAIL: float stat not persisted\n"); ++fails; }

	bool ach = false;
	us->GetAchievement("FIRST_BLOOD", &ach);
	if (!ach) { printf("FAIL: achievement not persisted\n"); ++fails; }

	// Cleanup so re-runs start clean.
	rs->FileDelete(kFile);
	us->ResetAllStats(true);
	SteamAPI_Shutdown();
	return fails ? 1 : 0;
}

int main(int argc, char** argv) {
	setenv("SteamAppId", "480", 1);
	if (argc > 1 && std::strcmp(argv[1], "write") == 0) return phase_write();
	if (argc > 1 && std::strcmp(argv[1], "read") == 0) return phase_read();

	auto run = [&](const char* phase) {
		pid_t pid = fork();
		if (pid == 0) { execl(argv[0], argv[0], phase, (char*)nullptr); _exit(127); }
		int st = 0; waitpid(pid, &st, 0);
		return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
	};
	int w = run("write");
	int r = run("read");
	printf("write phase=%d read phase=%d\n", w, r);
	if (w == 0 && r == 0) { printf("PASS: storage_test persistence round-trip\n"); return 0; }
	printf("FAILED: storage_test\n");
	return 1;
}
