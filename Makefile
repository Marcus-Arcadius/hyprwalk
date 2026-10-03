PLUGIN   := hypr3d.so
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:src/%.cpp=build/%.o)
DEP      := $(OBJ:.o=.d)

PKGS     := hyprland pixman-1 libdrm glesv2 egl cairo pangocairo hyprgraphics
# (hyprgraphics: the apps' icons, SVG too; Hyprland has it loaded already)
LIBS     := glesv2 cairo pangocairo hyprgraphics
# the microphone for lip sync (src/mic.cpp), when build.sh found PipeWire
ifeq ($(shell pkg-config --exists libpipewire-0.3 && echo yes),yes)
PKGS     += libpipewire-0.3
LIBS     += libpipewire-0.3
PIPEWIRE := -DH3D_PIPEWIRE
endif
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++26 -fPIC -fno-gnu-unique -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -DWLR_USE_UNSTABLE $(PIPEWIRE) $(shell pkg-config --cflags $(PKGS))
LDFLAGS  += -shared
# (after the objects: a linker with --as-needed, as Debian's and Ubuntu's have it, leaves out a library named
# before what uses it)
LDLIBS   += $(shell pkg-config --libs $(LIBS))

all: $(PLUGIN)

$(PLUGIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.cpp Makefile | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(PLUGIN)

-include $(DEP)

.PHONY: all clean
