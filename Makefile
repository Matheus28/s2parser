ifeq ($(OS),Windows_NT)
	EXECUTABLE_EXT = .exe
endif


prod:
	mkdir -p bin && clang++ -g3 -O3 -std=c++17 src/main.cpp -lstorm -o bin/s2parser$(EXECUTABLE_EXT)
	
debug:
	mkdir -p bin && clang++ -g3 -std=c++17 src/main.cpp -lstorm -o bin/s2parser$(EXECUTABLE_EXT)
	