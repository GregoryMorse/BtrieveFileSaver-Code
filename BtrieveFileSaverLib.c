/* 

	Copyright (C)2010 dbcoretech ltd. <BtrieveFileSaver@dbcoretech.com>

	This file is part of BtrieveFileSaver.

    BtrieveFileSaver is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    BtrieveFileSaver is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with BtrieveFileSaver.  If not, see <http://www.gnu.org/licenses/>.

	If you need further assitense or if you have questions, please contact us:
	dbcoretech.com (use our blog) or BtrieveFileSaver@dbcoretech.com

	______________________________________________________________________________________
	Change History:

	3rd August 2010 - C. Fiedler dbcoretech
	Changes to implement physical page stepping and including the 4096byte break at 
	6.x files.
	Prior to this change the lib was scanning the files by just reading binary data
	now you are jumping page wise accross the file and store the different page types.
	Thanks to Andrey Libowsky for the help on the 4096byte information. Only having this
	information you can jump on 6.x files by multiplying the page size. 

	11th August 2010 - N.Stagno dbcoretech
	Bug fix - reset GlobalCurPage variable.
	Prior to this change the tool would not be able to use multiple files in a loop because 
	the GlobalCurPage variable would not be reset to 0L while opening the new file.
	Check Line 317 (BF_OPEN)

	12. August 2010 - C. Fiedler dbcoretech
	Bug fix - read and/or extract data from files Version 5.x
	Prior to this change the tool would not find any data page on files version 5.x because 
	at the function getPageType the switch wasn't checking for BTRIEVE_FILE_V5
	Check Line 100 (getPageType)

	16th August 2010 - N.Stagno dbcoretech
	Bug fix - read and extract variable data from files Version 3.x/4.x/5.x/6.x
	Prior to the fix the function has scanned the traced VAR-Pages to find the right 
	page (using pageID). Now we use a function copied from Jim Kyles code (lp_pp)
	(Check getVariableData)

	18th August 2010 - C. Fiedler dbcoretech
	Bug fix reading files version 3.x/4.x/5.x and getting different results compared
	to BUTIL - SAVE.
	Prior to the fix the indication that a record is empty was incorrect. Now we scan 
	the record for any data other then 0x00 starting at the record adress + header
	(Check  BF_GET_REC). There are many other changes to the code so basically it is 
	a complete re-writing. we fixed:
			-	dont read pages where offset is behind the size of file
			-	dont read pages with offset 0x00 (that is allways the FCR)
			-	ignore records that are empty (V3,V4,V5)
			-	change DAT_HEADER_PAGE_V3 
			-	do not use pages that don't have a usage count
	many more minor changes too.
______________________________________________________________________________________

*/

#include "BtrieveFileSaverLib.h"
#include <limits.h>

#define BFS_PAGE_QUANTUM 512U
#define BFS_MAX_RECORD_BYTES UINT32_C(0x00100000)

static int file_range_valid(const CLIENT_STRUCT *cl, uint64_t offset, uint64_t length)
{
    return offset <= cl->fileSize && length <= cl->fileSize - offset
        && offset <= (uint64_t)LONG_MAX;
}

static int page_range_valid(const CLIENT_STRUCT *cl, size_t offset, size_t length)
{
    return offset <= cl->fPageSize && length <= (size_t)cl->fPageSize - offset;
}

/* Returns 1 for a new link, 0 for a cycle, and -1 on allocation failure. */
static int remember_fragment(uint64_t **table, size_t *capacity, size_t *count, uint64_t link)
{
    size_t index, oldIndex;
    uint64_t *grown;
    link += 1; /* Zero is the empty-entry marker. */
    if (*capacity == 0 || (*count + 1) * 2 >= *capacity) {
        const size_t nextCapacity = *capacity == 0 ? 32 : *capacity * 2;
        if (nextCapacity > (size_t)BFS_MAX_RECORD_BYTES * 4) return -1;
        grown = (uint64_t*)calloc(nextCapacity, sizeof(uint64_t));
        if (grown == NULL) return -1;
        for (oldIndex = 0; oldIndex < *capacity; ++oldIndex) {
            if ((*table)[oldIndex] == 0) continue;
            index = (size_t)((*table)[oldIndex] * UINT64_C(11400714819323198485)) & (nextCapacity - 1);
            while (grown[index] != 0) index = (index + 1) & (nextCapacity - 1);
            grown[index] = (*table)[oldIndex];
        }
        free(*table);
        *table = grown;
        *capacity = nextCapacity;
    }
    index = (size_t)(link * UINT64_C(11400714819323198485)) & (*capacity - 1);
    while ((*table)[index] != 0) {
        if ((*table)[index] == link) return 0;
        index = (index + 1) & (*capacity - 1);
    }
    (*table)[index] = link;
    ++*count;
    return 1;
}

static int compare_page_links(const void *left, const void *right)
{
    const PAGE_LINK *a = (const PAGE_LINK*)left;
    const PAGE_LINK *b = (const PAGE_LINK*)right;
    if (a->pId != b->pId) return a->pId < b->pId ? -1 : 1;
    if (a->indexA != b->indexA) return a->indexA > b->indexA ? -1 : 1;
    return a->offset < b->offset ? -1 : a->offset != b->offset;
}


/*
*
*	Function freeClient
*
*	<input>	cl				CLIENT_STRUCT pointer
*	<input> ErrorCode		unsigned short int, error code that will be returned at exit
*
*	This function releases allocated resources and returns the given error code.
*	It resets the NumXXXPages to 0L just in case the pointer will be reused later on.
*
*/
unsigned short int freeClient(CLIENT_STRUCT *cl, unsigned short int errorCode)
{
    if (cl != NULL) {
        if (cl->fHandle != NULL) fclose(cl->fHandle);
        free(cl->DATArr);
        free(cl->VATArr);
        free(cl->CUR_DPAGE);
        free(cl->CUR_VPAGE);
        memset(cl, 0, sizeof(*cl));
    }
    return errorCode;
}

/*
*
*	Function byte_swap
*	
*	<input/output>	i	4 byte integer to be byte-swapped
*
*	Just swap the bytes of a 32 bit int value
*
*/
static uint32_t byte_swap(uint32_t i )
{
	return ((i>>16)&0xFFFF) | (i<<16);
}

static uint16_t read_le16(const void *data)
{
	const unsigned char *bytes = (const unsigned char*)data;
	return (uint16_t)(bytes[0] | ((uint16_t)bytes[1] << 8));
}

static uint32_t read_le32(const void *data)
{
	const unsigned char *bytes = (const unsigned char*)data;
	return (uint32_t)bytes[0] |
		((uint32_t)bytes[1] << 8) |
		((uint32_t)bytes[2] << 16) |
		((uint32_t)bytes[3] << 24);
}

static uint32_t read_word_swapped_u32(const void *data)
{
	return (uint32_t)byte_swap(read_le32(data));
}

static void write_le32(void *data, uint32_t value)
{
	unsigned char *bytes = (unsigned char*)data;
	bytes[0] = (unsigned char)(value & 0xff);
	bytes[1] = (unsigned char)((value >> 8) & 0xff);
	bytes[2] = (unsigned char)((value >> 16) & 0xff);
	bytes[3] = (unsigned char)((value >> 24) & 0xff);
}

static uint32_t read_vrec_page_id_v5(const char *data)
{
	uint32_t value = (uint32_t)(unsigned char)data[0] |
		((uint32_t)(unsigned char)data[1] << 16) |
		((uint32_t)(unsigned char)data[2] << 24);
	return value == PAGE_KICK_OFF ? value : byte_swap(value);
}

/*
*
*	Function getPageType
*
*	<input>	cl				CLIENT_STRUCT pointer 
*	<input> tmpPage			char pointer			// the page to be identified
*
*	This function is switching the supported versions and returns the type of page if identified. 
*	Sure you can do the same thing by using a struct union but for readability reason we did it 
*	this way.
*
*/
#if defined(_MSC_VER) && _MSC_VER >= 1950
/*
 * MSVC 19.50's global optimizer drops the V3/V4/V5 arm from this sparse
 * 16-bit switch in the complete library translation unit. Keep the original
 * decoder and disable only that optimization pass for this function. The
 * WineVDM golden fixtures exercise this path in release builds.
 */
#pragma optimize("g", off)
#endif
char	getPageType (CLIENT_STRUCT *cl, char *tmpPage)
{
	switch (cl->fVersion){
		case BTRIEVE_FILE_V3:
		case BTRIEVE_FILE_V4:
		case BTRIEVE_FILE_V5:{
			if (read_le16(tmpPage + 4) & 0x8000) return DAT_PAGE_ID;
		}break;
		case BTRIEVE_FILE_V6:
		case BTRIEVE_FILE_V61:
		case BTRIEVE_FILE_V7:{
			return tmpPage[1];
		}break;
		case BTRIEVE_FILE_V8:
		case BTRIEVE_FILE_V9:{
			return tmpPage[4];
		}break;
	}
	return 0x00;
}
#if defined(_MSC_VER) && _MSC_VER >= 1950
#pragma optimize("", on)
#endif

/*
*
*	Function readPageFromFile
*
*	<input>			cl				CLIENT_STRUCT pointer 
*	<input/output>	tmpPage			char pointer holding the current page as container
*	<input>			pageOff			unsigned long page offset
*
*	Function to read a page based on it's offset within the file. 
*
*/
unsigned short int readPageFromFile(CLIENT_STRUCT *cl, void *page, unsigned long offset)
{
    if (cl == NULL || page == NULL || cl->fHandle == NULL || cl->fPageSize < BFS_PAGE_QUANTUM
        || !file_range_valid(cl, offset, cl->fPageSize)) return IO_ERROR;
    if (fseek(cl->fHandle, (long)offset, SEEK_SET) != 0
        || fread(page, cl->fPageSize, 1, cl->fHandle) != 1) return IO_ERROR;
    return NO_ERROR;
}

/*
*
*	Function addPage
*
*	<input>	cl				CLIENT_STRUCT pointer 
*	<input>	tmpPage			char pointer holding the current page as container
*	<input>	pageType		char indicating the type of page
*
*	Function to add the page if the page type is either VAT_PAGE or DAT_PAGE. 
*
*/
unsigned long int addPage(CLIENT_STRUCT *cl, char *page, char type)
{
    PAGE_LINK **array;
    unsigned long *count, *capacity, next;
    PAGE_LINK *grown;
    PAGE_LINK entry;
    long position;
    if (type == DAT_PAGE_ID) {
        array = &cl->DATArr; count = &cl->numDATPages; capacity = &cl->datCapacity;
    } else if (type == VAT_PAGE_ID) {
        array = &cl->VATArr; count = &cl->numVATPages; capacity = &cl->vatCapacity;
    } else return NO_ERROR;
    position = ftell(cl->fHandle);
    if (position < cl->fPageSize) return IO_ERROR;
    entry.offset = (unsigned long)position - cl->fPageSize;
    if (cl->fVersion >= BTRIEVE_FILE_V8) {
        entry.pId = read_le16(page);
        entry.indexA = read_le16(page + 6);
    } else {
        entry.pId = byte_swap(read_le32(page) & PAGE_KICK_OFF);
        entry.indexA = read_le16(page + 4);
    }
    if (*count == *capacity) {
        next = *capacity == 0 ? 32 : *capacity * 2;
        if (next < *capacity || next > SIZE_MAX / sizeof(PAGE_LINK)) return MEM_ERROR;
        grown = (PAGE_LINK*)realloc(*array, (size_t)next * sizeof(PAGE_LINK));
        if (grown == NULL) return MEM_ERROR;
        *array = grown; *capacity = next;
    }
    (*array)[(*count)++] = entry;
    return NO_ERROR;
}

/*
*
*	Function getNextPhysicalPage
*
*	<input>	cl				CLIENT_STRUCT pointer 
*	<input/output> tmpPage	char pointer holding the current page as container
*
*	Function to read the file until either the EOF is reached or another page is found. 
*	Note this function is changed from the original getNextPage function. The new version 
*	reads a file by its physical pages and returns the page type at the end or 0x00 if EOF.
*
*/
/* Physical scanning state is per client, not process-global. */

char	getNextPhysicalPage (CLIENT_STRUCT *cl, char* tmpPage)
{
	unsigned long int	pageID;
	char				pType = 0x00;

	pageID = cl->scanPage++;
	pageID = (pageID * cl->fPageSize);
	if (cl->fVersion >= BTRIEVE_FILE_V6 && cl->fVersion < BTRIEVE_FILE_V7){
		if (pageID >= (2 + 30 * (cl->numPointerPerPat + 2)) * cl->fPageSize)
			pageID += 4096;
	}

	/* now read the page into the CUR_DPAGE container */
	if ((readPageFromFile (cl, tmpPage, pageID)) != NO_ERROR) 
		return 0x00;
	pType = getPageType (cl, tmpPage);
	return (pType == 0x00)? MAX_SINGLE_INT_VAL : pType;
}

/*
*
*	Function readAllPagesFromFile 
*
*	<input>	cl				CLIENT_STRUCT pointer 
*
*	This function to scan the file for given pages. Note that we know of files that have 
*	holes not fitting to the page size! Therefore we are scanning in full page size and 
*	if a hole is indicated we use smaller steps. 
*
*/
unsigned short int readAllPagesFromFile(CLIENT_STRUCT *cl)
{
    uint64_t logicalPage, offset;
    unsigned long status;
    char type;
    for (logicalPage = 0; logicalPage < cl->fNumPages; ++logicalPage) {
        offset = logicalPage * cl->fPageSize;
        if (cl->fVersion >= BTRIEVE_FILE_V6 && cl->fVersion < BTRIEVE_FILE_V7
            && logicalPage >= 2 + 30 * (cl->numPointerPerPat + 2)) offset += 4096;
        if (!file_range_valid(cl, offset, cl->fPageSize)) break;
        if (readPageFromFile(cl, cl->CUR_DPAGE, (unsigned long)offset) != NO_ERROR) return IO_ERROR;
        type = getPageType(cl, cl->CUR_DPAGE);
        status = addPage(cl, cl->CUR_DPAGE, type);
        if (status != NO_ERROR) return (unsigned short)status;
    }
    write_le32(cl->CUR_DPAGE, 0);
    return NO_ERROR;
}

/*
*
*	Function sortPages
*	
*	<input/output>	in		CLIENT_STRUCT pointer, should be NULL and will be used to allocate memory 
*	<input>	pageType		char indicating the type of page
*
*	This function reorganizes all pages by sorting them based on pageId. 
*
*/
void sortPages(CLIENT_STRUCT *cl, char type)
{
    PAGE_LINK *array;
    unsigned long *count, readIndex, written = 0;
    if (type == DAT_PAGE_ID) { array = cl->DATArr; count = &cl->numDATPages; }
    else if (type == VAT_PAGE_ID) { array = cl->VATArr; count = &cl->numVATPages; }
    else return;
    if (*count > 1) qsort(array, *count, sizeof(PAGE_LINK), compare_page_links);
    for (readIndex = 0; readIndex < *count; ++readIndex) {
        if (written == 0 || array[readIndex].pId != array[written - 1].pId)
            array[written++] = array[readIndex];
    }
    *count = written;
}

/*
*
*	Function BF_OPEN (Btrieve File ReadRecords)
*	
*	<input/output>	in				CLIENT_STRUCT pointer, should be NULL and will be used to allocate memory 
*	<input>			fName			char pointer containing the file that should be read
*
*	This function offers the entrance to the lib by opening a file. The file later can be referenced by its
*	cl pointer. This is similar to Btrieve where the position block is a concurrency storage. 
*
*/
unsigned short int BF_OPEN(CLIENT_STRUCT *cl, char *fileName)
{
    FCR header, shadow;
    unsigned int pageSize, dataHeader, requiredRecord;
    unsigned short status;
    long size;
    if (cl == NULL) return CLIENT_CONNECTION_ERROR;
    memset(cl, 0, sizeof(*cl));
    cl->curRecordId = MAX_LONG_INT_VAL;
    cl->curRecOff = MAX_LONG_INT_VAL;
    cl->curDPageID = 1;
    cl->fHandle = fileName == NULL ? NULL : fopen(fileName, "rb");
    if (cl->fHandle == NULL) return IO_ERROR;
    if (fread(&header, sizeof(header), 1, cl->fHandle) != 1) return freeClient(cl, IO_ERROR);
    if (header.PageId == 0) {
        cl->fVersion = (short)abs(header.Version);
        cl->recHeaderSize = 0;
    } else if (header.PageId == FCR_IDENT) {
        cl->fVersion = (short)abs(header.NewFileVersion);
        cl->recHeaderSize = 2;
    } else return freeClient(cl, NO_BTR_FILE);
    switch (cl->fVersion) {
    case BTRIEVE_FILE_V3: case BTRIEVE_FILE_V4: case BTRIEVE_FILE_V5:
    case BTRIEVE_FILE_V6: case BTRIEVE_FILE_V61: case BTRIEVE_FILE_V7:
    case BTRIEVE_FILE_V8: case BTRIEVE_FILE_V9: break;
    default: return freeClient(cl, NO_BTR_FILE);
    }
    pageSize = cl->fVersion >= BTRIEVE_FILE_V8
        ? (unsigned int)(unsigned short)header.PageSizeV8 * PAGE_SIZE_MULTIPLIER
        : (unsigned short)header.PageSize;
    if (pageSize < BFS_PAGE_QUANTUM || pageSize > UINT16_MAX
        || pageSize % BFS_PAGE_QUANTUM != 0) return freeClient(cl, IO_ERROR);
    cl->fPageSize = (unsigned short)pageSize;
    cl->IFixedRecLen = header.IFixedRecordLength;
    cl->FixRecLen = header.FixedRecordLength;
    cl->VarRecsAllowed = header.VarRecsAllowed;
    cl->VFragParam = cl->fVersion >= BTRIEVE_FILE_V8 ? VFRAG_CUT_V8
        : cl->fVersion > BTRIEVE_FILE_V5 ? VFRAG_CUT_V6 : VFRAG_CUT_V5;
    dataHeader = cl->fVersion >= BTRIEVE_FILE_V8 ? DAT_PAGE_HEADER_SIZE_V8 : DAT_PAGE_HEADER_SIZE_V6;
    requiredRecord = cl->FixRecLen + cl->recHeaderSize;
    if (cl->VarRecsAllowed) requiredRecord += cl->fVersion >= BTRIEVE_FILE_V8 ? 12 : 4;
    if (cl->FixRecLen == 0 || cl->IFixedRecLen < requiredRecord
        || cl->IFixedRecLen > pageSize - dataHeader) return freeClient(cl, IO_ERROR);
    if (fseek(cl->fHandle, 0, SEEK_END) != 0 || (size = ftell(cl->fHandle)) < 0)
        return freeClient(cl, IO_ERROR);
    cl->fileSize = (uint64_t)size;
    if (cl->fileSize < pageSize || cl->fileSize % pageSize != 0) return freeClient(cl, IO_ERROR);
    cl->fNumPages = (unsigned long)(cl->fileSize / pageSize);
    if (cl->fVersion > BTRIEVE_FILE_V5) {
        if (!file_range_valid(cl, pageSize, sizeof(shadow))
            || fseek(cl->fHandle, pageSize, SEEK_SET) != 0
            || fread(&shadow, sizeof(shadow), 1, cl->fHandle) != 1) return freeClient(cl, IO_ERROR);
        cl->numRecs = read_word_swapped_u32(
            header.FCRUsageCount > shadow.FCRUsageCount ? header.numRecs : shadow.numRecs);
    } else cl->numRecs = read_word_swapped_u32(header.numRecs);
    if (cl->fVersion >= BTRIEVE_FILE_V6 && cl->fVersion < BTRIEVE_FILE_V7)
        cl->numPointerPerPat = (pageSize - PAT_PAGE_HEADER_SIZE_V6) / PAT_POINTER_SIZE_V6;
    cl->CUR_DPAGE = (char*)calloc(pageSize, 1);
    if (cl->CUR_DPAGE == NULL) return freeClient(cl, MEM_ERROR);
    status = readAllPagesFromFile(cl);
    if (status != NO_ERROR) return freeClient(cl, status);
    sortPages(cl, DAT_PAGE_ID);
    sortPages(cl, VAT_PAGE_ID);
    if (cl->VarRecsAllowed) {
        cl->CUR_VPAGE = (char*)calloc(pageSize, 1);
        if (cl->CUR_VPAGE == NULL) return freeClient(cl, MEM_ERROR);
    }
    return NO_ERROR;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *\
 *                                                           *
 *      This procedure converts a Logical Page number to     *
 *      a file offset, using the PAT for new format files.   *
 *                                                           *
\* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */
static long int lp_pp(CLIENT_STRUCT *cl, long int logicalPage)
{
    uint64_t offset, first, second, pat;
    unsigned long pagesPerPat;
    unsigned short firstUsage, secondUsage;
    unsigned char raw[4];
    if (logicalPage < 0 || logicalPage == 0xFFFFFFL) return -1;
    if (cl->fVersion > BTRIEVE_FILE_V5) {
        pagesPerPat = (cl->fPageSize >> 2) - 2;
        pat = 2;
        while ((unsigned long)logicalPage > pagesPerPat) {
            logicalPage -= pagesPerPat;
            pat += cl->fPageSize >> 2;
            if (pat >= cl->fNumPages) return -1;
        }
        first = pat * cl->fPageSize;
        second = first + cl->fPageSize;
        if (!file_range_valid(cl, first + 4, 2) || !file_range_valid(cl, second + 4, 2)) return -1;
        if (fseek(cl->fHandle, (long)(first + 4), SEEK_SET) != 0 || fread(raw, 2, 1, cl->fHandle) != 1) return -1;
        firstUsage = read_le16(raw);
        if (fseek(cl->fHandle, (long)(second + 4), SEEK_SET) != 0 || fread(raw, 2, 1, cl->fHandle) != 1) return -1;
        secondUsage = read_le16(raw);
        offset = (firstUsage > secondUsage ? first : second) + ((uint64_t)logicalPage << 2) + 4;
        if (!file_range_valid(cl, offset, 4) || fseek(cl->fHandle, (long)offset, SEEK_SET) != 0
            || fread(raw, 4, 1, cl->fHandle) != 1) return -1;
        logicalPage = byte_swap(read_le32(raw)) & 0xFFFFFFL;
        if (logicalPage == 0xFFFFFFL) return -1;
    }
    offset = (uint64_t)logicalPage * cl->fPageSize;
    return file_range_valid(cl, offset, cl->fPageSize) ? (long)offset : -1;
}

/*
*
*	Function getNextDataPage
*	
*	<input/output>	cl				CLIENT_STRUCT pointer	-	containing information about concurrency and ressources
*
*	Jump to the next available Data page and if non, return END_OF_FILE.
*
*/
unsigned short int getNextDataPage (CLIENT_STRUCT *cl)
{
	unsigned short int retval = NO_ERROR;
	long int pOff;
	unsigned long int cPId = cl->curDPageID;

	memset (cl->CUR_DPAGE, 0x00, cl->fPageSize);

	while (getPageType (cl, cl->CUR_DPAGE) != DAT_PAGE_ID){
		uint64_t fileSize;

		if (cPId >= cl->fNumPages)
			return END_OF_FILE;

		pOff = lp_pp (cl, cPId++);
		fileSize = (uint64_t)cl->fNumPages * cl->fPageSize;
		if (pOff < 0 || (uint64_t)pOff >= fileSize)
			continue;

		if ((retval = readPageFromFile (cl, cl->CUR_DPAGE, (unsigned long)pOff)) != NO_ERROR)
			return retval;

		if (cl->fVersion == BTRIEVE_FILE_V6 && read_le16(cl->CUR_DPAGE + 2) != cPId - 1){
			write_le32(cl->CUR_DPAGE, 0);
			continue;
		}

		if (cl->fVersion == BTRIEVE_FILE_V3){
			uint32_t usage = ((uint32_t)(unsigned char)cl->CUR_DPAGE[4] << 16)
				| ((uint32_t)(unsigned char)cl->CUR_DPAGE[6] << 8)
				| (unsigned char)cl->CUR_DPAGE[7];
			if (usage == 0){
				memset (cl->CUR_DPAGE, 0x00, 10);
				continue;
			}
		}
	}
	cl->curDPageID = cPId;
	return NO_ERROR;
}

/*
*
*	Function getVariableData
*	
*	<input/output>	cl				CLIENT_STRUCT pointer	-	containing information about concurrency and ressources
*	<output>		dataBuffer		char pointer			-	to retrieve the requested record data
*	<input/output>	dbLen			unsigned long pointer	-	length indicator
*
*	Copy the data of the current record and retrieve its variable portion.
*
*/
unsigned short int getVariableData(CLIENT_STRUCT *cl, char *buffer, unsigned long *bufferLength, char *record)
{
    uint32_t pageId;
    unsigned int fragmentNumber;
    unsigned long length = cl->FixRecLen, capacity = *bufferLength;
    uint64_t *visited = NULL;
    size_t visitedCapacity = 0, visitedCount = 0;
    unsigned short status = NO_ERROR;
    size_t recordOffset = (size_t)(record - cl->CUR_DPAGE);
    size_t pointerOffset = recordOffset + cl->recHeaderSize + cl->FixRecLen;
    if (!page_range_valid(cl, recordOffset + cl->recHeaderSize, cl->FixRecLen)
        || !page_range_valid(cl, pointerOffset, cl->fVersion >= BTRIEVE_FILE_V8 ? 12 : 4)) return IO_ERROR;
    if (buffer != NULL && capacity > 0)
        memcpy(buffer, record + cl->recHeaderSize, length < capacity ? length : capacity);
    if (cl->fVersion >= BTRIEVE_FILE_V8) {
        pageId = read_le16(cl->CUR_DPAGE + pointerOffset);
        fragmentNumber = read_le16(record + cl->FixRecLen + 10);
    } else {
        pageId = read_vrec_page_id_v5(cl->CUR_DPAGE + pointerOffset);
        fragmentNumber = (unsigned char)cl->CUR_DPAGE[pointerOffset + 3];
    }
    while (pageId != PAGE_KICK_OFF && fragmentNumber != MAX_SHORT_INT_VAL) {
        long pageOffset = -1;
        unsigned long index;
        size_t tableIndex, endIndex, fragmentOffset, endOffset, dataOffset, added;
        unsigned short tableEntry;
        int remembered = remember_fragment(&visited, &visitedCapacity, &visitedCount,
            ((uint64_t)pageId << 16) | fragmentNumber);
        if (remembered <= 0) { status = remembered < 0 ? MEM_ERROR : IO_ERROR; break; }
        if (cl->fVersion >= BTRIEVE_FILE_V7) {
            for (index = 0; index < cl->numVATPages; ++index)
                if (cl->VATArr[index].pId == pageId) { pageOffset = (long)cl->VATArr[index].offset; break; }
        } else pageOffset = lp_pp(cl, pageId);
        if (pageOffset < 0 || readPageFromFile(cl, cl->CUR_VPAGE, (unsigned long)pageOffset) != NO_ERROR) {
            status = IO_ERROR; break;
        }
        tableIndex = (cl->fPageSize - 1U) / sizeof(uint16_t);
        if (fragmentNumber >= tableIndex) { status = IO_ERROR; break; }
        tableIndex -= fragmentNumber;
        tableEntry = read_le16(cl->CUR_VPAGE + tableIndex * sizeof(uint16_t));
        if (tableEntry == UINT16_MAX) { status = IO_ERROR; break; }
        fragmentOffset = tableEntry & 0x7FFF;
        endIndex = tableIndex;
        do {
            if (endIndex == 0) { status = IO_ERROR; break; }
            --endIndex;
        } while (read_le16(cl->CUR_VPAGE + endIndex * sizeof(uint16_t)) == UINT16_MAX);
        if (status != NO_ERROR) break;
        endOffset = read_le16(cl->CUR_VPAGE + endIndex * sizeof(uint16_t)) & 0x7FFF;
        if (endOffset <= fragmentOffset || !page_range_valid(cl, fragmentOffset, endOffset - fragmentOffset)
            || endOffset > endIndex * sizeof(uint16_t)) { status = IO_ERROR; break; }
        dataOffset = fragmentOffset + cl->VFragParam;
        if (cl->fVersion < BTRIEVE_FILE_V6) {
            if ((tableEntry & 0x8000) != 0) {
                if (endOffset - fragmentOffset < sizeof(VRECPTR)) { status = IO_ERROR; break; }
                pageId = read_vrec_page_id_v5(cl->CUR_VPAGE + fragmentOffset);
                fragmentNumber = (unsigned char)cl->CUR_VPAGE[fragmentOffset + 3];
                dataOffset += sizeof(VRECPTR);
            } else {
                pageId = PAGE_KICK_OFF;
                fragmentNumber = MAX_SHORT_INT_VAL;
            }
        } else {
            if (endOffset - fragmentOffset < (size_t)cl->VFragParam) { status = IO_ERROR; break; }
            if (cl->fVersion >= BTRIEVE_FILE_V8) {
                pageId = read_le16(cl->CUR_VPAGE + fragmentOffset);
                fragmentNumber = read_le16(cl->CUR_VPAGE + fragmentOffset + 4);
            } else {
                pageId = read_vrec_page_id_v5(cl->CUR_VPAGE + fragmentOffset);
                fragmentNumber = (unsigned char)cl->CUR_VPAGE[fragmentOffset + 3];
            }
        }
        if (dataOffset > endOffset) { status = IO_ERROR; break; }
        added = endOffset - dataOffset;
        if (added == 0 || added > BFS_MAX_RECORD_BYTES - length) { status = IO_ERROR; break; }
        if (buffer != NULL && length < capacity) {
            size_t available = capacity - length;
            memcpy(buffer + length, cl->CUR_VPAGE + dataOffset, added < available ? added : available);
        }
        length += (unsigned long)added;
    }
    free(visited);
    *bufferLength = length;
    if (status == NO_ERROR && (capacity == 0 || capacity < length)) status = DATA_BUFFER_TO_SHORT;
    return status;
}

/*
*
*	Function getNextRecord
*	
*	<input/output>	cl				CLIENT_STRUCT pointer	-	containing information about concurrency and ressources
*
*	get the next available record pointer.
*
*/
signed short int getNextRecord(CLIENT_STRUCT *cl)
{
    const unsigned int header = cl->fVersion >= BTRIEVE_FILE_V8 ? DAT_PAGE_HEADER_SIZE_V8 : DAT_PAGE_HEADER_SIZE_V6;
    unsigned long count, offset;
    unsigned short status;
    if (cl->IFixedRecLen == 0 || cl->fPageSize < header) return IO_ERROR;
    count = (cl->fPageSize - header) / cl->IFixedRecLen;
    if (cl->curRecordId != MAX_LONG_INT_VAL && cl->curRecordId >= cl->numRecs - 1) return END_OF_FILE;
    if (cl->curRecOff >= count) {
        status = getNextDataPage(cl);
        if (status != NO_ERROR) return status;
        cl->curRecOff = 0;
    }
    offset = header + cl->curRecOff * cl->IFixedRecLen;
    if (!page_range_valid(cl, offset, cl->IFixedRecLen)) return IO_ERROR;
    cl->curRecAdr = cl->CUR_DPAGE + offset;
    ++cl->curRecOff;
    return NO_ERROR;
}

/*
*
*	Function BF_GET_REC (Btrieve File Get Next Record)
*	
*	<input/output>	cl				CLIENT_STRUCT pointer	-	containing information about concurrency and ressources
*	<output>		dataBuffer		char pointer			-	to retrieve the requested records
*	<input/output>	dbLen			unsigned long pointer	-	length indicator
*
*	Afer a client has opened a file it can start reading the file. To do so he calls BF_GET_REC to retrieve all records
*	based on their physical location within the file. The function returns EOF if no more records are available. 
*	Note that there is no such function like step first. This is because BF_GET_REC recognizes if your are at the 
*	beginning of a file.
*
*/
unsigned short int	BF_GET_REC (CLIENT_STRUCT *cl, char *dataBuffer, unsigned long *dbLen)
{
	signed short int	retval		= NO_ERROR;

	if (!cl) return CLIENT_CONNECTION_ERROR;

	if (cl->numRecs == 0L) return END_OF_FILE;
	while ((retval = getNextRecord (cl)) == NO_ERROR){
		if (cl->fVersion >= BTRIEVE_FILE_V6 && read_le16(cl->curRecAdr) != 0x0000){
			break;
		}else if (cl->fVersion < BTRIEVE_FILE_V6){
			unsigned long int	i		= 0;
			char				isEmpty	= true;

			if (cl->fVersion == BTRIEVE_FILE_V3 && read_le16(cl->curRecAdr) == 0x0000) continue;

			for (i = ((cl->fVersion >= BTRIEVE_FILE_V4)? RECORD_HEADER_SIZE_V4 : RECORD_HEADER_SIZE_V3) ; 
					i < cl->IFixedRecLen ; i++){ 
				if (cl->curRecAdr[i]){
					isEmpty = false;
					break;
				}
			}

			if ((isEmpty == true)) continue;
			else break;
		}
	}

	if (retval != NO_ERROR) return retval;

	if (cl->curRecordId == MAX_LONG_INT_VAL)
		cl->curRecordId = 0;
	else
		cl->curRecordId ++;

	if (cl->VarRecsAllowed){
		retval = getVariableData (cl, dataBuffer, dbLen, cl->curRecAdr);
	}else{
		if (dataBuffer && *dbLen > 0)
			memcpy (dataBuffer, cl->curRecAdr + cl->recHeaderSize, (cl->FixRecLen > *dbLen) ? *dbLen : cl->FixRecLen);
		if (*dbLen == 0 || *dbLen < cl->FixRecLen){
			retval = DATA_BUFFER_TO_SHORT;
		}
		*dbLen = cl->FixRecLen;
	}
	return retval;
}

/*
*
*	Function BF_CLOSE (Btrieve File Close)
*	
*	<input/output>	in				CLIENT_STRUCT pointer
*
*	This function releases all resources loaded by the client. 
*
*/
unsigned short int	BF_CLOSE (CLIENT_STRUCT *cl)
{
	if (!cl) return CLIENT_CONNECTION_ERROR;
	return freeClient (cl, NO_ERROR);
}
