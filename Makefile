# Mirror of CMakeLists.txt — keep build flags, libs, and vendored deps in sync.
# Default: builds both wistribute.exe (download) and wistribute-offline.exe (embedded).

CC        ?= gcc
WINDRES   ?= windres
CURL      ?= curl

CSTD       = -std=c23
CFLAGS    ?= -O2 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function
LDFLAGS   ?= -municode -mwindows -s

ZBS_URL    ?= https://github.com/pkulchenko/ZeroBraneStudio/archive/refs/heads/master.zip
MINIZ_TAG  ?= 3.1.1

BUILD      = build
MINIZ_DIR  = $(BUILD)/_deps/miniz
PAYLOAD    = payload/zbs.zip

INCLUDES   = -Isrc -I$(MINIZ_DIR)
DEFINES    = -DUNICODE -D_UNICODE
COMMON_LIBS= -lcomctl32 -lshell32 -lole32 -luuid -luser32 -lgdi32

# miniz.c is compiled separately so we can silence its warnings.
COMMON_SRC = src/main.c src/unzip.c src/gui.c
MINIZ_OBJ  = $(BUILD)/miniz.o
RES_OBJ    = $(BUILD)/wistribute.res.o

.PHONY: all download embedded clean distclean
all: download embedded

download: $(BUILD)/wistribute.exe
embedded: $(BUILD)/wistribute-offline.exe

# --- vendored miniz ----------------------------------------------------------
$(MINIZ_DIR)/miniz.c $(MINIZ_DIR)/miniz.h:
	@mkdir -p $(MINIZ_DIR)
	$(CURL) -fsSL -o $(MINIZ_DIR)/miniz.zip \
	    https://github.com/richgel999/miniz/releases/download/$(MINIZ_TAG)/miniz-$(MINIZ_TAG).zip
	cd $(MINIZ_DIR) && cmake -E tar xf miniz.zip miniz.c miniz.h

# --- payload zip -------------------------------------------------------------
$(PAYLOAD):
	@mkdir -p payload
	$(CURL) -fsSL -o $(PAYLOAD) "$(ZBS_URL)"

# --- manifest resource -------------------------------------------------------
$(RES_OBJ): res/wistribute.rc res/wistribute.manifest | $(BUILD)
	$(WINDRES) -Ires -i res/wistribute.rc -O coff -o $(RES_OBJ)

$(BUILD):
	@mkdir -p $(BUILD)

# miniz amalgamation has many internal -Wunused-function warnings — silence them.
$(MINIZ_OBJ): $(MINIZ_DIR)/miniz.c $(MINIZ_DIR)/miniz.h | $(BUILD)
	$(CC) $(CSTD) -O2 -w $(DEFINES) $(INCLUDES) -c $(MINIZ_DIR)/miniz.c -o $@

# --- download-mode exe -------------------------------------------------------
$(BUILD)/wistribute.exe: $(COMMON_SRC) src/http.c $(MINIZ_OBJ) $(RES_OBJ) | $(BUILD)
	$(CC) $(CSTD) $(CFLAGS) $(DEFINES) -DMODE_DOWNLOAD $(INCLUDES) \
	    $(COMMON_SRC) src/http.c $(MINIZ_OBJ) $(RES_OBJ) \
	    -o $@ $(LDFLAGS) $(COMMON_LIBS) -lwinhttp

# --- embedded-mode exe -------------------------------------------------------
$(BUILD)/wistribute-offline.exe: $(COMMON_SRC) src/embed.c $(MINIZ_OBJ) $(PAYLOAD) $(RES_OBJ) | $(BUILD)
	$(CC) $(CSTD) $(CFLAGS) $(DEFINES) -DMODE_EMBEDDED $(INCLUDES) \
	    $(COMMON_SRC) src/embed.c $(MINIZ_OBJ) $(RES_OBJ) \
	    -o $@ $(LDFLAGS) $(COMMON_LIBS)

clean:
	rm -rf $(BUILD)

distclean: clean
	rm -rf payload
