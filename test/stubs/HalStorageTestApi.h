#pragma once

// Test-only API for the host-side HalStorage stub. Tests include this header
// alongside the real <HalStorage.h> to seed read content for paths that
// halStorage.openFileForRead() should then serve back as readable HalFiles.

#include <HalStorage.h>

#include <string>

namespace test_stubs {

// Pre-populate `path` so the next halStorage.openFileForRead(path, ...) call
// succeeds and yields a HalFile that reads back `content` byte-for-byte.
void seedHalFileContent(const std::string& path, std::string content);

// Drop all previously-seeded content. Useful between tests for isolation.
void clearHalFileContent();

// Convenience: seed a unique synthetic path and return an already-open
// readable HalFile.
HalFile makeReadableHalFile(std::string content);

}  // namespace test_stubs
