/**
 * =============================================================================
 * CS2Fixes
 * Copyright (C) 2023-2025 Source2ZE
 * =============================================================================
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License, version 3.0, as published by the
 * Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "commands.h"
#include "adminsystem.h"
#include "common.h"
#include "ctimer.h"
#include "detours.h"
#include "discord.h"
#include "engine/igameeventsystem.h"
#include "entity/cbaseentity.h"
#include "entity/cbasemodelentity.h"
#include "entity/ccsplayercontroller.h"
#include "entity/ccsplayerpawn.h"
#include "entity/ccsweaponbase.h"
#include "entity/cparticlesystem.h"
#include "entity/lights.h"
#include "httpmanager.h"
#include "leader.h"
#include "networksystem/inetworkmessages.h"
#include "playermanager.h"
#include "recipientfilters.h"
#include "tier0/vprof.h"
#include "usermessages.pb.h"
#include "utils/entity.h"
#include "utlstring.h"
#include "zombiereborn.h"
#undef snprintf
#include "vendor/nlohmann/json.hpp"

#include "tier0/memdbgon.h"

using json = nlohmann::json;

extern IGameEventSystem* g_gameEventSystem;
extern CGameEntitySystem* g_pEntitySystem;
extern IVEngineServer2* g_pEngineServer2;
extern ISteamHTTP* g_http;
extern CConVar<CUtlString> g_cvarFlashLightAttachment;

CConVar<bool> g_cvarEnableCommands("cs2f_commands_enable", FCVAR_NONE, "Whether to enable chat commands", false);
CConVar<bool> g_cvarEnableAdminCommands("cs2f_admin_commands_enable", FCVAR_NONE, "Whether to enable admin chat commands", false);
CConVar<bool> g_cvarEnableWeapons("cs2f_weapons_enable", FCVAR_NONE, "Whether to enable weapon commands", false);

int GetGrenadeAmmo(CCSPlayer_WeaponServices* pWeaponServices, const WeaponInfo_t* pWeaponInfo)
{
	if (!pWeaponServices || pWeaponInfo->m_eSlot != GEAR_SLOT_GRENADES)
		return -1;

	// TODO: look into molotov vs inc interaction
	if (strcmp(pWeaponInfo->m_pClass, "weapon_hegrenade") == 0)
		return pWeaponServices->m_iAmmo[AMMO_OFFSET_HEGRENADE];
	if (strcmp(pWeaponInfo->m_pClass, "weapon_molotov") == 0 || strcmp(pWeaponInfo->m_pClass, "weapon_incgrenade") == 0)
		return pWeaponServices->m_iAmmo[AMMO_OFFSET_MOLOTOV];
	if (strcmp(pWeaponInfo->m_pClass, "weapon_decoy") == 0)
		return pWeaponServices->m_iAmmo[AMMO_OFFSET_DECOY];
	if (strcmp(pWeaponInfo->m_pClass, "weapon_flashbang") == 0)
		return pWeaponServices->m_iAmmo[AMMO_OFFSET_FLASHBANG];
	if (strcmp(pWeaponInfo->m_pClass, "weapon_smokegrenade") == 0)
		return pWeaponServices->m_iAmmo[AMMO_OFFSET_SMOKEGRENADE];
	return -1;
}

int GetGrenadeAmmoTotal(CCSPlayer_WeaponServices* pWeaponServices)
{
	if (!pWeaponServices)
		return -1;

	int grenadeAmmoOffsets[] = {
		AMMO_OFFSET_HEGRENADE,
		AMMO_OFFSET_FLASHBANG,
		AMMO_OFFSET_SMOKEGRENADE,
		AMMO_OFFSET_DECOY,
		AMMO_OFFSET_MOLOTOV,
	};

	int totalGrenades = 0;
	for (int i = 0; i < (sizeof(grenadeAmmoOffsets) / sizeof(int)); i++)
		totalGrenades += pWeaponServices->m_iAmmo[grenadeAmmoOffsets[i]];

	return totalGrenades;
}

void ParseWeaponCommand(const CCommand& args, CCSPlayerController* player)
{
	if (!g_cvarEnableWeapons.Get() || !player || !player->m_hPawn())
		return;

	VPROF("ParseWeaponCommand");

	const auto pPawn = reinterpret_cast<CCSPlayerPawn*>(player->GetPawn());

	const char* command = args[0];
	if (!V_strncmp("c_", command, 2))
		command = command + 2;

	const auto pWeaponInfo = FindWeaponInfoByAlias(command);

	if (!pWeaponInfo || pWeaponInfo->m_nPrice == 0)
		return;

	if (pPawn->m_iHealth() <= 0 || pPawn->m_iTeamNum != CS_TEAM_CT)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You can only buy weapons when human.");
		return;
	}

	CCSPlayer_ItemServices* pItemServices = pPawn->m_pItemServices;
	CCSPlayer_WeaponServices* pWeaponServices = pPawn->m_pWeaponServices;

	// it can sometimes be null when player joined on the very first round?
	if (!pItemServices || !pWeaponServices)
		return;

	int money = player->m_pInGameMoneyServices->m_iAccount;

	if (money < pWeaponInfo->m_nPrice)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You can't afford %s! It costs $%i, you only have $%i", pWeaponInfo->m_pName, pWeaponInfo->m_nPrice, money);
		return;
	}

	static ConVarRefAbstract ammo_grenade_limit_default("ammo_grenade_limit_default"), ammo_grenade_limit_total("ammo_grenade_limit_total"), mp_weapons_allow_typecount("mp_weapons_allow_typecount");

	int iGrenadeLimitDefault = ammo_grenade_limit_default.GetInt();
	int iGrenadeLimitTotal = ammo_grenade_limit_total.GetInt();
	int iWeaponLimit = mp_weapons_allow_typecount.GetInt();

	if (pWeaponInfo->m_eSlot == GEAR_SLOT_GRENADES)
	{
		int iMatchingGrenades = GetGrenadeAmmo(pWeaponServices, pWeaponInfo);
		int iTotalGrenades = GetGrenadeAmmoTotal(pWeaponServices);

		if (iMatchingGrenades >= iGrenadeLimitDefault)
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You cannot carry any more %ss (Max %i)", pWeaponInfo->m_pName, iGrenadeLimitDefault);
			return;
		}

		if (iTotalGrenades >= iGrenadeLimitTotal)
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You cannot carry any more grenades (Max %i)", iGrenadeLimitTotal);
			return;
		}
	}

	int maxAmount;

	if (pWeaponInfo->m_nMaxAmount)
		maxAmount = pWeaponInfo->m_nMaxAmount;
	else if (pWeaponInfo->m_eSlot == GEAR_SLOT_GRENADES)
		maxAmount = iGrenadeLimitDefault;
	else
		maxAmount = iWeaponLimit == -1 ? 9999 : iWeaponLimit;

	CUtlVector<WeaponPurchaseCount_t>* weaponPurchases = pPawn->m_pActionTrackingServices->m_weaponPurchasesThisRound().m_weaponPurchases;
	bool found = false;
	FOR_EACH_VEC(*weaponPurchases, i)
	{
		WeaponPurchaseCount_t& purchase = (*weaponPurchases)[i];
		if (purchase.m_nItemDefIndex == pWeaponInfo->m_iItemDefinitionIndex)
		{
			// Note ammo_grenade_limit_total is not followed here, only for checking inventory space
			if (purchase.m_nCount >= maxAmount)
			{
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You cannot buy any more %s (Max %i)", pWeaponInfo->m_pName, maxAmount);
				return;
			}
			purchase.m_nCount += 1;
			found = true;
			break;
		}
	}

	if (!found)
	{
		WeaponPurchaseCount_t purchase = {};

		purchase.m_nCount = 1;
		purchase.m_nItemDefIndex = pWeaponInfo->m_iItemDefinitionIndex;

		weaponPurchases->AddToTail(purchase);
	}

	if (pWeaponInfo->m_eSlot == GEAR_SLOT_RIFLE || pWeaponInfo->m_eSlot == GEAR_SLOT_PISTOL)
	{
		CUtlVector<CHandle<CBasePlayerWeapon>>* weapons = pWeaponServices->m_hMyWeapons();

		FOR_EACH_VEC(*weapons, i)
		{
			CBasePlayerWeapon* weapon = (*weapons)[i].Get();

			if (!weapon)
				continue;

			if (weapon->GetWeaponVData()->m_GearSlot() == pWeaponInfo->m_eSlot)
			{
				pWeaponServices->DropWeapon(weapon);
				break;
			}
		}
	}

	CBasePlayerWeapon* pWeapon = pItemServices->GiveNamedItemAws(pWeaponInfo->m_pClass);

	// Normally shouldn't be possible, but avoid issues in some edge cases
	if (!pWeapon)
		return;

	player->m_pInGameMoneyServices->m_iAccount = money - pWeaponInfo->m_nPrice;

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have purchased %s for $%i", pWeaponInfo->m_pName, pWeaponInfo->m_nPrice);
}

void WeaponCommandCallback(const CCommandContext& context, const CCommand& args)
{
	CCSPlayerController* pController = nullptr;
	if (context.GetPlayerSlot().Get() != -1)
		pController = (CCSPlayerController*)g_pEntitySystem->GetEntityInstance((CEntityIndex)(context.GetPlayerSlot().Get() + 1));

	// Only allow connected players to run chat commands
	if (pController && !pController->IsConnected())
		return;

	ParseWeaponCommand(args, pController);
}

void RegisterWeaponCommands()
{
	const auto& weapons = GenerateWeaponCommands();

	for (const auto& aliases : weapons | std::views::values)
	{
		for (const auto& alias : aliases)
		{
			new CChatCommand(alias.c_str(), ParseWeaponCommand, "- Buys this weapon", ADMFLAG_NONE, CMDFLAG_NOHELP);

			char cmdName[64];
			V_snprintf(cmdName, sizeof(cmdName), "%s%s", COMMAND_PREFIX, alias.c_str());

			new ConCommand(cmdName, WeaponCommandCallback, "Buys this weapon", FCVAR_RELEASE | FCVAR_CLIENT_CAN_EXECUTE | FCVAR_LINKED_CONCOMMAND);
		}
	}
}

void ParseChatCommand(const char* pMessage, CCSPlayerController* pController)
{
	if (!pController || !pController->IsConnected())
		return;

	VPROF("ParseChatCommand");

	CCommand args;
	args.Tokenize(pMessage);
	std::string name = args[0];

	for (int i = 0; name[i]; i++)
		name[i] = tolower(name[i]);

	uint16 index = g_CommandList.Find(hash_32_fnv1a_const(name.c_str()));

	if (g_CommandList.IsValidIndex(index))
		(*g_CommandList[index])(args, pController);
}

bool CChatCommand::CheckCommandAccess(CCSPlayerController* pPlayer, uint64 flags)
{
	if (!pPlayer)
		return false;

	int slot = pPlayer->GetPlayerSlot();

	ZEPlayer* pZEPlayer = g_playerManager->GetPlayer(slot);

	if (!pZEPlayer)
		return false;

	if ((flags & FLAG_LEADER) == FLAG_LEADER)
	{
		if (!g_cvarEnableLeader.Get())
			return false;
		if (!pZEPlayer->IsAdminFlagSet(FLAG_LEADER))
		{
			if (!pZEPlayer->IsLeader())
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, CHAT_PREFIX "You must be a leader to use this command.");
				return false;
			}
			else if (g_cvarLeaderActionsHumanOnly.Get() && pPlayer->m_iTeamNum != CS_TEAM_CT)
			{
				ClientPrint(pPlayer, HUD_PRINTTALK, CHAT_PREFIX "You must be a human to use this command.");
				return false;
			}
		}
	}
	else if (!pZEPlayer->IsAdminFlagSet(flags))
	{
		ClientPrint(pPlayer, HUD_PRINTTALK, CHAT_PREFIX "You don't have access to this command.");
		return false;
	}

	return true;
}

void ClientPrintAll(int hud_dest, const char* msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[256];
	V_vsnprintf(buf, sizeof(buf), msg, args);

	va_end(args);

	INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	auto data = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();

	data->set_dest(hud_dest);
	data->add_param(buf);

	CRecipientFilter filter;
	filter.AddAllPlayers();

	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, data, 0);

	delete data;

	ConMsg("%s\n", buf);
}

void ClientPrint(CCSPlayerController* player, int hud_dest, const char* msg, ...)
{
	va_list args;
	va_start(args, msg);

	char buf[256];
	V_vsnprintf(buf, sizeof(buf), msg, args);

	va_end(args);

	if (!player || !player->IsConnected() || player->IsBot())
	{
		ConMsg("%s\n", buf);
		return;
	}

	INetworkMessageInternal* pNetMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	auto data = pNetMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();

	data->set_dest(hud_dest);
	data->add_param(buf);

	CSingleRecipientFilter filter(player->GetPlayerSlot());

	g_gameEventSystem->PostEventAbstract(-1, false, &filter, pNetMsg, data, 0);

	delete data;
}

CConVar<bool> g_cvarEnableStopSound("cs2f_stopsound_enable", FCVAR_NONE, "Whether to enable stopsound", false);

CON_COMMAND_CHAT(stopsound, "- Toggle weapon sounds")
{
	if (!g_cvarEnableStopSound.Get())
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	int iPlayer = player->GetPlayerSlot();
	bool bStopSet = g_playerManager->IsPlayerUsingStopSound(iPlayer);
	bool bSilencedSet = g_playerManager->IsPlayerUsingSilenceSound(iPlayer);

	g_playerManager->SetPlayerStopSound(iPlayer, bSilencedSet);
	g_playerManager->SetPlayerSilenceSound(iPlayer, !bSilencedSet && !bStopSet);

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have %s weapon sounds.", bSilencedSet ? "disabled" : !bSilencedSet && !bStopSet ? "silenced" :
																																		  "enabled");
}

CON_COMMAND_CHAT(toggledecals, "- Toggle world decals, if you're into having 10 fps in ZE")
{
	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	int iPlayer = player->GetPlayerSlot();
	bool bSet = !g_playerManager->IsPlayerUsingStopDecals(iPlayer);

	g_playerManager->SetPlayerStopDecals(iPlayer, bSet);

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have %s world decals.", bSet ? "disabled" : "enabled");
}

CConVar<bool> g_cvarEnableNoShake("cs2f_noshake_enable", FCVAR_NONE, "Whether to enable noshake command", false);
CConVar<float> g_cvarMaxShakeAmp("cs2f_maximum_shake_amplitude", FCVAR_NONE, "Shaking Amplitude bigger than this will be clamped", -1.0f, true, -1.0f, true, 16.0f);
CON_COMMAND_CHAT(noshake, "- toggle noshake")
{
	if (!g_cvarEnableNoShake.Get())
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	int iPlayer = player->GetPlayerSlot();
	bool bSet = !g_playerManager->IsPlayerUsingNoShake(iPlayer);

	g_playerManager->SetPlayerNoShake(iPlayer, bSet);
	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have %s noshake.", bSet ? "enabled" : "disabled");
}

CConVar<bool> g_cvarEnableHide("cs2f_hide_enable", FCVAR_NONE, "Whether to enable hide (WARNING: randomly crashes clients since 2023-12-13 CS2 update)", false);
CConVar<int> g_cvarDefaultHideDistance("cs2f_hide_distance_default", FCVAR_NONE, "The default distance for hide", 250, true, 0, false, 0);
CConVar<int> g_cvarMaxHideDistance("cs2f_hide_distance_max", FCVAR_NONE, "The max distance for hide", 2000, true, 0, false, 0);

CON_COMMAND_CHAT(hide, "<distance> - Hide nearby players")
{
	// Silently return so the command is completely hidden
	if (!g_cvarEnableHide.Get())
		return;

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	int distance;

	if (args.ArgC() < 2)
		distance = g_cvarDefaultHideDistance.Get();
	else
		distance = V_StringToInt32(args[1], -1);

	if (distance > g_cvarMaxHideDistance.Get() || distance < 0)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You can only hide players between 0 and %i units away.", g_cvarMaxHideDistance.Get());
		return;
	}

	ZEPlayer* pZEPlayer = player->GetZEPlayer();

	// Something has to really go wrong for this to happen
	if (!pZEPlayer)
	{
		Warning("%s Tried to access a null ZEPlayer!!\n", player->GetPlayerName());
		return;
	}

	// allows for toggling hide by turning off when hide distance matches.
	if (pZEPlayer->GetHideDistance() == distance)
		distance = 0;

	pZEPlayer->SetHideDistance(distance);

	if (distance == 0)
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Hiding players is now disabled.");
	else
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Now hiding players within %i units.", distance);
}

void PrintHelp(const CCommand& args, CCSPlayerController* player)
{
	std::vector<std::string> rgstrCommands;
	if (args.ArgC() < 2)
	{
		if (!player)
		{
			ClientPrint(player, HUD_PRINTCONSOLE, "The list of all commands is:");

			FOR_EACH_VEC(g_CommandList, i)
			{
				CChatCommand* cmd = g_CommandList[i];

				if (!cmd->IsCommandFlagSet(CMDFLAG_NOHELP))
					rgstrCommands.push_back(std::string("c_") + cmd->GetName() + " " + cmd->GetDescription());
			}
		}
		else
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "The list of all available commands will be shown in console.");
			ClientPrint(player, HUD_PRINTCONSOLE, "The list of all commands you can use is:");

			ZEPlayer* pZEPlayer = player->GetZEPlayer();

			FOR_EACH_VEC(g_CommandList, i)
			{
				CChatCommand* cmd = g_CommandList[i];
				uint64 flags = cmd->GetAdminFlags();

				if ((pZEPlayer->IsAdminFlagSet(flags) || ((flags & FLAG_LEADER) == FLAG_LEADER && pZEPlayer->IsLeader()))
					&& !cmd->IsCommandFlagSet(CMDFLAG_NOHELP))
					rgstrCommands.push_back(std::string("!") + cmd->GetName() + " " + cmd->GetDescription());
			}
		}
	}
	else
	{
		const char* pszSearchTerm = args[1];
		bool bOnlyCheckStart = false;

		// If a user's search starts with a prefix needed to actually use a command,
		// assume that they only want to see commands starting with that substring
		if (V_strnicmp(args[1], "c_", 2) == 0)
		{
			bOnlyCheckStart = true;
			pszSearchTerm++;
			pszSearchTerm++;
		}
		else if (V_strnicmp(args[1], "!", 1) == 0 || V_strnicmp(args[1], "/", 1) == 0)
		{
			bOnlyCheckStart = true;
			pszSearchTerm++;
		}

		if (!player)
		{
			FOR_EACH_VEC(g_CommandList, i)
			{
				CChatCommand* cmd = g_CommandList[i];

				if (!cmd->IsCommandFlagSet(CMDFLAG_NOHELP)
					&& ((!bOnlyCheckStart && V_stristr(cmd->GetName(), pszSearchTerm))
						|| (bOnlyCheckStart && V_strnicmp(cmd->GetName(), pszSearchTerm, strlen(pszSearchTerm)) == 0)))
					rgstrCommands.push_back(std::string("c_") + cmd->GetName() + " " + cmd->GetDescription());
			}
		}
		else
		{
			ZEPlayer* pZEPlayer = player->GetZEPlayer();

			FOR_EACH_VEC(g_CommandList, i)
			{
				CChatCommand* cmd = g_CommandList[i];
				uint64 flags = cmd->GetAdminFlags();

				if ((pZEPlayer->IsAdminFlagSet(flags) || ((flags & FLAG_LEADER) == FLAG_LEADER && pZEPlayer->IsLeader()))
					&& !cmd->IsCommandFlagSet(CMDFLAG_NOHELP)
					&& ((!bOnlyCheckStart && V_stristr(cmd->GetName(), pszSearchTerm))
						|| (bOnlyCheckStart && V_strnicmp(cmd->GetName(), pszSearchTerm, strlen(pszSearchTerm)) == 0)))
					rgstrCommands.push_back(std::string("!") + cmd->GetName() + " " + cmd->GetDescription());
			}
		}

		if (rgstrCommands.size() == 0)
		{
			ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "No commands matched \"%s\".", args[1]);
			if (player)
				ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "No commands matched \"%s\".", args[1]);
			return;
		}

		ClientPrint(player, HUD_PRINTCONSOLE, "The list of all commands matching \"%s\" is:", args[1]);
		if (player)
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "The list of all commands matching \"%s\" will be shown in console.", args[1]);
	}

	std::sort(rgstrCommands.begin(), rgstrCommands.end());

	for (const auto& strCommand : rgstrCommands)
		ClientPrint(player, HUD_PRINTCONSOLE, strCommand.c_str());

	if (player)
		ClientPrint(player, HUD_PRINTCONSOLE, "! can be replaced with / for a silent chat command, or c_ for console usage");
}

CON_COMMAND_CHAT(help, "- Display list of commands in console")
{
	PrintHelp(args, player);
}

CON_COMMAND_CHAT(find, "<text> - Search for specific commands and list them in console")
{
	PrintHelp(args, player);
}

CON_COMMAND_CHAT(spec, "[name] - Spectate another player or join spectators")
{
	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)player->GetPawn();

	if (!player)
	{
		ClientPrint(player, HUD_PRINTCONSOLE, CHAT_PREFIX "You cannot use this command from the server console.");
		return;
	}

	if (args.ArgC() < 2)
	{
		if (player->m_iTeamNum() == CS_TEAM_SPECTATOR)
		{
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Already spectating.");
		}
		else
		{
			if (pPawn && pPawn->IsAlive())
				pPawn->CommitSuicide(false, true);

			player->SwitchTeam(CS_TEAM_SPECTATOR);
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Moved to spectators.");
		}
		return;
	}

	int iCommandPlayer = player ? player->GetPlayerSlot() : -1;
	int iNumClients = 0;
	int pSlot[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlot, NO_MULTIPLE | NO_SELF | NO_DEAD | NO_SPECTATOR | NO_IMMUNITY))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlot[0]);

	if (!pTarget)
		return;

	if (player->m_iTeamNum() != CS_TEAM_SPECTATOR)
	{
		if (pPawn && pPawn->IsAlive())
			pPawn->CommitSuicide(false, true);

		player->SwitchTeam(CS_TEAM_SPECTATOR);
	}

	// 1 frame delay as observer services will be null on same frame as spectator team switch
	CHandle<CCSPlayerController> hPlayer = player->GetHandle();
	CHandle<CCSPlayerController> hTarget = pTarget->GetHandle();
	new CTimer(0.0f, false, false, [hPlayer, hTarget]() {
		CCSPlayerController* pPlayer = hPlayer.Get();
		CCSPlayerController* pTargetPlayer = hTarget.Get();
		if (!pPlayer || !pTargetPlayer)
			return -1.0f;
		CPlayer_ObserverServices* pObserverServices = pPlayer->GetPawn()->m_pObserverServices();
		if (!pObserverServices)
			return -1.0f;
		pObserverServices->m_iObserverMode.Set(OBS_MODE_IN_EYE);
		pObserverServices->m_iObserverLastMode.Set(OBS_MODE_ROAMING);
		pObserverServices->m_hObserverTarget.Set(pTargetPlayer->GetPawn());
		ClientPrint(pPlayer, HUD_PRINTTALK, CHAT_PREFIX "Spectating player %s.", pTargetPlayer->GetPlayerName());
		return -1.0f;
	});
}

CON_COMMAND_CHAT(getpos, "- Get your position and angles")
{
	if (!player)
		return;

	Vector vecAbsOrigin = player->GetPawn()->GetAbsOrigin();
	QAngle angRotation = player->GetPawn()->GetAbsRotation();

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "setpos %f %f %f;setang %f %f %f", vecAbsOrigin.x, vecAbsOrigin.y, vecAbsOrigin.z, angRotation.x, angRotation.y, angRotation.z);
	ClientPrint(player, HUD_PRINTCONSOLE, "setpos %f %f %f;setang %f %f %f", vecAbsOrigin.x, vecAbsOrigin.y, vecAbsOrigin.z, angRotation.x, angRotation.y, angRotation.z);
}

CON_COMMAND_CHAT(info, "<name> - Get a player's information")
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !info <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];
	ETargetType nType;

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_BOT | NO_IMMUNITY, nType))
		return;

	ZEPlayer* pPlayer = player ? player->GetZEPlayer() : nullptr;
	bool bIsAdmin = pPlayer ? pPlayer->IsAdminFlagSet(ADMFLAG_RCON) : true;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);
		ZEPlayer* zpTarget = pTarget->GetZEPlayer();

		ClientPrint(player, HUD_PRINTCONSOLE, "%s", pTarget->GetPlayerName());
		ClientPrint(player, HUD_PRINTCONSOLE, "\tUser ID: %i", g_pEngineServer2->GetPlayerUserId(pTarget->GetPlayerSlot()).Get());

		if (zpTarget->IsAuthenticated())
			ClientPrint(player, HUD_PRINTCONSOLE, "\tSteam64 ID: %llu", zpTarget->GetSteamId64());
		else
			ClientPrint(player, HUD_PRINTCONSOLE, "\tSteam64 ID: %llu (Unauthenticated)", zpTarget->GetUnauthenticatedSteamId());

		if (bIsAdmin)
			ClientPrint(player, HUD_PRINTCONSOLE, "\tIP Address: %s", zpTarget->GetIpAddress());
	}

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Printed matching player%s information to console.", (iNumClients == 1) ? "'s" : "s'");
}

CON_COMMAND_CHAT(showteam, "<name> - Get a player's current team")
{
	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !showteam <name>");
		return;
	}

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_MULTIPLE | NO_IMMUNITY))
		return;

	CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[0]);

	switch (pTarget->m_iTeamNum())
	{
		case CS_TEAM_SPECTATOR:
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is a\x08 spectator\x01.", pTarget->GetPlayerName());
			break;
		case CS_TEAM_T:
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is a\x09 terrorist\x01.", pTarget->GetPlayerName());
			break;
		case CS_TEAM_CT:
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is a\x0B counter-terrorist\x01.", pTarget->GetPlayerName());
			break;
		default:
			ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "%s is not on a team.", pTarget->GetPlayerName());
	}
}

#if _DEBUG
CON_COMMAND_CHAT(myuid, "- Test")
{
	if (!player)
		return;

	int iPlayer = player->GetPlayerSlot();

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Your userid is %i, slot: %i, retrieved slot: %i", g_pEngineServer2->GetPlayerUserId(iPlayer).Get(), iPlayer, g_playerManager->GetSlotFromUserId(g_pEngineServer2->GetPlayerUserId(iPlayer).Get()));
}

CON_COMMAND_CHAT(myhandle, "- Test")
{
	if (!player)
		return;

	int entry = player->GetHandle().GetEntryIndex();
	int serial = player->GetHandle().GetSerialNumber();

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "entry index: %d    serial number: %d", entry, serial);
}

CON_COMMAND_CHAT(fl, "- Flashlight")
{
	if (!player)
		return;

	CCSPlayerPawn* pPawn = (CCSPlayerPawn*)player->GetPawn();

	auto ptr = pPawn->m_pMovementServices->m_nButtons().m_pButtonStates();

	Vector origin = pPawn->GetAbsOrigin();
	Vector forward;
	AngleVectors(pPawn->m_angEyeAngles(), &forward);

	origin.z += 64.0f;
	origin += forward * 54.0f; // The minimum distance such that an awp wouldn't block the light

	CBarnLight* pLight = CreateEntityByName<CBarnLight>("light_barn");

	pLight->m_bEnabled = true;
	pLight->m_Color->SetColor(255, 255, 255, 255);
	pLight->m_flBrightness = 1.0f;
	pLight->m_flRange = 2048.0f;
	pLight->m_flSoftX = 1.0f;
	pLight->m_flSoftY = 1.0f;
	pLight->m_flSkirt = 0.5f;
	pLight->m_flSkirtNear = 1.0f;
	pLight->m_vSizeParams->Init(45.0f, 45.0f, 0.03f);
	pLight->m_nCastShadows = 1;
	pLight->m_nDirectLight = 3;
	pLight->Teleport(&origin, &pPawn->m_angEyeAngles(), nullptr);

	// Have to use keyvalues for this since the schema prop is a resource handle
	CEntityKeyValues* pKeyValues = new CEntityKeyValues();
	pKeyValues->SetString("lightcookie", "materials/effects/lightcookies/flashlight.vtex");

	pLight->DispatchSpawn(pKeyValues);

	pLight->SetParent(pPawn);
	pLight->AcceptInput("SetParentAttachmentMaintainOffset", g_cvarFlashLightAttachment.Get().String());
}

CON_COMMAND_CHAT(say, "<message> - Say something using console")
{
	ClientPrintAll(HUD_PRINTTALK, "%s", args.ArgS());
}

CON_COMMAND_CHAT(test_target, "<name> [blocked flag] [...] - Test string targetting")
{
	if (!player)
		return;

	if (args.ArgC() < 2)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !test_target <name> [blocked flag] [...]");
		return;
	}

	uint64 iBlockedFlags = NO_TARGET_BLOCKS;
	for (int i = 1; i < args.ArgC(); i++)
		if (!V_stricmp(args[i], "NO_RANDOM"))
			iBlockedFlags |= NO_RANDOM;
		else if (!V_stricmp(args[i], "NO_MULTIPLE"))
			iBlockedFlags |= NO_MULTIPLE;
		else if (!V_stricmp(args[i], "NO_SELF"))
			iBlockedFlags |= NO_SELF;
		else if (!V_stricmp(args[i], "NO_BOT"))
			iBlockedFlags |= NO_BOT;
		else if (!V_stricmp(args[i], "NO_HUMAN"))
			iBlockedFlags |= NO_HUMAN;
		else if (!V_stricmp(args[i], "NO_UNAUTHENTICATED"))
			iBlockedFlags |= NO_UNAUTHENTICATED;
		else if (!V_stricmp(args[i], "NO_DEAD"))
			iBlockedFlags |= NO_DEAD;
		else if (!V_stricmp(args[i], "NO_ALIVE"))
			iBlockedFlags |= NO_ALIVE;
		else if (!V_stricmp(args[i], "NO_TERRORIST"))
			iBlockedFlags |= NO_TERRORIST;
		else if (!V_stricmp(args[i], "NO_COUNTER_TERRORIST"))
			iBlockedFlags |= NO_COUNTER_TERRORIST;
		else if (!V_stricmp(args[i], "NO_SPECTATOR"))
			iBlockedFlags |= NO_SPECTATOR;
		else if (!V_stricmp(args[i], "NO_IMMUNITY"))
			iBlockedFlags |= NO_IMMUNITY;

	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, iBlockedFlags))
		return;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Targeting %s", pTarget->GetPlayerName());
		Message("Targeting %s\n", pTarget->GetPlayerName());
	}
}

CON_COMMAND_CHAT(particle, "- Spawn a particle")
{
	if (!player)
		return;

	Vector vecAbsOrigin = player->GetPawn()->GetAbsOrigin();
	vecAbsOrigin.z += 64.0f;

	CParticleSystem* particle = CreateEntityByName<CParticleSystem>("info_particle_system");

	particle->m_bStartActive(true);
	particle->m_iszEffectName(args[1]);
	particle->Teleport(&vecAbsOrigin, nullptr, nullptr);

	particle->DispatchSpawn();

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have spawned a particle with effect name: %s", particle->m_iszEffectName().String());
	Message("You have spawned a particle with effect name: %s\n", particle->m_iszEffectName().String());
}

CON_COMMAND_CHAT(particle_kv, "- Spawn a particle but using keyvalues to spawn")
{
	if (!player)
		return;

	Vector vecAbsOrigin = player->GetPawn()->GetAbsOrigin();
	vecAbsOrigin.z += 64.0f;

	CParticleSystem* particle = CreateEntityByName<CParticleSystem>("info_particle_system");

	CEntityKeyValues* pKeyValues = new CEntityKeyValues();

	pKeyValues->SetString("effect_name", args[1]);
	pKeyValues->SetBool("start_active", true);
	pKeyValues->SetVector("origin", vecAbsOrigin);

	particle->DispatchSpawn(pKeyValues);

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have spawned a particle using keyvalues with effect name: %s", particle->m_iszEffectName().String());
	Message("You have spawned a particle using keyvalues with effect name: %s\n", particle->m_iszEffectName().String());
}

CON_COMMAND_CHAT(dispatch_particle, "- Test")
{
	if (!player)
		return;

	CRecipientFilter filter;
	filter.AddAllPlayers();

	player->GetPawn()->DispatchParticle(args[1], &filter);
}

CON_COMMAND_CHAT(emitsound, "- Emit a sound from the entity under crosshair")
{
	if (!player)
		return;

	CBaseEntity* pEntity = UTIL_FindPickerEntity(player);

	if (!pEntity)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "No entity found");
		return;
	}

	pEntity->EmitSound(args[1]);

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Playing %s on %s", args[1], pEntity->GetClassname());
	Message("Playing %s on %s", args[1], pEntity->GetClassname());
}

CON_COMMAND_CHAT(getstats, "- Get your stats")
{
	if (!player)
		return;

	CSMatchStats_t* stats = &player->m_pActionTrackingServices->m_matchStats();

	ClientPrint(player, HUD_PRINTCENTER,
				"Kills: %i\n"
				"Deaths: %i\n"
				"Assists: %i\n"
				"Damage: %i",
				stats->m_iKills.Get(), stats->m_iDeaths.Get(), stats->m_iAssists.Get(), stats->m_iDamage.Get());

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Kills: %d", stats->m_iKills.Get());
	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Deaths: %d", stats->m_iDeaths.Get());
	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Assists: %d", stats->m_iAssists.Get());
	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Damage: %d", stats->m_iDamage.Get());
}

CON_COMMAND_CHAT(setkills, "- Set your kills")
{
	if (!player)
		return;

	player->m_pActionTrackingServices->m_matchStats().m_iKills = atoi(args[1]);

	ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "You have set your kills to %d.", atoi(args[1]));
}

CON_COMMAND_CHAT(setcollisiongroup, "<group> - Set a player's collision group")
{
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TARGET_BLOCKS))
		return;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);
		uint8 group = atoi(args[2]);
		uint8 oldgroup = pTarget->m_hPawn->m_pCollision->m_CollisionGroup;

		pTarget->m_hPawn->m_pCollision->m_CollisionGroup = group;
		pTarget->m_hPawn->m_pCollision->m_collisionAttribute().m_nCollisionGroup = group;
		pTarget->GetPawn()->CollisionRulesChanged();

		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Setting collision group on %s from %d to %d.", pTarget->GetPlayerName(), oldgroup, group);
	}
}

CON_COMMAND_CHAT(setsolidtype, "<solidtype> - Set a player's solid type")
{
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TARGET_BLOCKS))
		return;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		uint8 type = atoi(args[2]);
		uint8 oldtype = pTarget->m_hPawn->m_pCollision->m_nSolidType;

		pTarget->m_hPawn->m_pCollision->m_nSolidType = (SolidType_t)type;
		pTarget->GetPawn()->CollisionRulesChanged();

		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Setting solid type on %s from %d to %d.", pTarget->GetPlayerName(), oldtype, type);
	}
}

CON_COMMAND_CHAT(setinteraction, "<flags> - Set a player's interaction flags")
{
	int iNumClients = 0;
	int pSlots[MAXPLAYERS];

	if (!g_playerManager->CanTargetPlayers(player, args[1], iNumClients, pSlots, NO_TARGET_BLOCKS))
		return;

	for (int i = 0; i < iNumClients; i++)
	{
		CCSPlayerController* pTarget = CCSPlayerController::FromSlot(pSlots[i]);

		uint64 oldInteractAs = pTarget->m_hPawn->m_pCollision->m_collisionAttribute().m_nInteractsAs;
		uint64 newInteract = oldInteractAs | ((uint64)1 << 53);

		pTarget->m_hPawn->m_pCollision->m_collisionAttribute().m_nInteractsAs = newInteract;
		pTarget->m_hPawn->m_pCollision->m_collisionAttribute().m_nInteractsExclude = newInteract;
		pTarget->GetPawn()->CollisionRulesChanged();

		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Setting interaction flags on %s from %llx to %llx.", pTarget->GetPlayerName(), oldInteractAs, newInteract);
	}
}

void HttpCallbackSuccess(HTTPRequestHandle request, json response)
{
	ClientPrintAll(HUD_PRINTTALK, response.dump().c_str());
}

void HttpCallbackError(HTTPRequestHandle request, EHTTPStatusCode statusCode, json response)
{
	ClientPrintAll(HUD_PRINTTALK, response.dump().c_str());
}

CON_COMMAND_CHAT(http, "<get/post/patch/put/delete> <url> [content] - Test an HTTP request")
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !http <get/post> <url> [content]");
		return;
	}

	if (!V_strcmp(args[1], "get"))
		g_HTTPManager.Get(args[2], &HttpCallbackSuccess, &HttpCallbackError);
	else if (!V_strcmp(args[1], "post"))
		g_HTTPManager.Post(args[2], args[3], &HttpCallbackSuccess, &HttpCallbackError);
	else if (!V_strcmp(args[1], "patch"))
		g_HTTPManager.Patch(args[2], args[3], &HttpCallbackSuccess, &HttpCallbackError);
	else if (!V_strcmp(args[1], "put"))
		g_HTTPManager.Put(args[2], args[3], &HttpCallbackSuccess, &HttpCallbackError);
	else if (!V_strcmp(args[1], "delete"))
		g_HTTPManager.Delete(args[2], args[3], &HttpCallbackSuccess, &HttpCallbackError);
}

CON_COMMAND_CHAT(discordbot, "<bot> <message> - Send a message to a discord webhook")
{
	if (args.ArgC() < 3)
	{
		ClientPrint(player, HUD_PRINTTALK, CHAT_PREFIX "Usage: !discord <bot> <message>");
		return;
	}

	g_pDiscordBotManager->PostDiscordMessage(args[1], args[2]);
}
#endif // _DEBUG
