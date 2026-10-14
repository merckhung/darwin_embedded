#include <stdlib.h>

typedef uint8_t SMBIOS_TABLE_STRING;

typedef struct {
  char AnchorString[4];
  uint8_t EntryPointStructureChecksum;
  uint8_t EntryPointLength;
  uint8_t MajorVersion;
  uint8_t MinorVersion;
  uint16_t MaxStructureSize;
  uint8_t EntryPointRevision;
  uint8_t FormattedArea[5];
  struct {
    void *TableAddress;
    uint16_t TableLength;
  } DMI;
} SMBIOS_ENTRY_POINT_STRUCTURE;

typedef struct {
  uint8_t Type;
  uint8_t Length;
  uint16_t Handle;
} SMBIOS_TABLE_HEADER;

typedef struct {
  SMBIOS_TABLE_HEADER Hdr;
  uint8_t Location;
  uint8_t Use;
  uint8_t MemoryErrorCorrection;
  uint32_t MaximumCapacity;
  uint16_t MemoryErrorInformationHandle;
  uint16_t NumberOfMemoryDevices;
} SMBIOS_TABLE_TYPE16;

typedef struct {
  SMBIOS_TABLE_HEADER Hdr;
  uint8_t ArrayHandle;
  uint8_t MemoryErrorInformationHandle;
  uint16_t Size; // in MB
  uint16_t FormFactor;
  uint8_t DeviceSet;
  SMBIOS_TABLE_STRING DeviceLocator;
  SMBIOS_TABLE_STRING BankLocator;
  uint8_t MemoryType;
  uint16_t TypeDetail;
  uint16_t Speed;
  SMBIOS_TABLE_STRING Manufacturer;
  SMBIOS_TABLE_STRING SerialNumber;
  SMBIOS_TABLE_STRING AssetTag;
  SMBIOS_TABLE_STRING PartNumber;
  uint8_t Attributes;
  uint32_t ExtendedSize;
  uint16_t ConfiguredMemoryClockSpeed;
} SMBIOS_TABLE_TYPE17;