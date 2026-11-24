CC = gcc
CFLAGS = -O2 -Wall -Wextra -std=gnu11
FULL_WARNINGS = -Wpedantic -Wshadow -Wformat=2 -Wstrict-prototypes -Wmissing-prototypes

# Prefer git tags (vX.Y.Z); fall back to short hash / protocol.h default.
GIT_VERSION := $(shell git describe --tags --always --dirty 2>/dev/null)
ifneq ($(GIT_VERSION),)
CFLAGS += -DNSIMAJ_VERSION=\"$(GIT_VERSION)\"
endif

COMMON_SRC = common/frame.c common/json.c common/util.c common/firewall.c \
	common/log.c
SERVER_SRC = server/main.c server/fleet.c server/http.c server/tui.c \
	server/selfupdate.c server/ctl.c server/client.c $(COMMON_SRC)
AGENT_SRC  = agent/main.c agent/jobs.c agent/osinfo.c $(COMMON_SRC)

.PHONY: all warnings clean rebuild install-assets install-server test-local

all: nsimajd nsimaj-agent install-assets

warnings: CFLAGS += $(FULL_WARNINGS)
warnings: all

nsimajd: $(SERVER_SRC) protocol/protocol.h
	$(CC) $(CFLAGS) $(SERVER_SRC) -o $@

nsimaj-agent: $(AGENT_SRC) protocol/protocol.h
	$(CC) $(CFLAGS) $(AGENT_SRC) -o $@

install-assets: nsimaj-agent
	cp -f nsimaj-agent bootstrap/nsimaj-agent
	chmod +x bootstrap/install.sh bootstrap/nsimaj-agent \
		bootstrap/nsimajd-update.sh bootstrap/install-server.sh

install-server: all
	@echo "Run: sudo ./bootstrap/install-server.sh"
	sudo ./bootstrap/install-server.sh

clean:
	rm -f nsimajd nsimaj-agent bootstrap/nsimaj-agent

rebuild: clean all

test-local: all
	@echo "Run daemon + TUI + agent:"
	@echo "  ./nsimajd --daemon --secret nsimaj --assets ./bootstrap -y"
	@echo "  ./nsimajd"
	@echo "  ./nsimaj-agent --server 127.0.0.1 --secret nsimaj"
