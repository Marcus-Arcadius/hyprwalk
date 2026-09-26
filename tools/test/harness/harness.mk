# The offscreen render harness: shot.cpp and stubs.cpp linked with the plugin's own
# objects, all but main.o, panels.o and apps.o (those need a running Hyprland) and mic.o (the
# harness reads WAV files for lip sync, not the microphone). Built into
# build/test/. Don't run make on this directly; build.sh next to it runs it in the
# build shell of the Hyprland you are running, from the repo root.
include Makefile

HARNESS := tools/test/harness
TESTDIR := build/test
SHOT    := $(TESTDIR)/shot

.DEFAULT_GOAL := $(SHOT)

# --warn-unresolved-symbols: the plugin's objects name Hyprland functions the
# harness never calls; they're listed in unresolved.txt (stubs.cpp has the rest)
$(SHOT): $(TESTDIR)/shot.o $(TESTDIR)/stubs.o $(filter-out build/main.o build/panels.o build/apps.o build/mic.o,$(OBJ))
	$(CXX) -o $@ $^ $(shell pkg-config --libs egl glesv2 hyprutils cairo pangocairo) \
	    -Wl,--warn-unresolved-symbols > $(TESTDIR)/link.log 2>&1 || { cat $(TESTDIR)/link.log; exit 1; }
	@grep -oE "undefined (symbol: |reference to ).*" $(TESTDIR)/link.log | sed -E "s/undefined (symbol: |reference to )//" \
	    | sort -u > $(TESTDIR)/unresolved.txt || true
	@echo ":: built $@ ($$(wc -l < $(TESTDIR)/unresolved.txt) unresolved symbols, see $(TESTDIR)/unresolved.txt)"

$(TESTDIR)/%.o: $(HARNESS)/%.cpp $(HARNESS)/harness.mk | $(TESTDIR)
	$(CXX) $(CXXFLAGS) -Isrc -MMD -MP -c $< -o $@

$(TESTDIR):
	mkdir -p $@

-include $(TESTDIR)/shot.d $(TESTDIR)/stubs.d
