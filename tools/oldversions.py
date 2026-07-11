#!/usr/bin/env python3
"""Historical interface-version extraction for steamemu's code generator.

A game is compiled against ONE version of each ISteam* interface and calls its
methods by vtable slot index.  Valve inserts/removes/retypes methods between
versions, so slot N of SteamUser017 may be a different method than slot N of
SteamUser023.  If we hand a game our newest vtable for an older requested
version, it calls the wrong slot -> reads garbage -> "disconnected"/crash.

The fix: one concrete class multiply-inherits EVERY
historical version of the interface, and dispatch returns the pointer cast to
the exact requested version's sub-object -- each version gets its own correct
sub-vtable.  See tools/gen.py for how the emitted pieces are wired together.

This module reconstructs every historical version from the reference SDK's git
history (./third_party/SteamworksSDK, 74 ordinary commits with v1.00 .. v1.64 subjects --
the repo has no tags), which is the authoritative, uncurated source, plus the
vendored headers in tools/inter_versions/ for versions that shipped BETWEEN
those release-granularity commits (e.g. SteamFriends016) and therefore never
appear in the history.  "Uncurated" matters: the raw history still spells old 
annotation macros (CALL_RESULT) and, crucially, still declares methods whose RETURN TYPE 
later changed (ISteamFriends::SetPersonaName went void -> SteamAPICall_t).  C++ cannot
override two same-signature methods that differ only in return type, so the non-canonical 
variant is renamed in the old class body (keeping its vtable SLOT) and the concrete class
forwards it to the canonical method.

Extraction failures raise (SystemExit) rather than degrade: silently shipping
newest-only vtables, or silently skipping one version, is an ABI break for the
games that request the missing version.  gen.py passes its VERSIONS map so the
two files cannot drift apart unnoticed.

Output (consumed by gen.py):
  build(sdk, expect=None) -> dict with
    'prelude'        : compat #defines / forward-decls so old bodies compile now
    'class_defs'     : all renamed version classes, concatenated (-> versions.h)
    'version_classes': {iface: [(verstring, renamed_classname), ...]}  (non-newest)
    'newest'         : {iface: verstring}
    'extras'         : {iface: [override_str, ...]}  injected into the concrete class
    'null_versions'  : {iface: [verstring, ...]}  versions the real client NULLs
"""
import os
import re
import subprocess

# Interface -> (reference header, version-string prefix(es) that identify it).
# (Several interfaces share isteammatchmaking.h; the prefix disambiguates.)
# A tuple of prefixes is allowed: SDK 1.24-1.34 literally define
#   #define STEAMCONTROLLER_INTERFACE_VERSION "STEAMCONTROLLER_INTERFACE_VERSION"
# (the only DIGITLESS version string in the whole history), which the
# "SteamController" prefix alone would drop -- and then FindInterface would
# return NULL for a known interface, failing init for that era's games.
IFACES = {
    "ISteamClient":            ("isteamclient.h",            "SteamClient"),
    "ISteamUser":              ("isteamuser.h",              "SteamUser"),
    "ISteamFriends":           ("isteamfriends.h",           "SteamFriends"),
    "ISteamUtils":             ("isteamutils.h",             "SteamUtils"),
    "ISteamMatchmaking":       ("isteammatchmaking.h",       "SteamMatchMaking0"),
    "ISteamMatchmakingServers":("isteammatchmaking.h",       "SteamMatchMakingServers"),
    "ISteamParties":           ("isteammatchmaking.h",       "SteamParties"),
    "ISteamRemoteStorage":     ("isteamremotestorage.h",     "STEAMREMOTESTORAGE"),
    "ISteamUserStats":         ("isteamuserstats.h",         "STEAMUSERSTATS"),
    "ISteamApps":              ("isteamapps.h",              "STEAMAPPS"),
    "ISteamNetworking":        ("isteamnetworking.h",        "SteamNetworking0"),
    "ISteamScreenshots":       ("isteamscreenshots.h",       "STEAMSCREENSHOTS"),
    "ISteamMusic":             ("isteammusic.h",             "STEAMMUSIC"),
    "ISteamHTTP":              ("isteamhttp.h",              "STEAMHTTP"),
    "ISteamInput":             ("isteaminput.h",             "SteamInput"),
    "ISteamController":        ("isteamcontroller.h",        ("SteamController", "STEAMCONTROLLER_INTERFACE_VERSION")),
    "ISteamUGC":               ("isteamugc.h",               "STEAMUGC"),
    "ISteamHTMLSurface":       ("isteamhtmlsurface.h",       "STEAMHTMLSURFACE"),
    "ISteamInventory":         ("isteaminventory.h",         "STEAMINVENTORY"),
    "ISteamVideo":             ("isteamvideo.h",             "STEAMVIDEO"),
    "ISteamGameServer":        ("isteamgameserver.h",        "SteamGameServer0"),
    "ISteamGameServerStats":   ("isteamgameserverstats.h",   "SteamGameServerStats"),
    "ISteamNetworkingSockets": ("isteamnetworkingsockets.h", "SteamNetworkingSockets"),
    "ISteamNetworkingUtils":   ("isteamnetworkingutils.h",   "SteamNetworkingUtils"),
    "ISteamNetworkingMessages":("isteamnetworkingmessages.h","SteamNetworkingMessages"),
    "ISteamRemotePlay":        ("isteamremoteplay.h",        "STEAMREMOTEPLAY"),
    "ISteamTimeline":          ("isteamtimeline.h",          "STEAMTIMELINE"),
    "ISteamParentalSettings":  ("isteamparentalsettings.h",  "STEAMPARENTALSETTINGS"),
}

# Version strings real games request that are ABSENT from BOTH the SDK git
# history and tools/inter_versions/, but whose vtable is known to be identical to 
# a neighboring version: serve that neighbor's sub-object. Keyed verstring -> verstring 
# whose class to reuse.
ALIAS_VERSIONS = {
    "SteamGameServer006": "SteamGameServer008",
    "SteamGameServer007": "SteamGameServer008",
}

# Version strings for which the REAL steamclient returns NULL. Returning our 
# newest vtable instead would hand the game wrong slots; returning NULL is 
# the behavior it was built for.
NULL_VERSIONS = {
    "ISteamController":        ["SteamController001", "SteamController002"],
    "ISteamNetworkingSockets": ["SteamNetworkingSockets007"],
}

# Directory of vendored headers for versions that shipped BETWEEN the SDK
# repo's release commits. Each file declares the interface under its canonical name
# plus a "#define ..._INTERFACE_VERSION... "<verstring>"" and is parsed through
# the exact same machinery as a git-history header body.
INTER_VERSIONS_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "inter_versions")

# Compat prelude: makes the historical class bodies compile against the CURRENT
# SDK's types.  Old headers use un-prefixed annotation macros and reference a
# handful of interfaces/types that were later removed or renamed.
PRELUDE = r"""// GENERATED by tools/oldversions.py -- do not edit.
// Compat shims so historical interface class bodies compile against the current
// SDK.  Everything here is confined to versions.h and #undef'd at the bottom.
#include "steam/steam_api.h"

// Old un-prefixed annotation macros -> current STEAM_* equivalents.
#ifndef CALL_RESULT
#define CALL_RESULT(x) STEAM_CALL_RESULT(x)
#define CALL_BACK(x) STEAM_CALL_BACK(x)
#define OUT_STRUCT() STEAM_OUT_STRUCT()
#define OUT_STRING() STEAM_OUT_STRING()
#define OUT_ARRAY_CALL(a,b,c) STEAM_OUT_ARRAY_CALL(a,b,c)
#define OUT_ARRAY_COUNT(a,b) STEAM_OUT_ARRAY_COUNT(a,b)
#define ARRAY_COUNT(a) STEAM_ARRAY_COUNT(a)
#define ARRAY_COUNT_D(a,b) STEAM_ARRAY_COUNT_D(a,b)
#define BUFFER_COUNT(a) STEAM_BUFFER_COUNT(a)
#define OUT_BUFFER_COUNT(a) STEAM_OUT_BUFFER_COUNT(a)
#define OUT_STRING_COUNT(a) STEAM_OUT_STRING_COUNT(a)
#define DESC(a) STEAM_DESC(a)
#define IGNOREATTR(...)
#define METHOD_DESC(...)
#endif
// Annotation macros some old bodies spell already-prefixed, but which the
// current SDK does not define -- make them no-ops.
#ifndef STEAM_METHOD_DESC
#define STEAM_METHOD_DESC(...)
#endif
#ifndef STEAM_IGNOREATTR
#define STEAM_IGNOREATTR(...)
#endif

// Interfaces referenced (as pointer return types) by old ISteamClient versions
// but since removed from the SDK -- a forward declaration is all the old class
// bodies need.
class ISteamGameSearch;
class ISteamAppList;
class ISteamUnifiedMessages;
class ISteamMasterServerUpdater;
class ISteamGameStats;
class ISteamContentServer;
class ISteamMusicRemote;
class ISteamController;
class ISteamParentalSettings;
class ISteamHTMLSurface;
class ISteamInventory;
class ISteamVideo;
class ISteamNetworkingSocketsCallbacks;
class ISteamNetworkingConnectionCustomSignaling;
class ISteamNetworkingCustomSignalingRecvContext;
class ISteamPS3OverlayRender;

// Types removed/renamed since; old class bodies only use them as parameter or
// return types, so an opaque declaration suffices for the vtable layout.
enum EMatchMakingType { k_eMatchMakingType_compat = 0 };
// Passed BY VALUE to the historical ISteamRemoteStorage::UpdatePublishedFile
// (SDK ~1.15-1.20), so its size/alignment are ABI (on 32-bit Windows the
// callee pops the argument bytes). This reproduces the historical data layout
// exactly (isteamremotestorage.h at v1.17); the member functions it also had
// don't affect layout and are omitted.
#pragma pack( push, 8 )
struct RemoteStorageUpdatePublishedFileRequest_t
{
	PublishedFileId_t m_unPublishedFileId;
	const char *m_pchFile;
	const char *m_pchPreviewFile;
	const char *m_pchTitle;
	const char *m_pchDescription;
	ERemoteStoragePublishedFileVisibility m_eVisibility;
	SteamParamStringArray_t *m_pTags;
	bool m_bUpdateFile;
	bool m_bUpdatePreviewFile;
	bool m_bUpdateTitle;
	bool m_bUpdateDescription;
	bool m_bUpdateVisibility;
	bool m_bUpdateTags;
};
#pragma pack( pop )
struct SteamNetworkingQuickConnectionStatus;
// SDK <= 1.34 gamepad state blob (digitless STEAMCONTROLLER_INTERFACE_VERSION
// era); only ever passed by pointer, so an opaque declaration suffices.
struct SteamControllerState_t;
extern "C" typedef void ( *SteamAPI_PostAPIResultInProcess_t )( SteamAPICall_t callHandle, void *, uint32 unCallbackSize, int iCallbackNum );
"""

PRELUDE_UNDEF = r"""
// Keep the compat annotation macros out of the rest of the translation unit.
#ifdef CALL_RESULT
#undef CALL_RESULT
#undef CALL_BACK
#undef OUT_STRUCT
#undef OUT_STRING
#undef OUT_ARRAY_CALL
#undef OUT_ARRAY_COUNT
#undef ARRAY_COUNT
#undef ARRAY_COUNT_D
#undef BUFFER_COUNT
#undef OUT_BUFFER_COUNT
#undef OUT_STRING_COUNT
#undef DESC
#undef IGNOREATTR
#undef METHOD_DESC
#endif
"""

# Return-type conflicts: a method whose return type changed across versions.
# The concrete class inherits every version, but C++ forbids two overrides that
# differ only in return type, so the OLDER (non-canonical) variant is renamed to
# "<name>_compat" in its class body (its vtable slot is unchanged) and the
# concrete class supplies a forwarder to the canonical (newest) method.
#
# The common case -- an older variant that returned `void` where the method later
# grew a real return (SteamAPICall_t / HServerListRequest) -- is handled
# automatically: the forwarder just calls the canonical method and drops the
# result.  Only conflicts whose OLD variant returns non-void need an explicit
# conversion here.  Keyed by "Iface::method"; value is fn(name, arg_names) -> body.
CONFLICT_FORWARD = {
    # uint32 (IPv4)  ->  SteamIPAddress_t : project the struct back to its IPv4.
    "ISteamGameServer::GetPublicIP":
        lambda name, a: "return GetPublicIP().m_unIPv4;",
    # bool  ->  EResult : success iff the modern call returns k_EResultOK.
    "ISteamNetworkingSockets::GetHostedDedicatedServerAddress":
        lambda name, a: "return GetHostedDedicatedServerAddress(%s) == k_EResultOK;" % a[0],
}

# Old-only signatures (removed or re-parameterized in the NEWEST version) whose
# behavior we do have: forward to the surviving implementation instead of the
# neutral stub, so a game served an old sub-vtable still gets working auth,
# lobbies, stats and P2P (it would otherwise initialize cleanly and then find
# those silently dead). Keyed "Iface::Method(<_param_types tuple>)"; value is
# fn(arg_names) -> body. Anything not listed keeps the neutral default.
OLD_BODIES = {
    # The SteamNetworkingIdentity param was added in SteamUser023; older callers
    # get a ticket with no target identity (our auth ignores it anyway).
    "ISteamUser::GetAuthSessionTicket(void*,int,uint32*)":
        lambda a: "return GetAuthSessionTicket(%s, %s, %s, nullptr);" % (a[0], a[1], a[2]),
    "ISteamGameServer::GetAuthSessionTicket(void*,int,uint32*)":
        lambda a: "return GetAuthSessionTicket(%s, %s, %s, nullptr);" % (a[0], a[1], a[2]),
    # cMaxMembers was added in SteamMatchMaking009; 250 is Steam's lobby cap.
    "ISteamMatchmaking::CreateLobby(ELobbyType)":
        lambda a: "return CreateLobby(%s, 250);" % a[0],
    "ISteamMatchmaking::CreateLobby(bool)":
        lambda a: "CreateLobby(%s ? k_ELobbyTypePrivate : k_ELobbyTypePublic, 250);" % a[0],
    # Removed in STEAMUSERSTATS013 (stats requests became per-user). Old games
    # gate ALL stats/achievement work on the UserStatsReceived_t this queues.
    "ISteamUserStats::RequestCurrentStats()":
        lambda a: "RequestUserStats(emu::LocalSteamID()); return true;",
    # nChannel was added in SteamNetworking003; older callers mean channel 0.
    "ISteamNetworking::SendP2PPacket(CSteamID,const void*,uint32,EP2PSend)":
        lambda a: "return SendP2PPacket(%s, %s, %s, %s, 0);" % (a[0], a[1], a[2], a[3]),
    "ISteamNetworking::IsP2PPacketAvailable(uint32*)":
        lambda a: "return IsP2PPacketAvailable(%s, 0);" % a[0],
    "ISteamNetworking::ReadP2PPacket(void*,uint32,uint32*,CSteamID*)":
        lambda a: "return ReadP2PPacket(%s, %s, %s, %s, 0);" % (a[0], a[1], a[2], a[3]),
}

# return-type (greedy-min, may end in * / & glued to the name), method name, then
# the parameter list (greedy so function-pointer params with inner ')' survive).
_METH = re.compile(
    r'virtual\s+(.+?)\b([A-Za-z_]\w*)\s*\(([^;{]*)\)\s*(const)?\s*=\s*0',
    re.S)


def _run(sdk, *args):
    return subprocess.check_output(["git", "-C", sdk, *args], stderr=subprocess.DEVNULL)


class _GitBatch:
    """One long-lived `git cat-file --batch` pipe instead of one `git show`
    subprocess per (interface, commit) -- the walk makes ~2000 blob reads, and
    shared headers (isteammatchmaking.h serves three interfaces) repeat, so
    results are also memoized by (commit, path)."""

    def __init__(self, sdk):
        self._proc = subprocess.Popen(
            ["git", "-C", sdk, "cat-file", "--batch"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL)
        self._cache = {}

    def blob(self, commit, path):
        """File content at commit as latin-1 text, or None if absent."""
        key = (commit, path)
        if key in self._cache:
            return self._cache[key]
        self._proc.stdin.write(("%s:%s\n" % (commit, path)).encode())
        self._proc.stdin.flush()
        hdr = self._proc.stdout.readline().decode()
        if not hdr or hdr.split()[-1] in ("missing", "ambiguous"):
            self._cache[key] = None
            return None
        size = int(hdr.split()[2])
        buf = b""
        while len(buf) < size + 1:                 # content + trailing LF
            chunk = self._proc.stdout.read(size + 1 - len(buf))
            if not chunk:
                raise SystemExit("oldversions: git cat-file pipe died reading %s:%s"
                                 % (commit, path))
            buf += chunk
        text = buf[:size].decode("latin-1")
        self._cache[key] = text
        return text

    def close(self):
        try:
            self._proc.stdin.close()
            self._proc.wait(timeout=10)
        except Exception:
            self._proc.kill()


def _strip_comments(t):
    t = re.sub(r'/\*.*?\*/', '', t, flags=re.S)
    return re.sub(r'//[^\n]*', '', t)


def _blank_comments(t):
    """Same-length copy with //... and /*...*/ replaced by spaces (newlines kept),
    so brace/`;` scanning isn't fooled by punctuation inside comments."""
    out = list(t)
    i, n = 0, len(t)
    while i < n:
        if t[i] == '/' and i + 1 < n and t[i + 1] == '/':
            while i < n and t[i] != '\n':
                out[i] = ' '
                i += 1
        elif t[i] == '/' and i + 1 < n and t[i + 1] == '*':
            j = t.find('*/', i + 2)
            j = n if j < 0 else j + 2
            while i < j:
                if t[i] != '\n':
                    out[i] = ' '
                i += 1
        else:
            i += 1
    return "".join(out)


def _extract_class_body(text, cn):
    """Return (start_index, end_index_after_brace, body_text) for the DEFINITION
    of `class cn` (skipping any forward declaration `class cn;`)."""
    scan = _blank_comments(text)
    for m in re.finditer(r'\bclass\s+' + re.escape(cn) + r'\b', scan):
        brace = scan.find("{", m.end())
        semi = scan.find(";", m.end())
        if brace < 0 or (0 <= semi < brace):
            continue                     # forward declaration, not the definition
        depth, i = 0, brace
        while i < len(scan):
            if scan[i] == '{':
                depth += 1
            elif scan[i] == '}':
                depth -= 1
                if depth == 0:
                    return (m.start(), i + 1, text[brace + 1:i])
            i += 1
    return None


def _norm(t):
    t = re.sub(r'\s+', ' ', t).strip()
    t = re.sub(r'\s*\*', '*', t)
    t = re.sub(r'\s*&', '&', t)
    return t


def _split_params(pstr):
    out, depth, cur = [], 0, ''
    for ch in pstr:
        if ch in '(<[':
            depth += 1
        elif ch in ')>]':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return out


# A trailing identifier is only a parameter NAME if stripping it still leaves a
# type. Type keywords ("unsigned int") and single-token unnamed value params
# ("bool") must keep their last token or the overload key / emitted forwarder
# would be corrupted ("unsigned", "").
_TYPE_KEYWORDS = {"void", "bool", "char", "short", "int", "long", "signed",
                  "unsigned", "float", "double", "const", "volatile",
                  "struct", "class", "enum"}


def _trailing_name(p):
    """The parameter name at the end of declaration `p`, or None if unnamed."""
    if p.endswith(('*', '&')):
        return None
    # Array parameters ("CSteamID groupIDs[]"): the name precedes the brackets.
    m = re.search(r'(\b[A-Za-z_]\w*)\s*((?:\[[^\]]*\]\s*)+)$', p)
    if m:
        return m.group(1) if p[:m.start()].strip() else None
    m = re.search(r'(\b[A-Za-z_]\w*)\s*$', p)
    if not m or m.group(1) in _TYPE_KEYWORDS:
        return None
    if not p[:m.start()].strip():                  # single token: it IS the type
        return None
    return m.group(1)


def _param_types(pstr):
    """Canonical parameter TYPE tuple (names/defaults stripped) -- the overload
    key. Array parameters decay to the pointer they are in the signature
    ("CSteamID groupIDs[]" -> "CSteamID*"), so spelling differences between
    versions don't fork the key."""
    types = []
    for p in _split_params(pstr):
        p = p.strip()
        if not p or p == 'void':
            continue
        p = re.sub(r'=[^,]*$', '', p).strip()          # default arg
        arr = re.search(r'(\b[A-Za-z_]\w*)?\s*((?:\[[^\]]*\]\s*)+)$', p)
        if arr and arr.group(2):
            head = p[:arr.start()].strip()
            p = (head if head else arr.group(1) or "") + "*"
        else:
            name = _trailing_name(p)
            if name:
                p = p[:p.rfind(name)].strip()
        types.append(_norm(p))
    return tuple(types)


def _emit_params(pstr):
    """Parameter list for an override decl: keep types+names, drop defaults.
    Unnamed parameters get the same synthesized name _param_names reports, so a
    forwarder body can reference every argument."""
    out = []
    for i, p in enumerate(_split_params(pstr)):
        p = re.sub(r'=[^,]*$', '', p).strip()          # default arg
        if not p or p == 'void':
            continue
        if _trailing_name(p) is None:
            p = "%s p%d" % (p, i)
        out.append(_norm(p))
    return out


def _param_names(pstr):
    names = []
    for i, p in enumerate(_split_params(pstr)):
        p = re.sub(r'=[^,]*$', '', p).strip()
        if not p or p == 'void':
            continue
        names.append(_trailing_name(p) or "p%d" % i)
    return names


# SAL-style annotation macros that DECORATE a method/parameter but are not part
# of its C++ type. They carry balanced parens (with commas) that would otherwise
# break method/param parsing, and they are #undef'd outside versions.h, so they
# must be stripped from the parsed signatures used to emit overrides.
_DECOR = ["CALL_RESULT", "CALL_BACK", "OUT_STRUCT", "OUT_STRING", "OUT_ARRAY_CALL",
          "OUT_ARRAY_COUNT", "ARRAY_COUNT_D", "ARRAY_COUNT", "OUT_BUFFER_COUNT",
          "BUFFER_COUNT", "OUT_STRING_COUNT", "METHOD_DESC", "DESC", "IGNOREATTR"]


def _match_paren(s, open_idx):
    depth = 0
    for i in range(open_idx, len(s)):
        if s[i] == '(':
            depth += 1
        elif s[i] == ')':
            depth -= 1
            if depth == 0:
                return i
    return -1


def _remove_macro(s, name, unwrap=False):
    """Remove every `name( ...balanced... )`; keep the inner text if unwrap."""
    pat = re.compile(r'\b' + re.escape(name) + r'\b\s*\(')
    out, i = [], 0
    while True:
        m = pat.search(s, i)
        if not m:
            out.append(s[i:])
            return "".join(out)
        close = _match_paren(s, m.end() - 1)
        if close < 0:
            out.append(s[i:])
            return "".join(out)
        out.append(s[i:m.start()])
        if unwrap:
            out.append(s[m.end():close])
        i = close + 1


def _preprocess(body):
    body = _strip_comments(body)
    body = _remove_macro(body, "STEAM_PRIVATE_API", unwrap=True)  # keep the methods
    for d in _DECOR:
        body = _remove_macro(body, "STEAM_" + d)
        body = _remove_macro(body, d)
    return body


def _blank_macro_spans(scan, names):
    """Same-length copy of `scan` with every `NAME( ...balanced... )` span
    space-filled (newlines kept), so regex matching over it cannot be fooled by
    punctuation inside annotation macros while positions still map to the
    original text."""
    out = list(scan)
    for name in names:
        for m in re.finditer(r'\b' + re.escape(name) + r'\s*\(', scan):
            close = _match_paren(scan, m.end() - 1)
            if close < 0:
                continue
            for i in range(m.start(), close + 1):
                if out[i] != '\n':
                    out[i] = ' '
    return "".join(out)


def _rename_method(src, name, ptypes, newname):
    """Rename the declaration of `name` whose parameter-type tuple is `ptypes`
    to `newname` in raw class text. Matching runs over a copy with comments and
    annotation-macro spans blanked (same length), so a comment mentioning
    `virtual ... name(` or a same-named overload cannot absorb the rename."""
    scan = _blank_comments(src)
    scan = _blank_macro_spans(scan, ["STEAM_" + d for d in _DECOR] + _DECOR)
    for m in _METH.finditer(scan):
        if m.group(2) != name or _param_types(m.group(3)) != ptypes:
            continue
        return src[:m.start(2)] + newname + src[m.end(2):]
    raise SystemExit("oldversions: rename target %s(%s) not found in class body"
                     % (name, ",".join(ptypes)))


def _methods(body):
    """Parsed pure-virtuals: list of dicts {name, ptypes, ret, const, pstr}."""
    body = _preprocess(body)
    out = []
    for ret, name, params, c in _METH.findall(body):
        out.append({"name": name, "ptypes": _param_types(params),
                    "ret": _norm(ret), "const": bool(c), "pstr": params})
    return out


_PREPROC = re.compile(r'^[ \t]*#[ \t]*(if|ifdef|ifndef|elif|else|endif)\b[^\n]*',
                      re.M)


def _vtable_key(body):
    """Key under which two versions may share one emitted class: the ordered
    method signature list INTERLEAVED with preprocessor conditionals. Two
    versions with the same parsed methods but a #ifdef gate around different
    ones would compile to different vtables, so gate positions are part of the
    identity."""
    body = _preprocess(body)
    events = []
    for m in _PREPROC.finditer(body):
        events.append((m.start(), ("pp", _norm(m.group(0)))))
    for m in _METH.finditer(body):
        events.append((m.start(), ("m", m.group(2), _param_types(m.group(3)),
                                   _norm(m.group(1)), bool(m.group(4)))))
    return tuple(ev for _, ev in sorted(events, key=lambda e: e[0]))


def _sanitize(ver):
    return re.sub(r'\W', '_', ver)


def _vernum(ver):
    """Trailing version number, or -1 for the one digitless string in history."""
    m = re.search(r'(\d+)$', ver)
    return int(m.group(1)) if m else -1


_VERRE = re.compile(r'#define\s+\w*INTERFACE_VERSION\w*\s+"([^"]+)"')


def _load_supplements():
    """tools/inter_versions/*.h -> {verstring: header text}. Each vendored file
    must declare exactly one interface version."""
    out = {}
    if not os.path.isdir(INTER_VERSIONS_DIR):
        return out
    for fn in sorted(os.listdir(INTER_VERSIONS_DIR)):
        if not fn.endswith(".h"):
            continue
        text = open(os.path.join(INTER_VERSIONS_DIR, fn), encoding="latin-1").read()
        vs = _VERRE.findall(text)
        if len(vs) != 1:
            raise SystemExit("oldversions: %s must define exactly one interface "
                             "version, found %r" % (fn, vs))
        if vs[0] in out:
            raise SystemExit("oldversions: duplicate supplement for %s" % vs[0])
        out[vs[0]] = text
    return out


def build(sdk, expect=None):
    """Extract every historical interface version. `expect` (gen.py's VERSIONS
    map, iface -> newest version string) cross-checks that IFACES covers every
    generated interface and that the extracted newest matches gen.py -- the two
    hand-maintained maps cannot drift apart silently."""
    commits = _run(sdk, "rev-list", "--reverse", "HEAD").decode().split()
    git = _GitBatch(sdk)
    supplements = _load_supplements()
    sup_unused = set(supplements)

    if expect:
        lacking = sorted(cn for cn in expect if cn not in IFACES)
        if lacking:
            raise SystemExit("oldversions: IFACES lacks entries for %s (add them "
                             "so version dispatch covers every interface)" % lacking)

    prelude_parts = [PRELUDE]
    class_defs = []
    version_classes = {}
    newest = {}
    extras = {}
    null_versions = {}

    for iface, (hdr, prefix) in IFACES.items():
        prefixes = prefix if isinstance(prefix, tuple) else (prefix,)
        path = "public/steam/" + hdr
        # verstring -> newest commit body that still declares it (oldest-first
        # loop, keep overwriting -> the last write is the newest such commit).
        seen = {}
        order = []
        for c in commits:
            b = git.blob(c, path)
            if b is None:
                continue
            for v in _VERRE.findall(b):
                if v.startswith(prefixes):
                    if v not in seen:
                        order.append(v)
                    seen[v] = b
        if not order:
            raise SystemExit("oldversions: no version strings found for %s in %s "
                             "-- wrong header or prefix in IFACES?" % (iface, hdr))
        # Splice in the vendored inter-release versions (numeric position; they
        # can never be the newest -- the expect cross-check would catch that).
        for sv in sorted((v for v in supplements if v.startswith(prefixes)),
                         key=_vernum):
            if sv in seen:
                raise SystemExit("oldversions: supplement %s duplicates the SDK "
                                 "git history; delete the vendored file" % sv)
            idx = next((k for k, ov in enumerate(order)
                        if _vernum(ov) > _vernum(sv)), len(order))
            order.insert(idx, sv)
            seen[sv] = supplements[sv]
            sup_unused.discard(sv)
        newest_ver = order[-1]
        newest[iface] = newest_ver
        if expect and iface in expect and expect[iface] != newest_ver:
            raise SystemExit("oldversions: newest %s extracted as %s but gen.py "
                             "VERSIONS says %s" % (iface, newest_ver, expect[iface]))
        for nv in NULL_VERSIONS.get(iface, []):
            if nv in seen:
                raise SystemExit("oldversions: NULL_VERSIONS entry %s exists in "
                                 "the extracted history" % nv)
        null_versions[iface] = list(NULL_VERSIONS.get(iface, []))

        # Parse every version's method set. A version whose class body cannot
        # be extracted/parsed is an ABI hole, not something to skip.
        parsed = {}
        bodytext = {}
        for v in order:
            body = _extract_class_body(seen[v], iface)
            if not body:
                raise SystemExit("oldversions: could not extract the %s class "
                                 "body declaring %s" % (iface, v))
            bodytext[v] = body[2]
            parsed[v] = _methods(body[2])
            if not parsed[v]:
                raise SystemExit("oldversions: no virtual methods parsed for %s "
                                 "(%s)" % (v, iface))
        newest_methods = parsed.get(newest_ver, [])
        newest_keys = {(m["name"], m["ptypes"], m["const"]) for m in newest_methods}

        # For each method that changed return type, find the canonical (newest)
        # variant and the set of (name,params) that need renaming in older bodies.
        # conflict_rename[verstring] = set of method names to rename in that body.
        conflict_rename = {}
        conflict_overrides = []
        by_sig = {}
        for v in order:
            for m in parsed.get(v, []):
                by_sig.setdefault((m["name"], m["ptypes"]), {}).setdefault(
                    (m["ret"], m["const"]), []).append(v)
        for (name, ptypes), variants in by_sig.items():
            if len(variants) < 2:
                continue
            fkey = "%s::%s" % (iface, name)
            # Canonical = the variant the NEWEST version declares.
            canon = None
            for rc, vs in variants.items():
                if newest_ver in vs:
                    canon = rc
                    break
            if canon is None:                    # method absent from newest: pick last
                canon = list(variants.keys())[-1]
            # Older, non-canonical variants: rename in their bodies + one forwarder.
            emitted = set()
            for (ret, const), vs in variants.items():
                if (ret, const) == canon:
                    continue
                for v in vs:
                    conflict_rename.setdefault(v, set()).add((name, ptypes))
                if (ret, const) in emitted:
                    continue
                emitted.add((ret, const))
                # Build the forwarder override using a representative old decl.
                sample_v = vs[0]
                sm = next(m for m in parsed[sample_v]
                          if m["name"] == name and m["ptypes"] == ptypes)
                pdecl = _emit_params(sm["pstr"])
                pnames = _param_names(sm["pstr"])
                if fkey in CONFLICT_FORWARD:
                    body = CONFLICT_FORWARD[fkey](name, pnames)
                elif ret == "void":
                    # Old variant returned nothing: call the canonical, drop result.
                    body = "%s(%s);" % (name, ", ".join(pnames))
                else:
                    raise SystemExit(
                        "oldversions: unhandled return-type conflict %s(%s): %s\n"
                        "  old variant returns non-void; add a CONFLICT_FORWARD entry."
                        % (name, ",".join(ptypes),
                           {rc: vv for rc, vv in variants.items()}))
                # No `override`: a few conflicting methods are platform-gated
                # (#ifdef _PS3 / SDR) in the old body, so the renamed base slot may
                # not exist on this platform -- then this is a harmless dead
                # member; where the base does declare it, it still overrides.
                conflict_overrides.append(
                    "%s %s_compat(%s)%s { %s }" % (
                        ret, name, ", ".join(pdecl),
                        " const" if const else "", body))

        # Emit renamed class defs for every NON-newest version. Versions whose
        # parsed vtable (method order, signatures, return types, constness) is
        # identical to an already-emitted sibling -- or to the newest interface
        # itself -- reuse that class instead of duplicating it; the multiply-
        # inheriting concrete class then carries one base serving both strings.
        vclasses = []
        vtab_of = {_vtable_key(bodytext[newest_ver]): iface}
        for v in order:
            if v == newest_ver:
                continue
            vkey = _vtable_key(bodytext[v])
            dup = vtab_of.get(vkey)
            if dup is not None:
                vclasses.append((v, dup))
                continue
            start, end, body = _extract_class_body(seen[v], iface)
            src = seen[v][start:end]
            newname = "%s_%s" % (iface, _sanitize(v))
            # Rename the class, its destructor, and give a body-less dtor a body.
            src = re.sub(r'\bclass\s+' + re.escape(iface) + r'\b',
                         'class ' + newname, src, count=1)
            src = re.sub(r'~' + re.escape(iface) + r'\b', '~' + newname, src)
            src = re.sub(r'(~' + re.escape(newname) +
                         r'\s*\([^)]*\))\s*(?:[A-Za-z_]+\s*)?;', r'\1 {}', src)
            # Rename conflicting methods (return-type clashes) -> "<name>_compat".
            for name, ptypes in conflict_rename.get(v, ()):
                src = _rename_method(src, name, ptypes, name + "_compat")
            # _extract_class_body ends at the closing brace; add the class ';'.
            class_defs.append(src + ";")
            vclasses.append((v, newname))
            vtab_of[vkey] = newname
        # Version strings absent from every source but known to share a neighboring 
        # version's vtable: alias them.
        for av in sorted(v for v in ALIAS_VERSIONS if v.startswith(prefixes)):
            tv = ALIAS_VERSIONS[av]
            if av in seen:
                raise SystemExit("oldversions: ALIAS_VERSIONS entry %s exists in "
                                 "the extracted history" % av)
            if tv == newest_ver:
                vclasses.append((av, iface))
            elif tv in dict(vclasses):
                vclasses.append((av, dict(vclasses)[tv]))
            else:
                raise SystemExit("oldversions: alias %s -> %s: target version "
                                 "not extracted" % (av, tv))
        version_classes[iface] = vclasses
        vclass_of = dict(vclasses)

        # Types (enums/structs) defined NESTED inside a version's interface class.
        # A method that uses one is a different overload per version (the nested
        # type is nominally distinct across bases), and outside the class the name
        # is ambiguous -- so such methods are emitted PER version with the type
        # qualified to its own base class, and never deduped across versions.
        nested = set()
        for v in order:
            span = _extract_class_body(seen[v], iface)
            if span:
                for t in re.findall(r'\b(?:enum(?:\s+class)?|struct|class)\s+(\w+)\s*[:{]',
                                    _blank_comments(span[2])):
                    nested.add(t)

        def qualify(text, vclass):
            for t in nested:
                text = re.sub(r'\b' + re.escape(t) + r'\b', vclass + "::" + t, text)
            return text

        # Extra overrides the concrete class must supply: signatures present in
        # some OLD version but absent from the newest (removed or retyped
        # methods), so the class is concrete for every inherited base.
        seen_extra = set(newest_keys)
        seen_nested = set()
        ex = list(conflict_overrides)
        for v in order:
            if v == newest_ver:
                continue
            renamed = conflict_rename.get(v, set())
            vclass = vclass_of.get(v)
            for m in parsed.get(v, []):
                if (m["name"], m["ptypes"]) in renamed:
                    continue                     # handled by the forwarder above
                sigtext = " ".join(m["ptypes"]) + " " + m["ret"]
                uses_nested = vclass and any(
                    re.search(r'\b' + re.escape(t) + r'\b', sigtext) for t in nested)
                if uses_nested:
                    key = (v, m["name"], m["ptypes"], m["const"])
                    if key in seen_nested:
                        continue
                    seen_nested.add(key)
                    ret = qualify(m["ret"], vclass)
                    pdecl = [qualify(p, vclass) for p in _emit_params(m["pstr"])]
                else:
                    key = (m["name"], m["ptypes"], m["const"])
                    if key in seen_extra:
                        continue
                    seen_extra.add(key)
                    ret = m["ret"]
                    pdecl = _emit_params(m["pstr"])
                fwd = OLD_BODIES.get(
                    "%s::%s(%s)" % (iface, m["name"], ",".join(m["ptypes"])))
                if fwd is not None:
                    default = " %s " % fwd(_param_names(m["pstr"]))
                else:
                    default = ("" if m["ret"] == "void"
                               else ' return ""; ' if m["ret"] == "const char *"
                               else " return {}; ")
                # No `override`: methods gated behind #ifdef _PS3 / SDR in the old
                # body don't exist on this platform (so there is nothing to
                # override -- a harmless dead member); real old-only methods still
                # override their base implicitly.
                ex.append('%s %s(%s)%s { EMU_LOG("%s::%s"); %s}' % (
                    ret, m["name"], ", ".join(pdecl),
                    " const" if m["const"] else "", iface, m["name"], default))
        extras[iface] = ex

    git.close()
    if sup_unused:
        raise SystemExit("oldversions: vendored versions matched no interface "
                         "prefix: %s" % sorted(sup_unused))

    return {
        "prelude": "".join(prelude_parts),
        "prelude_undef": PRELUDE_UNDEF,
        "class_defs": "\n\n".join(class_defs),
        "version_classes": version_classes,
        "newest": newest,
        "extras": extras,
        "null_versions": null_versions,
    }


if __name__ == "__main__":
    import sys
    sdk = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "third_party", "SteamworksSDK")
    r = build(sdk)
    n = sum(len(v) for v in r["version_classes"].values())
    e = sum(len(v) for v in r["extras"].values())
    print("versions: %d old classes, %d extra overrides across %d interfaces" %
          (n, e, len(r["version_classes"])))
