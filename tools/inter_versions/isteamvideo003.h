// STEAMVIDEO_INTERFACE_V003 -- inter-release interface version. No header in
// the Steamworks SDK git history ever defines it, but the v1.58a release's
// steam_api.json (machine-readable API description, methods in vtable order)
// already describes it while that release's isteamvideo.h still said V002:
// V002's four methods plus the five timeline methods that later moved to
// ISteamTimeline (SDK 1.59). Reconstructed verbatim from that json.
// Vtable order is ABI -- do not reorder or edit method declarations.
#define STEAMEMU_INTER_INTERFACE_VERSION "STEAMVIDEO_INTERFACE_V003"

class ISteamVideo
{
public:
	// Get a URL suitable for streaming the given Video app ID's video
	virtual void GetVideoURL( AppId_t unVideoAppID ) = 0;

	// returns true if user is uploading a live broadcast
	virtual bool IsBroadcasting( int *pnNumViewers ) = 0;

	// Get the OPF Details for 360 Video Playback
	virtual void GetOPFSettings( AppId_t unVideoAppID ) = 0;
	virtual bool GetOPFStringForApp( AppId_t unVideoAppID, char *pchBuffer, int32 *pnBufferSize ) = 0;

	// Timeline decorations (moved to ISteamTimeline in SDK 1.59)
	virtual void AddTimelineHighlightMarker( const char *pchIcon, const char *pchTitle, const char *pchDescription, uint32 unPriority ) = 0;
	virtual void AddTimelineTimestamp( const char *pchTitle ) = 0;
	virtual void AddTimelineRangeStart( const char *pchID, const char *pchTitle ) = 0;
	virtual void AddTimelineRangeEnd( const char *pchID ) = 0;
	virtual void SetTimelineGameMode( ETimelineGameMode eMode ) = 0;
};
