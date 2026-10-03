# build/test/sound_test: emote sounds on their own, with the plugin's sound.o (decoding), speaker.o (PipeWire playback)
# and third_party.o (stb_vorbis). sound_check.sh next to it builds it through the repo's build.sh and runs it against
# its own PipeWire; don't run make on this directly.
include Makefile

TESTDIR := build/test
UNIT    := $(TESTDIR)/sound_test

.DEFAULT_GOAL := $(UNIT)

$(UNIT): tools/test/sound/sound_test.cpp build/sound.o build/speaker.o build/third_party.o tools/test/sound/sound.mk | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -Isrc -o $@ $(filter %.cpp %.o,$^) $(if $(PIPEWIRE),$(shell pkg-config --libs libpipewire-0.3))

$(TESTDIR):
	mkdir -p $@
