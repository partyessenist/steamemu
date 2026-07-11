//====== steamemu ============================================================
// steamemu.ini configuration test.
//
// The config subsystem is a per-process singleton (loaded once), so like
// lan_test we run the checks in a forked child: the parent writes a temp
// steamemu.ini, points STEAMEMU_CONFIG at it, and clears the env overrides so
// the FILE values are what's exercised. The child then asserts that every key
// flows through to the Steam interfaces:
//   - steam_id     -> ISteamUser::GetSteamID
//   - persona_name -> ISteamFriends::GetPersonaName
//   - language     -> ISteamApps::GetCurrentGameLanguage (+ UI language default)
//   - country      -> ISteamUtils::GetIPCountry
//   - [dlc]        -> BIsDlcInstalled(known/unknown) + GetDLCCount + names
//   - save_path    -> ISteamRemoteStorage writes land under it on disk
//
// POSIX-only (fork/exec); this host is Linux.
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "steam/steam_api.h"

// Shared expectations (the parent writes these into the ini; the child rebuilds
// the expected values from the same constants).
static const uint32 kAccountId = 1234567;
static const char*  kName      = "ConfigTester";
static const char*  kLang      = "french";
static const char*  kCountry   = "DE";
static const uint32 kAppId     = 480;
static const uint32 kDlcA      = 555;
static const uint32 kDlcB      = 556;
static const uint32 kDlcUnknown = 999;
static const char*  kCloudFile = "cfgsave.dat";

static int check(bool ok, const char* what) {
	if (!ok) printf("FAIL: %s\n", what);
	return ok ? 0 : 1;
}

static int run_child() {
	if (!SteamAPI_Init()) { printf("FAIL: init\n"); return 2; }
	int fails = 0;

	// steam_id -> GetSteamID
	CSteamID expected(kAccountId, k_EUniversePublic, k_EAccountTypeIndividual);
	uint64 got = SteamUser()->GetSteamID().ConvertToUint64();
	printf("steamID got=%llu want=%llu\n",
	       (unsigned long long)got, (unsigned long long)expected.ConvertToUint64());
	fails += check(got == expected.ConvertToUint64(), "steam_id -> GetSteamID");

	// persona_name -> GetPersonaName
	fails += check(std::strcmp(SteamFriends()->GetPersonaName(), kName) == 0,
	               "persona_name -> GetPersonaName");

	// language -> GetCurrentGameLanguage; ui_language defaults to language.
	fails += check(std::strcmp(SteamApps()->GetCurrentGameLanguage(), kLang) == 0,
	               "language -> GetCurrentGameLanguage");
	fails += check(std::strcmp(SteamUtils()->GetSteamUILanguage(), kLang) == 0,
	               "ui_language defaults to language");

	// country -> GetIPCountry
	fails += check(std::strcmp(SteamUtils()->GetIPCountry(), kCountry) == 0,
	               "country -> GetIPCountry");

	// [dlc] -> BIsDlcInstalled / GetDLCCount / BGetDLCDataByIndex
	fails += check(SteamApps()->BIsDlcInstalled(kDlcA), "BIsDlcInstalled(known A)");
	fails += check(SteamApps()->BIsDlcInstalled(kDlcB), "BIsDlcInstalled(known B)");
	fails += check(!SteamApps()->BIsDlcInstalled(kDlcUnknown), "BIsDlcInstalled(unknown)==false");
	fails += check(SteamApps()->GetDLCCount() == 2, "GetDLCCount==2");

	AppId_t da = 0; bool avail = false; char dname[64] = {0};
	bool okIdx = SteamApps()->BGetDLCDataByIndex(0, &da, &avail, dname, sizeof(dname));
	printf("dlc[0]: ok=%d app=%u avail=%d name='%s'\n", okIdx, da, avail, dname);
	fails += check(okIdx && da == kDlcA && avail && std::strcmp(dname, "First DLC") == 0,
	               "BGetDLCDataByIndex(0)");

	// save_path -> RemoteStorage writes land there on disk.
	const char* payload = "config-save-path-check";
	fails += check(SteamRemoteStorage()->FileWrite(kCloudFile, payload,
	                                                (int)std::strlen(payload) + 1),
	               "FileWrite");
	const char* savePath = std::getenv("CONFIG_TEST_SAVE");
	if (savePath) {
		std::string onDisk = std::string(savePath) + "/remote/" + kCloudFile;
		struct stat st;
		bool exists = ::stat(onDisk.c_str(), &st) == 0;
		printf("cloud file at '%s' exists=%d\n", onDisk.c_str(), exists);
		fails += check(exists, "RemoteStorage wrote under save_path");
		SteamRemoteStorage()->FileDelete(kCloudFile);  // keep re-runs clean
	} else {
		fails += check(false, "CONFIG_TEST_SAVE set");
	}

	SteamAPI_Shutdown();
	return fails ? 1 : 0;
}

// Second child: an account-id-0 steam_id (the "nobody" base SteamID) must be
// rejected and overridden with the default account, not handed to the game.
static int run_child_zeroid() {
	if (!SteamAPI_Init()) { printf("FAIL: init\n"); return 2; }
	CSteamID id = SteamUser()->GetSteamID();
	printf("zeroid child: account=%u valid=%d\n", id.GetAccountID(), id.IsValid());
	int fails = 0;
	fails += check(id.GetAccountID() == 1000001, "account-id-0 steam_id overridden to default");
	SteamAPI_Shutdown();
	return fails ? 1 : 0;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "child") == 0)
		return run_child();
	if (argc > 1 && std::strcmp(argv[1], "child_zeroid") == 0)
		return run_child_zeroid();

	// Parent: build a temp dir with a steamemu.ini and a save_path under it.
	char tmpl[] = "/tmp/steamemu_cfg_XXXXXX";
	char* dir = mkdtemp(tmpl);
	if (!dir) { perror("mkdtemp"); printf("FAILED: config_test\n"); return 1; }
	std::string iniPath  = std::string(dir) + "/steamemu.ini";
	std::string savePath = std::string(dir) + "/cloud";

	FILE* f = std::fopen(iniPath.c_str(), "w");
	if (!f) { perror("fopen"); printf("FAILED: config_test\n"); return 1; }
	std::fprintf(f,
		"; generated by config_test\n"
		"account_id = %u\n"
		"persona_name = %s\n"
		"language = %s\n"
		"country = %s\n"
		"app_id = %u\n"
		"save_path = %s\n"
		"\n"
		"[dlc]\n"
		"%u = First DLC\n"
		"%u = Second DLC\n",
		kAccountId, kName, kLang, kCountry, kAppId, savePath.c_str(), kDlcA, kDlcB);
	std::fclose(f);

	// Env overrides would shadow the file values -- clear them so the FILE is
	// what's under test.
	unsetenv("STEAMEMU_STEAMID");
	unsetenv("STEAMEMU_NAME");
	unsetenv("SteamAppId");
	unsetenv("SteamGameId");
	setenv("STEAMEMU_CONFIG", iniPath.c_str(), 1);
	setenv("CONFIG_TEST_SAVE", savePath.c_str(), 1);

	pid_t pid = fork();
	if (pid == 0) {
		execl(argv[0], argv[0], "child", (char*)nullptr);
		perror("execl");
		_exit(127);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
	printf("child exit=%d\n", code);
	if (code != 0) { printf("FAILED: config_test\n"); return 1; }

	// Second scenario: a steam_id whose account id is 0 must be overridden.
	std::string zeroIni = std::string(dir) + "/steamemu_zeroid.ini";
	FILE* zf = std::fopen(zeroIni.c_str(), "w");
	if (!zf) { perror("fopen"); printf("FAILED: config_test\n"); return 1; }
	std::fprintf(zf, "; account-id-0 base SteamID -- must be rejected\n"
	                 "steam_id = 76561197960265728\n");
	std::fclose(zf);
	setenv("STEAMEMU_CONFIG", zeroIni.c_str(), 1);

	pid_t pid2 = fork();
	if (pid2 == 0) {
		execl(argv[0], argv[0], "child_zeroid", (char*)nullptr);
		perror("execl");
		_exit(127);
	}
	int status2 = 0;
	waitpid(pid2, &status2, 0);
	int code2 = WIFEXITED(status2) ? WEXITSTATUS(status2) : -1;
	printf("zeroid child exit=%d\n", code2);
	if (code2 != 0) { printf("FAILED: config_test\n"); return 1; }

	printf("PASS: config_test all checks passed\n");
	return 0;
}
