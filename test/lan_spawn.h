//====== steamemu ============================================================
// Portable two-child spawner for the LAN integration tests.
//
// Each such test spawns two copies of itself, one per emulator instance, with a
// DISTINCT identity (STEAMEMU_STEAMID) but the same AppID, then waits for both.
// The child logic uses only the Steam API and is already portable; only the
// spawn differs per platform (Windows _spawnl/_cwait, POSIX fork/exec/waitpid),
// which this header hides so the tests build and run on both.
//
// The caller sets SteamAppId (and any other shared env) before calling; each
// child additionally gets STEAMEMU_STEAMID and, if given, STEAMEMU_NAME.
//============================================================================
#pragma once

#include <cstdio>

#if defined(_WIN32)
#include <process.h>
#define LAN_SETENV(k, v) _putenv_s((k), (v))
#else
#include <sys/wait.h>
#include <unistd.h>
#define LAN_SETENV(k, v) setenv((k), (v), 1)
#endif

struct LanChild {
	const char* role;     // passed as argv[1] to the child
	const char* steamId;  // STEAMEMU_STEAMID for the child
	const char* name;     // STEAMEMU_NAME for the child, or nullptr to leave unset
};

// Spawn two children of argv0, set each one's identity env just before spawning
// it (inherited by the child), then wait for both. Returns 0 iff both exit 0.
inline int LanRunTwoChildren(const char* argv0, const LanChild children[2]) {
#if defined(_WIN32)
	intptr_t handles[2];
	for (int i = 0; i < 2; ++i) {
		LAN_SETENV("STEAMEMU_STEAMID", children[i].steamId);
		if (children[i].name) LAN_SETENV("STEAMEMU_NAME", children[i].name);
		handles[i] = _spawnl(_P_NOWAIT, argv0, argv0, children[i].role, (const char*)nullptr);
		if (handles[i] == -1) { perror("_spawnl"); return 1; }
	}
	int rc = 0;
	for (int i = 0; i < 2; ++i) {
		int status = 0;
		int code = (_cwait(&status, handles[i], 0) == -1) ? -1 : status;
		printf("child %s exit=%d\n", children[i].role, code);
		if (code != 0) rc = 1;
	}
	return rc;
#else
	pid_t pids[2];
	for (int i = 0; i < 2; ++i) {
		pid_t pid = fork();
		if (pid == 0) {
			LAN_SETENV("STEAMEMU_STEAMID", children[i].steamId);
			if (children[i].name) LAN_SETENV("STEAMEMU_NAME", children[i].name);
			execl(argv0, argv0, children[i].role, (char*)nullptr);
			perror("execl");
			_exit(127);
		}
		pids[i] = pid;
	}
	int rc = 0;
	for (int i = 0; i < 2; ++i) {
		int status = 0;
		waitpid(pids[i], &status, 0);
		int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
		printf("child %s exit=%d\n", children[i].role, code);
		if (code != 0) rc = 1;
	}
	return rc;
#endif
}
