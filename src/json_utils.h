#ifndef H_76033D807EBE40598B3B42EABAF91B50
#define H_76033D807EBE40598B3B42EABAF91B50

#include "picojson.h"
#include <initializer_list>
#include <variant>
#include <type_traits>
#include <string>
#include <string_view>
#include <optional>
#include <functional>

namespace json {

namespace internal {
	template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
	template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
	
	
	template<typename T>
	struct WrapComplex_t {
		typedef std::reference_wrapper<const T> type;
	};
	
	template<>
	struct WrapComplex_t<int> {
		typedef int type;
	};
	
	template<>
	struct WrapComplex_t<double> {
		typedef double type;
	};
	
	template<>
	struct WrapComplex_t<bool> {
		typedef bool type;
	};
	
	template<typename T>
	using WrapComplex = typename WrapComplex_t<T>::type;
	
	
	///////////
	template<typename T>
	struct WrapComplex2_t {
		typedef const T& type;
	};
	
	template<>
	struct WrapComplex2_t<int> {
		typedef int type;
	};
	
	template<>
	struct WrapComplex2_t<double> {
		typedef double type;
	};
	
	template<>
	struct WrapComplex2_t<bool> {
		typedef bool type;
	};
	
	template<typename T>
	using WrapComplex2 = typename WrapComplex2_t<T>::type;
}

template<typename T>
inline std::optional<internal::WrapComplex<T>> Reach(const picojson::value &root, std::initializer_list<std::variant<const char*, int>> path){
	const picojson::value *cur = &root;
	for(auto &v : path){
		if(!cur) return {};
		bool bad = false;
		std::visit(internal::overloaded {
			[&](const char *key){
				if(cur->is<picojson::object>()){
					const auto &obj = cur->get<picojson::object>();
					auto it = obj.find(key);
					
					if(it == obj.end()){
						bad = true;
					}else{
						cur = &it->second;
					}
				}else{
					bad = true;
				}
			},
			
			[&](int key){
				if(cur->is<picojson::array>()){
					const auto &arr = cur->get<picojson::array>();
					if((size_t) key >= arr.size()){
						bad = true;
					}else{
						cur = &arr[key];
					}
				}else{
					bad = true;
				}
			},
		}, v);
		
		if(bad) return {};
	}
	
	if(!cur) return {};
	
	if constexpr(std::is_same_v<T, int> || std::is_same_v<T, float> || std::is_same_v<T, double>){
		if(cur->is<double>()){
			return cur->get<double>();
		}else if(cur->is<std::string>()){
			return atof(cur->get<std::string>().data());
		}else{
			return {};
		}
	}else if constexpr(std::is_same_v<T, bool>){
		return cur->evaluate_as_boolean();
	}else if constexpr(std::is_same_v<T, std::string>){
		//if(cur->is<double>()){
		//	return std::to_string(cur->get<double>());
		//}
		
		if(cur->is<std::string>()){
			return cur->get<std::string>();
		}else{
			return {};
		}
	}else if constexpr(std::is_same_v<T, picojson::object>){
		if(cur->is<picojson::object>()){
			return cur->get<picojson::object>();
		}else{
			return {};
		}
	}else if constexpr(std::is_same_v<T, picojson::array>){
		if(cur->is<picojson::array>()){
			return cur->get<picojson::array>();
		}else{
			return {};
		}
	}else if constexpr(std::is_same_v<T, picojson::value>){
		return *cur;
	}else{
		static_assert(!sizeof(T), "Invalid parameter T");
	}
}

template<typename T>
inline internal::WrapComplex2<T> AssertReach(const picojson::value &root, std::initializer_list<std::variant<const char*, int>> path){
	const auto&& v = Reach<T>(root, path);
	assert(v);
	return *v;
}

template<typename T>
inline std::optional<T> Reach(std::string_view v, std::initializer_list<std::variant<const char*, int>> path){
	picojson::value root;
	std::string err;
	picojson::parse(root, v.begin(), v.end(), &err);
	if(!err.empty()) return {};
	return Reach<T>(root, path);
}

template<typename T>
inline std::optional<T> Reach(const char *v, std::initializer_list<std::variant<const char*, int>> path){
	if(v == nullptr) return {};
	return Reach<T>(std::string_view(v), path);
}



}

#endif
