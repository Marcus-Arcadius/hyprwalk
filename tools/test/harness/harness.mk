# The offscreen render harness: shot.cpp and stubs.cpp linked with the plugin's objects, except main, panels and apps
# (they need a running Hyprland), mic (the harness reads WAV files) and speaker (tools/test/sound plays sounds). Built
# into build/test/ by build.sh next to it, from the repo root; don't run make on this directly.
include Makefile

HARNESS := tools/test/harness
TESTDIR := build/test
SHOT    := $(TESTDIR)/shot

.DEFAULT_GOAL := $(SHOT)

# --warn-unresolved-symbols: the plugin's objects name Hyprland functions the harness never calls (listed in
# unresolved.txt; stubs.cpp has the rest)
$(SHOT): $(TESTDIR)/shot.o $(TESTDIR)/stubs.o $(filter-out build/main.o build/panels.o build/apps.o build/mic.o build/speaker.o,$(OBJ))
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
