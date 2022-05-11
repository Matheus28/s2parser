INC_FLAGS := -Isrc/
INC_FLAGS += -Ihttplib/

SHARED_FLAGS := -g3 -std=c++17 -pthread $(INC_FLAGS) $(CFLAGS) $(CXXFLAGS)
LIBS = -lstorm

prod:
	mkdir -p bin && $(CXX) -O3 $(SHARED_FLAGS) src/main.cpp $(LIBS) -o bin/s2parser
	
debug:
	mkdir -p bin && $(CXX)     $(SHARED_FLAGS) src/main.cpp $(LIBS) -o bin/s2parser
	