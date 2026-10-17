# make sets CXX to its own default; replace that one, keep a user-given one.
ifeq ($(origin CXX),default)
CXX = i686-w64-mingw32-g++
endif
CXXFLAGS ?= -std=c++17 -Os -Wall -Wextra -Werror
LDFLAGS ?= -shared -static-libgcc -static-libstdc++

build/xinput9_1_0.dll: dc_modern.cpp dc_modern.def
	mkdir -p build
	$(CXX) $(CXXFLAGS) $(LDFLAGS) -o $@ dc_modern.cpp dc_modern.def

clean:
	rm -rf build

.PHONY: clean
