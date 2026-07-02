#include "feed/feed_log.h"

#include <charconv>
#include <limits>

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

FeedLogReader::FeedLogReader(const std::string& path)
    : in_(path), buffer_(kMaxLineBytes) {
  open_ = in_.is_open();
}

std::optional<FeedLogRecord> FeedLogReader::next() {
  while (in_.good()) {
    // Bounded C-style getline: a line that does not fit the buffer sets
    // failbit without eofbit, and is discarded below instead of being
    // slurped into an unbounded std::string.
    in_.getline(buffer_.data(), static_cast<std::streamsize>(buffer_.size()));
    const auto extracted = in_.gcount();
    if (in_.fail()) {
      if (in_.eof() || in_.bad()) return std::nullopt;  // clean end / IO error
      in_.clear();
      in_.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      ++malformed_;
      continue;
    }
    // gcount includes the extracted-but-not-stored newline, except when the
    // final line ends at EOF without one.
    const auto length =
        static_cast<std::size_t>(extracted) - (in_.eof() ? 0 : 1);
    const std::string_view line(buffer_.data(), length);

    const auto tab1 = line.find('\t');
    const auto tab2 = (tab1 == std::string_view::npos)
                          ? std::string_view::npos
                          : line.find('\t', tab1 + 1);
    if (tab2 == std::string_view::npos) {
      ++malformed_;
      continue;
    }
    FeedLogRecord record;
    const char* begin = line.data();
    const char* end = begin + tab1;
    const auto [ptr, ec] = std::from_chars(begin, end, record.recv_ns);
    if (ec != std::errc{} || ptr != end || record.recv_ns < 0) {
      ++malformed_;
      continue;
    }
    const auto venue =
        model::venue_from_string(line.substr(tab1 + 1, tab2 - tab1 - 1));
    if (!venue) {
      ++malformed_;
      continue;
    }
    record.venue = *venue;
    record.payload = std::string(line.substr(tab2 + 1));
    return record;
  }
  return std::nullopt;
}

}  // namespace basis::feed
