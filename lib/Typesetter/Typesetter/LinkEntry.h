#pragma once

#include <cstring>

// Generic on-page navigation target: a short visible label paired with an
// href / anchor string the reader resolves on activation. EPUB stores
// footnote markers in this shape (label = "1", href = "#fn1"); Markdown
// inline links will use the same shape (label = "click here",
// href = "#section-name" or "https://..."). Fixed-size char arrays keep
// the struct a POD that serializes by raw byte copy.
#define LINK_LABEL_LEN 32
#define LINK_HREF_LEN 96

struct LinkEntry {
  char label[LINK_LABEL_LEN];
  char href[LINK_HREF_LEN];

  LinkEntry() {
    label[0] = '\0';
    href[0] = '\0';
  }
};
