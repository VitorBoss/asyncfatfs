#include <ctype.h>
#include <string.h>

#include "fat_standard.h"

bool fat16_isEndOfChainMarker(uint16_t clusterNumber)
{
    return clusterNumber >= 0xFFF8;
}

// Pass the cluster number after fat32_decodeClusterNumber().
bool fat32_isEndOfChainMarker(uint32_t clusterNumber)
{
    return clusterNumber >= 0x0FFFFFF8;
}

bool exFat_isEndOfChainMarker(uint32_t clusterNumber)
{
    return clusterNumber >= 0xFFFFFFF8;
}

/**
 * FAT32 cluster numbers are really only 28 bits, and the top 4 bits must be left alone and not treated as part of the
 * cluster number (so various FAT drivers can use those bits for their own purposes, or they can be used in later
 * extensions)
 */
uint32_t fat32_decodeClusterNumber(uint32_t clusterNumber)
{
    return clusterNumber & 0x0FFFFFFF;
}

// fat32 needs fat32_decodeClusterNumber() applied first.
bool fat_isFreeSpace(uint32_t clusterNumber)
{
    return clusterNumber == 0;
}

bool fat_isDirectoryEntryTerminator(fatDirectoryEntry_t *entry)
{
    return entry->fileName[0] == 0x00;
}

bool fat_isDirectoryEntryEmpty(fatDirectoryEntry_t *entry)
{
    return (unsigned char) entry->fileName[0] == FAT_DELETED_FILE_MARKER;
}

bool fat_isVolumeLabel(fatDirectoryEntry_t *entry)
{
    return ((unsigned char) entry->attrib & 0x3F) == FAT_FILE_ATTRIBUTE_VOLUME_ID;
}

bool fat_isLfnEntry(fatDirectoryEntry_t *entry)
{
    return ((unsigned char) entry->fileName[0] & FAT_LONG_FILENAME_ENTRY) != 0;
}

bool fat_isAvailableEntry(fatDirectoryEntry_t *entry, fatFilesystemType_e fileSystemType)
{
    if (fileSystemType != FAT_FILESYSTEM_TYPE_EXFAT) {
        return (fat_isDirectoryEntryEmpty(entry) || fat_isDirectoryEntryTerminator(entry));
    } else {
        return ((unsigned char) entry->fileName[0] & EXFAT_DIR_MASK) == 0;
    }
}

/**
 * Convert the given "prefix.ext" style fileName to the FAT format to be stored on disk.
 *
 * fatFilename must point to a buffer which is FAT_FILENAME_LENGTH bytes long. The buffer is not null-terminated.
 */
void fat_convertFileNameToFATStyle(const char *fileName, uint8_t *fatFilename)
{
    for (int i = 0; i < 8; i++) {
        if (*fileName == '\0' || *fileName == '.') {
            *fatFilename = ' ';
        } else {
            *fatFilename = toupper((unsigned char)*fileName);
            fileName++;
        }
        fatFilename++;
    }

    if (*fileName == '.') {
        fileName++;
    }

    for (int i = 0; i < 3; i++) {
         if (*fileName == '\0') {
             *fatFilename = ' ';
         } else {
             *fatFilename = toupper((unsigned char)*fileName);
             fileName++;
         }
         fatFilename++;
     }
}

bool fat_compareLongFileEntry (const char *fileName, fatLfnDirectoryEntry_t *entry)
{
    int i, x;
    uint16_t wChar, fileWChar;
    uint16_t lastWChar;

    if (entry->lfnAlwaysZero != 0) {
        return false;
    }
    i = ((entry->fileName[0] & 0x3F) - 1) * 13;

    lastWChar = 1;
    for (x = 0; x < 26; x+=2) {
        if (x < 8) {
            fileWChar = ((uint16_t)entry->fileName[x + 2] << 8) | entry->fileName[x + 1];
        } else if (x < 22) {
            fileWChar = ((uint16_t)entry->midFileName[x - 7] << 8) | entry->midFileName[x - 8];
        } else {
            fileWChar = ((uint16_t)entry->endFileName[x - 21] << 8) | entry->endFileName[x - 22];
        }
        wChar = ((uint16_t)fileName[i + 1] << 8) | fileName[i];
        if (lastWChar != 0) {
            if ((i >= FAT_LONG_FILENAME_LENGTH + 1) || (fat_unicodeToUpperCase(fileWChar) != fat_unicodeToUpperCase(wChar))) {
                return false;
            }
            lastWChar = fileWChar;
        } else if (fileWChar != 0xFFFF) {
            return false;
        }
    }

	if ((entry->fileName[0] & FAT_LONG_FILENAME_ENTRY) && lastWChar && wChar) {
        return false;	// Last segment matched but different length
    }

    return true;
}

uint8_t fat_createCheckSum (const char *fileName)
{
    int i;
    uint8_t sum;

    for (i = sum = 0; i < 11; i++) { /* Calculate sum of fileName */
        sum = (sum >> 1) + (sum << 7) + fileName[i];
    }
    return sum;
}

// fileName points to up-cased file name
uint16_t exFat_nameHash (const char *fileName)
{
    int i;
		uint16_t nameLength;
    uint16_t hash = 0;
		nameLength = strlen(fileName);

    for (i = 0; i < nameLength; i++)
        hash = (((hash & 1) ? 0x8000 : 0) | (hash >> 1)) + fileName[i];
    return hash;
}

uint32_t exFat_upCaseTableCheckSum (const char *data, int size)
{
    int i;
    uint32_t sum;

    for (i = sum = 0; i < size; i++) { /* Calculate sum of fileName */
        sum = (((sum & 1) ? 0x80000000 : 0) | (sum >> 1)) + data[i];
    }
    return sum;
}

bool fat_unicodeToUpperCase (uint16_t uni)
{
    const uint16_t* p;
    uint16_t uc, bc, nc, cmd;
    static const uint16_t cvt1[] = {	/* Compressed up conversion table for U+0000 - U+0FFF */
        /* Basic Latin */
        0x0061,0x031A,
        /* Latin-1 Supplement */
        0x00E0,0x0317,
        0x00F8,0x0307,
        0x00FF,0x0001,0x0178,
        /* Latin Extended-A */
        0x0100,0x0130,
        0x0132,0x0106,
        0x0139,0x0110,
        0x014A,0x012E,
        0x0179,0x0106,
        /* Latin Extended-B */
        0x0180,0x004D,0x0243,0x0181,0x0182,0x0182,0x0184,0x0184,0x0186,0x0187,0x0187,0x0189,0x018A,0x018B,0x018B,0x018D,0x018E,0x018F,0x0190,0x0191,0x0191,0x0193,0x0194,0x01F6,0x0196,0x0197,0x0198,0x0198,0x023D,0x019B,0x019C,0x019D,0x0220,0x019F,0x01A0,0x01A0,0x01A2,0x01A2,0x01A4,0x01A4,0x01A6,0x01A7,0x01A7,0x01A9,0x01AA,0x01AB,0x01AC,0x01AC,0x01AE,0x01AF,0x01AF,0x01B1,0x01B2,0x01B3,0x01B3,0x01B5,0x01B5,0x01B7,0x01B8,0x01B8,0x01BA,0x01BB,0x01BC,0x01BC,0x01BE,0x01F7,0x01C0,0x01C1,0x01C2,0x01C3,0x01C4,0x01C5,0x01C4,0x01C7,0x01C8,0x01C7,0x01CA,0x01CB,0x01CA,
        0x01CD,0x0110,
        0x01DD,0x0001,0x018E,
        0x01DE,0x0112,
        0x01F3,0x0003,0x01F1,0x01F4,0x01F4,
        0x01F8,0x0128,
        0x0222,0x0112,
        0x023A,0x0009,0x2C65,0x023B,0x023B,0x023D,0x2C66,0x023F,0x0240,0x0241,0x0241,
        0x0246,0x010A,
        /* IPA Extensions */
        0x0253,0x0040,0x0181,0x0186,0x0255,0x0189,0x018A,0x0258,0x018F,0x025A,0x0190,0x025C,0x025D,0x025E,0x025F,0x0193,0x0261,0x0262,0x0194,0x0264,0x0265,0x0266,0x0267,0x0197,0x0196,0x026A,0x2C62,0x026C,0x026D,0x026E,0x019C,0x0270,0x0271,0x019D,0x0273,0x0274,0x019F,0x0276,0x0277,0x0278,0x0279,0x027A,0x027B,0x027C,0x2C64,0x027E,0x027F,0x01A6,0x0281,0x0282,0x01A9,0x0284,0x0285,0x0286,0x0287,0x01AE,0x0244,0x01B1,0x01B2,0x0245,0x028D,0x028E,0x028F,0x0290,0x0291,0x01B7,
        /* Greek, Coptic */
        0x037B,0x0003,0x03FD,0x03FE,0x03FF,
        0x03AC,0x0004,0x0386,0x0388,0x0389,0x038A,
        0x03B1,0x0311,
        0x03C2,0x0002,0x03A3,0x03A3,
        0x03C4,0x0308,
        0x03CC,0x0003,0x038C,0x038E,0x038F,
        0x03D8,0x0118,
        0x03F2,0x000A,0x03F9,0x03F3,0x03F4,0x03F5,0x03F6,0x03F7,0x03F7,0x03F9,0x03FA,0x03FA,
        /* Cyrillic */
        0x0430,0x0320,
        0x0450,0x0710,
        0x0460,0x0122,
        0x048A,0x0136,
        0x04C1,0x010E,
        0x04CF,0x0001,0x04C0,
        0x04D0,0x0144,
        /* Armenian */
        0x0561,0x0426,

        0x0000	/* EOT */
    };
    static const uint16_t cvt2[] = {	/* Compressed up conversion table for U+1000 - U+FFFF */
        /* Phonetic Extensions */
        0x1D7D,0x0001,0x2C63,
        /* Latin Extended Additional */
        0x1E00,0x0196,
        0x1EA0,0x015A,
        /* Greek Extended */
        0x1F00,0x0608,
        0x1F10,0x0606,
        0x1F20,0x0608,
        0x1F30,0x0608,
        0x1F40,0x0606,
        0x1F51,0x0007,0x1F59,0x1F52,0x1F5B,0x1F54,0x1F5D,0x1F56,0x1F5F,
        0x1F60,0x0608,
        0x1F70,0x000E,0x1FBA,0x1FBB,0x1FC8,0x1FC9,0x1FCA,0x1FCB,0x1FDA,0x1FDB,0x1FF8,0x1FF9,0x1FEA,0x1FEB,0x1FFA,0x1FFB,
        0x1F80,0x0608,
        0x1F90,0x0608,
        0x1FA0,0x0608,
        0x1FB0,0x0004,0x1FB8,0x1FB9,0x1FB2,0x1FBC,
        0x1FCC,0x0001,0x1FC3,
        0x1FD0,0x0602,
        0x1FE0,0x0602,
        0x1FE5,0x0001,0x1FEC,
        0x1FF3,0x0001,0x1FFC,
        /* Letterlike Symbols */
        0x214E,0x0001,0x2132,
        /* Number forms */
        0x2170,0x0210,
        0x2184,0x0001,0x2183,
        /* Enclosed Alphanumerics */
        0x24D0,0x051A,
        0x2C30,0x042F,
        /* Latin Extended-C */
        0x2C60,0x0102,
        0x2C67,0x0106, 0x2C75,0x0102,
        /* Coptic */
        0x2C80,0x0164,
        /* Georgian Supplement */
        0x2D00,0x0826,
        /* Full-width */
        0xFF41,0x031A,

        0x0000	/* EOT */
    };


    if (uni < 0x10000) {	/* Is it in BMP? */
        uc = (uint16_t)uni;
        p = uc < 0x1000 ? cvt1 : cvt2;
        for (;;) {
            bc = *p++;								/* Get the block base */
            if (bc == 0 || uc < bc) break;			/* Not matched? */
            nc = *p++; cmd = nc >> 8; nc &= 0xFF;	/* Get processing command and block size */
            if (uc < bc + nc) {	/* In the block? */
                switch (cmd) {
                case 0:	uc = p[uc - bc]; break;		/* Table conversion */
                case 1:	uc -= (uc - bc) & 1; break;	/* Case pairs */
                case 2: uc -= 16; break;			/* Shift -16 */
                case 3:	uc -= 32; break;			/* Shift -32 */
                case 4:	uc -= 48; break;			/* Shift -48 */
                case 5:	uc -= 26; break;			/* Shift -26 */
                case 6:	uc += 8; break;				/* Shift +8 */
                case 7: uc -= 80; break;			/* Shift -80 */
                case 8:	uc -= 0x1C60; break;		/* Shift -0x1C60 */
                }
                break;
            }
            if (cmd == 0) p += nc;	/* Skip table if needed */
        }
        uni = uc;
    }

    return uni;
}

void fat_convertLongFileNameToFATStyle(const char *fileName, uint8_t *fatFilename)
{
		unsigned int n, seq, i, j;
		uint8_t buffer[8];
		uint16_t *wChar = (uint16_t*) fileName;
		uint16_t curWChar;

		//for (n = 1; n < 100; n++) {
    n = 1;
				seq = n;
				if (n > 5) {
						uint32_t sreg;
						//generate a hash number instead of sequential number
						while (wChar[i]) {	/* Create a CRC as hash value */
								curWChar = wChar[i++];
								for (i = 0; i < 16; i++) {
										sreg = (sreg << 1) + (curWChar & 1);
										curWChar >>= 1;
										if (sreg & 0x10000) sreg ^= 0x11021;
								}
						}
						seq = (unsigned int)sreg;
				}

				// Make suffix (~ + hexadecimal)
				i = 7;
				while (i && seq) {
						buffer[i] = (uint8_t)((seq % 16) + '0'); seq /= 16;
						if (buffer[i] > '9') buffer[i] += 7;
						i--;
				}
				buffer[i] = '~';

				// Append the suffix to the SFN body
				for (j = 0; j < i && fatFilename[j] != ' '; j++) {
						// Find the offset to append
						/*
						if (dbc_1st(fatFilename[j])) {
							//Avoid DBC break up
							if (j == i - 1) break;
							j++;
						}
						*/
				}

				while (j < 8) {	/* Append the suffix */
						fatFilename[j++] = (i < 8) ? buffer[i++] : ' ';
				}
		//}
}
