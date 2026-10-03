# Emotes' sounds on their own: build/test/sound_test, from sound_test.cpp and the plugin's own build/sound.o (decoding),
# build/speaker.o (playing through PipeWire) and build/third_party.o (stb_vorbis). Don't run make on this directly;
# sound_check.sh next to it builds it through the repo's build.sh (in the build shell of the Hyprland you are running)
# and runs it against a PipeWire of its own.
include Makefile

TESTDIR := build/test
UNIT    := $(TESTDIR)/sound_test

.DEFAULT_GOAL := $(UNIT)

$(UNIT): tools/test/sound/sound_test.cpp build/sound.o build/speaker.o build/third_party.o tools/test/sound/sound.mk | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^) $(if $(PIPEWIRE),$(shell pkg-config --libs libpipewire-0.3))

$(TESTDIR):
	mkdir -p $@
