// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_DNS_DNS_RESPONSE_H_
#define NET_DNS_DNS_RESPONSE_H_

#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

#include "base/macros.h"
#include "base/memory/ref_counted.h"
#include "base/strings/string_piece.h"
#include "net/base/net_export.h"
#include "net/dns/dns_response_result_extractor.h"
#include "net/dns/public/dns_protocol.h"
#include "third_party/abseil-cpp/absl/types/optional.h"

namespace base {
class BigEndianWriter;
}  // namespace base

namespace net {

class DnsQuery;
class IOBuffer;

namespace dns_protocol {
struct Header;
}  // namespace dns_protocol

// Structure representing a Resource Record as specified in RFC 1035, Section
// 4.1.3.
struct NET_EXPORT_PRIVATE DnsResourceRecord {
  DnsResourceRecord();
  explicit DnsResourceRecord(const DnsResourceRecord& other);
  DnsResourceRecord(DnsResourceRecord&& other);
  ~DnsResourceRecord();

  DnsResourceRecord& operator=(const DnsResourceRecord& other);
  DnsResourceRecord& operator=(DnsResourceRecord&& other);

  // A helper to set |owned_rdata| that also sets |rdata| to point to it. The
  // |value| must be non-empty. See the definition of |owned_rdata| below.
  void SetOwnedRdata(std::string value);

  // NAME (variable length) + TYPE (2 bytes) + CLASS (2 bytes) + TTL (4 bytes) +
  // RDLENGTH (2 bytes) + RDATA (variable length)
  //
  // Uses |owned_rdata| for RDATA if non-empty.
  size_t CalculateRecordSize() const;

  std::string name;  // in dotted form
  uint16_t type = 0;
  uint16_t klass = 0;
  uint32_t ttl = 0;
  // Points to the original response buffer or otherwise to |owned_rdata|.
  base::StringPiece rdata;
  // Used to construct a DnsResponse from data. This field is empty if |rdata|
  // points to the response buffer.
  std::string owned_rdata;
};

// Iterator to walk over resource records of the DNS response packet.
class NET_EXPORT_PRIVATE DnsRecordParser {
 public:
  // Construct an uninitialized iterator.
  DnsRecordParser();

  // Construct an iterator to process the `packet` of given `length`.
  // `offset` points to the beginning of the answer section. `ReadRecord()` will
  // fail if called more than `num_records` times, no matter whether or not
  // there is additional data at the end of the buffer that may appear to be a
  // valid record.
  DnsRecordParser(const void* packet,
                  size_t length,
                  size_t offset,
                  size_t num_records);

  // Returns |true| if initialized.
  bool IsValid() const { return packet_ != nullptr; }

  // Returns |true| if no more bytes remain in the packet.
  bool AtEnd() const { return cur_ == packet_ + length_; }

  // Returns current offset into the packet.
  size_t GetOffset() const { return cur_ - packet_; }

  // Parses a (possibly compressed) DNS name from the packet starting at
  // |pos|. Stores output (even partial) in |out| unless |out| is NULL. |out|
  // is stored in the dotted form, e.g., "example.com". Returns number of bytes
  // consumed or 0 on failure.
  // This is exposed to allow parsing compressed names within RRDATA for TYPEs
  // such as NS, CNAME, PTR, MX, SOA.
  // See RFC 1035 section 4.1.4.
  unsigned ReadName(const void* pos, std::string* out) const;

  // Parses the next resource record into |record|. Returns true if succeeded.
  bool ReadRecord(DnsResourceRecord* record);

  // Read a question section, returns true if succeeded. In `DnsResponse`,
  // expected to be called during parse, after which the current offset will be
  // after all questions.
  bool ReadQuestion(std::string& out_dotted_qname, uint16_t& out_qtype);

 private:
  const char* packet_ = nullptr;
  size_t length_ = 0;
  size_t num_records_ = 0;
  size_t num_records_parsed_ = 0;
  // Current offset within the packet.
  const char* cur_ = nullptr;
};

// Buffer-holder for the DNS response allowing easy access to the header fields
// and resource records. After reading into |io_buffer| must call InitParse to
// position the RR parser.
class NET_EXPORT_PRIVATE DnsResponse {
 public:
  // Constructs a response buffer large enough to store one byte more than
  // largest possible response, to detect malformed responses.
  DnsResponse();

  // Constructs a response message from `answers` and the originating `query`.
  // After the successful construction, and the parser is also initialized.
  //
  // If `validate_records` is false, DCHECKs validating the correctness of
  // records will be skipped. Intended for tests to allow creation of malformed
  // responses.
  DnsResponse(uint16_t id,
              bool is_authoritative,
              const std::vector<DnsResourceRecord>& answers,
              const std::vector<DnsResourceRecord>& authority_records,
              const std::vector<DnsResourceRecord>& additional_records,
              const absl::optional<DnsQuery>& query,
              uint8_t rcode = dns_protocol::kRcodeNOERROR,
              bool validate_records = true);

  // Constructs a response buffer of given length. Used for TCP transactions.
  explicit DnsResponse(size_t length);

  // Constructs a response from the passed buffer.
  DnsResponse(scoped_refptr<IOBuffer> buffer, size_t size);

  // Constructs a response from |data|. Used for testing purposes only!
  DnsResponse(const void* data, size_t length, size_t answer_offset);

  // Move-only.
  DnsResponse(DnsResponse&& other);
  DnsResponse& operator=(DnsResponse&& other);

  ~DnsResponse();

  // Internal buffer accessor into which actual bytes of response will be
  // read.
  IOBuffer* io_buffer() { return io_buffer_.get(); }
  const IOBuffer* io_buffer() const { return io_buffer_.get(); }

  // Size of the internal buffer.
  size_t io_buffer_size() const { return io_buffer_size_; }

  // Assuming the internal buffer holds |nbytes| bytes, returns true iff the
  // packet matches the |query| id and question. This should only be called if
  // the response is constructed from a raw buffer.
  bool InitParse(size_t nbytes, const DnsQuery& query);

  // Assuming the internal buffer holds |nbytes| bytes, initialize the parser
  // without matching it against an existing query. This should only be called
  // if the response is constructed from a raw buffer.
  bool InitParseWithoutQuery(size_t nbytes);

  // Does not require the response to be fully parsed and valid, but will return
  // nullopt if the ID is unknown. The ID will only be known if the response is
  // successfully constructed from data or if InitParse...() has been able to
  // parse at least as far as the ID (not necessarily a fully successful parse).
  absl::optional<uint16_t> id() const;

  // Returns true if response is valid, that is, after successful InitParse, or
  // after successful construction of a new response from data.
  bool IsValid() const;

  // All of the methods below are valid only if the response is valid.

  // Accessors for the header.
  uint16_t flags() const;  // excluding rcode
  uint8_t rcode() const;

  unsigned question_count() const;
  unsigned answer_count() const;
  unsigned authority_count() const;
  unsigned additional_answer_count() const;

  const std::vector<uint16_t>& qtypes() const {
    DCHECK(parser_.IsValid());
    DCHECK_EQ(question_count(), qtypes_.size());
    return qtypes_;
  }
  const std::vector<std::string>& dotted_qnames() const {
    DCHECK(parser_.IsValid());
    DCHECK_EQ(question_count(), dotted_qnames_.size());
    return dotted_qnames_;
  }

  // Shortcuts to get qtype or qname for single-query responses. Should only be
  // used in cases where there is known to be exactly one question (e.g. because
  // that has been validated by `InitParse()`).
  uint16_t GetSingleQType() const;
  base::StringPiece GetSingleDottedName() const;

  // Returns an iterator to the resource records in the answer section.
  // The iterator is valid only in the scope of the DnsResponse.
  // This operation is idempotent.
  DnsRecordParser Parser() const;

 private:
  bool WriteHeader(base::BigEndianWriter* writer,
                   const dns_protocol::Header& header);
  bool WriteQuestion(base::BigEndianWriter* writer, const DnsQuery& query);
  bool WriteRecord(base::BigEndianWriter* writer,
                   const DnsResourceRecord& record,
                   bool validate_record);
  bool WriteAnswer(base::BigEndianWriter* writer,
                   const DnsResourceRecord& answer,
                   const absl::optional<DnsQuery>& query,
                   bool validate_record);

  // Convenience for header access.
  const dns_protocol::Header* header() const;

  // Buffer into which response bytes are read.
  scoped_refptr<IOBuffer> io_buffer_;

  // Size of the buffer.
  size_t io_buffer_size_;

  // Iterator constructed after InitParse positioned at the answer section.
  // It is never updated afterwards, so can be used in accessors.
  DnsRecordParser parser_;
  bool id_available_ = false;
  std::vector<std::string> dotted_qnames_;
  std::vector<uint16_t> qtypes_;
};

}  // namespace net

#endif  // NET_DNS_DNS_RESPONSE_H_
