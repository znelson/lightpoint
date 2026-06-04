#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace FsHelpers {

std::string normalisePath(const std::string& path);

void sortFileList(std::vector<std::string>& strs);

/**
 * Check if the given filename ends with the specified extension (case-insensitive).
 */
bool checkFileExtension(std::string_view fileName, const char* extension);

// Check for either .jpg or .jpeg extension (case-insensitive)
bool hasJpgExtension(std::string_view fileName);

// Check for .png extension (case-insensitive)
bool hasPngExtension(std::string_view fileName);

// Check for .bmp extension (case-insensitive)
bool hasBmpExtension(std::string_view fileName);

// Check for .gif extension (case-insensitive)
bool hasGifExtension(std::string_view fileName);

// Check for .epub extension (case-insensitive)
bool hasEpubExtension(std::string_view fileName);

// Check for either .xtc or .xtch extension (case-insensitive)
bool hasXtcExtension(std::string_view fileName);

// Check for .txt extension (case-insensitive)
bool hasTxtExtension(std::string_view fileName);

// Check for .md extension (case-insensitive)
bool hasMarkdownExtension(std::string_view fileName);

// Check for .css extension (case-insensitive)
bool hasCssExtension(std::string_view fileName);

std::string extractFolderPath(const std::string& filePath);

/**
 * Sanitize a filename/path component for FAT32 in a caller-provided buffer.
 * Replaces invalid path characters, spaces, and control characters with '-'.
 */
void sanitizePathComponentForFat32(const char* input, char* output, size_t maxLen);

}  // namespace FsHelpers
