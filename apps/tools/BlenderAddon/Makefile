.PHONY: help lint format check test test-blender clean sync

ADDON_DIR := io_import_p3d
TEST_DIR := ../../tests/BlenderAddon
DLL ?= $(HOME)/.local/lib/PoseidonFormats.so

help: ## Show this help
	@grep -E '^[a-zA-Z_-]+:.*?## ' $(MAKEFILE_LIST) | sort | \
		awk 'BEGIN {FS = ":.*?## "}; {printf "  \033[36m%-15s\033[0m %s\n", $$1, $$2}'

sync: ## Install dev dependencies via uv
	uv sync

lint: ## Run ruff linter
	uv run ruff check $(ADDON_DIR) $(TEST_DIR)

format: ## Auto-format code with ruff
	uv run ruff format $(ADDON_DIR) $(TEST_DIR)
	uv run ruff check --fix $(ADDON_DIR) $(TEST_DIR)

check: ## Run linter + format check (CI mode, no changes)
	uv run ruff format --check $(ADDON_DIR) $(TEST_DIR)
	uv run ruff check $(ADDON_DIR) $(TEST_DIR)

test: ## Run pytest (pass DLL=path to override)
	LD_PRELOAD=$(DLL) POSEIDON_FORMATS_DLL=$(DLL) \
		uv run pytest $(TEST_DIR) --dll=$(DLL) $(PYTEST_ARGS)

test-blender: ## Run Blender integration tests (requires blender in PATH)
	@command -v blender >/dev/null 2>&1 || { echo "blender not found — skipping"; exit 0; }
	LD_PRELOAD=$(DLL) POSEIDON_FORMATS_DLL=$(DLL) blender --background --factory-startup 		--python $(TEST_DIR)/blender_rig_check.py

clean: ## Remove caches and build artifacts
	rm -rf $(ADDON_DIR)/__pycache__ $(TEST_DIR)/__pycache__ .ruff_cache .pytest_cache
