/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//================================================
// CoaXioN Source SDK p2p networking: "CoaXioN Coplay"
// Author : Tholp / Jackson S
//================================================

//Shared defines

// Ok, here the rundown on the concept behind this
// After socket opening and all the handshake stuff:
// The client has a UDP listener that relays all packets -
// through the Steam datagram, the server machine recieves them and -
// sends the UDP packets locally to the game server which has a similar mechanism to send back to the client.
// This allows us to make use of the P2P features steam offers within the Source SDK without any networking code changes
#pragma once
#ifndef COPLAY_H
#define COPLAY_H

#define COPLAY_MSG_COLOR Color(170, 255, 0, 255)
#define COPLAY_DEBUG_MSG_COLOR Color(255, 170, 0, 255)

#define COPLAY_MAX_PACKETS 16

//YYYY-MM-DD-(a-z) if theres multiple in a day
#define COPLAY_VERSION "2024-07-26-a"

//For vpcless quick testing, leave this commented when commiting
//#define COPLAY_USE_LOBBIES
//#undef COPLAY_USE_LOBBIES

#include <cbase.h>
#include "SDL2/SDL_net.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/isteamfriends.h"
#include "steam/steam_api.h"
#ifdef COPLAY_USE_LOBBIES
#include "steam/isteammatchmaking.h"
#endif


#include "tier0/valve_minmax_off.h"	// GCC 4.2.2 headers screw up our min/max defs.
#include <string>
#include "tier0/valve_minmax_on.h"

enum JoinFilter
{
	eP2PFilter_OFF = -1,
	eP2PFilter_CONTROLLED = 0,// requires a password appended to coplay_connect to make a connection
							  // given by the host running coplay_getconnectcommand
							  // passwords are not user settable and are randomized every socket open or
							  // when running coplay_rerandomize_password
	eP2PFilter_FRIENDS = 1,
	eP2PFilter_EVERYONE = 2,
};
#ifndef COPLAY_USE_LOBBIES

#define COPLAY_NETMSG_NEEDPASS "NeedPass"
#define COPLAY_NETMSG_OK "OK"
#endif
enum ConnectionRole
{
	eConnectionRole_UNAVAILABLE = -1,// Waiting on Steam
	eConnectionRole_NOT_CONNECTED = 0,
	eConnectionRole_HOST,
	eConnectionRole_CLIENT
};

enum ConnectionEndReason // see the enum ESteamNetConnectionEnd in steamnetworkingtypes.h
{
	k_ESteamNetConnectionEnd_App_NotOpen = 1001,
	k_ESteamNetConnectionEnd_App_ServerFull,
	k_ESteamNetConnectionEnd_App_RemoteIssue,//couldn't open a socket
	k_ESteamNetConnectionEnd_App_ClosedByPeer,

	// incoming connection rejected
	k_ESteamNetConnectionEnd_App_NotFriend,
	k_ESteamNetConnectionEnd_App_BadPassword,
};

extern ConVar coplay_joinfilter;
extern ConVar coplay_timeoutduration;
extern ConVar coplay_connectionthread_hz;
extern ConVar coplay_portrange_begin;
extern ConVar coplay_portrange_end;
extern ConVar coplay_forceloopback;

extern ConVar coplay_debuglog_socketcreation;
extern ConVar coplay_debuglog_socketspam;
extern ConVar coplay_debuglog_steamconnstatus;
extern ConVar coplay_debuglog_scream;
#ifdef COPLAY_USE_LOBBIES
extern ConVar coplay_debuglog_lobbyupdated;
#endif

static uint32 SwapEndian32(uint32 num)
{
	byte newnum[4];
	newnum[0] = ((byte*)&num)[3];
	newnum[1] = ((byte*)&num)[2];
	newnum[2] = ((byte*)&num)[1];
	newnum[3] = ((byte*)&num)[0];
	return *((uint32*)newnum);
}

static uint16 SwapEndian16(uint16 num)
{
	byte newnum[2];
	newnum[0] = ((byte*)&num)[1];
	newnum[1] = ((byte*)&num)[0];
	return *((uint16*)newnum);
}
#ifdef COPLAY_USE_LOBBIES
static bool IsUserInLobby(CSteamID LobbyID, CSteamID UserID)
{
	uint32 numMembers = SteamMatchmaking()->GetNumLobbyMembers(LobbyID);
	for (uint32 i = 0; i < numMembers; i++)
	{
		if (UserID.ConvertToUint64() == SteamMatchmaking()->GetLobbyMemberByIndex(LobbyID, i).ConvertToUint64())
			return true;
	}
	return false;
}
#endif
//a single SDL/Steam connection pair, clients will only have 0 or 1 of these, one per remote player on the host
class CCoplayConnection : public CThread
{
	int Run();
public:
	CCoplayConnection(HSteamNetConnection hConn);

	bool	  GameReady;// only check for inital messaging for passwords, if needed, a connecting client cant know for sure
	UDPsocket LocalSocket;
	uint16	Port;
	IPaddress SendbackAddress;

	HSteamNetConnection	 SteamConnection;
	float				   TimeStarted;

	void QueueForDeletion(){DeletionQueued = true;}

private:
	bool DeletionQueued;

	float LastPacketTime;//This is for when the steam connection is still being kept alive but there is no actual activity
};

struct PendingConnection// for when we make a steam connection to ask for a password but
						// not letting it send packets to the game server yet
{
	PendingConnection() {
		SteamConnection = 0;
		TimeCreated = 0.0f;
	};

	HSteamNetConnection SteamConnection;
	float			   TimeCreated;
};

class CCoplayConnectionHandler;
extern CCoplayConnectionHandler *g_pCoplayConnectionHandler;

//Handles all the Steam callbacks and connection management
class CCoplayConnectionHandler : public CAutoGameSystemPerFrame
{
public:
	CCoplayConnectionHandler()
	{
		msSleepTime = 3;
		Role = eConnectionRole_UNAVAILABLE;
	}

	virtual bool Init()
	{
		ConColorMsg(COPLAY_MSG_COLOR, "[Coplay] Initialization started...\n");

	#ifdef GAME_DLL //may see if we can support dedicated servers at some point
		if (!engine->IsDedicatedServer())
		{
			Remove(this);
			return true;
		}
	#endif

		if (SDL_Init(0))
		{
			Error("SDL Failed to Initialize: \"%s\"", SDL_GetError());
		}
		if (SDLNet_Init())
		{
			Error("SDLNet Failed to Initialize: \"%s\"", SDLNet_GetError());
		}

		SteamNetworkingUtils()->InitRelayNetworkAccess();


		g_pCoplayConnectionHandler = this;
		return true;
	}

	virtual void Update(float frametime);

	virtual void Shutdown()
	{
		CloseAllConnections(true);
	}

	virtual void PostInit()
	{
		// Some cvars we need on
		ConVarRef net_usesocketsforloopback("net_usesocketsforloopback");// allows connecting to 127.* addresses
		net_usesocketsforloopback.SetValue(true);
#ifndef COPLAY_DONT_SET_THREADMODE
		ConVarRef host_thread_mode("host_thread_mode");// fixes game logic speedup, see the README for the required fix for this
		// FC: don't set to 2 for the moment, there are multiple issues regarding speed up and jiggle physics breaking.
		host_thread_mode.SetValue(0);
#endif
		// When accepting InviteUserToGame(), we need to pass on the launch param
		// May need to move this at some point
		char szCommandLine[256] = "";
		SteamApps()->GetLaunchCommandLine(szCommandLine, sizeof(szCommandLine));
		DevMsg(1, "LaunchCmdLine: '%s'\n", szCommandLine);
		if (V_strncmp(szCommandLine, "+coplay_connect", 15) == 0)
		{
			GameRichPresenceJoinRequested_t *pRequest = new GameRichPresenceJoinRequested_t();
			Q_snprintf(pRequest->m_rgchConnect, sizeof(pRequest->m_rgchConnect), "%s", szCommandLine);
			JoinGame(pRequest);
		}
	}

	virtual void LevelInitPostEntity();
	virtual void LevelShutdownPreEntity();

	void		OpenP2PSocket();
	void		CloseP2PSocket();
	void		CloseAllConnections(bool waitforjoin = false);
	bool		CreateSteamConnectionTuple(HSteamNetConnection hConn);

	int		 GetConnectCommand(std::string &out);// 0: OK, 1: Not Hosting, 2: Use Coplay_invite instead

	ConnectionRole GetRole(){return Role;}
	void		   SetRole(ConnectionRole newrole);
#ifdef COPLAY_USE_LOBBIES
	CSteamID	GetLobby(){return Lobby;}
#else
	std::string		 GetPassword(){return Password;}
	void				RechoosePassword();
#endif

	uint32		 msSleepTime;

private:
	ConnectionRole	  Role;
	HSteamListenSocket  HP2PSocket;
#ifdef COPLAY_USE_LOBBIES
	CSteamID			Lobby;
#else
public:
	std::string				Password;// we use this same variable for a password we need to send if we're the client, or the one we need to check agaisnt if we're the server
	CUtlVector<PendingConnection> PendingConnections; // cant connect to the server but has a steam connection to send a password

#endif
public:
	CUtlVector<CCoplayConnection*> Connections;

	CCallResult<CCoplayConnectionHandler, LobbyMatchList_t> LobbyListResult;
	void OnLobbyListcmd( LobbyMatchList_t *pLobbyMatchList, bool bIOFailure);

private:
	STEAM_CALLBACK(CCoplayConnectionHandler, ConnectionStatusUpdated, SteamNetConnectionStatusChangedCallback_t);
	STEAM_CALLBACK(CCoplayConnectionHandler, JoinGame,				GameRichPresenceJoinRequested_t);
#ifdef COPLAY_USE_LOBBIES
	STEAM_CALLBACK(CCoplayConnectionHandler, LobbyCreated,			LobbyCreated_t);
	STEAM_CALLBACK(CCoplayConnectionHandler, LobbyJoined,			 LobbyEnter_t);
	STEAM_CALLBACK(CCoplayConnectionHandler, LobbyJoinRequested,	  GameLobbyJoinRequested_t);
#endif
};


#endif
