IDF_PATH  ?= $(HOME)/esp/esp-idf
IDF_VENV  ?= $(HOME)/.espressif/python_env/idf6.0_py3.13_env
IDF_PY    := $(IDF_VENV)/bin/python3
IDF_SCRIPT := $(IDF_PATH)/tools/idf.py
SHIM      := /tmp/idf-pyshim
PY313     := $(shell command -v python3.13 2>/dev/null)
PORT      ?= /dev/cu.usbmodem211101

IDFENV = \
	mkdir -p $(SHIM) && \
	ln -sf "$(PY313)" $(SHIM)/python && \
	ln -sf "$(PY313)" $(SHIM)/python3 && \
	export IDF_PATH="$(IDF_PATH)" && \
	export IDF_PYTHON_ENV_PATH="$(IDF_VENV)" && \
	export PATH="$(SHIM):$$PATH" && \
	. "$(IDF_PATH)/export.sh" >/dev/null 2>&1 &&

.PHONY: build flash monitor run set-target

set-target:
	$(IDFENV) $(IDF_PY) $(IDF_SCRIPT) set-target esp32s3

build:
	$(IDFENV) $(IDF_PY) $(IDF_SCRIPT) build

flash:
	$(IDFENV) $(IDF_PY) $(IDF_SCRIPT) -p $(PORT) flash

monitor:
	$(IDFENV) $(IDF_PY) $(IDF_SCRIPT) -p $(PORT) monitor

run: build flash monitor
