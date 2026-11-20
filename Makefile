.DEFAULT_GOAL := build

PYTHON ?= python3
SDK_RELEASE ?= latest
VENV ?= $(CURDIR)/.venv
UFBT := $(VENV)/bin/ufbt
SDK_INDEX_URL ?= https://up.unleashedflip.com/directory.json

ifeq ($(SDK_RELEASE),latest)
SDK_UPDATE_ARGS = --hw-target=f7 --index-url="$(SDK_INDEX_URL)" --channel=release
else
SDK_URL ?= https://github.com/DarkFlippers/unleashed-firmware/releases/download/$(SDK_RELEASE)/flipper-z-f7-sdk-$(SDK_RELEASE).zip
SDK_LOCAL ?= lib/flipper-z-f7-sdk-$(SDK_RELEASE).zip
endif
ifneq ($(strip $(SDK_URL)),)
SDK_UPDATE_ARGS = --hw-target=f7 --url="$(SDK_URL)"
endif

ifneq ($(strip $(SDK_LOCAL)),)
SDK_UPDATE_ARGS = --hw-target=f7 --local="$(SDK_LOCAL)"

$(SDK_LOCAL):
	mkdir -p "$(@D)"
	wget -O "$@.tmp" "$(SDK_URL)"
	mv "$@.tmp" "$@"
endif

# Separate SDK/build state by release; share the downloaded toolchain.
export UFBT_HOME := $(CURDIR)/.ufbt/$(SDK_RELEASE)
export FBT_TOOLCHAIN_PATH ?= $(CURDIR)/.ufbt
SDK_STATE := $(UFBT_HOME)/current/ufbt_state.json

.PHONY: build setup sdk help

build: setup
	"$(UFBT)"

# Follow new stable releases on every default build. uFBT reuses an unchanged SDK.
ifeq ($(SDK_RELEASE),latest)
setup: sdk
else
setup: $(SDK_STATE)
endif

$(UFBT):
	$(PYTHON) -m venv "$(VENV)"
	"$(VENV)/bin/python" -m pip install ufbt

$(SDK_STATE): $(UFBT) $(SDK_LOCAL)
	"$(UFBT)" update $(SDK_UPDATE_ARGS)

# Explicitly refresh the selected SDK, even when already cached.
sdk: $(UFBT) $(SDK_LOCAL)
	"$(UFBT)" update $(SDK_UPDATE_ARGS)

help:
	@printf '%s\n' \
	  'make                         Build for latest stable Unleashed (default)' \
	  'make SDK_RELEASE=unlshd-084  Build for Unleashed 084' \
	  'make SDK_RELEASE=unlshd-093  Build for Unleashed 093' \
	  'make setup                   Install ufbt and the selected SDK' \
	  'make sdk                     Refresh the selected SDK' \
	  'SDK_LOCAL=/path/to/sdk.zip    Use an existing SDK archive' \
	  'Output: dist/ir_boundary_lab.fap'
