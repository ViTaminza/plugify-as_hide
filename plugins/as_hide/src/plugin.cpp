// as_hide — Plugify port of the Pisex cs2-admin_system module "Hide"
// (ref/pisex/cs2-admin_system_modules/Hide/as_hide.cpp).
//
// Behaviour kept identical:
//   * "mm_hide" (console, the original's RegCommand flags: FCVAR_LINKED_CONCOMMAND |
//     FCVAR_SERVER_CAN_EXECUTE | FCVAR_CLIENT_CAN_EXECUTE) and "!hide" (chat) toggle the hide mode
//     for the caller; both need the @admin/hide permission.
//   * Turning it on: sv_disable_teamselect_menu 1, then 0.1 s later (once the controller/pawn
//     exist) SendAction("hide_on"), suicide when alive, ChangeTeam 1 (spectator) then 0 (none),
//     0.1 s later sv_disable_teamselect_menu 0.
//   * Turning it off (or using "jointeam" while hidden): SendAction("hide_off"), ChangeTeam 1 and
//     the translated "Hide_Off"; jointeam is *not* blocked (the engine keeps processing it, like
//     the original's `return false`).
//   * The hide flag of a slot is reset as soon as a different player uses the slot
//     (OnClientAuthorized in the original).
//
// Port notes:
//   * engine->ServerCommand() -> s2sdk::ServerCommand(); the client commands go through
//     pisexcompat's g_pPlayers (ChangeTeam/CommitSuicide), exactly like the original's IPlayersApi.
//   * The original created the follow-up 0.1 s timer *inside* the first timer callback. The
//     TimerSystem lock is recursive (no deadlock), but the creation is deferred with
//     pisex::NextFrame() so no timer is added while the timer loop runs the callback;
//     pisex::Poll() creates it on the next frame. The 0.1 s delay is kept.
//   * s2sdk's OnClientAuthenticated does not fire for every client on a live server, so the slot
//     flags are additionally reconciled once per second (pisex::GetSteamID64 per slot); a slot
//     whose SteamID64 changed is treated exactly like a fresh OnClientAuthorized.
//   * No client is queried before a map exists: everything runs from command/timer callbacks (a
//     map is running by definition) or from the reconcile inside OnPluginUpdate.

#include <admin_api.hpp>
#include <pisex/pisex.hpp>
#include <plg/plugin.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>

namespace {

admin_system::AdminApi* g_pAdminApi = &admin_system::AdminApi::Get();

// CS2 has 64 player slots.
constexpr int kMaxSlots = 64;
constexpr auto kReconcileInterval = std::chrono::seconds(1);

constexpr const char* kHideCommand = "mm_hide";

// Pisex's RegCommand created "mm_*" names as client-executable commands:
// FCVAR_LINKED_CONCOMMAND | FCVAR_SERVER_CAN_EXECUTE | FCVAR_CLIENT_CAN_EXECUTE.
constexpr s2sdk::ConVarFlag kHideCommandFlags =
        static_cast<s2sdk::ConVarFlag>(static_cast<int64_t>(s2sdk::ConVarFlag::LinkedConcommand) |
                                       static_cast<int64_t>(s2sdk::ConVarFlag::ServerCanExecute) |
                                       static_cast<int64_t>(s2sdk::ConVarFlag::ClientCanExecute));

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

bool ValidSlot(int iSlot) {
	return iSlot >= 0 && iSlot < kMaxSlots;
}

void ServerCommand(const char* command) {
	if (!__s2sdk_ServerCommand) {
		return;
	}
	try {
		s2sdk::ServerCommand(plg::string(command));
	} catch (...) {
	}
}

class HidePlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// OnClientAuthorized of the original (pisexcompat hook over s2sdk OnClientAuthenticated).
	void OnClientAuthorized(int iSlot);
	// Once per second while a map is running: see "Port notes" above.
	void ReconcileSlots();

	// Command bodies.
	s2sdk::ResultType HandleHideCommand(int32_t caller);
	bool HandleJoinTeam(int iSlot);

private:
	void ToggleHide(int iSlot);

	std::array<bool, kMaxSlots> m_hidden{};
	std::array<uint64_t, kMaxSlots> m_steamids{};

	bool m_hideCommandRegistered = false;
	std::chrono::milliseconds m_sinceReconcile{0};
};

HidePlugin g_hidePlugin;

// s2sdk console-command trampoline ("mm_hide").
s2sdk::ResultType OnHideCommand(int32_t caller, s2sdk::ConCommandContext context,
								const plg::vector<plg::string>& args) {
	(void)context;
	(void)args;
	try {
		return g_hidePlugin.HandleHideCommand(caller);
	} catch (const std::exception& ex) {
		LogError(std::string("as_hide: exception in 'mm_hide': ") + ex.what());
	} catch (...) {
		LogError("as_hide: unknown exception in 'mm_hide'");
	}
	return s2sdk::ResultType::Handled;
}

plg::PluginResult HidePlugin::OnPluginStart() {
	try {
		// Never fatal: only logs when the s2sdk bindings are missing.
		pisex::Init("as_hide");

		// The original's HookOnClientAuthorized: resets a slot's hide flag when a player
		// authenticates there. The same reset also happens in ReconcileSlots() below.
		pisex::g_pPlayers->HookOnClientAuthorized([](int iSlot, uint64_t /*steamid*/) {
			g_hidePlugin.OnClientAuthorized(iSlot);
		});

		// "mm_hide": registered ourselves because the Pisex flags (client-executable) matter;
		// pisexcompat's RegCommand sets ClientCanExecute|Release, not the
		// LinkedConcommand|ServerCanExecute this command needs.
		if (__s2sdk_AddConsoleCommand) {
			m_hideCommandRegistered =
					s2sdk::AddConsoleCommand(kHideCommand, "Toggles the admin hide mode",
											 kHideCommandFlags, &OnHideCommand, s2sdk::HookMode::Pre);
		}
		if (!m_hideCommandRegistered) {
			LogError("as_hide: registering the 'mm_hide' console command failed (continuing without it)");
		}

		// "!hide" (chat) and the "jointeam" hook, exactly as in the original's RegCommand calls.
		pisex::g_pUtils->RegCommand({}, {"!hide"}, [](int iSlot, const char* /*content*/) -> bool {
			try {
				return g_hidePlugin.HandleHideCommand(iSlot) == s2sdk::ResultType::Handled;
			} catch (...) {
				return true;
			}
		});
		pisex::g_pUtils->RegCommand({"jointeam"}, {}, [](int iSlot, const char* /*content*/) -> bool {
			try {
				return g_hidePlugin.HandleJoinTeam(iSlot);
			} catch (...) {
				return false;
			}
		});

		LogInfo("as_hide: ready; 'mm_hide' and '!hide' registered");
	} catch (const std::exception& ex) {
		LogError(std::string("as_hide: OnPluginStart failed: ") + ex.what());
	} catch (...) {
		LogError("as_hide: OnPluginStart failed with an unknown exception");
	}
	return {};
}

plg::PluginResult HidePlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	// The plugify update callback must not let an exception through.
	try {
		pisex::Poll(); // timers, deferred work, "a map is running"
		if (!pisex::IsGameReady()) {
			return {};
		}
		m_sinceReconcile += dt;
		if (m_sinceReconcile < kReconcileInterval) {
			return {};
		}
		m_sinceReconcile = std::chrono::milliseconds(0);
		ReconcileSlots();
	} catch (const std::exception& ex) {
		LogError(std::string("as_hide: update failed: ") + ex.what());
	} catch (...) {
		LogError("as_hide: update failed with an unknown exception");
	}
	return {};
}

plg::PluginResult HidePlugin::OnPluginEnd() {
	try {
		if (m_hideCommandRegistered) {
			m_hideCommandRegistered = false;
			if (__s2sdk_RemoveCommand) {
				s2sdk::RemoveCommand(kHideCommand, &OnHideCommand);
			}
		}
	} catch (const std::exception& ex) {
		LogError(std::string("as_hide: OnPluginEnd failed: ") + ex.what());
	} catch (...) {
		LogError("as_hide: OnPluginEnd failed with an unknown exception");
	}
	// Drops the "!hide"/"jointeam" registrations, the player hooks and the timers.
	pisex::Shutdown();
	return {};
}

void HidePlugin::OnClientAuthorized(int iSlot) {
	if (!ValidSlot(iSlot)) {
		return;
	}
	m_hidden[static_cast<std::size_t>(iSlot)] = false;
	m_steamids[static_cast<std::size_t>(iSlot)] = 0;
}

void HidePlugin::ReconcileSlots() {
	for (int i = 0; i < kMaxSlots; ++i) {
		try {
			const uint64_t steamid = pisex::g_pPlayers->GetSteamID64(i);
			if (steamid == 0) {
				// No authenticated player in the slot (or the slot is between occupants).
				m_steamids[static_cast<std::size_t>(i)] = 0;
				m_hidden[static_cast<std::size_t>(i)] = false;
				continue;
			}
			if (m_steamids[static_cast<std::size_t>(i)] == steamid) {
				continue;
			}
			// A different player is using the slot now: same reset the original performed from
			// OnClientAuthorized (which does not fire reliably on a live server).
			if (m_hidden[static_cast<std::size_t>(i)]) {
				m_hidden[static_cast<std::size_t>(i)] = false;
				LogInfo("as_hide: slot " + std::to_string(i) +
						" now holds another player - the hide flag was reset");
			}
			m_steamids[static_cast<std::size_t>(i)] = steamid;
		} catch (const std::exception& ex) {
			LogError("as_hide: reconcile of slot " + std::to_string(i) + " failed: " + ex.what());
		} catch (...) {
			LogError("as_hide: reconcile of slot " + std::to_string(i) + " failed with an unknown exception");
		}
	}
}

s2sdk::ResultType HidePlugin::HandleHideCommand(int32_t caller) {
	// A console/say call always has a player behind it; -1 (server console) fails HasPermission,
	// exactly like the original.
	if (g_pAdminApi->HasPermission(caller, "@admin/hide")) {
		ToggleHide(caller);
	}
	return s2sdk::ResultType::Handled;
}

bool HidePlugin::HandleJoinTeam(int iSlot) {
	if (!ValidSlot(iSlot) || !m_hidden[static_cast<std::size_t>(iSlot)]) {
		return false; // let the engine process jointeam
	}
	g_pAdminApi->SendAction(iSlot, "hide_off", "");
	pisex::PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("Hide_Off"));
	m_hidden[static_cast<std::size_t>(iSlot)] = false;
	return false; // the original let the team change happen
}

void HidePlugin::ToggleHide(int iSlot) {
	if (!ValidSlot(iSlot)) {
		return;
	}

	if (m_hidden[static_cast<std::size_t>(iSlot)]) {
		g_pAdminApi->SendAction(iSlot, "hide_off", "");
		m_hidden[static_cast<std::size_t>(iSlot)] = false;
		pisex::g_pPlayers->ChangeTeam(iSlot, 1);
		pisex::PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("Hide_Off"));
		return;
	}

	m_hidden[static_cast<std::size_t>(iSlot)] = true;
	ServerCommand("sv_disable_teamselect_menu 1");

	// The original's 0.1 s timer: by then the controller and pawn are in place.
	pisex::CreateTimer(0.1f, [iSlot]() -> float {
		// CCSPlayerController::FromSlot() + GetPlayerPawn() of the original (0 = gone).
		if (pisex::GetPlayerPawn(iSlot) == 0) {
			return -1.0f; // the original stopped the timer here too
		}
		g_pAdminApi->SendAction(iSlot, "hide_on", "");
		if (pisex::IsAlive(iSlot)) {
			pisex::g_pPlayers->CommitSuicide(iSlot, false, true);
		}
		pisex::g_pPlayers->ChangeTeam(iSlot, 1);
		pisex::g_pPlayers->ChangeTeam(iSlot, 0);

		// The original created the 0.1 s follow-up timer right here. The TimerSystem lock is
		// recursive, so creating it inside this callback would not deadlock; the creation is still
		// deferred to the next frame so no timer is added while the timer loop runs this callback -
		// pisex::Poll() then creates it, keeping the original 0.1 s delay.
		pisex::NextFrame([]() {
			pisex::CreateTimer(0.1f, []() -> float {
				ServerCommand("sv_disable_teamselect_menu 0");
				return -1.0f;
			});
		});

		pisex::PrintToChat(iSlot, "%s", g_pAdminApi->GetTranslation("Hide_On"));
		return -1.0f;
	});
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_hidePlugin)
