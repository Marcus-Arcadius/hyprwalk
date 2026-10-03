# Tiling mode's ring (src/tiling.cpp) on its own: build/test/tile_unit, from tile_unit.cpp and the plugin's own
# build/tiling.o (and walker.o and collision.o: the body the ring goes with). Don't run make on this directly; run.sh
# next to it builds it through the repo's build.sh (in the build shell of the Hyprland you are running) and runs it.
include Makefile

TESTDIR := build/test
UNIT    := $(TESTDIR)/tile_unit

.DEFAULT_GOAL := $(UNIT)

$(UNIT): tools/test/tiling/tile_unit.cpp build/tiling.o build/walker.o build/collision.o tools/test/tiling/tiling.mk | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^)

$(TESTDIR):
	mkdir -p $@
