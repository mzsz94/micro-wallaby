SHELL := /bin/bash

.PHONY: deps setup detect build flash-check flash monitor web-install web-dev web-build clean shell

deps:
	brew install cmake ninja dtc ccache python@3.12

setup:
	./scripts/setup.sh

detect:
	./scripts/detect-board.sh

build:
	./scripts/build.sh

flash-check: build
	DRY_RUN=1 ./scripts/flash.sh

flash: build
	./scripts/flash.sh

monitor:
	./scripts/monitor.sh

web-install:
	cd web && npm install

web-dev:
	cd web && npm run dev

web-build:
	cd web && npm run build

shell:
	@echo 'source .venv/bin/activate'
	@echo 'export ZEPHYR_BASE="$(CURDIR)/zephyr"'
	@echo 'export ZEPHYR_SDK_INSTALL_DIR="$(CURDIR)/.zephyr-sdk"'

clean:
	rm -rf "$(CURDIR)/build"
