# The fuzzers' harness: tools/test/harness's shot rebuilt with AddressSanitizer and UndefinedBehaviorSanitizer from the
# plugin's sources, into build-asan/ apart from the plugin's objects. Build it through the repo's build.sh:
#   ./build.sh -f tools/test/fuzz/asan.mk
include Makefile

ASAN    := build-asan
SHOT_SAN := $(ASAN)/shot
# GCC's undefined leaves out float-cast-overflow (a file's NaN or 1e308 made an int)
SANITIZE := -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=undefined,float-cast-overflow -fno-omit-frame-pointer
SAN_OBJ := $(patsubst src/%.cpp,$(ASAN)/%.o,$(filter-out src/main.cpp src/panels.cpp src/mic.cpp src/speaker.cpp,$(SRC))) $(ASAN)/shot.o $(ASAN)/stubs.o

.DEFAULT_GOAL := $(SHOT_SAN)

$(SHOT_SAN): $(SAN_OBJ)
	$(CXX) $(SANITIZE) -o $@ $^ $(shell pkg-config --libs egl glesv2 hyprutils cairo pangocairo) \
	    -Wl,--warn-unresolved-symbols > $(ASAN)/link.log 2>&1 || { cat $(ASAN)/link.log; exit 1; }
	@echo ":: built $@"

$(ASAN)/%.o: src/%.cpp tools/test/fuzz/asan.mk | $(ASAN)
	$(CXX) $(CXXFLAGS) -O1 $(SANITIZE) -MMD -MP -c $< -o $@

$(ASAN)/%.o: tools/test/harness/%.cpp tools/test/fuzz/asan.mk | $(ASAN)
	$(CXX) $(CXXFLAGS) -O1 $(SANITIZE) -Isrc -MMD -MP -c $< -o $@

$(ASAN):
	mkdir -p $@

-include $(SAN_OBJ:.o=.d)
