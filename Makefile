# ThreadStorm Makefile for Windows + GCC (MSYS2/MinGW)
# Usage:
#   make          – build the main server
#   make test     – build and run the test suite
#   make clean    – remove build artefacts

CC      = gcc
CFLAGS  = -Wall -Wextra -std=c11 -O2 -Iinclude
LDFLAGS = -lws2_32 -lpsapi

SRC_DIR  = src
INC_DIR  = include
BIN_DIR  = bin

SRCS = $(SRC_DIR)/main.c \
       $(SRC_DIR)/common.c \
       $(SRC_DIR)/server.c \
       $(SRC_DIR)/request_queue.c \
       $(SRC_DIR)/thread_pool.c \
       $(SRC_DIR)/metrics.c \
       $(SRC_DIR)/sync_lab.c \
       $(SRC_DIR)/benchmark.c

TARGET  = $(BIN_DIR)/threadstorm.exe
TEST_TARGET = $(BIN_DIR)/test_suite.exe

TEST_SRCS = tests/test_suite.c \
            $(SRC_DIR)/common.c \
            $(SRC_DIR)/server.c \
            $(SRC_DIR)/request_queue.c \
            $(SRC_DIR)/thread_pool.c \
            $(SRC_DIR)/metrics.c \
            $(SRC_DIR)/sync_lab.c \
            $(SRC_DIR)/benchmark.c

.PHONY: all test clean dirs

all: dirs $(TARGET)
	@echo.
	@echo Build complete.  Run:  $(TARGET)
	@echo Dashboard:        http://127.0.0.1:8080/
	@echo.

dirs:
	@if not exist $(BIN_DIR) mkdir $(BIN_DIR)
	@if not exist logs mkdir logs

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test: dirs $(TARGET) $(TEST_TARGET)
	@echo.
	@echo Running test suite...
	@echo.
	$(TEST_TARGET)

$(TEST_TARGET): $(TEST_SRCS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	@if exist $(BIN_DIR) rmdir /s /q $(BIN_DIR)
	@echo Cleaned.

