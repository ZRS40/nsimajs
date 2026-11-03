CC = gcc
TARGET = app
SRC = main.c network.c arp.c sshcon.c

CFLAGS = -O2
FULL_WARNINGS = -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes -Werror

.PHONY: all warnings clean rebuild

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(SRC) -o $(TARGET)

warnings:
	$(CC) $(CFLAGS) $(FULL_WARNINGS) $(SRC) -o $(TARGET)

clean:
	rm -f $(TARGET)

rebuild: clean all
