# Makefile: Windows (MinGW) -> downloader.exe (Win32 GUI + HTTPS/BearSSL),
#          Linux/macOS      -> downloader (консольный + HTTPS/BearSSL).
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

BEAR_INC := third_party/bearssl/inc
BEAR_SRCINC := third_party/bearssl/src
BEAR_SRCS := $(wildcard third_party/bearssl/src/*/*.c) \
             $(wildcard third_party/bearssl/src/*.c)
CFLAGS += -I$(BEAR_INC) -I$(BEAR_SRCINC)

CORE_SRCS := src/dl_port.c src/dl_core.c src/dl_hist.c src/tls_bear.c

ifdef IS_WIN
  TARGET := downloader.exe
  SRCS := $(CORE_SRCS) src/main_win32.c $(BEAR_SRCS)
  LIBS := -lws2_32 -lcomctl32 -lcomdlg32 -lgdi32 -lshell32 -ladvapi32 -lcrypt32
  RM := del /Q 2>NUL
else
  TARGET := downloader
  SRCS := $(CORE_SRCS) src/main_linux.c $(BEAR_SRCS)
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
