# Makefile: Windows (MinGW) -> downloader.exe (Win32 GUI),
#          Linux/macOS      -> downloader (консольный).
# Использование: make        (собрать)
#                make clean  (убрать бинарник)

UNAME_S := $(shell uname -s 2>/dev/null)

ifeq ($(OS),Windows_NT)
  IS_WIN := 1
endif
ifneq (,$(findstring MINGW,$(UNAME_S)))
  IS_WIN := 1
endif
ifneq (,$(findstring MSYS,$(UNAME_S)))
  IS_WIN := 1
endif

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

ifdef IS_WIN
  TARGET := downloader.exe
  SRCS := src/dl_port.c src/dl_core.c src/dl_hist.c src/main_win32.c
  LIBS := -lws2_32 -lcomctl32
  RM := del /Q 2>NUL
else
  TARGET := downloader
  SRCS := src/dl_port.c src/dl_core.c src/dl_hist.c src/main_linux.c
  CFLAGS += -pthread -D_FILE_OFFSET_BITS=64
  LIBS := -pthread
  RM := rm -f
endif

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $(SRCS) $(LIBS)

clean:
	-$(RM) $(TARGET)

.PHONY: all clean
