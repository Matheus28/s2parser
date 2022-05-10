#undef _UNICODE

#include "s2parser.h"

#include "StormLib.h"

#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>

double countClock(std::chrono::steady_clock::time_point start, std::chrono::steady_clock::time_point end){
	return std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
}

struct BroadcastListener : Listener {
	void AddListener(std::unique_ptr<Listener> p){
		m_Listeners.emplace_back(std::move(p));
	}
	
	// Forwarders below
	
	void OnEvent(int64_t gameloop, int userid, std::string_view name) override {
		for(auto &v : m_Listeners) v->OnEvent(gameloop, userid, name);
	}
	
	void OnEventEnd(int64_t gameloop, int userid, std::string_view name) override {
		for(auto &v : m_Listeners) v->OnEventEnd(gameloop, userid, name);
	}
	
	void OnEnterUserType(std::string_view name) override {
		for(auto &v : m_Listeners) v->OnEnterUserType(name);
	}
	
	void OnExitUserType(std::string_view name) override {
		for(auto &v : m_Listeners) v->OnExitUserType(name);
	}
	
	void OnEnterStruct() override {
		for(auto &v : m_Listeners) v->OnEnterStruct();
	}
	
	void OnStructField(std::string_view name) override {
		for(auto &v : m_Listeners) v->OnStructField(name);
	}
	
	void OnExitStruct() override {
		for(auto &v : m_Listeners) v->OnExitStruct();
	}
	
	void OnValueNull() override {
		for(auto &v : m_Listeners) v->OnValueNull();
	}
	
	void OnValueInt(int64_t value) override {
		for(auto &v : m_Listeners) v->OnValueInt(value);
	}
	
	void OnValueString(std::string_view value) override {
		for(auto &v : m_Listeners) v->OnValueString(value);
	}
	
	void OnValueBits(std::vector<uint8_t> value) override {
		for(auto &v : m_Listeners) v->OnValueBits(value);
	}
	
private:
	std::vector<std::unique_ptr<Listener>> m_Listeners;
};

static BroadcastListener GetTrackerListener();
static BroadcastListener GetGameListener();
static BroadcastListener GetDetailsListener();

int64_t WindowsToUnixTimestamp(int64_t v){
	return (v - 116444735995904000) / 10000000;
}

struct MPQCloser {
	void operator()(HANDLE v) const {
		SFileCloseArchive(v);
	}
};

std::optional<std::string> GetMPQFile(HANDLE mpq, const char *filename){
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

int main(){
	std::chrono::steady_clock::time_point start, end;
	
	std::cout << "main()" << std::endl;
	std::cout << std::setprecision(2);
	std::cout << std::fixed;
	
	
	const char *filename = "bin/test.SC2Replay";
	
	std::unique_ptr<void, MPQCloser> mpq;
	
	{
		start = std::chrono::steady_clock::now();
		HANDLE tmp;
		if(!SFileOpenArchive(filename, 0, 0, &tmp)){
			std::cerr << "Failed to open archive " << filename << std::endl;
			return 1;
		}
		
		mpq = std::unique_ptr<void, MPQCloser>(tmp);
		end = std::chrono::steady_clock::now();
		std::cout << "Opening archive took " << countClock(start, end) << " ms" << std::endl;
	}
	
	start = std::chrono::steady_clock::now();
	std::ifstream in{"s2protocol/json/protocol87702.json"};
	ProtocolParser p{in};
	end = std::chrono::steady_clock::now();
	std::cout << "Parsing protocol took " << countClock(start, end) << " ms" << std::endl;
	
	if(auto str = GetMPQFile(mpq.get(), "replay.tracker.events")){
		start = std::chrono::steady_clock::now();
		
		auto l = GetTrackerListener();
		p.DecodeEventStream(
			*str,
			true,
			"NNet.Replay.Tracker.EEventId",
			false,
			&l
		);
		
		end = std::chrono::steady_clock::now();
		std::cout << "Parsing tracker events took " << countClock(start, end) << " ms" << std::endl;
	}
	
	if(auto str = GetMPQFile(mpq.get(), "replay.game.events")){
		start = std::chrono::steady_clock::now();
		
		auto l = GetTrackerListener();
		p.DecodeEventStream(
			*str,
			false,
			"NNet.Game.EEventId",
			true,
			&l
		);
		
		end = std::chrono::steady_clock::now();
		std::cout << "Parsing game events took " << countClock(start, end) << " ms" << std::endl;
	}
	
	if(auto str = GetMPQFile(mpq.get(), "replay.details")){
		start = std::chrono::steady_clock::now();
		
		auto l = GetDetailsListener();
		p.DecodeInstance(
			*str,
			true,
			"NNet.Game.SDetails",
			&l
		);
		
		end = std::chrono::steady_clock::now();
		std::cout << "Parsing details took " << countClock(start, end) << " ms" << std::endl;
	}
	
	std::cout << "~main()" << std::endl;
	return 0;
}

struct BasicStructListener : NullListener {
	enum Type {
		kTypeInt,
		kTypeString,
	};
	
	struct Field {
		typedef bool(*field_filter_t)(std::variant<int64_t, std::string_view>);
		
		Field(Type type, std::string_view gameFieldName, std::string_view jsonFieldName, const field_filter_t& fieldFilter = nullptr)
		: type(type)
		, gameFieldName(gameFieldName)
		, jsonFieldName(jsonFieldName)
		, fieldFilter(fieldFilter)
		{
			
		}
		
		Type type;
		std::string_view gameFieldName;
		std::string_view jsonFieldName;
		field_filter_t fieldFilter;
		
		bool hasValue = false;
		int64_t intValue;
		std::string_view stringValue;
	};
	
	BasicStructListener(std::string_view eventName, std::string_view jsonName, const std::initializer_list<Field> &fields_) : m_EventName(eventName), m_JSONName(jsonName), fields(fields_){
		size_t index = 0;
		for(auto &v : fields){
			m_FieldByGameName[v.gameFieldName] = &v;
		}
	}
	
	virtual void OnEvent(int64_t gameloop, int userid, std::string_view name){
		m_bListenToThisEvent = (name == m_EventName);
		
		m_NextIntValue = nullptr;
		m_NextStringValue = nullptr;
		
		if(m_bListenToThisEvent){
			for(auto &v : fields){
				v.hasValue = false;
			}
		}
	}
	
	virtual void OnEventEnd(int64_t gameloop, int userid, std::string_view name){
		if(!m_bListenToThisEvent) return;
		
		picojson::object root;
		root.emplace("type", picojson::value(m_JSONName));
		root.emplace("time", picojson::value(gameloop));
		if(userid >= 0) root.emplace("userid", picojson::value((int64_t) userid));
		
		auto &obj = root.emplace("data", picojson::object()).first->second.get<picojson::object>();
		for(auto &v : fields){
			if(!v.hasValue){
				std::cerr << "Incomplete event " << m_EventName << ", missing " << v.gameFieldName << std::endl;
				return;
			}
			
			if(v.type == kTypeInt){
				obj.emplace(v.jsonFieldName, picojson::value(v.intValue));
			}else{
				obj.emplace(v.jsonFieldName, picojson::value(std::string(v.stringValue)));
			}
		}
		
		std::cout << picojson::value(std::move(root)).serialize(false) << "\n";
	}
	
	virtual void OnStructField(std::string_view name){
		if(!m_bListenToThisEvent) return;
		
		m_NextIntValue = nullptr;
		m_NextStringValue = nullptr;
		
		auto it = m_FieldByGameName.find(name);
		if(it == m_FieldByGameName.end()) return;
		
		fieldFilter = it->second->fieldFilter;
		m_NextHasValue = &it->second->hasValue;
		if(it->second->type == kTypeInt){
			m_NextIntValue = &it->second->intValue;
		}else{
			m_NextStringValue = &it->second->stringValue;
		}
	}
	
	virtual void OnValueInt(int64_t v){
		if(m_NextIntValue){
			*m_NextIntValue = v;
			*m_NextHasValue = true;
			m_NextIntValue = nullptr;
			
			if(fieldFilter && !fieldFilter(v)){
				m_bListenToThisEvent = false;
			}
		}
	}
	
	virtual void OnValueString(std::string_view value){
		if(m_NextStringValue){
			*m_NextStringValue = value;
			*m_NextHasValue = true;
			m_NextStringValue = nullptr;
			
			if(fieldFilter && !fieldFilter(value)){
				m_bListenToThisEvent = false;
			}
		}
	}
	
private:
	std::string_view m_EventName;
	std::string m_JSONName;
	std::vector<Field> fields;
	std::unordered_map<std::string_view, Field*> m_FieldByGameName;
	
	bool m_bListenToThisEvent = false;
	
	bool *m_NextHasValue = nullptr;
	std::string_view *m_NextStringValue = nullptr;
	int64_t *m_NextIntValue = nullptr;
	Field::field_filter_t fieldFilter = nullptr;
};

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
