#undef _UNICODE

#include "s2loader.h"

int64_t WindowsToUnixTimestamp(int64_t v){
	return (v - 116444735995904000) / 10000000;
}

BroadcastListener GetTrackerListener(){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		"NNet.Replay.Tracker.EEventId.e_unitBorn",
		"unitBorn",
		{
			{
				BasicStructListener::kTypeString, "m_unitTypeName", "type", [](std::variant<int64_t, std::string_view> v){
					auto &value = std::get<std::string_view>(v);
					
					static const auto prefix = "ReplayStats";
					if(value.size() < strlen(prefix) || strncmp(value.data(), prefix, strlen(prefix)) != 0){
						return false;
					}
					
					return true;
				}
			},
			{ BasicStructListener::kTypeInt, "m_x", "x" },
			{ BasicStructListener::kTypeInt, "m_y", "y" },
			{ BasicStructListener::kTypeInt, "m_controlPlayerId", "player" },
		}
	)));
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		"NNet.Replay.Tracker.EEventId.e_upgrade",
		"upgrade",
		{
			{ BasicStructListener::kTypeInt, "m_playerId", "player" },
			{
				BasicStructListener::kTypeString, "m_upgradeTypeName", "upgradeType", [](std::variant<int64_t, std::string_view> v){
					auto &value = std::get<std::string_view>(v);
					
					if(value == "SprayTerran") return false; // Not used atm but I don't ever wanna see this
					if(value == "SprayZerg") return false;
					if(value == "SprayProtoss") return false;
					if(value == "VoidStoryUnitGlobalUpgrade") return false;
					
					
					return true;
				}
			},
			{ BasicStructListener::kTypeInt, "m_count", "count" },
		}
	)));
	
	return r;
}

BroadcastListener GetGameListener(){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		"NNet.Game.EEventId.e_gameUserLeave",
		"playerLeft",
		{
			{ BasicStructListener::kTypeString, "m_leaveReason", "reason" },
		}
	)));
	
	return r;
}

BroadcastListener GetDetailsListener(){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		"NNet.Game.SDetails",
		"details",
		{
			//m_playerList
			{ BasicStructListener::kTypeString, "m_title", "title" },
			{ BasicStructListener::kTypeInt, "m_timeUTC", "timeUTC" },
			{ BasicStructListener::kTypeInt, "m_timeLocalOffset", "timeLocalOffset" },
		}
	)));
	
	return r;
}

int main(){
	SC2ReplayListeners listeners;
	
	//auto tracker = GetTrackerListener();
	//listeners.tracker = &tracker;
	
	auto game = GetGameListener();
	listeners.game = &game;
	
	auto details = JSONBuilderListener();
	listeners.details = &details;
	
	auto header = JSONBuilderListener();
	listeners.header = &header;
	
	if(!LoadSC2Replay("bin/test.SC2Replay", listeners)){
		return 1;
	}
	
	for(auto &v : details.GetJSON()){
		std::cout << v.serialize(true) << std::endl;
	}
	
	for(auto &v : header.GetJSON()){
		std::cout << v.serialize(true) << std::endl;
	}
	
	return 0;
}
