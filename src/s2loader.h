#ifndef H_ECAB76F888654CC5B58D6075F1FBEE53
#define H_ECAB76F888654CC5B58D6075F1FBEE53
#undef _UNICODE

#include "s2parser.h"

#include "StormLib.h"

#include <sstream>
#include <sstream>
#include <fstream>
#include <iomanip>

#ifndef _WIN32
#include <dirent.h>
#endif

inline std::optional<std::string> GetMPQFile(HANDLE mpq, const char *filename){
	HANDLE file;
	if(!SFileOpenFileEx(mpq, filename, 0, &file) || !file) return {};
	
	DWORD low;
	DWORD high;
	low = SFileGetFileSize(file, &high);
	if(high != 0){ // Too large anyway
		SFileCloseFile(file);
		return {};
	}
	
	DWORD read;
	
	std::string ret;
	ret.resize(low);
	SFileReadFile(file, ret.data(), low, &read, nullptr);
	SFileCloseFile(file);
	
	if(low != read) return {};
	return ret;
}

inline std::string& GetProtocolsDir(){
	static std::string v = "s2protocol/json";
	return v;
}

inline uint32_t GetLatestProtocolVersion(){
	static uint32_t v = []() -> uint32_t {
#ifndef _WIN32
		DIR *dir;
		if(auto dir = opendir(GetProtocolsDir().c_str())){
			uint32_t highestVersion = 0;
			
			struct dirent *ent;
			while((ent = readdir(dir)) != nullptr) {
				unsigned int value;
				if(sscanf(ent->d_name, "protocol%u.json", &value) == 1){
					highestVersion = std::max(highestVersion, (uint32_t) value);
				}
			}
			
			closedir(dir);
			
			if(highestVersion == 0){
				std::cerr << "Protocol directory doesn't seem to have any protocols? " << GetProtocolsDir() << std::endl;
				exit(1);
			}
			
			return highestVersion;
		} else {
			std::cerr << "Protocol directory invalid: " << GetProtocolsDir() << std::endl;
			exit(1);
		}
#else
		std::cerr << "Add this code for windows lol" << std::endl;
		abort();
		return -1; // FIXME
#endif
	}();
	
	return v;
}

inline std::optional<ProtocolParser> LoadProtocol(uint32_t version){
	std::stringstream filename;
	filename << GetProtocolsDir() << "/protocol" << version << ".json";
	
	std::ifstream in{filename.str()};
	if(in.bad() || !in.is_open()) return {};
	
	ProtocolParser p;
	if(!p.Init(in)) return {};
	
	return std::move(p);
}

inline ProtocolParser& GetLatestProtocolParser(){
	static ProtocolParser v = [](){
		auto v = LoadProtocol(GetLatestProtocolVersion());
		if(!v){
			std::cerr << "Couldn't load latest protocol, this is bad. See you on the other side" << std::endl;
			abort();
		}
		
		return std::move(*v);
	}();
	
	return v;
}

struct SC2ReplayListeners {
	Listener *header = nullptr;
	Listener *tracker = nullptr;
	Listener *game = nullptr;
	Listener *details = nullptr;
};

inline bool LoadSC2Replay(const char *filename, SC2ReplayListeners &listeners){
	struct MPQCloser {
		void operator()(HANDLE v) const {
			SFileCloseArchive(v);
		}
	};
	
	std::unique_ptr<void, MPQCloser> mpq;
	
	{
		HANDLE tmp;
		if(!SFileOpenArchive(filename, 0, 0, &tmp)){
			std::cerr << "Failed to open archive " << filename << std::endl;
			return false;
		}
		
		mpq = std::unique_ptr<void, MPQCloser>(tmp);
	}
	
	uint32_t baseBuild = 0;
	std::string userData;
	
	{
		DWORD userDataLen = 0;
		SFileGetFileInfo(mpq.get(), SFileMpqUserData, NULL, 0, &userDataLen);
		
		userData.resize(userDataLen);
		if(!SFileGetFileInfo(mpq.get(), SFileMpqUserData, userData.data(), userData.size(), nullptr)){
			std::cerr << "SFileGetFileInfo failed (" << userDataLen << ")" << std::endl;
			return false;
		}
		
		struct ProtocolListener : public NullListener {
			ProtocolListener(uint32_t &baseBuild) : baseBuild(baseBuild){}
			
			void OnStructField(std::string_view name) override {
				listening = (name == "m_baseBuild");
			}
			
			void OnExitStruct() override {
				listening = false;
			}
			
			void OnValueInt(int64_t value) override {
				if(listening){
					baseBuild = (uint32_t) value;
					listening = false;
				}
			}
			
			bool listening = false;
			uint32_t &baseBuild;
		};
		
		ProtocolListener l{baseBuild};
		if(!GetLatestProtocolParser().DecodeInstance<true>(userData, "NNet.Replay.SHeader", &l)){
			std::cerr << "DecodeInstance for replay header failed" << std::endl;
			return false;
		}
		
		if(baseBuild == 0){
			std::cerr << "No m_baseBuild in archive" << std::endl;
			return false;
		}
	}
	
	ProtocolParser *replayProtocol = nullptr;
	
	if(baseBuild == GetLatestProtocolVersion()){
		replayProtocol = &GetLatestProtocolParser();
	}else{
		// We need to load the right protocol version for this
		static std::unordered_map<uint32_t, ProtocolParser> protocolCache;
		
		if(auto it = protocolCache.find(baseBuild); it != protocolCache.end()){
			replayProtocol = &it->second;
		}else{
			auto tmp = LoadProtocol(baseBuild);
			if(!tmp){
				std::cerr << "We don't have a decoder for protocol " << baseBuild << std::endl;
				return false;
			}
			
			const auto &p = protocolCache.emplace(baseBuild, std::move(*tmp));
			replayProtocol = &p.first->second;
		}
	}
	
	if(listeners.header){
		if(!replayProtocol->DecodeInstance<true>(userData, "NNet.Replay.SHeader", listeners.header)){
			return false;
		}
	}
	
	if(listeners.tracker){
		if(auto str = GetMPQFile(mpq.get(), "replay.tracker.events")){
			if(!replayProtocol->DecodeEventStream<true, false>(
				*str,
				"NNet.Replay.Tracker.EEventId",
				listeners.tracker
			)){
				return false;
			}
		}
	}
	
	if(listeners.game){
		if(auto str = GetMPQFile(mpq.get(), "replay.game.events")){
			if(!replayProtocol->DecodeEventStream<false, true>(
				*str,
				"NNet.Game.EEventId",
				listeners.game
			)){
				return false;
			}
		}
	}
	
	if(listeners.details){
		if(auto str = GetMPQFile(mpq.get(), "replay.details")){
			if(!replayProtocol->DecodeInstance<true>(
				*str,
				"NNet.Game.SDetails",
				listeners.details
			)){
				return false;
			}
		}
	}
	
	return true;
}

#endif
