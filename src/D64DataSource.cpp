#include <stdio.h>
#include <string.h>
#include "D64DataSource.h"
#include "cbmlayout.h"

bool D64DataSource::initWithDataSource(DataSource* dataSource, const char* fileName, ConsoleLogger* logger)
{
    _fileDataSource = dataSource;
    _logger = logger;
    _cbmTrackLayout = (uint32_t*)LAYOUT_CBM;

    // mount the file as a D64 drive
    bool success = _fileDataSource->openFileForReading((unsigned char*)fileName);
    if (!success)
    {
        return false;
    }
    _fileDataSource->requestReadBufferSize(BLOCK_SIZE);

    cbmMount();
    return true;
}

void D64DataSource::openFileForWriting(unsigned char* fileName) 
{
    printf("open file for writing\n");
    memcpy(_fileName, fileName, 21);
    printf("h\n");
    _cbmBuffer = _fileDataSource->getBuffer();
    _fileTrackBlock[0] = 1;
    _fileTrackBlock[1] = 0;

    cbmFindEmptyBlock(_fileTrackBlock);
}

bool D64DataSource::openFileForReading(unsigned char* fileName) 
{
    CBMFile_Entry* entry = cbmSearch(fileName, 0);
    if (entry == NULL)
    {
        return false;
    }

    _fileTrackBlock[0] = entry->dataBlock[0];
    _fileTrackBlock[1] = entry->dataBlock[1];
    return true;
}


uint32_t D64DataSource::seek(unsigned int pos) 
{
    return 0; 
}

bool D64DataSource::openDirectory(const char* dirName) 
{
    _dirTrackBlock[0] = 18;
    _dirTrackBlock[1] = 1;
    _dirIndexInBuffer = 0;

    cbmReadBlock(_dirTrackBlock);
    return true;
}
unsigned int D64DataSource::getNextFileBlock() 
{
    if (_fileTrackBlock[0] == 0)
    {
        return 0;
    }

    cbmReadBlock(_fileTrackBlock);

    _fileTrackBlock[0] = _cbmBuffer[0];
    _fileTrackBlock[1] = _cbmBuffer[1];

    if (_fileTrackBlock[0] == 0)
    {
        return _fileTrackBlock[1] - 1;
    }

    return BLOCK_SIZE - 2;
}


bool D64DataSource::isLastBlock() 
{
    if (_fileTrackBlock[0] == 0)
    {
        return true;
    }
    
    return false;
}

bool D64DataSource::getNextDirectoryEntry() 
{
    do 
    {
        _currentFileEntry = cbmGetNextFileEntry();
        if (_currentFileEntry == NULL)
        {
            return false;
        }
    } 
    while (_currentFileEntry->fileType == 0x80); // skip deleted

    return true;
}

bool D64DataSource::isInitialized() 
{
    return true;
}

void D64DataSource::writeBufferToFile(unsigned int numBytes) 
{
    // writing a single data buffer
    // data will be in bytes 2->BLOCK_SIZE
    // write track and block into first 2 bytes

    //uint8_t tb[2];
    uint8_t tb[2];
    tb[0] = _fileTrackBlock[0];
    tb[1] = _fileTrackBlock[1];

    printf("writeBufferToFile %d t %d b %d\n", numBytes, tb[0], tb[1]);
    cbmBAM(tb, 'a');
    if (numBytes < writeBufferSize())
    {
        _cbmBuffer[0] = 0;
        _cbmBuffer[1] = numBytes - 1; // TODO: fix this value
    }
    else
    {
        // find the next empty block
        if (!cbmFindEmptyBlock(_fileTrackBlock))
        {
            printf("no\n");
            return;
        }

        // point to the next block
        _cbmBuffer[0] = _fileTrackBlock[0];
        _cbmBuffer[1] = _fileTrackBlock[1];
    }

    cbmWriteBlock(tb);
}

void D64DataSource::cbmWriteBlock(uint8_t* tb)
{
    uint32_t loc = cbmBlockLocation(tb);
    uint32_t pos = _fileDataSource->seek(loc);

    printf("writing %d %d, loc %d\n", tb[0], tb[1], loc);
    _fileDataSource->writeBufferToFile(BLOCK_SIZE);
}

void D64DataSource::closeFile()
{
    printf("closeFile\n");
}

void D64DataSource::openCurrentDirectory() 
{
    _dirTrackBlock[0] = 18;
    _dirTrackBlock[1] = 1;
    _dirIndexInBuffer = 0;

    cbmReadBlock(_dirTrackBlock);

    _dirTrackBlock[0] = _cbmBuffer[0];
    _dirTrackBlock[1] = _cbmBuffer[1];
    _currentFileEntry = NULL;
}
unsigned char* D64DataSource::getFilename() 
{
    if (_currentFileEntry)
    {
        memset(_fileName, 0, 21);
        cbmCopyString(_fileName, _currentFileEntry->fileName);
        int len = strlen((char*)_fileName);
        if (_currentFileEntry->fileType == 0x81)
        {
            _fileName[len] = '.';
            _fileName[len+1] = 'S';
            _fileName[len+2] = 'E';
            _fileName[len+3] = 'Q';
        }
        else if (_currentFileEntry->fileType == 0x82)
        {
            _fileName[len] = '.';
            _fileName[len+1] = 'P';
            _fileName[len+2] = 'R';
            _fileName[len+3] = 'G';
        }
        else if (_currentFileEntry->fileType == 0x83)
        {
            _fileName[len] = '.';
            _fileName[len+1] = 'U';
            _fileName[len+2] = 'S';
            _fileName[len+3] = 'R';
        }
        else if (_currentFileEntry->fileType == 0x84)
        {
            _fileName[len] = '.';
            _fileName[len+1] = 'R';
            _fileName[len+2] = 'E';
            _fileName[len+3] = 'L';
        }

        return _fileName;
    }
    return NULL;
}
unsigned char* D64DataSource::getBuffer() 
{
    return _cbmBuffer + 2;
}
unsigned int D64DataSource::writeBufferSize() 
{
    return BLOCK_SIZE - 2;
}

unsigned int D64DataSource::readBufferSize() 
{
    return BLOCK_SIZE - 2;
}

uint32_t D64DataSource::cbmBlockLocation(uint8_t* tb)
{
    uint8_t track = tb[0];
    uint8_t block = tb[1];
    return _cbmTrackLayout[track-1] + (block * BLOCK_SIZE);
}

void D64DataSource::cbmMount()
{
    printf("load header\n");
    CBMHeader* header = cbmLoadHeader();
    printf("done load header\n");
    // copy the bam
    memcpy(_cbmBAM, header->bam, BAM_SIZE);
    printf("done mount\n");
}

uint8_t* D64DataSource::cbmReadBlock(uint8_t* tb)
{

    uint32_t loc = cbmBlockLocation(tb);
    printf("cbmReadBlock %d %d %ld\n", tb[0], tb[1], loc);

    // seek to the right place in datasource
    uint32_t actualPos = _fileDataSource->seek(loc);
    uint32_t offset = loc - actualPos;

    _fileDataSource->getNextFileBlock();
    uint8_t* buf = _fileDataSource->getBuffer();
    
    // point to the buffer containing this block
    _cbmBuffer = &buf[offset];
    return _cbmBuffer;
}

void D64DataSource::cbmPrintHeader(CBMDisk* disk)
{
    CBMHeader* header = &(disk->header);
    _logger->log("diskName: ");
    char tmp[8];
    for (int i = 0; i < 16; i++)
    {
        sprintf(tmp, "%c", header->diskName[i]);
        _logger->log(tmp);
    }
    _logger->log("\r\n");
}

CBMHeader* D64DataSource::cbmLoadHeader()
{
    uint8_t tb[]={18,0};
    uint8_t *buffer;

    buffer = cbmReadBlock(tb);
    if (buffer != NULL)
    {
        return (CBMHeader*)buffer;
    }

    return NULL;
}

void D64DataSource::cbmPrintFileEntry(CBMFile_Entry* entry)
{
    char tmp[32];
    _logger->log("fname: ");
    for (int i = 0; i < 16; i++)
    {
        sprintf(tmp, "%c", entry->fileName[i]);
        _logger->log(tmp);
    }
    sprintf(tmp, " \t\ttype: %X\r\ndataBlock %X %X\r\n", entry->fileType, entry->dataBlock[0], entry->dataBlock[1]);
    _logger->log(tmp);
}

CBMFile_Entry* D64DataSource::cbmGetNextFileEntry()
{
    int numEntriesInBlock = BLOCK_SIZE / sizeof(CBMFile_Entry);
    if (_dirIndexInBuffer >= numEntriesInBlock)
    {
        if (_dirTrackBlock[0] == 0)
        {
            return NULL;
        }
        // read next block
        cbmReadBlock(_dirTrackBlock);
        _dirTrackBlock[0] = _cbmBuffer[0];
        _dirTrackBlock[1] = _cbmBuffer[1];
        _dirIndexInBuffer = 0;
    }

    CBMFile_Entry* entriesInBlock = (CBMFile_Entry*)_cbmBuffer;
    CBMFile_Entry* entry = &entriesInBlock[_dirIndexInBuffer++];

    return entry;
}

uint8_t* D64DataSource::cbmCopyString(uint8_t* dest, const uint8_t* source)
{
    for (int c = 0; c < 17; c++)
    {
        if (source[c] == 0 || source[c] == 160 || source[c] == '.')
        {
            break;
        }

        dest[c] = source[c];
    }

    return dest;
}

uint8_t* D64DataSource::cbmD64StringCString(uint8_t* dest, const uint8_t* source)
{
    memset(dest, ' ', 17);
    dest[17] = 0;

    return cbmCopyString(dest, source);
}

CBMFile_Entry* D64DataSource::cbmSearch(uint8_t* searchNameA, uint8_t fileType)
{
    uint8_t fileName[18];
    uint8_t searchName[18];

    cbmD64StringCString(searchName, searchNameA);
    openCurrentDirectory();

    CBMFile_Entry* entry = cbmGetNextFileEntry();
    while (entry != NULL)
    {
        cbmPrintFileEntry(entry);
        cbmD64StringCString(fileName, entry->fileName);

        if (strcmp((const char*)searchName, (const char*)fileName) == 0)
        {
            return entry;
        }

        entry = cbmGetNextFileEntry();
    }

    return NULL;
}

//uint8_t* D64DataSource::cbmEmptyBlockC

/*
bool D64DataSource::cbmSave(uint8_t* fileName, uint8_t fileType, CBMData* data)
{
    // search for existing file

    uint8_t* tb;
    //cbmWriteBlockChain(data);
}
*/

bool D64DataSource::cbmFindEmptyBlock(uint8_t* tb)
{
    uint8_t* sectors = cbmSectorsPerTrack();
    while (tb[0] <= MAX_TRACKS)
    {
        if (cbmIsBlockFree(tb))
        {
            printf("block is free: %d %d\n", tb[0], tb[1]);
            return true;
        }

        tb[1]++;
        if (tb[1] == sectors[tb[0]])
        {
            if (tb[0] == 17)
            {
                tb[0] += 2;
            }
            else
            {
                tb[0] += 1;
            }

            tb[1] = 0;
        }
    }

    return false;
}

bool D64DataSource::cbmIsBlockFree(uint8_t* tb)
{
    return cbmBAM(tb, 'r');
}

uint8_t* D64DataSource::cbmSectorsPerTrack()
{
    if (_sectors[1] != 0)
    {
        return _sectors;
    }

    int b = 21;

    for (int a = 1; a <= MAX_TRACKS; a++)
    {
        if (a == 18)
        {
            b -= 2;
        }

        if (a == 25)
        {
            b--;
        }

        if (a == 31)
        {
            b--;
        }

        _sectors[a] = b;
    }

    return _sectors;
}

// Allocation routines
int D64DataSource::cbmBAM(uint8_t *tb, char s)
{
        // Three purpose function:
        // Check if a sector has been allocateded in the BAM.
        // Mark a sector allocated.
        // Mark a secotry free.
        // tb = 2 byte array, track and block to check or assign value.
        // s = 'r', 'a', 'f', r/read a/write or f/free.
        // Returns Zero for not available and non zero value for available sector.
        //byte *bam=(*disk).header.bam;
        //uint8_t* bam = disk->header.bam;
        uint8_t* bam = _cbmBAM;
        uint8_t *freeSectors;
        //unsigned int b = 0;
        uint32_t b = 0;

        // If track sent to funtion is out of range, return out of range error.
        if (tb[0] > MAX_TRACKS)
        {
            return OUT_OF_RANGE;
        }

        // Multiply (track-1) by 4 to find start of track information in BAM
        // Every 4 bytes in the BAM represents all of the
        // sector informtion for each track.

        // Bytes per track correspond as follows:
        // 1st byte number of sectors free
        // 2nd byte sectors 0-7
        // 3rd byte sectors 8-15
        // 4th byte sectors 16-20, remaining bits unused.
        // Note: a 1 in the corresponding bit field
        // indicates an avilable sector and a 0 is allocated.
        bam += (tb[0] - 1) * 4;
        freeSectors = bam;
        bam += (tb[1] / 8) + 1;     // Increment BAM pointer to proper byte.
        //b = _rotl(1, tb[1]);        // Rotate bit 1 left by number of sectors
        //if (b > 128) 
        //{
        //    b = _rotl(b,8); // Integer to byte correction for proper comparison.
        //}

        uint32_t lshift = tb[1];
        if (tb[1] >= 8)
        {
            lshift += 8;
        }

        while (lshift >= 32)
        {
            lshift -= 32;
        }

        b = 1 << lshift;

        if ('a' == s)              // 'a' allocates a sector in BAM.
        {
            if (freeSectors[0] > 0)
            {
                b ^= 0xFF;
                bam[0] &= b;
                freeSectors[0]--;
            }
        }
        if ('f'==s)  // 'f' frees a sector in BAM
        {              
            if (freeSectors[0] < 0xFF)
            {
                bam[0] |= b;
                freeSectors[0]++;
            }
        }                                
        
        return (int) (b & bam[0]);
}


DataSource* D64DataSource::getFileDataSource()
{
    return _fileDataSource;
}