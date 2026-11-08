#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef _MSC_VER
#pragma pack(push, 1)
#define FAT_PACKED
#else
#define FAT_PACKED __attribute__((packed))
#endif

//https://wiki.osdev.org/FAT#FAT_32
//https://www.ntfs.com/exfat-overview.htm
//https://www.ntfs.com/fat-filenames.htm
#define MBR_PARTITION_TYPE_EXFAT     0x07
#define MBR_PARTITION_TYPE_GPT       0xEE
#define MBR_PARTITION_TYPE_FAT16     0x06
#define MBR_PARTITION_TYPE_FAT32     0x0B
#define MBR_PARTITION_TYPE_FAT32_LBA 0x0C
#define MBR_PARTITION_TYPE_FAT16_LBA 0x0E

// Signature bytes found at index 510 and 511 in the volume ID sector
#define FAT_VOLUME_ID_SIGNATURE_1 0x55
#define FAT_VOLUME_ID_SIGNATURE_2 0xAA

#define FAT_DIRECTORY_ENTRY_SIZE 32
#define FAT_SMALLEST_LEGAL_CLUSTER_NUMBER 2

#define FAT_MAXIMUM_FILESIZE 0xFFFFFFFF

#define FAT12_MAX_CLUSTERS 4084
#define FAT16_MAX_CLUSTERS 65524

#define FAT_FILE_ATTRIBUTE_READ_ONLY 0x01
#define FAT_FILE_ATTRIBUTE_HIDDEN    0x02
#define FAT_FILE_ATTRIBUTE_SYSTEM    0x04
#define FAT_FILE_ATTRIBUTE_VOLUME_ID 0x08
#define FAT_FILE_ATTRIBUTE_LONGNAME  0x0F
#define FAT_FILE_ATTRIBUTE_DIRECTORY 0x10
#define FAT_FILE_ATTRIBUTE_ARCHIVE   0x20

#define FAT_FILENAME_LENGTH 11
#define FAT_DELETED_FILE_MARKER 0xE5
#define FAT_LONG_FILENAME_LENGTH 256
#define FAT_LONG_FILENAME_ENTRY 0x40

// exFAT directory entry types
#define	EXFAT_DIR_MASK	    0x80	// Allocation bit mask
#define	EXFAT_DIR_BITMAP	0x81	// Allocation bitmap
#define	EXFAT_DIR_UPCASE	0x82	// Up-case table
#define	EXFAT_DIR_VLABEL	0x83	// Volume label
#define	EXFAT_DIR_FILEDIR	0x85	// File and directory
#define	EXFAT_DIR_STREAM	0xC0	// Stream extension
#define	EXFAT_DIR_FILENAME	0xC1	// Name extension

#define FAT_MAKE_DATE(year, month, day)     (day | (month << 5) | ((year - 1980) << 9))
#define FAT_MAKE_TIME(hour, minute, second) ((second / 2) | (minute << 5) | (hour << 11))

typedef enum {
    FAT_FILESYSTEM_TYPE_INVALID,
    FAT_FILESYSTEM_TYPE_FAT12,
    FAT_FILESYSTEM_TYPE_FAT16,
    FAT_FILESYSTEM_TYPE_FAT32,
    FAT_FILESYSTEM_TYPE_EXFAT,
} fatFilesystemType_e;

typedef struct mbrPartitionEntry_t {
    uint8_t bootFlag;
    uint8_t chsBegin[3];
    uint8_t type;
    uint8_t chsEnd[3];
    uint32_t lbaBegin;
    uint32_t numSectors;
} FAT_PACKED mbrPartitionEntry_t;

typedef struct fat16Descriptor_t {
    uint8_t driveNumber;
    uint8_t reserved1;
    uint8_t bootSignature;
    uint32_t volumeID;
    char volumeLabel[11];
    char fileSystemType[8];
} FAT_PACKED fat16Descriptor_t;

typedef struct fat32Descriptor_t {
    uint32_t FATSize32;
    uint16_t extFlags;
    uint16_t fsVer;
    uint32_t rootCluster;
    uint16_t fsInfo;
    uint16_t backupBootSector;
    uint8_t reserved[12];
    uint8_t driveNumber;
    uint8_t reserved1;
    uint8_t bootSignature;
    uint32_t volumeID;
    char volumeLabel[11];
    char fileSystemType[8];
} FAT_PACKED fat32Descriptor_t;

typedef struct bpbFat_t {
    uint16_t bytesPerSector;
    uint8_t sectorsPerCluster;
    uint16_t reservedSectorCount;
    uint8_t numFATs;
    uint16_t rootEntryCount;
    uint16_t totalSectors16;
    uint8_t media;
    uint16_t FATSize16;
    uint16_t sectorsPerTrack;
    uint16_t numHeads;
    uint32_t hiddenSectors;
    uint32_t totalSectors32;
    union {
        fat16Descriptor_t fat16;
        fat32Descriptor_t fat32;
    } fatDescriptor;
} FAT_PACKED bpbFat_t;

typedef struct bpbExFat_t {
  uint8_t mustBeZero[53];
  uint64_t partitionOffset;
  uint64_t volumeLength;
  uint32_t fatOffset;
  uint32_t fatLength;
  uint32_t clusterHeapOffset;
  uint32_t clusterCount;
  uint32_t rootDirectoryCluster;
  uint32_t volumeSerialNumber;
  uint16_t fileSystemRevision;
  uint16_t volumeFlags;
  uint8_t bytesPerSectorShift;
  uint8_t sectorsPerClusterShift;
  uint8_t numFATs;
  uint8_t driveSelect;
  uint8_t percentInUse;
  uint8_t reserved[7];
} FAT_PACKED bpbExFat_t;

typedef struct fatVolumeID_t {
    uint8_t jmpBoot[3];
    char oemName[8];
    union {
        uint8_t bpb[109];
        bpbExFat_t exFat;
        bpbFat_t fat;
    };
} FAT_PACKED fatVolumeID_t;

typedef struct fatDirectoryEntry_t {
    char fileName[FAT_FILENAME_LENGTH];
    uint8_t attrib;
    uint8_t ntReserved;
    uint8_t creationTimeTenths;
    uint16_t creationTime;
    uint16_t creationDate;
    uint16_t lastAccessDate;
    uint16_t firstClusterHigh;
    uint16_t lastWriteTime;
    uint16_t lastWriteDate;
    uint16_t firstClusterLow;
    uint32_t fileSize;
} FAT_PACKED fatDirectoryEntry_t;

typedef struct fatLfnDirectoryEntry_t {
    char fileName[FAT_FILENAME_LENGTH];
    uint8_t attrib;
    uint8_t entryType;
    uint8_t lfnChecksum;
    char midFileName[FAT_FILENAME_LENGTH + 1];
    uint16_t lfnAlwaysZero;
    char endFileName[4];
} FAT_PACKED fatLfnDirectoryEntry_t;

typedef struct exFatFileEntry_t {
    uint8_t type;
    uint8_t numOfEntries;
    uint16_t checksum;
    uint16_t fileAttirb;
    uint16_t reserved1;
    uint16_t creationTime;
    uint16_t creationDate;
    uint16_t lastWriteTime;
    uint16_t lastWriteDate;
    uint16_t lastAccessTime;
    uint16_t lastAccessDate;
    uint8_t creationTimeTenths;
    uint8_t lastWriteTimeTenths;
    uint8_t creationUtcOffset;
    uint8_t lastWriteUtcOffset;
    uint8_t lastAccessUtcOffset;
    char reserved3[7];
} FAT_PACKED exFatFileEntry_t;

typedef struct exFatStreamEntry_t {
    uint8_t type;
    uint8_t flags;
    uint8_t reserved0;
    uint8_t nameLength;
    uint16_t nameHash;
    uint16_t reserved1;
    uint64_t fileSize;
    uint32_t reserved2;
    uint32_t firstCluster;
    uint64_t dataSize;
} FAT_PACKED exFatStreamEntry_t;

typedef struct exFatFileNameEntry_t {
    uint8_t type;
    uint8_t flags;
    char fileName[FAT_DIRECTORY_ENTRY_SIZE-2];
} FAT_PACKED exFatFileNameEntry_t;

typedef struct exFatUpTableEntry_t {
    uint8_t type;
    uint8_t reserved1[3];
    uint32_t tableChecksum;
    uint8_t reserved2[12];
    uint32_t firstCluster;
    uint64_t dataSize;
} FAT_PACKED exFatUpTableEntry_t;

typedef struct exFatVolumeEntry_t {
    uint8_t type;
    uint8_t charCount;
    char fileName[FAT_DIRECTORY_ENTRY_SIZE-10];
    uint8_t reserved[8];
} FAT_PACKED exFatVolumeEntry_t;

uint32_t fat32_decodeClusterNumber(uint32_t clusterNumber);

bool exFat_isEndOfChainMarker(uint32_t clusterNumber);
bool fat32_isEndOfChainMarker(uint32_t clusterNumber);
bool fat16_isEndOfChainMarker(uint16_t clusterNumber);

bool fat_isFreeSpace(uint32_t clusterNumber);

bool fat_isDirectoryEntryTerminator(fatDirectoryEntry_t *entry);
bool fat_isDirectoryEntryEmpty(fatDirectoryEntry_t *entry);
bool fat_isVolumeLabel(fatDirectoryEntry_t *entry);

bool fat_compareLongFileEntry (const char *fileName, fatLfnDirectoryEntry_t *entry);
bool fat_isLfnEntry(fatDirectoryEntry_t *entry);
bool fat_isAvailableEntry(fatDirectoryEntry_t *entry, fatFilesystemType_e fileSystemType);

void fat_convertFileNameToFATStyle(const char *fileName, uint8_t *fatFilename);
void fat_convertLongFileNameToFATStyle(const char *fileName, uint8_t *fatFilename);
uint8_t fat_createCheckSum (const char *fileName);
uint16_t exFat_nameHash (const char *fileName);
uint32_t exFat_upCaseTableCheckSum (const char *data, int size);
uint16_t fat_unicodeToUpperCase (uint16_t uni);

uint16_t exFat_entrySetChecksum(const uint8_t *entries, unsigned int size);

void fat_convertFilenameToFATStyle(const char *filename, uint8_t *fatFilename);

#ifdef _MSC_VER
#pragma pack(pop)
#endif
