#ifndef H_ECAB76F888654CC5B58D6075F1FBEE53
#define H_ECAB76F888654CC5B58D6075F1FBEE53
#undef _UNICODE

#include "s2parser.h"

#include "StormLib.h"

#include <sstream>
#include <sstream>
#include <fstream>
#include <iomanip>

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

inline uint32_t GetLatestProtocolVersion(){
	return 87702;
}

inline std::optional<ProtocolParser> LoadProtocol(uint32_t version){
	std::stringstream filename;
	filename << "s2protocol/json/protocol" << version << ".json";
	
	std::ifstream in{filename.str()};
	ProtocolParser p;
	if(!p.Init(in)) return {};
	
	return p;
}

inline ProtocolParser& GetLatestProtocolParser(){
	static auto v = *LoadProtocol(GetLatestProtocolVersion());
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
		if(!GetLatestProtocolParser().DecodeInstance(userData, true, "NNet.Replay.SHeader", &l)){
			std::cerr << "DecodeInstance for replay header failed" << std::endl;
			return false;
		}
		
		if(baseBuild == 0){
			std::cerr << "No m_baseBuild in archive" << std::endl;
			return false;
		}
	}
	
	ProtocolParser *replayProtocol = nullptr;
	std::optional<ProtocolParser> replayProtocolContainer;
	
	if(baseBuild == GetLatestProtocolVersion()){
		replayProtocol = &GetLatestProtocolParser();
	}else{
		// We need to load the right protocol version for this
		replayProtocolContainer = LoadProtocol(baseBuild);
		if(!replayProtocolContainer){
			std::cerr << "We don't have a decoder for protocol " << baseBuild << std::endl;
			return 1;
		}
		
		replayProtocol = &*replayProtocolContainer;
	}
	
	if(listeners.header){
		replayProtocol->DecodeInstance(userData, true, "NNet.Replay.SHeader", listeners.header);
	}
	
	if(listeners.tracker){
		if(auto str = GetMPQFile(mpq.get(), "replay.tracker.events")){
			replayProtocol->DecodeEventStream(
				*str,
				true,
				"NNet.Replay.Tracker.EEventId",
				false,
				listeners.tracker
			);
		}
	}
	
	if(listeners.game){
		if(auto str = GetMPQFile(mpq.get(), "replay.game.events")){
			replayProtocol->DecodeEventStream(
				*str,
				false,
				"NNet.Game.EEventId",
				true,
				listeners.game
			);
		}
	}
	
	if(listeners.details){
		if(auto str = GetMPQFile(mpq.get(), "replay.details")){
			replayProtocol->DecodeInstance(
				*str,
				true,
				"NNet.Game.SDetails",
				listeners.details
			);
		}
	}
	
	return true;
}

#endif
