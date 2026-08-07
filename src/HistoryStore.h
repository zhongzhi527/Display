#pragma once

#include <Arduino.h>

namespace ScaleHistory {

constexpr uint16_t MAX_RECORDS = 100;
constexpr size_t NAME_CAPACITY = 51;      // 50 bytes plus terminator.
constexpr size_t NOTES_CAPACITY = 501;    // 500 bytes plus terminator.
constexpr size_t TIMEZONE_CAPACITY = 41;  // IANA zone, e.g. Asia/Singapore.

enum class Result : uint8_t {
  Ok,
  NotReady,
  NotFound,
  Conflict,
  IoError,
};

// Fixed-size on-flash record. The weight snapshot and original log time are
// immutable; only name, notes, and updatedAtMs are changed by editRecord().
struct __attribute__((packed)) Record {
  uint32_t marker = 0;
  uint32_t id = 0;
  uint64_t loggedAtMs = 0;
  uint64_t updatedAtMs = 0;
  int16_t timezoneOffsetMinutes = 0;
  uint8_t combinedMode = 0;
  uint8_t includedMask = 0;
  uint8_t connectionMask = 0;
  uint8_t reserved = 0;
  uint16_t revision = 1;
  int32_t totalGrams = 0;
  int32_t scaleGrams[2] = {};
  uint8_t scaleStatus[2] = {};
  uint8_t scaleFlags[2] = {};
  char timezone[TIMEZONE_CAPACITY] = {};
  char name[NAME_CAPACITY] = {};
  char notes[NOTES_CAPACITY] = {};
  uint32_t checksum = 0;
};

class Store {
public:
  using RecordVisitor = bool (*)(const Record &record, void *context);

  bool begin();
  bool ready() const { return ready_; }
  uint16_t count() const { return header_.activeCount; }
  uint16_t capacity() const { return MAX_RECORDS; }

  Result add(Record &record);
  Result get(uint32_t id, Record &record);
  Result edit(uint32_t id,
              uint16_t expectedRevision,
              const char *name,
              const char *notes,
              uint64_t updatedAtMs);
  Result remove(uint32_t id);
  Result clear();

  // Visits valid records newest-first while holding a single filesystem file
  // open. Returning false from the callback stops the visit successfully.
  Result visitNewest(RecordVisitor visitor, void *context);

  // Returns IDs newest-first. Offset supports low-memory API pagination.
  size_t listIds(uint32_t *ids, size_t maximum, size_t offset = 0);

  // Public only so the implementation can checksum the exact persisted
  // representation; callers should treat this as an internal file header.
  struct __attribute__((packed)) Header {
    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t headerSize = 0;
    uint16_t recordSize = 0;
    uint16_t capacity = 0;
    uint16_t activeCount = 0;
    uint16_t reserved = 0;
    uint32_t nextId = 1;
    uint32_t generation = 0;
    uint32_t checksum = 0;
  };

  static constexpr uint8_t HEADER_COPIES = 2;

private:

  bool initializeFile();
  bool readHeaderAt(uint8_t slot, Header &header);
  bool persistHeader();
  bool validateHeader(const Header &header) const;
  Result readRecordAt(uint16_t index, Record &record);
  bool writeRecordAt(uint16_t index, Record &record);
  Result findRecord(uint32_t id,
                    uint16_t &index,
                    Record *record = nullptr);
  bool rescanAndRepairHeader();

  Header header_;
  uint8_t activeHeaderSlot_ = 0;
  bool ready_ = false;
};

} // namespace ScaleHistory
