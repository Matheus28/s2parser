#ifndef H_F3332459A8D147ADAAAF7200CE2F2667
#define H_F3332459A8D147ADAAAF7200CE2F2667

#include <string_view>
#include <cstdint>
#include <vector>
#include <memory>

#include "picojson.h"

struct Listener {
	virtual ~Listener(){}
	
	virtual void OnEvent(int64_t gameloop, int userid, std::string_view name) = 0;
	virtual void OnEventEnd(int64_t gameloop, int userid, std::string_view name) = 0;
	
	virtual void OnEnterUserType(std::string_view name) = 0;
	virtual void OnExitUserType(std::string_view name) = 0;
	
	virtual void OnEnterStruct() = 0;
	virtual void OnStructField(std::string_view name) = 0;
	virtual void OnExitStruct() = 0;
	
	virtual void OnEnterArray() = 0;
	virtual void OnExitArray() = 0;
	
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
	
	void OnEnterArray() override {}
	void OnExitArray() override {}
	
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
	
	void OnEnterArray() override {
		for(auto &v : m_Listeners) v->OnEnterArray();
	}
	
	void OnExitArray() override {
		for(auto &v : m_Listeners) v->OnExitArray();
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

struct JSONBuilderListener : NullListener {
	JSONBuilderListener(bool stripPrefix) : m_bStripPrefix(stripPrefix) {
		m_Stack.emplace_back(picojson::array());
	}
	
	void OnEnterStruct() override {
		m_Keys.push_back(m_LastKey);
		m_Stack.emplace_back(picojson::object());
	}
	
	void OnStructField(std::string_view name) override {
		if(m_bStripPrefix && name.size() >= 2 && name[0] == 'm' && name[1] == '_') name.remove_prefix(2);
		m_LastKey = name;
	}
	
	void OnExitStruct() override {
		auto v = std::move(m_Stack.back());
		m_Stack.pop_back();
		assert(v.is<picojson::object>());
		
		assert(!m_Keys.empty());
		m_LastKey = std::move(m_Keys.back());
		m_Keys.pop_back();
		
		OnValue(std::move(v));
	}
	
	void OnEnterArray() override {
		m_Keys.push_back(m_LastKey);
		m_Stack.emplace_back(picojson::array());
	}
	
	void OnExitArray() override {
		auto v = std::move(m_Stack.back());
		m_Stack.pop_back();
		assert(v.is<picojson::array>());
		
		assert(!m_Keys.empty());
		m_LastKey = std::move(m_Keys.back());
		m_Keys.pop_back();
		
		OnValue(std::move(v));
	}
	
	void OnValueNull() override {
		OnValue(picojson::value());
	}
	
	void OnValueInt(int64_t v) override {
		OnValue(picojson::value(v));
	}
	
	void OnValueString(std::string_view v) override {
		OnValue(picojson::value(std::string(v)));
	}
	
	void OnValueBits(std::vector<uint8_t> v) override {
		OnValue(picojson::value(std::string((char*) v.data(), v.size())));
	}
	
	picojson::array& GetJSON(){
		assert(m_Stack.size() == 1);
		return m_Stack.back().get<picojson::array>();
	}
	
	const picojson::array& GetJSON() const {
		assert(m_Stack.size() == 1);
		return m_Stack.back().get<picojson::array>();
	}
	
private:
	void OnValue(picojson::value v){
		assert(!m_Stack.empty());
		if(m_Stack.back().is<picojson::object>()){
			m_Stack.back().get<picojson::object>()[std::string(m_LastKey)] = v;
		}else if(m_Stack.back().is<picojson::array>()){
			m_Stack.back().get<picojson::array>().push_back(v);
		}else{
			assert(false);
		}
	}
	
	bool m_bStripPrefix;
	std::string_view m_LastKey = "";
	std::vector<std::string_view> m_Keys;
	std::vector<picojson::value> m_Stack;
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

struct ScopeFilterListener : Listener {
	ScopeFilterListener(
		std::initializer_list<std::string> ll,
		bool includeDescendants,
		Listener *sub
	)
	: m_Sub(sub)
	, m_bIncludeDescendants(includeDescendants)
	, m_DesiredScope(ll)
	{
		
	}
	
	void OnEnterStruct() override {
		m_CurScope.push_back(m_LastKey);
		UpdateListening();
		if(m_bListening) m_Sub->OnEnterStruct();
	}
	
	void OnStructField(std::string_view name) override {
		if(m_bListening) m_Sub->OnStructField(name);
		
		//if(name.size() >= 2 && name[0] == 'm' && name[1] == '_') name.remove_prefix(2);
		m_LastKey = name;
	}
	
	void OnExitStruct() override {
		if(m_bListening) m_Sub->OnExitStruct();
		
		m_CurScope.pop_back();
		UpdateListening();
	}
	
	void OnEnterArray() override {
		m_CurScope.push_back(m_LastKey);
		UpdateListening();
		if(m_bListening) m_Sub->OnEnterArray();
	}
	
	void OnExitArray() override {
		if(m_bListening) m_Sub->OnExitArray();
		
		m_CurScope.pop_back();
		UpdateListening();
	}
	
	void OnEvent(int64_t gameloop, int userid, std::string_view name) override {
		m_LastKey = "";
		if(m_bListening) m_Sub->OnEvent(gameloop, userid, name);
	}
	
	void OnEventEnd(int64_t gameloop, int userid, std::string_view name) override {
		if(m_bListening) m_Sub->OnEventEnd(gameloop, userid, name);
	}
	
	void OnEnterUserType(std::string_view name) override {
		if(m_bListening) m_Sub->OnEnterUserType(name);
	}
	
	void OnExitUserType(std::string_view name) override {
		if(m_bListening) m_Sub->OnExitUserType(name);
	}
	
	void OnValueNull() override {
		if(m_bListening) m_Sub->OnValueNull();
	}
	
	void OnValueInt(int64_t value) override {
		if(m_bListening) m_Sub->OnValueInt(value);
	}
	
	void OnValueString(std::string_view value) override {
		if(m_bListening) m_Sub->OnValueString(value);
	}
	
	void OnValueBits(std::vector<uint8_t> value) override {
		if(m_bListening) m_Sub->OnValueBits(value);
	}
	
private:
	void UpdateListening(){
		bool nextListening = false;
		
		auto curScopeBegin = m_CurScope.begin();
		
		// When an event is being streamed, we enter a structure we no name
		// Let's skip it for the purposes of checking if we're listening
		if(curScopeBegin != m_CurScope.end() && curScopeBegin->empty()){
			++curScopeBegin;
		}
		
		auto curScopeSize = m_CurScope.end() - curScopeBegin;
		
		if(m_bIncludeDescendants){
			if(curScopeSize >= m_DesiredScope.size()){
				nextListening = std::equal(m_DesiredScope.begin(), m_DesiredScope.end(), curScopeBegin);
			}
		}else{
			if(curScopeSize == m_DesiredScope.size()){
				nextListening = std::equal(m_DesiredScope.begin(), m_DesiredScope.end(), curScopeBegin);
			}
		}
		
		if(nextListening != m_bListening){
			m_bListening = nextListening;
			
			// Is this needed?
			if(m_bListening){
				OnStructField(m_LastKey);
			}
		}
	}
	
	bool m_bListening = false;
	bool m_bIgnoreNextStruct = false;
	
	std::vector<std::string> m_DesiredScope;
	bool m_bIncludeDescendants;
	Listener *m_Sub;
	
	std::string_view m_LastKey;
	std::vector<std::string_view> m_CurScope;
};

#endif
