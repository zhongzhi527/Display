#include "HistoryStore.h"

#include <FS.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <stddef.h>
#include <string.h>

namespace ScaleHistory {
namespace {

constexpr char HISTORY_PATH[] = "/weight-history.bin";
constexpr uint32_t HEADER_MAGIC = 0x48535431UL; // HST1
constexpr uint16_t FORMAT_VERSION = 1;
constexpr uint32_t RECORD_MARKER = 0x4C4F4731UL; // LOG1
constexpr char HISTORY_META_NAMESPACE[] = "hist_meta";
constexpr char FILESYSTEM_INITIALIZED_KEY[] = "fs_init";

uint32_t checksumBytes(const void *data, size_t length) {
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  uint32_t hash = 2166136261UL;
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= 16777619UL;
  }
  return hash;
}

uint32_t headerChecksum(const Store::Header &header) {
  return checksumBytes(&header, offsetof(Store::Header, checksum));
}

uint32_t recordChecksum(const Record &record) {
  return checksumBytes(&record, offsetof(Record, checksum));
}

size_t recordOffset(uint16_t index) {
  return sizeof(Store::Header) * Store::HEADER_COPIES +
         static_cast<size_t>(index) * sizeof(Record);
}

size_t expectedFileSize() {
  return sizeof(Store::Header) * Store::HEADER_COPIES +
         static_cast<size_t>(MAX_RECORDS) * sizeof(Record);
}

bool generationNewer(uint32_t left, uint32_t right) {
  return static_cast<int32_t>(left - right) > 0;
}

enum class SlotReadResult : uint8_t {
  Valid,
  EmptyOrCorrupt,
  IoError,
};

SlotReadResult readRecordFromFile(File &file,
                                  uint16_t index,
                                  Record &record) {
  if (!file || index >= MAX_RECORDS ||
      !file.seek(recordOffset(index), SeekSet) ||
      file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record)) !=
          sizeof(record)) {
    return SlotReadResult::IoError;
  }
  if (record.marker != RECORD_MARKER || record.id == 0 ||
      record.revision == 0 ||
      record.timezone[TIMEZONE_CAPACITY - 1] != '\0' ||
      record.name[NAME_CAPACITY - 1] != '\0' ||
      record.notes[NOTES_CAPACITY - 1] != '\0' ||
      record.checksum != recordChecksum(record)) {
    return SlotReadResult::EmptyOrCorrupt;
  }
  return SlotReadResult::Valid;
}

} // namespace

static_assert(sizeof(Record) < 700,
              "History records must remain compact.");

bool Store::validateHeader(const Header &header) const {
  return header.magic == HEADER_MAGIC &&
         header.version == FORMAT_VERSION &&
         header.headerSize == sizeof(Header) &&
         header.recordSize == sizeof(Record) &&
         header.capacity == MAX_RECORDS &&
         header.activeCount <= MAX_RECORDS &&
         header.nextId != 0 &&
         header.checksum == headerChecksum(header);
}

bool Store::readHeaderAt(uint8_t slot, Header &header) {
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file ||
      !file.seek(static_cast<size_t>(slot) * sizeof(Header), SeekSet) ||
      file.read(reinterpret_cast<uint8_t *>(&header), sizeof(Header)) !=
          sizeof(Header)) {
    return false;
  }
  return validateHeader(header);
}

bool Store::persistHeader() {
  Header next = header_;
  ++next.generation;
  if (next.generation == 0) {
    next.generation = 1;
  }
  next.checksum = headerChecksum(next);
  const uint8_t targetSlot = (activeHeaderSlot_ + 1U) % HEADER_COPIES;

  File file = LittleFS.open(HISTORY_PATH, "r+");
  if (!file ||
      !file.seek(static_cast<size_t>(targetSlot) * sizeof(Header), SeekSet) ||
      file.write(reinterpret_cast<const uint8_t *>(&next), sizeof(Header)) !=
          sizeof(Header)) {
    return false;
  }
  file.flush();
  file.close();
  header_ = next;
  activeHeaderSlot_ = targetSlot;
  return true;
}

bool Store::initializeFile() {
  LittleFS.remove(HISTORY_PATH);
  File file = LittleFS.open(HISTORY_PATH, "w");
  if (!file) {
    return false;
  }

  Header first;
  first.magic = HEADER_MAGIC;
  first.version = FORMAT_VERSION;
  first.headerSize = sizeof(Header);
  first.recordSize = sizeof(Record);
  first.capacity = MAX_RECORDS;
  first.activeCount = 0;
  first.nextId = 1;
  first.generation = 1;
  first.checksum = headerChecksum(first);

  Header second = first;
  second.generation = 2;
  second.checksum = headerChecksum(second);
  if (file.write(reinterpret_cast<const uint8_t *>(&first), sizeof(first)) !=
          sizeof(first) ||
      file.write(reinterpret_cast<const uint8_t *>(&second), sizeof(second)) !=
          sizeof(second)) {
    file.close();
    return false;
  }

  Record blank;
  for (uint16_t index = 0; index < MAX_RECORDS; ++index) {
    if (file.write(reinterpret_cast<const uint8_t *>(&blank), sizeof(blank)) !=
        sizeof(blank)) {
      file.close();
      return false;
    }
  }
  file.flush();
  const bool complete = file.size() == expectedFileSize();
  file.close();
  if (!complete) {
    return false;
  }
  header_ = second;
  activeHeaderSlot_ = 1;
  return true;
}

Result Store::readRecordAt(uint16_t index, Record &record) {
  if (index >= MAX_RECORDS) {
    return Result::NotFound;
  }
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file) {
    return Result::IoError;
  }
  const SlotReadResult result = readRecordFromFile(file, index, record);
  if (result == SlotReadResult::Valid) {
    return Result::Ok;
  }
  return result == SlotReadResult::IoError ? Result::IoError
                                           : Result::NotFound;
}

bool Store::writeRecordAt(uint16_t index, Record &record) {
  if (index >= MAX_RECORDS) {
    return false;
  }
  record.checksum = recordChecksum(record);
  File file = LittleFS.open(HISTORY_PATH, "r+");
  if (!file || !file.seek(recordOffset(index), SeekSet) ||
      file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) !=
          sizeof(record)) {
    return false;
  }
  file.flush();
  return true;
}

Result Store::findRecord(uint32_t id,
                         uint16_t &index,
                         Record *record) {
  if (id == 0) {
    return Result::NotFound;
  }
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file) {
    return Result::IoError;
  }
  Record candidate;
  for (uint16_t slot = 0; slot < MAX_RECORDS; ++slot) {
    const SlotReadResult readResult =
        readRecordFromFile(file, slot, candidate);
    if (readResult == SlotReadResult::IoError) {
      return Result::IoError;
    }
    if (readResult != SlotReadResult::Valid || candidate.id != id) {
      continue;
    }
    index = slot;
    if (record != nullptr) {
      *record = candidate;
    }
    return Result::Ok;
  }
  return Result::NotFound;
}

bool Store::rescanAndRepairHeader() {
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file) {
    return false;
  }
  uint16_t activeCount = 0;
  uint32_t maximumId = 0;
  Record record;
  for (uint16_t index = 0; index < MAX_RECORDS; ++index) {
    const SlotReadResult readResult =
        readRecordFromFile(file, index, record);
    if (readResult == SlotReadResult::IoError) {
      return false;
    }
    if (readResult != SlotReadResult::Valid) {
      continue;
    }
    ++activeCount;
    if (record.id > maximumId) {
      maximumId = record.id;
    }
  }

  uint32_t repairedNextId = maximumId + 1U;
  if (repairedNextId == 0) {
    repairedNextId = 1;
  }
  if (header_.nextId > repairedNextId) {
    repairedNextId = header_.nextId;
  }
  if (header_.activeCount == activeCount &&
      header_.nextId >= repairedNextId && header_.nextId != 0) {
    return true;
  }
  header_.activeCount = activeCount;
  header_.nextId = repairedNextId;
  return persistHeader();
}

bool Store::begin() {
  ready_ = false;
  Preferences metadata;
  const bool metadataReady =
      metadata.begin(HISTORY_META_NAMESPACE, false);
  const bool filesystemWasInitialized =
      metadataReady && metadata.getBool(FILESYSTEM_INITIALIZED_KEY, false);

  if (!LittleFS.begin(false)) {
    // Format only on the first initialization. If a previously initialized
    // filesystem stops mounting, preserve it for recovery rather than
    // silently erasing all history.
    if (!metadataReady || filesystemWasInitialized || !LittleFS.begin(true)) {
      if (metadataReady) {
        metadata.end();
      }
      return false;
    }
  }
  if (metadataReady) {
    metadata.end();
  }

  Header first;
  Header second;
  const bool firstValid = readHeaderAt(0, first);
  const bool secondValid = readHeaderAt(1, second);
  if (!firstValid && !secondValid) {
    // Before the initialization marker is committed, a partial file can only
    // be an interrupted first-time setup and is safe to recreate. Once the
    // marker exists, preserve damaged history rather than silently erasing it.
    const bool historyFileExists = LittleFS.exists(HISTORY_PATH);
    if (filesystemWasInitialized && historyFileExists) {
      // Both redundant headers can be reconstructed from the checksummed
      // fixed record slots. A truncated file is left untouched for recovery.
      File damagedFile = LittleFS.open(HISTORY_PATH, "r");
      if (!damagedFile || damagedFile.size() != expectedFileSize()) {
        return false;
      }
      damagedFile.close();
      header_ = Header();
      header_.magic = HEADER_MAGIC;
      header_.version = FORMAT_VERSION;
      header_.headerSize = sizeof(Header);
      header_.recordSize = sizeof(Record);
      header_.capacity = MAX_RECORDS;
      header_.activeCount = 0;
      header_.nextId = 1;
      header_.generation = 0;
      header_.checksum = headerChecksum(header_);
      activeHeaderSlot_ = 0;
      if (!persistHeader()) {
        return false;
      }
    } else {
      const bool mayInitialize =
          !historyFileExists ||
          (metadataReady && !filesystemWasInitialized);
      if (!mayInitialize || !initializeFile()) {
        return false;
      }
    }
  } else if (firstValid &&
             (!secondValid || generationNewer(first.generation,
                                               second.generation))) {
    header_ = first;
    activeHeaderSlot_ = 0;
  } else {
    header_ = second;
    activeHeaderSlot_ = 1;
  }

  File historyFile = LittleFS.open(HISTORY_PATH, "r");
  if (!historyFile || historyFile.size() != expectedFileSize()) {
    if (historyFile) {
      historyFile.close();
    }
    // A short file before the completion marker is an interrupted first
    // initialization. Recreate it; never do this after successful use.
    if (!metadataReady || filesystemWasInitialized || !initializeFile()) {
      return false;
    }
  } else {
    historyFile.close();
  }

  ready_ = true;
  if (!rescanAndRepairHeader()) {
    ready_ = false;
  }
  if (ready_ && !filesystemWasInitialized) {
    Preferences completedMetadata;
    if (!completedMetadata.begin(HISTORY_META_NAMESPACE, false)) {
      ready_ = false;
    } else {
      const bool markerSaved =
          completedMetadata.putBool(FILESYSTEM_INITIALIZED_KEY, true) == 1;
      completedMetadata.end();
      if (!markerSaved) {
        ready_ = false;
      }
    }
  }
  return ready_;
}

Result Store::add(Record &record) {
  if (!ready_) {
    return Result::NotReady;
  }

  uint16_t targetIndex = MAX_RECORDS;
  uint16_t oldestIndex = 0;
  uint32_t oldestId = UINT32_MAX;
  uint16_t validCount = 0;
  Record existing;
  {
    File file = LittleFS.open(HISTORY_PATH, "r");
    if (!file) {
      return Result::IoError;
    }
    for (uint16_t index = 0; index < MAX_RECORDS; ++index) {
      const SlotReadResult readResult =
          readRecordFromFile(file, index, existing);
      if (readResult == SlotReadResult::IoError) {
        return Result::IoError;
      }
      if (readResult != SlotReadResult::Valid) {
        if (targetIndex == MAX_RECORDS) {
          targetIndex = index;
        }
        continue;
      }
      ++validCount;
      if (existing.id < oldestId) {
        oldestId = existing.id;
        oldestIndex = index;
      }
    }
  }
  const bool replacingActive = targetIndex == MAX_RECORDS;
  if (replacingActive) {
    targetIndex = oldestIndex;
  }

  record.marker = RECORD_MARKER;
  record.id = header_.nextId++;
  if (header_.nextId == 0) {
    header_.nextId = 1;
  }
  record.name[NAME_CAPACITY - 1] = '\0';
  record.notes[NOTES_CAPACITY - 1] = '\0';
  record.timezone[TIMEZONE_CAPACITY - 1] = '\0';
  record.revision = 1;
  if (!writeRecordAt(targetIndex, record)) {
    ready_ = false;
    return Result::IoError;
  }
  header_.activeCount =
      replacingActive
          ? validCount
          : static_cast<uint16_t>(
                validCount < MAX_RECORDS ? validCount + 1U : MAX_RECORDS);
  if (!persistHeader()) {
    ready_ = false;
    return Result::IoError;
  }
  return Result::Ok;
}

Result Store::get(uint32_t id, Record &record) {
  if (!ready_) {
    return Result::NotReady;
  }
  uint16_t index = 0;
  return findRecord(id, index, &record);
}

Result Store::edit(uint32_t id,
                   uint16_t expectedRevision,
                   const char *name,
                   const char *notes,
                   uint64_t updatedAtMs) {
  if (!ready_) {
    return Result::NotReady;
  }
  uint16_t index = 0;
  Record record;
  const Result findResult = findRecord(id, index, &record);
  if (findResult != Result::Ok) {
    return findResult;
  }
  if (expectedRevision == 0 || record.revision != expectedRevision) {
    return Result::Conflict;
  }
  strlcpy(record.name, name != nullptr ? name : "", sizeof(record.name));
  strlcpy(record.notes, notes != nullptr ? notes : "", sizeof(record.notes));
  record.updatedAtMs = updatedAtMs;
  ++record.revision;
  if (record.revision == 0) {
    record.revision = 1;
  }
  if (!writeRecordAt(index, record)) {
    ready_ = false;
    return Result::IoError;
  }
  return Result::Ok;
}

Result Store::remove(uint32_t id) {
  if (!ready_) {
    return Result::NotReady;
  }
  uint16_t index = 0;
  const Result findResult = findRecord(id, index);
  if (findResult != Result::Ok) {
    return findResult;
  }
  Record blank;
  if (!writeRecordAt(index, blank)) {
    ready_ = false;
    return Result::IoError;
  }
  if (header_.activeCount > 0) {
    --header_.activeCount;
  }
  if (!persistHeader()) {
    ready_ = false;
    return Result::IoError;
  }
  return Result::Ok;
}

Result Store::clear() {
  if (!ready_) {
    return Result::NotReady;
  }
  File file = LittleFS.open(HISTORY_PATH, "r+");
  if (!file || !file.seek(recordOffset(0), SeekSet)) {
    ready_ = false;
    return Result::IoError;
  }
  Record blank;
  for (uint16_t index = 0; index < MAX_RECORDS; ++index) {
    if (file.write(reinterpret_cast<const uint8_t *>(&blank),
                   sizeof(blank)) != sizeof(blank)) {
      ready_ = false;
      return Result::IoError;
    }
  }
  file.flush();
  file.close();
  header_.activeCount = 0;
  if (!persistHeader()) {
    ready_ = false;
    return Result::IoError;
  }
  return Result::Ok;
}

Result Store::visitNewest(RecordVisitor visitor, void *context) {
  if (!ready_) {
    return Result::NotReady;
  }
  if (visitor == nullptr) {
    return Result::IoError;
  }

  struct RecordReference {
    uint32_t id;
    uint16_t slot;
  };
  RecordReference references[MAX_RECORDS] = {};
  size_t count = 0;
  Record record;
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file) {
    return Result::IoError;
  }
  for (uint16_t slot = 0; slot < MAX_RECORDS; ++slot) {
    const SlotReadResult readResult =
        readRecordFromFile(file, slot, record);
    if (readResult == SlotReadResult::IoError) {
      return Result::IoError;
    }
    if (readResult == SlotReadResult::Valid) {
      references[count++] = {record.id, slot};
    }
  }

  for (size_t index = 1; index < count; ++index) {
    const RecordReference selected = references[index];
    size_t insertAt = index;
    while (insertAt > 0 &&
           references[insertAt - 1].id < selected.id) {
      references[insertAt] = references[insertAt - 1];
      --insertAt;
    }
    references[insertAt] = selected;
  }

  for (size_t index = 0; index < count; ++index) {
    const SlotReadResult readResult =
        readRecordFromFile(file, references[index].slot, record);
    if (readResult == SlotReadResult::IoError) {
      return Result::IoError;
    }
    if (readResult == SlotReadResult::Valid &&
        !visitor(record, context)) {
      break;
    }
  }
  return Result::Ok;
}

size_t Store::listIds(uint32_t *ids, size_t maximum, size_t offset) {
  if (!ready_ || ids == nullptr || maximum == 0) {
    return 0;
  }

  uint32_t allIds[MAX_RECORDS] = {};
  size_t count = 0;
  Record record;
  File file = LittleFS.open(HISTORY_PATH, "r");
  if (!file) {
    return 0;
  }
  for (uint16_t index = 0; index < MAX_RECORDS; ++index) {
    const SlotReadResult readResult =
        readRecordFromFile(file, index, record);
    if (readResult == SlotReadResult::IoError) {
      return 0;
    }
    if (readResult == SlotReadResult::Valid) {
      allIds[count++] = record.id;
    }
  }

  // At only 100 elements, insertion sort is smaller than bringing in a more
  // general sorter and keeps newest-first pagination deterministic.
  for (size_t index = 1; index < count; ++index) {
    const uint32_t selected = allIds[index];
    size_t insertAt = index;
    while (insertAt > 0 && allIds[insertAt - 1] < selected) {
      allIds[insertAt] = allIds[insertAt - 1];
      --insertAt;
    }
    allIds[insertAt] = selected;
  }

  if (offset >= count) {
    return 0;
  }
  const size_t available = count - offset;
  const size_t outputCount = min(maximum, available);
  memcpy(ids, allIds + offset, outputCount * sizeof(uint32_t));
  return outputCount;
}

} // namespace ScaleHistory
