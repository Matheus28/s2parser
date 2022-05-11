#ifndef H_F3332459A8D147ADAAAF7200CE2F2667
#define H_F3332459A8D147ADAAAF7200CE2F2667

#include <string_view>
#include <cstdint>
#include <vector>
#include <memory>

struct Listener {
	virtual ~Listener(){}
	
	virtual void OnEvent(int64_t gameloop, int userid, std::string_view name) = 0;
	virtual void OnEventEnd(int64_t gameloop, int userid, std::string_view name) = 0;
	
	virtual void OnEnterUserType(std::string_view name) = 0;
	virtual void OnExitUserType(std::string_view name) = 0;
	
	virtual void OnEnterStruct() = 0;
	virtual void OnStructField(std::string_view name) = 0;
	virtual void OnExitStruct() = 0;
	
	virtual void OnValueNull() = 0;
	virtual void OnValueInt(int64_t v) = 0;
	virtual void OnValueString(std::string_view) = 0;
	virtual void OnValueBits(std::vector<uint8_t>) = 0;
};

struct NullListener : Listener {
	void OnEvent(int64_t gameloop, int userid, std::string_view name) override {}
	void OnEventEnd(int64_t gameloop, int userid, std::string_view name) override {}
	
	void OnEnterUserType(std::string_view name) override {}
	void OnExitUserType(std::string_view name) override {}
	
	void OnEnterStruct() override {}
	void OnStructField(std::string_view name) override {}
	void OnExitStruct() override {}
	
	void OnValueNull() override {}
	void OnValueInt(int64_t v) override {}
	void OnValueString(std::string_view) override {}
	void OnValueBits(std::vector<uint8_t>) override {}
};

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

#endif
