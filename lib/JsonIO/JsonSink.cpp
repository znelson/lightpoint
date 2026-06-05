#include "JsonSink.h"

#include <HalStorage.h>

#include <cstring>

void HalFileSink::write(const char* data, size_t len) {
  // Sticky failure: once a flush short-writes, drop subsequent bytes. Also
  // drop writes after close() so a stale reference can't push more bytes
  // into a finalized file.
  if (!ok_ || closed_) return;

  // Route every byte through buf_ so the on-the-wire chunk size is
  // predictable (BUF_SIZE). Inputs larger than BUF_SIZE fan out across
  // multiple flushes naturally.
  while (len > 0) {
    if (used_ == BUF_SIZE) {
      flushIfNonEmpty();
      if (!ok_) return;
    }
    const size_t room = BUF_SIZE - used_;
    const size_t copy = len < room ? len : room;
    memcpy(buf_ + used_, data, copy);
    used_ += copy;
    data += copy;
    len -= copy;
  }
}

bool HalFileSink::close() {
  if (closed_) return ok_;
  flushIfNonEmpty();
  closed_ = true;
  return ok_;
}

HalFileSink::~HalFileSink() {
  // Last-chance flush for the "forgot to call close()" case. We can't
  // surface the result here -- the caller is gone -- but the file at least
  // doesn't lose its tail bytes silently.
  if (!closed_) flushIfNonEmpty();
}

void HalFileSink::flushIfNonEmpty() {
  if (used_ == 0) return;
  const size_t written = file_.write(buf_, used_);
  if (written != used_) {
    ok_ = false;
  }
  used_ = 0;
}
