#undef _UNICODE

#include "s2loader.h"

#include "httplib.h"

int64_t WindowsToUnixTimestamp(int64_t v){
	return (v - 116444735995904000) / 10000000;
}

BroadcastListener GetTrackerListener(std::ostream &out){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		out,
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
		out,
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

BroadcastListener GetGameListener(std::ostream &out){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		out,
		"NNet.Game.EEventId.e_gameUserLeave",
		"playerLeft",
		{
			{ BasicStructListener::kTypeString, "m_leaveReason", "reason" },
		}
	)));
	
	return r;
}

BroadcastListener GetDetailsListener(std::ostream &out){
	BroadcastListener r;
	
	r.AddListener(std::make_unique<BasicStructListener>(BasicStructListener(
		out,
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

// Returns region for this entry
std::string PatchUpCacheHandle(std::string &str){
	auto view = std::string_view(str);
	
	auto Trim = [&](std::string_view &v){
		while(!v.empty() && (v.front() == 0 || v.front() == ' ')) v.remove_prefix(1);
		while(!v.empty() && (v.back() == 0 || v.back() == ' ')) v.remove_suffix(1);
	};
	
	static auto NibletToChar = [](uint8_t v) -> char{
		switch(v){
			case 0: return '0';
			case 1: return '1';
			case 2: return '2';
			case 3: return '3';
			case 4: return '4';
			case 5: return '5';
			case 6: return '6';
			case 7: return '7';
			case 8: return '8';
			case 9: return '9';
			case 10: return 'a';
			case 11: return 'b';
			case 12: return 'c';
			case 13: return 'd';
			case 14: return 'e';
			case 15: return 'f';
			default: return '?';
		}
	};

	
	// s2ma or whatever
	auto type = view.substr(0,4);
	Trim(type);
	
	// US and such
	auto regionView = view.substr(4,4);
	Trim(regionView);
	
	auto region = std::string(regionView);
	std::transform(region.begin(), region.end(), region.begin(), [](int ch){
		// Only works for ascii but who cares
		if(ch >= 'A' && ch <= 'Z') return ch - ('Z' - 'z');
		return ch;
	});
	
	auto hashView = view.substr(8);
	
	{
		std::string_view scheme = "https";
		std::string_view domain = "classic.blizzard.com";
		if(region == "sea") region = "us";
		if(region == "cn"){
			scheme = "http";
			domain = "battlenet.com.cn";
		}
		
		std::stringstream ss;
		ss << scheme << "://" << region << "-s2-depot." << domain << "/";
		
		for(auto &v : hashView){
			uint8_t vv = (uint8_t) v;
			ss.put(NibletToChar((vv>>4) & 0xf));
			ss.put(NibletToChar(vv & 0xf));
		}
		
		ss << "." << type;
		
		str = ss.str();
	}
	
	return region;
}

bool ProcessReplay(const char *filename, std::ostream &out){
	const bool pretty = false;
	
	SC2ReplayListeners listeners;
	
	auto tracker = GetTrackerListener(out);
	listeners.tracker = &tracker;
	
	//auto game = GetGameListener(out);
	//listeners.game = &game;
	
	auto detailsJSON = JSONBuilderListener(true);
	listeners.details = &detailsJSON;
	
	auto headerJSON = JSONBuilderListener(true);
	listeners.header = &headerJSON;
	
	if(!LoadSC2Replay(filename, listeners)){
		return false;
	}
	
	if(detailsJSON.GetJSON().empty()) return false;
	if(headerJSON.GetJSON().empty()) return false;
	
	std::string region;
	
	{
		if(!detailsJSON.GetJSON().back().is<picojson::object>()) return false;
		
		auto &detailsObj = detailsJSON.GetJSON().back().get<picojson::object>();
		auto it = detailsObj.find("cacheHandles");
		if(it == detailsObj.end()) return false;
		if(!it->second.is<picojson::array>()) return false;
		for(auto &v : it->second.get<picojson::array>()){
			if(!v.is<picojson::array>()) return false;
			
			auto &blob = v.get<picojson::array>();
			
			std::string str;
			str.resize(blob.size());
			for(size_t i = 0; i < blob.size(); ++i){
				str[i] = (char) (uint8_t) blob[i].get<int64_t>();
			}
			
			region = PatchUpCacheHandle(str);
			v = picojson::value(std::move(str));
		}
		
		
		if(region.empty()){
			// local replay
		}
	}
	
	out << "{\"type\":\"details\",\"data\":" << detailsJSON.GetJSON().back().serialize(pretty) << "}\n";
	out << "{\"type\":\"header\",\"data\":" << headerJSON.GetJSON().back().serialize(pretty) << "}\n";
	
	{
		picojson::object extraData;
		
		extraData["region"] = region.empty() ? picojson::value() : picojson::value(region);
		
		out << "{\"type\":\"extra\",\"data\":" << picojson::value(std::move(extraData)).serialize(pretty) << "}\n";
	}
	
	return true;
}

#ifdef _WIN32
// Just to get intellisense to work, we don't compile on windows
char *mkdtemp(char *v){
	strcpy(v, "C:\\tmp\\");
	return v;
}

int rmdir(const char *pathname){
	return RemoveDirectoryA(pathname) == 0;
}
#endif

int main(int argc, const char *argv[]){
	const char *filename = nullptr;
	const char *protocols = nullptr;
	
	if(argc == 2){
		filename = argv[1];
	}else if(argc == 3){
		protocols = argv[1];
		filename = argv[2];
	}else{
		std::cerr << "Usage: " << argv[0] << " [path to protocols dir] <filename>" << std::endl;
		std::cerr << "Hint: by default the path to protocols dir is s2protocol/json/, which assumes you're running the executable from the root of the project" << std::endl;
		return 1;
	}
	
	
	if(protocols != nullptr) GetProtocolsDir() = protocols;
	
	if(strcmp(filename, "--http") == 0){
		httplib::Server server;
		
		server.set_payload_max_length(5 * 1024 * 1024); // 5 MiB
		
		server.Get("/", [](const httplib::Request &, httplib::Response &res){
			res.set_content("Hi\n", "text/plain");
		});
		
		server.Post("/", [](const httplib::Request &req, httplib::Response &res){
			char dirname[] = "sc2replay-XXXXXX";
			if(mkdtemp(dirname) == nullptr){
				res.status = 500;
				res.set_content("Server failed to create directory\n", "text/plain");
				return;
			}
			
			char filename[64];
			snprintf(filename, sizeof(filename), "%s/file.SC2Replay", dirname);
			
			auto f = fopen(filename, "wb");
			if(!f){
				res.status = 500;
				res.set_content("Server failed to create file\n", "text/plain");
				rmdir(dirname);
				return;
			}
			
			fwrite(req.body.data(), 1, req.body.size(), f);
			fclose(f);
			
			std::stringstream ss;
			if(ProcessReplay(filename, ss)){
				res.set_content(ss.str(), "application/json");
			}else{
				res.status = 400;
				res.set_content("Bad replay\n", "text/plain");
			}
			
			unlink(filename);
			rmdir(dirname);
		});
		
		std::cout << "Starting http server on port 8080" << std::endl;
		server.listen("0.0.0.0", 8080);
		return 0;
	}
	
	return ProcessReplay(filename, std::cout) ? 0 : 1;
}
