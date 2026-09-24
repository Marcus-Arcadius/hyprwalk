PLUGIN   := hypr3d.so
SRC      := $(wildcard src/*.cpp)
OBJ      := $(SRC:src/%.cpp=build/%.o)
DEP      := $(OBJ:.o=.d)

PKGS     := hyprland pixman-1 libdrm glesv2 egl cairo pangocairo
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++26 -fPIC -fno-gnu-unique -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
            -DWLR_USE_UNSTABLE $(shell pkg-config --cflags $(PKGS))
LDFLAGS  += -shared $(shell pkg-config --libs glesv2 cairo pangocairo)

all: $(PLUGIN)

$(PLUGIN): $(OBJ)
	$(CXX) $(LDFLAGS) -o $@ $^

build/%.o: src/%.cpp Makefile | build
	$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build $(PLUGIN)

-include $(DEP)

.PHONY: all clean
