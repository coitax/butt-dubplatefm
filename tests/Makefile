ROOT := $(abspath ..)
CXX ?= g++
CXXFLAGS ?= -std=c++11 -g -O1 -fsanitize=address -fno-omit-frame-pointer \
	-fno-stack-protector -ffunction-sections -fdata-sections -Wall -Wextra
CXXFLAGS += -I$(CURDIR)/support -I$(ROOT)/src -I$(ROOT)/src/FLTK \
	$(shell fltk-config --cxxflags)
LDFLAGS ?= -fsanitize=address -Wl,--gc-sections -pthread \
	$(shell fltk-config --ldflags --use-images)

PRODUCT_SRCS := \
	$(ROOT)/src/parseconfig.cpp \
	$(ROOT)/src/cfg.cpp \
	$(ROOT)/src/icecast.cpp \
	$(ROOT)/src/shoutcast.cpp \
	$(ROOT)/src/sockfuncs.cpp \
	$(ROOT)/src/util.cpp \
	$(ROOT)/src/timer.cpp \
	$(ROOT)/src/strfuncs.cpp \
	$(ROOT)/src/FLTK/fl_callbacks.cpp \
	$(ROOT)/src/FLTK/Fl_ILM216.cpp

BIN := $(CURDIR)/mount_status_overflow_test

.PHONY: all check clean

all: $(BIN)

$(BIN): mount_status_overflow_test.cpp $(PRODUCT_SRCS)
	$(CXX) $(CXXFLAGS) -o $@ mount_status_overflow_test.cpp $(PRODUCT_SRCS) $(LDFLAGS)

check: $(BIN)
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
		$(BIN) fit short
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
		$(BIN) fit gui-max
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
		$(BIN) overflow mount
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
		$(BIN) overflow user
	ASAN_OPTIONS=halt_on_error=1:abort_on_error=1:detect_leaks=0 \
		$(BIN) overflow both

clean:
	rm -f $(BIN)
