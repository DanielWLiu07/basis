#include "feed/feed_log.h"

#include <charconv>

namespace basis::feed {

FeedLogWriter::FeedLogWriter(const std::string& path)
    : out_(path, std::ios::out | std::ios::trunc) {}

bool FeedLogWriter::write(const FeedLogRecord& record) {
  if (!out_.good()) return false;
  if (record.payload.find('\n') != std::string::npos) return false;
  out_ << record.recv_ns << '\t' << model::to_string(record.venue) << '\t'
       << record.payload << '\n';
  return out_.good();
}

FeedLogReader::FeedLogReader(const std::string& path) : in_(path) {
  open_ = in_.is_open();
}

std::optional<FeedLogRecord> FeedLogReader::next() {
  std::string line;
  while (std::getline(in_, line)) {
    const auto tab1 = line.find('\t');
    const auto tab2 = (tab1 == std::string::npos)
                          ? std::string::npos
                          : line.find('\t', tab1 + 1);
    if (tab2 == std::string::npos) {
      ++malformed_;
      continue;
    }
    FeedLogRecord record;
    const char* begin = line.data();
    const char* end = begin + tab1;
    const auto [ptr, ec] = std::from_chars(begin, end, record.recv_ns);
    if (ec != std::errc{} || ptr != end) {
      ++malformed_;
      continue;
    }
    const auto venue = model::venue_from_string(
        std::string_view(line).substr(tab1 + 1, tab2 - tab1 - 1));
    if (!venue) {
      ++malformed_;
      continue;
    }
    record.venue = *venue;
    record.payload = line.substr(tab2 + 1);
    return record;
  }
  return std::nullopt;
}

}  // namespace basis::feed
