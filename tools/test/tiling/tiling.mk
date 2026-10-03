# build/test/tile_unit: tiling mode's ring (src/tiling.cpp) on its own, with walker.o and collision.o for the body the
# ring goes with. run.sh next to it builds it through the repo's build.sh and runs it; don't run make on this directly.
include Makefile

TESTDIR := build/test
UNIT    := $(TESTDIR)/tile_unit

.DEFAULT_GOAL := $(UNIT)

$(UNIT): tools/test/tiling/tile_unit.cpp build/tiling.o build/walker.o build/collision.o tools/test/tiling/tiling.mk | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^)

$(TESTDIR):
	mkdir -p $@
