#pragma once

#include <stdint.h>

namespace DualScaleBle {

constexpr char SERVICE_UUID[] = "7b8d1000-6f3b-4a5d-9c2e-4f1a2b3c4d5e";
constexpr char TELEMETRY_UUID[] = "7b8d1001-6f3b-4a5d-9c2e-4f1a2b3c4d5e";
constexpr char COMMAND_UUID[] = "7b8d1002-6f3b-4a5d-9c2e-4f1a2b3c4d5e";

constexpr uint16_t TELEMETRY_MAGIC = 0x5343;
constexpr uint16_t COMMAND_MAGIC = 0x434D;
constexpr uint8_t PROTOCOL_VERSION = 2;
constexpr uint8_t ALL_UNITS = 0;

enum class Status : uint8_t {
  Starting = 0,
  Ready = 1,
  NeedsCalibration = 2,
  NeedsTare = 3,
  Taring = 4,
  Calibrating = 5,
  NoSensor = 6,
  SensorError = 7,
  ShuttingDown = 8,
};

enum TelemetryFlag : uint8_t {
  SensorPresent = 1U << 0,
  AdcResponding = 1U << 1,
  Tared = 1U << 2,
  Calibrated = 1U << 3,
  WeightValid = 1U << 4,
  OledPresent = 1U << 5,
  WeightLocked = 1U << 6,
  UnlockRequested = 1U << 7,
};

enum class CommandType : uint8_t {
  Status = 1,
  Tare = 2,
  Calibrate = 3,
  PeerStatus = 4,
  SetWeightLock = 5,
  ClearWeightLock = 6,
};

enum class CommandResult : uint8_t {
  None = 0,
  Accepted = 1,
  Success = 2,
  Invalid = 3,
  Failed = 4,
  Busy = 5,
};

struct __attribute__((packed)) TelemetryPacket {
  uint16_t magic;
  uint8_t version;
  uint8_t unit;
  uint32_t sequence;
  int32_t weightGrams;
  uint16_t acknowledgedToken;
  uint8_t status;
  uint8_t flags;
  uint8_t commandResult;
  // Incremented by the scale for every local unlock request. Lock commands
  // echo the observed generation so stale BLE writes cannot re-lock a scale.
  uint8_t unlockRequestGeneration;
};

struct __attribute__((packed)) CommandPacket {
  uint16_t magic;
  uint8_t version;
  uint8_t targetUnit;
  uint16_t token;
  uint8_t command;
  uint8_t unlockRequestGeneration;
  int32_t valueGrams;
};

static_assert(sizeof(TelemetryPacket) == 18,
              "BLE telemetry must remain within one default notification.");
static_assert(sizeof(CommandPacket) == 12,
              "BLE command must remain within one default write.");

} // namespace DualScaleBle
