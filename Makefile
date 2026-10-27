CMAKE ?= cmake
CTEST ?= ctest
JOBS ?= 2
BUILD_DIR ?= build-debug
RELEASE_DIR ?= build-release
SANITIZE_DIR ?= build-sanitize

.PHONY: all debug test test-buffer-pool release sanitize

all: debug

debug:
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
	$(CMAKE) --build $(BUILD_DIR) --parallel $(JOBS)

test: debug
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure --no-tests=error

test-buffer-pool: debug
	$(CTEST) --test-dir $(BUILD_DIR) -R '^unit_buffer_pool$$' --output-on-failure --no-tests=error

release:
	$(CMAKE) -S . -B $(RELEASE_DIR) -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
	$(CMAKE) --build $(RELEASE_DIR) --parallel $(JOBS)
	$(CTEST) --test-dir $(RELEASE_DIR) --output-on-failure --no-tests=error

sanitize:
	$(CMAKE) -S . -B $(SANITIZE_DIR) -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON -DIDBS_ENABLE_SANITIZERS=ON
	$(CMAKE) --build $(SANITIZE_DIR) --parallel $(JOBS)
	$(CTEST) --test-dir $(SANITIZE_DIR) --output-on-failure --no-tests=error
