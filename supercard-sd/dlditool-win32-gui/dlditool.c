/*
    dlditool - Dynamically Linked Disk Interface patch tool
    Copyright (C) 2006  Michael Chisholm (Chishm)

	Send all queries to chishm@hotmail.com

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

	* v1.00 - 2006-12-25 - Chishm
		* Original release

	* v1.01 - 2006-12-30 - Chishm
		* Minor bugfix parsing 3 arguments

	* v1.10 - 2007-01-07 - Chishm
		* Removed assumptions about endianess of computer
			* Word size shouldn't matter now either, except that an int type must be at least 32 bits long
		* Added *.sc.nds and *.gba.nds file extensions
		* Improved command line argument parsing

	* v1.20 - 2007-01-11 - Chishm
		* Changed offset calculation method

	* v1.21 - 2007-01-12 - Chishm
		* Improved error messages
*/
#define VERSION "v1.20"

#include <stdio.h>
#include <malloc.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>

#ifndef bool
 typedef enum {false = 0, true = !0} bool;
#endif

typedef unsigned int addr_t;
typedef unsigned char data_t;

#define FEATURE_MEDIUM_CANREAD	0x00000001
#define FEATURE_MEDIUM_CANWRITE	0x00000002
#define FEATURE_SLOT_GBA	0x00000010
#define FEATURE_SLOT_NDS	0x00000020

#define MAGIC_TOKEN 0xBF8DA5ED

#define FIX_ALL	0x01
#define FIX_GLUE	0x02
#define FIX_GOT	0x04
#define FIX_BSS	0x08

#define DLDI_VERSION 1

enum DldiOffsets {
	DO_magicString = 0x00,			// "\xED\xA5\x8D\xBF Chishm"
	DO_magicToken = 0x00,			// 0xBF8DA5ED
	DO_magicShortString = 0x04,		// " Chishm"
	DO_version = 0x0C,
	DO_driverSize = 0x0D,
	DO_fixSections = 0x0E,
	DO_allocatedSpace = 0x0F,

	DO_friendlyName = 0x10,

	DO_text_start = 0x40,			// Data start
	DO_data_end = 0x44,				// Data end
	DO_glue_start = 0x48,			// Interworking glue start	-- Needs address fixing
	DO_glue_end = 0x4C,				// Interworking glue end
	DO_got_start = 0x50,			// GOT start					-- Needs address fixing
	DO_got_end = 0x54,				// GOT end
	DO_bss_start = 0x58,			// bss start					-- Needs setting to zero
	DO_bss_end = 0x5C,				// bss end

	// IO_INTERFACE data
	DO_ioType = 0x60,
	DO_features = 0x64,
	DO_startup = 0x68,	
	DO_isInserted = 0x6C,	
	DO_readSectors = 0x70,	
	DO_writeSectors = 0x74,
	DO_clearStatus = 0x78,
	DO_shutdown = 0x7C,
	DO_code = 0x80
};

const char dldiMagicString[] = "\xED\xA5\x8D\xBF Chishm";
const char dldiFileExtension[] = ".dldi";


void printUsage (char* programName) {
	printf ("Usage:\n");
	printf ("%s <dldi> <app>\n", programName);
	printf ("   <dldi>        the dldi patch file to apply\n");
	printf ("   <app>         the application binary to apply the patch to\n");
	return;
}

unsigned int readAddr (data_t *mem, addr_t offset) {
	return ( 
			(mem[offset + 0] << 0) |
			(mem[offset + 1] << 8) |
			(mem[offset + 2] << 16) |
			(mem[offset + 3] << 24)
		);
}

void writeAddr (data_t *mem, addr_t offset, addr_t value) {
	mem[offset + 0] = (data_t)(value >> 0);
	mem[offset + 1] = (data_t)(value >> 8);
	mem[offset + 2] = (data_t)(value >> 16);
	mem[offset + 3] = (data_t)(value >> 24);
}

int stringCaseInsensitiveCompare (const char *str1, const char *str2) {
	while (tolower(*str1) == tolower(*str2)) {
		if (*str1 == '\0') {
			return 0;
		}
		str1++;
		str2++;
	}
	return (tolower(*str1) - tolower(*str2));
}

bool stringEndsWith (const char *str, const char *end) {
	const char* strEnd;
	if (strlen (str) < strlen(end)) {
		return false;
	}
	strEnd = &str[strlen (str) - strlen(end)];
	return (stringCaseInsensitiveCompare (strEnd, end) == 0);			
}

bool stringStartsWith (const char *str, const char *start) {
	return (strstr (str, start) == str);
}

int quickFind (const char* data, const char* search, size_t dataLen, size_t searchLen) {
	const int* dataChunk = (const int*) data;
	int searchChunk = ((const int*)search)[0];
	int i;
	size_t dataChunkEnd = dataLen / sizeof(int);

	for ( i = 0; i < (int)dataChunkEnd; i++) {
		if (dataChunk[i] == searchChunk) {
			if ((i*sizeof(int) + searchLen) > dataLen) {
				return -1;
			}
			if (memcmp (&data[i*sizeof(int)], search, searchLen) == 0) {
				return i*sizeof(int);
			}
		}
	}

	return -1;
}


int main(int argc, char* argv[])
{
	
	char *dldiFileName = NULL;
	char *appFileName = NULL;

	addr_t memOffset;			// Offset of DLDI after the file is loaded into memory
	addr_t patchOffset;			// Position of patch destination in the file
	addr_t relocationOffset;	// Value added to all offsets within the patch to fix it properly
	addr_t ddmemOffset;			// Original offset used in the DLDI file
	addr_t ddmemStart;			// Start of range that offsets can be in the DLDI file
	addr_t ddmemEnd;			// End of range that offsets can be in the DLDI file
	addr_t ddmemSize;			// Size of range that offsets can be in the DLDI file

	addr_t addrIter;

	FILE* dldiFile;
	FILE* appFile;

	data_t *pDH;
	data_t *pAH;

	data_t *appFileData = NULL;
	size_t appFileSize = 0;
	data_t *dldiFileData = NULL;
	size_t dldiFileSize = 0;
	
	int i;

	printf ("Dynamically Linked Disk Interface patch tool " VERSION " by Michael Chisholm (Chishm)\n\n");

	for (i = 1; i < argc; i++) {
		if (dldiFileName == NULL) {
			dldiFileName = (char*) malloc (strlen (argv[i]) + 1 + sizeof(dldiFileExtension));
			if (!dldiFileName) {
				return -1;
			}
			strcpy (dldiFileName, argv[i]);
		} else if (appFileName == NULL) {
			appFileName = (char*) malloc (strlen (argv[i]) + 1);
			if (!appFileName) {
				return -1;
			}
			strcpy (appFileName, argv[i]);
		} else {
			printUsage (argv[0]);
			return -1;
		}
	}

	if ((dldiFileName == NULL) || (appFileName == NULL)) {
		printUsage (argv[0]);
		return -1;
	}

	if (!(dldiFile = fopen (dldiFileName, "rb"))) {
		printf ("Cannot open \"%s\" - %s\n", dldiFileName, strerror(errno));
		if (!stringEndsWith (dldiFileName, dldiFileExtension)) {
			strcat (dldiFileName, dldiFileExtension);
			printf ("Trying \"%s\"\n", dldiFileName);
			if (!(dldiFile = fopen (dldiFileName, "rb"))) {
				printf ("Cannot open \"%s\" - %s\n", dldiFileName, strerror(errno));
				return -1;
			}
		} else {
			return -1;
		}
	}

	if (!(appFile = fopen (appFileName, "rb+"))) {
		printf ("Cannot open \"%s\" - %s\n", appFileName, strerror(errno));
		return -1;
	}

	// Load the app file and the DLDI patch file
	fseek (appFile, 0, SEEK_END);
	appFileSize = ftell(appFile);
	appFileData = (data_t*) malloc (appFileSize);
	fseek (appFile, 0, SEEK_SET);

	fseek (dldiFile, 0, SEEK_END);
	dldiFileSize = ftell(dldiFile);
	dldiFileData = (data_t*) malloc (dldiFileSize);
	fseek (dldiFile, 0, SEEK_SET);

	if (!appFileData || !dldiFileData) {
		fclose (appFile);
		fclose (dldiFile);
		if (appFileData) free (appFileData);
		if (dldiFileData) free (dldiFileData);
		printf ("Out of memory\n");
		return -1;
	}

	fread (appFileData, 1, appFileSize, appFile);
	fread (dldiFileData, 1, dldiFileSize, dldiFile);
	fclose (dldiFile);

	// Find the DSDI reserved space in the file
	patchOffset = quickFind ((const char*)appFileData, dldiMagicString, appFileSize, sizeof(dldiMagicString)/sizeof(char));

	if (patchOffset < 0) {
		printf ("%s does not have a DLDI section\n", appFileName);
		return -1;
	}

	pDH = dldiFileData;
	pAH = &appFileData[patchOffset];

	// Make sure the DLDI file is valid and usable
	if (strcmp (dldiMagicString, (char*)&pDH[DO_magicString]) != 0) {
		printf ("Invalid DLDI file\n");
		return -1;
	}
	if (pDH[DO_version] != DLDI_VERSION) {
		printf ("Incorrect DLDI file version. Expected %d, found %d.\n", DLDI_VERSION, pDH[DO_version]);
		return -1;
	}
	if (pDH[DO_driverSize] > pAH[DO_allocatedSpace]) {
		printf ("Not enough space for patch. Available %d bytes, need %d bytes\n", ( 1 << pAH[DO_allocatedSpace]), ( 1 << pDH[DO_driverSize]) );
		return -1;
	}

	memOffset = readAddr (pAH, DO_text_start);
	if (memOffset == 0) {
			memOffset = readAddr (pAH, DO_startup) - DO_code;
	}
	ddmemOffset = readAddr (pDH, DO_text_start);
	relocationOffset = memOffset - ddmemOffset;

	printf ("Old driver:          %s\n", &pAH[DO_friendlyName]);
	printf ("New driver:          %s\n", &pDH[DO_friendlyName]);
	printf ("\n");
	printf ("Position in file:    0x%08X\n", patchOffset);
	printf ("Position in memory:  0x%08X\n", memOffset);
	printf ("Patch base address:  0x%08X\n", ddmemOffset);
	printf ("Relocation offset:   0x%08X\n", relocationOffset);
	printf ("\n");

	ddmemStart = readAddr (pDH, DO_text_start);
	ddmemSize = (1 << pDH[DO_driverSize]);
	ddmemEnd = ddmemStart + ddmemSize;

	// Remember how much space is actually reserved
	pDH[DO_allocatedSpace] = pAH[DO_allocatedSpace];
	// Copy the DLDI patch into the application
	memcpy (pAH, pDH, dldiFileSize);

	// Fix the section pointers in the header
	writeAddr (pAH, DO_text_start, readAddr (pAH, DO_text_start) + relocationOffset);
	writeAddr (pAH, DO_data_end, readAddr (pAH, DO_data_end) + relocationOffset);
	writeAddr (pAH, DO_glue_start, readAddr (pAH, DO_glue_start) + relocationOffset);
	writeAddr (pAH, DO_glue_end, readAddr (pAH, DO_glue_end) + relocationOffset);
	writeAddr (pAH, DO_got_start, readAddr (pAH, DO_got_start) + relocationOffset);
	writeAddr (pAH, DO_got_end, readAddr (pAH, DO_got_end) + relocationOffset);
	writeAddr (pAH, DO_bss_start, readAddr (pAH, DO_bss_start) + relocationOffset);
	writeAddr (pAH, DO_bss_end, readAddr (pAH, DO_bss_end) + relocationOffset);
	// Fix the function pointers in the header
	writeAddr (pAH, DO_startup, readAddr (pAH, DO_startup) + relocationOffset);
	writeAddr (pAH, DO_isInserted, readAddr (pAH, DO_isInserted) + relocationOffset);
	writeAddr (pAH, DO_readSectors, readAddr (pAH, DO_readSectors) + relocationOffset);
	writeAddr (pAH, DO_writeSectors, readAddr (pAH, DO_writeSectors) + relocationOffset);
	writeAddr (pAH, DO_clearStatus, readAddr (pAH, DO_clearStatus) + relocationOffset);
	writeAddr (pAH, DO_shutdown, readAddr (pAH, DO_shutdown) + relocationOffset);

	if (pDH[DO_fixSections] & FIX_ALL) { 
		// Search through and fix pointers within the data section of the file
		for (addrIter = (readAddr(pDH, DO_text_start) - ddmemStart); addrIter < (readAddr(pDH, DO_data_end) - ddmemStart); addrIter++) {
			if ((ddmemStart <= readAddr(pAH, addrIter)) && (readAddr(pAH, addrIter) < ddmemEnd)) {
				writeAddr (pAH, addrIter, readAddr(pAH, addrIter) + relocationOffset);
			}
		}
	}

	if (pDH[DO_fixSections] & FIX_GLUE) { 
		// Search through and fix pointers within the glue section of the file
		for (addrIter = (readAddr(pDH, DO_glue_start) - ddmemStart); addrIter < (readAddr(pDH, DO_glue_end) - ddmemStart); addrIter++) {
			if ((ddmemStart <= readAddr(pAH, addrIter)) && (readAddr(pAH, addrIter) < ddmemEnd)) {
				writeAddr (pAH, addrIter, readAddr(pAH, addrIter) + relocationOffset);
			}
		}
	}

	if (pDH[DO_fixSections] & FIX_GOT) { 
		// Search through and fix pointers within the Global Offset Table section of the file
		for (addrIter = (readAddr(pDH, DO_got_start) - ddmemStart); addrIter < (readAddr(pDH, DO_got_end) - ddmemStart); addrIter++) {
			if ((ddmemStart <= readAddr(pAH, addrIter)) && (readAddr(pAH, addrIter) < ddmemEnd)) {
				writeAddr (pAH, addrIter, readAddr(pAH, addrIter) + relocationOffset);
			}
		}
	}

	if (pDH[DO_fixSections] & FIX_BSS) { 
		// Initialise the BSS to 0
		memset (&pAH[readAddr(pDH, DO_bss_start) - ddmemStart] , 0, readAddr(pDH, DO_bss_end) - readAddr(pDH, DO_bss_start));
	}

	// Write the patch back to the file
	fseek (appFile, patchOffset, SEEK_SET);
	fwrite (pAH, 1, ddmemSize, appFile);
	fclose (appFile);

	free (appFileData);
	free (dldiFileData);

	printf ("Patched successfully\n");

	return 0;
}

