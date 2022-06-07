#ifndef H_3C936D3CBFAE4430A6AACA132C514188
#define H_3C936D3CBFAE4430A6AACA132C514188

#include <iostream>
#include <cassert>
#include <unordered_map>
#include <memory>

#include "picojson.h"
#include "json_utils.h"
#include "s2listeners.h"

// This is ported from s2protocol
struct BitPackedBuffer {
	enum Endianness {
		kBigEndian,
		kLittleEndian,
	};
	
	BitPackedBuffer(std::string_view str, Endianness e = kBigEndian) : m_Data(str), m_Endian(e) {
		
	}
	
	bool IsDone() const {
		return m_iNextBitsCount == 0 && m_iUsed >= m_Data.size();
	}
	
	size_t UsedBits() const {
		return m_iUsed * 8 - m_iNextBitsCount;
	}
	
	void ByteAlign(){
		m_iNextBitsCount = 0;
	}
	
	std::string_view ReadAlignedBytes(int n){
		ByteAlign();
		
		if(m_iUsed + n > m_Data.size()){
			m_iUsed = m_Data.size();
			return {};
		}
		
		auto data = m_Data.substr(m_iUsed, n);
		m_iUsed += n;
		
		return data;
	}
	
	uint64_t ReadBitsSmall(int bitsRequested){
		assert(bitsRequested <= 64);
		
		if(IsDone()) return 0;
		
		// Tiny optimization for the common case
		if(m_iNextBitsCount == 0 && bitsRequested == 8){
			return (uint8_t) m_Data[m_iUsed++];
		}
		
		uint64_t result = 0;
		int bitsRead = 0;
		
		while(bitsRead != bitsRequested){
			if(m_iNextBitsCount == 0){
				if(IsDone()){
					return 0;
				}
				
				// Consume a byte and put it in next bits
				m_iNextBits = (uint8_t) m_Data[m_iUsed];
				m_iNextBitsCount = 8;
				++m_iUsed;
			}
			
			// How many bits to grab
			auto copybits = std::min<int>(bitsRequested - bitsRead, m_iNextBitsCount);
			auto mask = ((1 << copybits) - 1);
			uint64_t copy = (m_iNextBits & mask);
			
			if(m_Endian == kBigEndian){
				// Put these bits from left to right
				result |= copy << (bitsRequested - bitsRead - copybits);
			}else{
				// Put these bits from right to left
				result |= copy << bitsRead;
			}
			
			// Consume bits we copied
			m_iNextBits >>= copybits;
			m_iNextBitsCount -= copybits;
			
			bitsRead += copybits;
		}
		
		return result;
	}
	
	std::string ReadBits(int bitsRequested){
		std::string vec;
		vec.resize((bitsRequested+7) / 8);
		size_t i = 0;
		while(bitsRequested >= 8){
			vec[i++] = (uint8_t) ReadBitsSmall(8);
			bitsRequested -= 8;
		}
		
		if(bitsRequested > 0){
			vec[i++] = (uint8_t) ReadBitsSmall(bitsRequested);
		}
		
		return vec;
	}
	
	template<int N>
	void ReadUnalignedBytes(uint8_t out[N]){
		for(size_t i = 0; i < N; ++i){
			out[i] = ReadBitsSmall(8);
		}
	}
	
	bool ExpectSkip(uint8_t v){
		uint8_t r = ReadBitsSmall(8);
		if(r != v){
			std::cerr << "Unexpected skip value: " << int(r) << " (expected " << int(v) << ")" << std::endl;
			return false;
		}
		
		return true;
	}
	
	int64_t Vint(){
		auto b = ReadBitsSmall(8);
		bool negative = b & 1;
		uint64_t result = (b >> 1) & 0x3f;
		
		int bits = 6;
		while((b & 0x80) != 0){
			b = ReadBitsSmall(8);
			result |= (b & 0x7F) << bits;
			bits += 7;
		}
		
		return negative ? -(int64_t) result : (int64_t) result;
	}
	
	void SkipInstance(){
		auto type = ReadBitsSmall(8);
		if(type == 0){ // Array
			auto len = Vint();
			for(size_t i = 0; i < len; ++i) SkipInstance();
		}else if(type == 1){ // Bitblob
			ReadAlignedBytes((Vint() + 7) / 8);
		}else if(type == 2){ // Blob
			ReadAlignedBytes(Vint());
		}else if(type == 3){ // Choice
			Vint(); // Tag
			SkipInstance();
		}else if(type == 4){ // Optional
			if(ReadBitsSmall(8) != 0){
				SkipInstance();
			}
		}else if(type == 5){ // Struct
			auto len = Vint();
			for(size_t i = 0; i < len; ++i){
				Vint(); // Tag
				SkipInstance();
			}
		}else if(type == 6){ // u8
			ReadAlignedBytes(1);
		}else if(type == 7){ // u32
			ReadAlignedBytes(4);
		}else if(type == 8){ // u64
			ReadAlignedBytes(8);
		}else if(type == 9){ // vint
			Vint();
		}
	}
	
private:
	size_t m_iUsed = 0;
	uint8_t m_iNextBitsCount = 0;
	uint8_t m_iNextBits = 0;
	std::string_view m_Data;
	Endianness m_Endian;
};

struct ProtocolParser {
	bool Init(std::istream &in){
		in >> m_Root;
		
		auto err = picojson::get_last_error();
		if(!err.empty()){
			std::cerr << "Failed to parse protocol file json: " << err << std::endl;
			return false;
		}
		
		auto &modules = json::AssertReach<picojson::array>(m_Root, {"modules"});
		for(auto &v : modules){
			LoadModule(v);
		}
		
		ResolveUserTypes();
		return true;
	}
	
	// Decodes a specific structure
	template<bool Versioned>
	bool DecodeInstance(
		std::string_view data,
		const std::string &typeName,
		Listener *listener
	){
		
		auto it = m_Types.find(typeName);
		if(it == m_Types.end()){
			std::cerr << "Can't find " << typeName << std::endl;
			return false;
		}
		
		auto *type = it->second.get();
		
		auto buf = BitPackedBuffer(data);
		
		listener->OnEvent(0, -1, typeName);
		type->Decode<Versioned>(buf, listener);
		listener->OnEventEnd(0, -1, typeName);
		
		return true;
	}
	
	// Decodes events prefixed with a gameloop and possibly userid
	template<bool Versioned, bool DecodeUserID>
	bool DecodeEventStream(
		std::string_view data,
		const std::string &eventIDType,
		Listener *listener
	){
		auto eventIDEnumIt = m_Types.find(eventIDType);
		if(eventIDEnumIt == m_Types.end()){
			std::cerr << "Bad event id type " << eventIDType << std::endl;
			return false;
		}
		
		auto *eventIDEnum = dynamic_cast<EnumType*>(eventIDEnumIt->second.get());
		if(eventIDEnum == nullptr){
			std::cerr << "Bad event id type " << eventIDType << std::endl;
			return false;
		}
		
		// Build this table so from a numerical id we can jump directly to a type
		std::vector<std::pair<std::string_view, Type*>> eventIDToType;
		for(auto &[typeName, enumValue] : eventIDEnum->fullnameToValue){
			auto it = m_EventIDToType.find(std::string(typeName));
			if(it == m_EventIDToType.end()){
				std::cerr << "Type " << typeName << " not found to link to enum " << eventIDType << std::endl;
				return false;
			}
			
			int index = std::stoi(std::string(enumValue));
			
			while(index >= eventIDToType.size()){
				eventIDToType.push_back({"?", nullptr});
			}
			
			eventIDToType[index] = { typeName, it->second };
		}
		
		auto useridIt = m_Types.find("NNet.Replay.SGameUserId");
		if(useridIt == m_Types.end()){
			std::cerr << "Can't find NNet.Replay.SGameUserId" << std::endl;
			return false;
		}
		
		auto useridType = useridIt->second.get();
		
		auto varuint32It = m_Types.find("NNet.SVarUint32");
		if(varuint32It == m_Types.end()){
			std::cerr << "Can't find NNet.SVarUint32" << std::endl;
			return false;
		}
		
		auto varuint32Type = varuint32It->second.get();
		
		struct TmpListener : NullListener {
			TmpListener(int64_t& intValue)
			: intValue(intValue)
			{
				
			}
			
			void OnValueInt(int64_t v) override {
				intValue = v;
			}
			
			int64_t& intValue;
		};
		
		int64_t decodedInt;
		TmpListener tmpListener{decodedInt};
		
		auto buf = BitPackedBuffer(data);
		
		int64_t gameloop = 0;
		while(!buf.IsDone()){
			varuint32Type->Decode<Versioned>(buf, &tmpListener);
			gameloop += decodedInt;
			
			int userid = -1;
			if constexpr(DecodeUserID){
				useridType->Decode<Versioned>(buf, &tmpListener);
				userid = decodedInt;
			}
			
			eventIDEnum->Decode<Versioned>(buf, &tmpListener);
			auto eventID = decodedInt;
			
			if(eventID < 0 || eventID >= eventIDToType.size() || eventIDToType[eventID].second == nullptr){
				std::cerr << "Bad event id " << eventID << std::endl;
				return false;
			}
			
			listener->OnEvent(gameloop, userid, eventIDToType[eventID].first);
			eventIDToType[eventID].second->Decode<Versioned>( buf, listener);
			listener->OnEventEnd(gameloop, userid, eventIDToType[eventID].first);
			
			// Next event is byte aligned
			buf.ByteAlign();
		}
		
		return true;
	}
	
private:
	struct UserType;
	struct Type {
		virtual ~Type(){}
		virtual void DecodeImplied(BitPackedBuffer &buf, Listener *listener) = 0;
		virtual void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) = 0;
		virtual void ResolveUserTypes(ProtocolParser *p){}
		virtual void Print(std::ostream &out) = 0;
		virtual UserType* ToUserType(){ return nullptr; }
		
		template<bool Versioned>
		void Decode(BitPackedBuffer &buf, Listener *listener){
			if constexpr(Versioned){
				return DecodeVersioned(buf, listener);
			}else{
				return DecodeImplied(buf, listener);
			}
		}
	};
	
	struct TypeHolderMultiple;
	struct TypeHolderSingle {
		Type* Own(std::unique_ptr<Type> p){
			auto ret = p.get();
			m_Owned = std::move(p);
			return ret;
		}
		
		void TransferOwnedTo(TypeHolderSingle *other){
			if(m_Owned) other->Own(std::move(m_Owned));
		}
		
		void TransferOwnedTo(TypeHolderMultiple *other);
		
		void DeleteOwned(){
			m_Owned.reset();
		}
		
	private:
		std::unique_ptr<Type> m_Owned;
	};
	
	struct TypeHolderMultiple {
		Type* Own(std::unique_ptr<Type> p){
			auto ret = p.get();
			m_OwnedMultiple.push_back(std::move(p));
			return ret;
		}
		
		void TransferOwnedTo(TypeHolderSingle *other){
			for(auto &v : m_OwnedMultiple){
				other->Own(std::move(v));
			}
			
			m_OwnedMultiple.clear();
		}
		
		void TransferOwnedTo(TypeHolderMultiple *other){
			for(auto &v : m_OwnedMultiple){
				other->Own(std::move(v));
			}
			
			m_OwnedMultiple.clear();
		}
		
	private:
		std::vector<std::unique_ptr<Type>> m_OwnedMultiple;
	};
	
	struct TypeWithBounds : Type {
		int64_t min;
		bool minInclusive = false;
		int64_t max;
		bool maxInclusive = false;
		uint8_t bitsNeededForBounds = 0;
		
		void PrintBounds(std::ostream &out){
			out << " " << (minInclusive?'[':'(') << min << ", " << max << (maxInclusive?']':')') << " " << int(bitsNeededForBounds) << " bits";
		}
	};
	
	struct NullType : Type {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueNull();
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueNull();
		}
		
		void Print(std::ostream &out) override {
			out << "null";
		}
	};
	
	struct BoolType : Type {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueInt(buf.ReadBitsSmall(1));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(6);
			listener->OnValueInt(buf.ReadBitsSmall(8) != 0);
		}
		
		void Print(std::ostream &out) override {
			out << "bool";
		}
	};
	
	struct IntType : TypeWithBounds {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueInt(int64_t((uint64_t) min + buf.ReadBitsSmall(bitsNeededForBounds)));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(9);
			listener->OnValueInt(buf.Vint());
		}
		
		void Print(std::ostream &out) override {
			out << "int";
			PrintBounds(out);
		}
	};
	
	// Same as an int
	struct EnumType : TypeWithBounds {
		std::unordered_map<std::string_view, std::string_view> fullnameToValue;
		std::vector<std::string_view> valueToName;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			int64_t value = min + buf.ReadBitsSmall(bitsNeededForBounds);
			
			listener->OnValueInt(value);
			if(value >= 0 && value <= valueToName.size()){
				listener->OnValueString(valueToName[value]);
			}
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(9);
			int64_t value = buf.Vint();
			
			listener->OnValueInt(value);
			if(value >= 0 && value <= valueToName.size()){
				listener->OnValueString(valueToName[value]);
			}
		}
		
		void Print(std::ostream &out) override {
			out << "enum";
			PrintBounds(out);
		}
	};
	
	// It's a bitmask. Always 64 bits int seems
	struct InumType : Type {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueInt(buf.ReadBitsSmall(64));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(9);
			listener->OnValueInt(buf.Vint());
		}
		
		void Print(std::ostream &out) override {
			out << "inum";
		}
	};
	
	struct BlobType : TypeWithBounds {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			auto len = min + buf.ReadBitsSmall(bitsNeededForBounds);
			listener->OnValueBlob(buf.ReadAlignedBytes(len));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(2);
			auto len = buf.Vint();
			listener->OnValueBlob(buf.ReadAlignedBytes(len));
		}
		
		void Print(std::ostream &out) override {
			out << "blob";
			PrintBounds(out);
		}
	};
	
	struct StringType : TypeWithBounds {
		bool ascii = false;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			auto len = min + buf.ReadBitsSmall(bitsNeededForBounds);
			listener->OnValueString(buf.ReadAlignedBytes(len));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(2);
			auto len = buf.Vint();
			listener->OnValueString(buf.ReadAlignedBytes(len));
		}
		
		void Print(std::ostream &out) override {
			out << "string";
			PrintBounds(out);
		}
	};
	
	struct UserType : Type {
		Type *sub = nullptr; // Resolved in ResolveUserTypes
		std::string_view fullname;
		
		UserType* ToUserType() override { return this; }
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			std::cerr << "Found a non-resolved user type, this is inefficient. Fix me\n";
			sub->DecodeImplied(buf, listener);
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			std::cerr << "Found a non-resolved user type, this is inefficient. Fix me\n";
			sub->DecodeVersioned(buf, listener);
		}
		
		void ResolveUserTypes(ProtocolParser *p) override {
			auto it = p->m_Types.find(std::string(fullname));
			if(it == p->m_Types.end()){
				std::cerr << "Unresolved UserType: " << fullname;
				abort();
			}
			
			sub = it->second.get();
			
			sub->ResolveUserTypes(p);
			
			// Skip user type over if possible
			if(auto t = sub->ToUserType()){
				sub = t->sub;
			}
		}
		
		void Print(std::ostream &out) override {
			out << "user<";
			sub->Print(out);
			out << ">";
		}
	};
	
	struct StructType : Type, TypeHolderMultiple {
		struct Field {
			int tag = -1;
			std::string_view name;
			Type *type;
		};
		
		std::vector<Type*> parents;
		std::vector<Field> fields;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnEnterStruct();
			
			// Parents always come first it seems
			for(auto &v : parents){
				v->DecodeImplied(buf, listener);
			}
			
			for(auto &v : fields){
				listener->OnStructField(v.name);
				v.type->DecodeImplied(buf, listener);
			}
			
			listener->OnExitStruct();
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnEnterStruct();
			
			buf.ExpectSkip(5);
			auto len = buf.Vint();
			for(size_t i = 0; i < len; ++i){
				auto tag = buf.Vint();
				
				if(auto field = GetFieldByTag(tag)){
					listener->OnStructField(field->name);
					field->type->DecodeVersioned(buf, listener);
				}else{
					buf.SkipInstance();
				}
			}
			
			listener->OnExitStruct();
		}
		
		Field* GetFieldByTag(int tag){
			if(tag < 0 || tag >= fields.size()) return nullptr;
			if(fields[tag].tag != tag) return nullptr;
			if(fields[tag].type == nullptr) return nullptr;
			return &fields[tag];
		}
		
		void ResolveUserTypes(ProtocolParser *p) override {
			for(auto &v : parents){
				v->ResolveUserTypes(p);
				
				if(auto t = v->ToUserType()){
					//TODO: we could delete this UserType by removing it from owned...
					v = t->sub;
				}
			}
			
			for(auto &v : fields){
				v.type->ResolveUserTypes(p);
				
				if(auto t = v.type->ToUserType()){
					//TODO: we could delete this UserType by removing it from owned...
					v.type = t->sub;
				}
			}
		}
		
		void Print(std::ostream &out) override {
			out << "struct<";
			
			bool first = true;
			for(auto &v : fields){
				if(first) first = false;
				else out << ", ";
				
				v.type->Print(out);
			}
			
			out << ">";
		}
	};
	
	// Just a 4 byte aligned bytes... which are a string
	struct FourCCType : Type {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueString(buf.ReadAlignedBytes(4));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(7);
			listener->OnValueString(buf.ReadAlignedBytes(4));
		}
		
		void Print(std::ostream &out) override {
			out << "fourcc";
		}
	};
	
	struct BitArrayType : TypeWithBounds {
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnValueBits(buf.ReadBits(min + buf.ReadBitsSmall(bitsNeededForBounds)));
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(1);
			auto len = buf.Vint();
			listener->OnValueBits(buf.ReadAlignedBytes((len + 7) / 8));
		}
		
		void Print(std::ostream &out) override {
			out << "bitarray";
			PrintBounds(out);
		}
	};
	
	struct OptionalType : Type, TypeHolderSingle {
		Type *sub;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			if(buf.ReadBitsSmall(1) != 0){
				sub->DecodeImplied(buf, listener);
			}else{
				listener->OnValueNull();
			}
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(4);
			if(buf.ReadBitsSmall(8) != 0){
				sub->DecodeVersioned(buf, listener);
			}else{
				listener->OnValueNull();
			}
		}
		
		void ResolveUserTypes(ProtocolParser *p) override {
			sub->ResolveUserTypes(p);
			
			if(auto t = sub->ToUserType()){
				sub = t->sub;
				DeleteOwned();
			}
		}
		
		void Print(std::ostream &out) override {
			out << "optional<";
			sub->Print(out);
			out << ">";
		}
	};
	
	struct ArrayType : TypeWithBounds, TypeHolderSingle {
		Type *elemType;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnEnterArray();
			auto len = min + buf.ReadBitsSmall(bitsNeededForBounds);
			
			for(size_t i = 0; i < len; ++i){
				elemType->DecodeImplied(buf, listener);
			}
			listener->OnExitArray();
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			listener->OnEnterArray();
			buf.ExpectSkip(0);
			auto len = buf.Vint();
			for(size_t i = 0; i < len; ++i){
				elemType->DecodeVersioned(buf, listener);
			}
			listener->OnExitArray();
		}
		
		void ResolveUserTypes(ProtocolParser *p) override {
			elemType->ResolveUserTypes(p);
			
			if(auto t = elemType->ToUserType()){
				elemType = t->sub;
				DeleteOwned();
			}
		}
		
		void Print(std::ostream &out) override {
			out << "array<";
			elemType->Print(out);
			out << ">";
			PrintBounds(out);
		}
	};
	
	struct DynArrayType : ArrayType {
		void Print(std::ostream &out) override {
			out << "dynarray<";
			elemType->Print(out);
			out << ">";
			PrintBounds(out);
		}
	};
	
	struct ChoiceType : TypeWithBounds, TypeHolderMultiple {
		struct Choice {
			int tag = -1;
			std::string name;
			Type *sub;
		};
		
		std::vector<Choice> choices;
		
		void DecodeImplied(BitPackedBuffer &buf, Listener *listener) override {
			auto id = min + buf.ReadBitsSmall(bitsNeededForBounds);
			
			if(id < choices.size()){
				choices[id].sub->DecodeImplied(buf, listener);
			}else{
				std::cerr << "Bad choice " << id << " (only have " << choices.size() << ")" << std::endl;
			}
		}
		
		void DecodeVersioned(BitPackedBuffer &buf, Listener *listener) override {
			buf.ExpectSkip(3);
			auto id = buf.Vint();
			
			if(id < choices.size()){
				choices[id].sub->DecodeVersioned(buf, listener);
			}else{
				buf.SkipInstance();
			}
		}
		
		void ResolveUserTypes(ProtocolParser *p) override {
			for(auto &v : choices){
				v.sub->ResolveUserTypes(p);
				
				if(auto t = v.sub->ToUserType()){
					//TODO: we could delete this UserType by removing it from owned...
					v.sub = t->sub;
				}
			}
		}
		
		void Print(std::ostream &out) override {
			out << "choice<";
			
			bool first = true;
			for(auto &v : choices){
				if(first) first = false;
				else out << ", ";
				
				v.sub->Print(out);
			}
			
			out << ">";
			PrintBounds(out);
		}
	};
	
	void LoadModule(const picojson::value &root){
		auto &decls = json::AssertReach<picojson::array>(root, {"decls"});
		
		for(auto &v : decls){
			LoadModuleDecl(v);
		}
	}
	
	void ResolveUserTypes(){
		for(auto &[name, v] : m_Types){
			v->ResolveUserTypes(this);
		}
	}
	
	void LoadModuleDecl(const picojson::value &root){
		auto &declType = json::AssertReach<std::string>(root, {"type"});
		
		if(declType == "TypeDecl"){
			LoadTypeDecl(root);
		}else if(declType == "ConstDecl"){
			LoadConstDecl(root, false);
		}else if(declType == "Module"){
			LoadModule(root);
		}else{
			std::cerr << "Invalid decl type within module: " << declType << std::endl;
			abort();
		}
	}
	
	void LoadTypeDecl(const picojson::value &root){
		auto &name = json::AssertReach<std::string>(root, {"fullname"});
		
		m_Types.insert_or_assign(name, ParseTypeInfo(json::AssertReach<picojson::value>(root, {"type_info"})));
	}
	
	std::unique_ptr<Type> ParseTypeInfo(const picojson::value &root){
		auto &type = json::AssertReach<std::string>(root, {"type"});
		
		if(type == "NullType"){
			return ParseNullType(root);
		}else if(type == "BoolType"){
			return ParseBoolType(root);
		}else if(type == "EnumType"){
			return ParseEnumType(root);
		}else if(type == "InumType"){
			return ParseInumType(root);
		}else if(type == "StringType"){
			return ParseStringType(root, false);
		}else if(type == "AsciiStringType"){
			return ParseStringType(root, true);
		}else if(type == "IntType"){
			return ParseIntType(root);
		}else if(type == "UserType"){
			return ParseUserType(root);
		}else if(type == "FourCCType"){
			return ParseFourCCType(root);
		}else if(type == "StructType"){
			return ParseStructType(root);
		}else if(type == "ChoiceType"){
			return ParseChoiceType(root);
		}else if(type == "ArrayType"){
			return ParseArrayType(root);
		}else if(type == "DynArrayType"){
			return ParseDynArrayType(root);
		}else if(type == "BitArrayType"){
			return ParseBitArrayType(root);
		}else if(type == "BlobType"){
			return ParseBlobType(root);
		}else if(type == "OptionalType"){
			return ParseOptionalType(root);
		}else{
			std::cerr << "Invalid typedecl type: " << type << std::endl;
			abort();
		}
	}
	
	std::unique_ptr<Type> ParseNullType(const picojson::value &root){
		return std::make_unique<NullType>();
	}
	
	std::unique_ptr<Type> ParseBoolType(const picojson::value &root){
		return std::make_unique<BoolType>();
	}
	
	std::unique_ptr<Type> ParseEnumType(const picojson::value &root){
		auto decl = std::make_unique<EnumType>();
		auto &values = json::AssertReach<picojson::array>(root, {"fields"});
		
		double minValue = 0;
		double maxValue = 0;
		
		for(auto &v : values){
			assert(json::AssertReach<std::string>(v, {"type"}) == "MemberEnumField");
			
			decl->fullnameToValue[json::AssertReach<std::string>(v, {"fullname"})] = json::AssertReach<std::string>(v, {"value", "value"});
			
			int64_t value = std::stoll(json::AssertReach<std::string>(v, {"value", "value"}));
			
			if(value >= decl->valueToName.size()) decl->valueToName.resize(value+1);
			decl->valueToName[value] = json::AssertReach<std::string>(v, {"name"});
			
			double valuef = std::stod(json::AssertReach<std::string>(v, {"value", "value"}));
			minValue = std::min(minValue, valuef);
			maxValue = std::max(maxValue, valuef);
		}
		
		decl->minInclusive = true;
		decl->maxInclusive = true;
		decl->min = minValue;
		decl->max = maxValue;
		decl->bitsNeededForBounds = ceil(log2(1 + maxValue - minValue));
		
		return decl;
	}
	
	std::unique_ptr<Type> ParseInumType(const picojson::value &root){
		auto decl = std::make_unique<InumType>();
		
		// Honestly don't care enough to parse it
		
		return decl;
	}
	
	std::unique_ptr<Type> ParseStringType(const picojson::value &root, bool ascii){
		auto decl = std::make_unique<StringType>();
		decl->ascii = ascii;
		
		ParseBounds(root, *decl);
		if(!ascii){
			// For some reason, non-ascii strings have their bounds 4 times higher than what it says here
			// that means 2 bits
			decl->bitsNeededForBounds += 2;
		}
		
		return decl;
	}
	
	std::unique_ptr<Type> ParseIntType(const picojson::value &root){
		auto decl = std::make_unique<IntType>();
		ParseBounds(root, *decl);
		return decl;
	}
	
	std::unique_ptr<Type> ParseUserType(const picojson::value &root){
		auto decl = std::make_unique<UserType>();
		decl->fullname = json::AssertReach<std::string>(root, {"fullname"});
		// We need to resolve this later, in ResolveUserTypes
		return decl;
	}
	
	std::unique_ptr<Type> ParseFourCCType(const picojson::value &root){
		return std::make_unique<FourCCType>();
	}
	
	std::unique_ptr<Type> ParseStructType(const picojson::value &root){
		auto decl = std::make_unique<StructType>();
		
		for(auto &v : json::AssertReach<picojson::array>(root, {"parents"})){
			auto fieldType = json::AssertReach<std::string>(v, {"type"});
			if(fieldType == "ParentStructField"){
				decl->parents.emplace_back(decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(v, {"type_info"}))));
			}else{
				std::cerr << "Wtf is this field inside a struct parent? " << fieldType << std::endl;
				abort();
			}
		}
		
		for(auto &v : json::AssertReach<picojson::array>(root, {"fields"})){
			auto fieldType = json::AssertReach<std::string>(v, {"type"});
			if(fieldType == "ConstDecl"){
				LoadConstDecl(v, true);
				if(json::AssertReach<std::string>(v, {"name"}) == "EEVENTID"){
					m_EventIDToType[json::AssertReach<std::string>(v, {"value", "fullname"})] = decl.get();
				}
				
			}else if(fieldType == "MemberStructField"){
				StructType::Field field;
				field.tag = -1;
				if(auto tag = json::Reach<int>(v, {"tag", "value"})){
					field.tag = *tag;
				}
				
				field.name = json::AssertReach<std::string>(v, {"name"});
				field.type = decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(v, {"type_info"})));
				decl->fields.emplace_back(std::move(field));
			}else{
				std::cerr << "Wtf is this field inside a struct? " << fieldType << std::endl;
				abort();
			}
		}
		
		for(size_t i = 0; i < decl->fields.size(); ++i){
			if(decl->fields[i].tag != -1 && decl->fields[i].tag != i){
				// Put us where our tag belongs
				while(decl->fields[i].tag >= decl->fields.size()){
					decl->fields.emplace_back();
				}
				
				std::swap(decl->fields[i], decl->fields[decl->fields[i].tag]);
				--i;
			}
		}
		
		return decl;
	}
	
	std::unique_ptr<Type> ParseChoiceType(const picojson::value &root){
		auto decl = std::make_unique<ChoiceType>();
		
		double minValue = 0;
		double maxValue = 0;
		
		for(auto &v : json::AssertReach<picojson::array>(root, {"fields"})){
			assert(json::AssertReach<std::string>(v, {"type"}) == "MemberChoiceField");
			
			int tag = json::AssertReach<int>(v, {"tag", "value"});
			assert(tag < 1024);
			
			ChoiceType::Choice choice;
			choice.name = json::AssertReach<std::string>(v, {"name"});
			choice.sub = decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(v, {"type_info"})));
			
			if(tag >= decl->choices.size()){
				decl->choices.resize(tag + 1);
			}
			
			decl->choices[tag] = std::move(choice);
			
			double valuef = std::stod(json::AssertReach<std::string>(v, {"tag", "value"}));
			minValue = std::min(minValue, valuef);
			maxValue = std::max(maxValue, valuef);
		}
		
		// Put choices in the correct position according to their tag
		for(size_t i = 0; i < decl->choices.size(); ++i){
			if(decl->choices[i].tag != -1 && decl->choices[i].tag != i){
				// Put us where our tag belongs
				while(decl->choices[i].tag >= decl->choices.size()){
					decl->choices.emplace_back();
				}
				
				std::swap(decl->choices[i], decl->choices[decl->choices[i].tag]);
				--i;
			}
		}
		
		// Make sure every slot is filled
		for(auto &v : decl->choices){
			assert(v.sub != nullptr);
		}
		
		decl->minInclusive = true;
		decl->maxInclusive = true;
		decl->min = minValue;
		decl->max = maxValue;
		decl->bitsNeededForBounds = ceil(log2(1 + maxValue - minValue));
		
		return decl;
	}
	
	std::unique_ptr<Type> ParseBitArrayType(const picojson::value &root){
		auto decl = std::make_unique<BitArrayType>();
		ParseBounds(root, *decl);
		return decl;
	}
	
	std::unique_ptr<Type> ParseBlobType(const picojson::value &root){
		auto decl = std::make_unique<BlobType>();
		ParseBounds(root, *decl);
		return decl;
	}
	
	std::unique_ptr<Type> ParseOptionalType(const picojson::value &root){
		auto decl = std::make_unique<OptionalType>();
		decl->sub = decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(root, {"type_info"})));
		return decl;
	}
	
	
	std::unique_ptr<Type> ParseArrayType(const picojson::value &root){
		auto decl = std::make_unique<ArrayType>();
		
		decl->elemType = decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(root, {"element_type"})));
		
		ParseBounds(root, *decl);
		return decl;
	}
	
	std::unique_ptr<Type> ParseDynArrayType(const picojson::value &root){
		auto decl = std::make_unique<DynArrayType>();
		
		decl->elemType = decl->Own(ParseTypeInfo(json::AssertReach<picojson::value>(root, {"element_type"})));
		
		ParseBounds(root, *decl);
		return decl;
	}
	
	void ParseBounds(const picojson::value &root, TypeWithBounds &decl){
		const auto &min = json::AssertReach<std::string>(root, {"bounds", "min", "evalue"});
		const auto &max = json::AssertReach<std::string>(root, {"bounds", "max", "evalue"});
		
		decl.minInclusive = json::AssertReach<bool>(root, {"bounds", "min", "inclusive"});
		decl.maxInclusive = json::AssertReach<bool>(root, {"bounds", "max", "inclusive"});
		
		try {
			decl.min = std::stoll(min);
		}catch(std::out_of_range){
			decl.min = INT64_MIN;
			decl.minInclusive = true;
		}
		
		try {
			decl.max = std::stoll(max);
		}catch(std::out_of_range){
			decl.max = INT64_MAX;
			decl.maxInclusive = true;
		}
		
		{ // We can use doubles here, it'll be okay
			auto minf = std::stod(min);
			auto maxf = std::stod(max);
			if(!decl.minInclusive) minf += 1;
			if(!decl.maxInclusive) maxf -= 1;
			
			decl.bitsNeededForBounds = ceil(log2(1 + maxf - minf));
		}
	}
	
	void LoadConstDecl(const picojson::value &root, bool local){
		/*
		auto name = json::AssertReach<std::string>(root, {"fullname"});
		if(name.empty()) return;
		
		auto value = json::Reach<std::string>(root, {"value", "value"});
		static std::string unknown = "???";
		m_Consts.insert_or_assign(name, value.value_or(unknown));
		*/
	}
	
	picojson::value m_Root;
	std::unordered_map<std::string, std::unique_ptr<Type>> m_Types;
	//std::unordered_map<std::string, std::string> m_Consts;
	std::unordered_map<std::string, Type*> m_EventIDToType;
};

inline void ProtocolParser::TypeHolderSingle::TransferOwnedTo(TypeHolderMultiple *other){
	if(m_Owned) other->Own(std::move(m_Owned));
}

#endif
