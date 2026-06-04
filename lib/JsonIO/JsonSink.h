#pragma once

#include <cstddef>
#include <string>

// Byte sink interface for JsonWriter. The writer pushes bytes here as it
// builds a document; concrete sinks decide where they land.
//
// Two design points worth noting:
//   1. Failure is sticky. Once a sink's ok() returns false (e.g. a flush to
//      disk failed), subsequent write() calls remain valid no-ops and ok()
//      continues to return false. Callers check ok() after the writer is
//      done rather than gating each write.
//   2. The interface is intentionally byte-oriented, not JSON-aware. JSON
//      structure (escaping, comma placement, depth) lives in JsonWriter; a
//      sink just appends what it's given.
class JsonSink {
 public:
  virtual ~JsonSink() = default;
  // Append `len` bytes. Implementations buffer or flush as they see fit.
  virtual void write(const char* data, size_t len) = 0;
  // True if every write so far has succeeded. Sinks that never fail (e.g.
  // StringSink) return true unconditionally.
  virtual bool ok() const = 0;
};

// Appends into a caller-owned std::string. Used by in-memory consumers (web
// settings API, host tests, anywhere the bytes need to be available as one
// contiguous owned string). Always ok() -- std::string append doesn't fail
// in practice; on bad_alloc the program is aborting anyway under
// -fno-exceptions.
class StringSink final : public JsonSink {
 public:
  explicit StringSink(std::string& out) : out_(out) {}
  void write(const char* data, size_t len) override { out_.append(data, len); }
  bool ok() const override { return true; }

 private:
  std::string& out_;
};

class HalFile;

// Streams writes through a fixed-size stack buffer to a HalFile, flushing
// on overflow and on destruction. Peak heap during a save drops from "full
// document size" to "BUF_SIZE + HalFile's own buffer." Use the typical
// scoping pattern so the destructor's tail-flush runs before ok() is
// checked:
//   {
//     HalFile f;
//     if (!halStorage.openFileForWrite("CPS", path, f)) return false;
//     HalFileSink sink(f);
//     buildXxxJson(obj, sink);
//   }  // sink flushes here
//   if (!sink.ok()) ... // can't -- sink is out of scope; check inside or
//                      //  use explicit close() (preferred)
//
// In practice saveXxx() uses sink.close() to flush and return status without
// relying on destructor ordering. close() is idempotent.
class HalFileSink final : public JsonSink {
 public:
  static constexpr size_t BUF_SIZE = 256;

  explicit HalFileSink(HalFile& file) : file_(file), used_(0), ok_(true), closed_(false) {}
  ~HalFileSink() override;

  HalFileSink(const HalFileSink&) = delete;
  HalFileSink& operator=(const HalFileSink&) = delete;
  HalFileSink(HalFileSink&&) = delete;
  HalFileSink& operator=(HalFileSink&&) = delete;

  void write(const char* data, size_t len) override;
  bool ok() const override { return ok_; }

  // Flush the tail buffer and mark the sink closed. Returns ok() after the
  // flush. Subsequent writes become no-ops. Calling close() is preferred
  // over relying on the destructor so callers can inspect the result.
  bool close();

 private:
  void flushIfNonEmpty();

  HalFile& file_;
  char buf_[BUF_SIZE] = {};
  size_t used_;
  bool ok_;
  bool closed_;
};
