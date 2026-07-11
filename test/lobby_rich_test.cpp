//====== steamemu ============================================================
// Two-instance test for the richer ISteamMatchmaking lobby features:
//   * RequestLobbyList string filters (matching + non-matching),
//   * RequestLobbyData for a lobby we have NOT joined,
//   * per-member data (SetLobbyMemberData / GetLobbyMemberData) both ways,
//   * lobby chat (SendLobbyChatMsg -> LobbyChatMsg_t -> GetLobbyChatEntry) both ways.
//
// The owner creates a lobby with data + its own member data and heartbeats a
// chat line; the joiner filters for it, reads its data pre-join, joins, sets its
// own member data and chats. Each side asserts it observed the other's member
// data and chat. POSIX-only (fork/exec).
//============================================================================
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <sys/wait.h>
#include <unistd.h>

#include "steam/steam_api.h"

static ISteamMatchmaking* g_mm = nullptr;
static CSteamID g_lobby;
struct Chat { uint64 from; std::string text; };
static std::vector<Chat> g_chats;

// Capture every lobby chat message and pull its body via GetLobbyChatEntry.
struct ChatSink {
	STEAM_CALLBACK(ChatSink, OnChat, LobbyChatMsg_t);
};
void ChatSink::OnChat(LobbyChatMsg_t* p) {
	if (!g_mm) return;
	char buf[256] = {0};
	CSteamID user;
	EChatEntryType type = k_EChatEntryTypeInvalid;
	int n = g_mm->GetLobbyChatEntry(CSteamID(p->m_ulSteamIDLobby), p->m_iChatID, &user,
	                                buf, sizeof(buf) - 1, &type);
	if (n > 0) g_chats.push_back({user.ConvertToUint64(), std::string(buf, n)});
}

static bool sawChat(uint64 from, const char* needle) {
	for (auto& c : g_chats)
		if (c.from == from && c.text.find(needle) != std::string::npos) return true;
	return false;
}

static void tick() {
	SteamAPI_RunCallbacks();
	std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

static const uint64 kOwnerId  = 3000001;
static const uint64 kJoinerId = 3000002;
static uint64 full(uint64 acct) {
	return CSteamID((uint32)acct, k_EUniversePublic, k_EAccountTypeIndividual).ConvertToUint64();
}

static int run_owner() {
	if (!SteamAPI_Init()) return 2;
	ChatSink sink;
	g_mm = SteamMatchmaking();
	g_mm->CreateLobby(k_ELobbyTypePublic, 4);
	for (int i = 0; i < 8; ++i) tick();       // let CreateLobby settle
	g_lobby = g_mm->GetLobbyByIndex(0);        // owner reads back its own lobby
	if (!g_lobby.IsValid()) { fprintf(stderr, "owner: no lobby\n"); return 3; }
	g_mm->SetLobbyData(g_lobby, "map", "de_dust");
	g_mm->SetLobbyData(g_lobby, "mode", "competitive");
	g_mm->SetLobbyMemberData(g_lobby, "class", "sniper");
	fprintf(stderr, "owner: lobby %llu ready\n", (unsigned long long)g_lobby.ConvertToUint64());

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	auto lastChat = std::chrono::steady_clock::now();
	bool ok = false;
	auto lingerUntil = deadline;
	while (std::chrono::steady_clock::now() < deadline) {
		tick();
		if (std::chrono::steady_clock::now() - lastChat > std::chrono::milliseconds(400)) {
			g_mm->SendLobbyChatMsg(g_lobby, "hi", 3);  // include NUL
			lastChat = std::chrono::steady_clock::now();
		}
		int members = g_mm->GetNumLobbyMembers(g_lobby);
		const char* jc = g_mm->GetLobbyMemberData(g_lobby, CSteamID(full(kJoinerId)), "class");
		if (!ok && members >= 2 && jc && std::strcmp(jc, "medic") == 0 && sawChat(full(kJoinerId), "yo")) {
			fprintf(stderr, "owner: members=%d joiner.class=%s + got joiner chat\n", members, jc);
			ok = true;
			// Keep gossiping/chatting a bit so the peer can confirm us too.
			lingerUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		}
		if (ok && std::chrono::steady_clock::now() > lingerUntil) break;
	}
	SteamAPI_Shutdown();
	return ok ? 0 : 1;
}

static int run_joiner() {
	if (!SteamAPI_Init()) return 2;
	ChatSink sink;
	g_mm = SteamMatchmaking();

	auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);

	// 1. Filtered discovery: a matching string filter finds the lobby; a
	//    non-matching one hides it. Our RequestLobbyList caches synchronously.
	CSteamID lobby;
	while (std::chrono::steady_clock::now() < deadline && !lobby.IsValid()) {
		g_mm->AddRequestLobbyListStringFilter("map", "de_dust", k_ELobbyComparisonEqual);
		g_mm->RequestLobbyList();
		for (int i = 0; i < 4; ++i) tick();
		lobby = g_mm->GetLobbyByIndex(0);
	}
	if (!lobby.IsValid()) { fprintf(stderr, "joiner: filter didn't find lobby\n"); return 3; }
	g_mm->AddRequestLobbyListStringFilter("map", "nosuchmap", k_ELobbyComparisonEqual);
	g_mm->RequestLobbyList();
	for (int i = 0; i < 2; ++i) tick();
	if (g_mm->GetLobbyByIndex(0).IsValid()) { fprintf(stderr, "joiner: non-matching filter still returned a lobby\n"); return 3; }
	g_lobby = lobby;
	fprintf(stderr, "joiner: filters ok, lobby=%llu\n", (unsigned long long)lobby.ConvertToUint64());

	// 2. RequestLobbyData for a lobby we have NOT joined; then read its data.
	g_mm->RequestLobbyData(lobby);
	bool haveData = false;
	while (std::chrono::steady_clock::now() < deadline && !haveData) {
		tick();
		const char* mode = g_mm->GetLobbyData(lobby, "mode");
		haveData = mode && std::strcmp(mode, "competitive") == 0;
	}
	if (!haveData) { fprintf(stderr, "joiner: no lobby data pre-join\n"); return 4; }
	fprintf(stderr, "joiner: read lobby data pre-join ok\n");

	// 3. Join, publish our own member data, chat, and confirm the owner's.
	g_mm->JoinLobby(lobby);
	g_mm->SetLobbyMemberData(lobby, "class", "medic");
	auto lastChat = std::chrono::steady_clock::now();
	bool ok = false;
	auto lingerUntil = deadline;
	while (std::chrono::steady_clock::now() < deadline) {
		tick();
		if (std::chrono::steady_clock::now() - lastChat > std::chrono::milliseconds(400)) {
			g_mm->SendLobbyChatMsg(lobby, "yo", 3);
			lastChat = std::chrono::steady_clock::now();
		}
		int members = g_mm->GetNumLobbyMembers(lobby);
		const char* oc = g_mm->GetLobbyMemberData(lobby, CSteamID(full(kOwnerId)), "class");
		if (!ok && members >= 2 && oc && std::strcmp(oc, "sniper") == 0 && sawChat(full(kOwnerId), "hi")) {
			fprintf(stderr, "joiner: members=%d owner.class=%s + got owner chat\n", members, oc);
			ok = true;
			lingerUntil = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		}
		if (ok && std::chrono::steady_clock::now() > lingerUntil) break;
	}
	SteamAPI_Shutdown();
	return ok ? 0 : 1;
}

int main(int argc, char** argv) {
	if (argc > 1 && std::strcmp(argv[1], "owner") == 0) return run_owner();
	if (argc > 1 && std::strcmp(argv[1], "joiner") == 0) return run_joiner();

	setenv("SteamAppId", "482", 1);
	struct { const char* role; const char* id; } cfg[2] = {
		{"owner", "3000001"}, {"joiner", "3000002"},
	};
	pid_t pids[2];
	for (int i = 0; i < 2; ++i) {
		pid_t pid = fork();
		if (pid == 0) {
			setenv("STEAMEMU_STEAMID", cfg[i].id, 1);
			execl(argv[0], argv[0], cfg[i].role, (char*)nullptr);
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
		printf("%s exit=%d\n", cfg[i].role, code);
		if (code != 0) rc = 1;
	}
	if (rc == 0) printf("PASS: lobby_rich_test member-data/chat/filters/request worked\n");
	else printf("FAILED: lobby_rich_test\n");
	return rc;
}
