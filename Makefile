ifeq ($(OS),Windows_NT)
	EXECUTABLE_EXT = .exe
endif

prod:
	clang++ -g3 -O3 -std=c++17 src/main.cpp -o bin/s2parser$(EXECUTABLE_EXT)
	
debug:
	clang++ -g3 -std=c++17 src/main.cpp -o bin/s2parser$(EXECUTABLE_EXT)
	