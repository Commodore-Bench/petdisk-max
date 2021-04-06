#include <avr/io.h>
#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/eeprom.h>
#include <util/delay.h>
#include "EspConn.h"
#include "Buffer.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdint.h>
#include "Serial.h"
#include "SerialLogger.h"
#include "EspHttp.h"
#include "SD_routines.h"
#include "SPI_routines.h"
#include "FAT32.h"
#include "IEEE488.h"
#include "DataSource.h"
#include "NetworkDataSource.h"
#include "D64DataSource.h"
#include "githash.h"

//#ifdef TESTING 1

#define ESP_CONTROL     DDRD
#define ESP_PORT        PORTD
#define ESP_CH_PD       PD4
#define ESP_RST         PD5
#define ESP_GPIO0       PD7
#define ESP_GPIO2       PD6

#define LED_CONTROL     DDRB
#define LED_PORT        PORTB
#define LED_PIN1        PB0
#define LED_PIN2        PB1

// addresses for PET IEEE commands
#define PET_LOAD_FNAME_ADDR     0xF0
#define PET_SAVE_FNAME_ADDR     0xF1
#define PET_OPEN_FNAME_MASK     0xF0
#define PET_READ_CMD_ADDR       0x60
#define PET_SAVE_CMD_ADDR       0x61
#define PET_OPEN_IO_ADDR        0x60
#define PET_CLOSE_FILE          0xE0

#define PET_ADDRESS_MASK        0x0F

const unsigned char _dirHeader[] PROGMEM =
{
    0x01,
    0x04,
    0x1F,
    0x04,
    0x00,
    0x00,
    0x12,
};

const unsigned char _versionString[] PROGMEM = "\"PETDISK MAX V1.0\"      ";
const unsigned char _firmwareString[] PROGMEM = "BUILD ";
const unsigned char _fileExtension[] PROGMEM =
{
    '.',
    'P',
    'R',
    'G',
    0x00,
};

const unsigned char _seqExtension[] PROGMEM =
{
    '.',
    'S',
    'E',
    'Q',
    0x00,
};

void pgm_memcpy(unsigned char *dest, unsigned char *src, int len)
{
    int i;
    for (i = 0; i < len; i++)
    {
        *dest++ = pgm_read_byte(&(*src++));
    }
}

void prog_init()
{
    // set reset for esp
    ESP_PORT = (1 << ESP_CH_PD) | (1 << ESP_RST);
    ESP_CONTROL = (1 << ESP_CH_PD) | (1 << ESP_RST);
    sei();
}
 
void reset_esp()
{
    ESP_PORT &= ~(1 << ESP_RST);
    ESP_PORT &= ~(1 << ESP_CH_PD);

    for (int i = 0; i < 20; i++)
    {
        _delay_loop_2(65535);
    }

    ESP_PORT |= 1 << ESP_RST;
    ESP_PORT |= 1 << ESP_CH_PD;
}

void init_led()
{
    LED_CONTROL |= 1 << LED_PIN1;
    LED_CONTROL |= 1 << LED_PIN2;
}

void set_led(bool value)
{
    if (value == true)
    {
        LED_PORT |= 1 << LED_PIN1;
        LED_PORT |= 1 << LED_PIN2;
    }
    else
    {
        LED_PORT &= ~(1 << LED_PIN1);
        LED_PORT &= ~(1 << LED_PIN2);
    }
}

void blink_led(int count, int ms_on, int ms_off)
{
    set_led(false);
    for (int i = 0; i < count; i++)
    {
        set_led(true);
        for (int j = 0; j < ms_on; j++)
        {
            _delay_ms(1);
        }
        set_led(false);
        for (int j = 0; j < ms_off; j++)
        {
            _delay_ms(1);
        }
    }
}

void checkForFirmware(char* buffer, FAT32* fat32, Serial1* log)
{
    log->transmitStringF(PSTR("checking\r\n"));
    if (!fat32->init())
    {
        log->transmitStringF(PSTR("noinit\r\n"));
        return;
    }

    sprintf_P(buffer, PSTR("FIRM*"));
    fat32->openCurrentDirectory();
    if (!fat32->findFile(buffer))
    {
        log->transmitStringF(PSTR("nofirm\r\n"));
        return;
    }

    log->transmitStringF(PSTR("gotfirmware\r\n"));
    // found firmware file, delete
    fat32->deleteFile();
}

void sd_test(DataSource* dataSource, Serial1* log)
{
    if (!dataSource->init())
    {
        log->transmitStringF(PSTR("no init\r\n"));
    }

    log->transmitStringF(PSTR("init\r\n"));
}

#define MIN_DEVICE_ID 8
#define MAX_DEVICE_ID 16

// define a configuration structure
// this will be written to eeprom

#define DEVICE_NONE         0  
#define DEVICE_SD0          1
#define DEVICE_SD1          2
#define DEVICE_URL_BASE     3
#define DEVICE_END          3

#define FILENAME_MAX_LENGTH 21

struct pd_config {
    uint8_t device_type[9];
    char urls[4][64];
    char wifi_ssid[33];
    char wifi_password[64];
};

typedef enum _pdstate
{
    IDLE,
    BUS_LISTEN,
    BUS_TALK,
    LOAD_FNAME_READ,
    SAVE_FNAME_READ,
    OPEN_FNAME_READ,
    OPEN_FNAME_READ_DONE,
    OPEN_FNAME_READ_DONE_FOR_WRITING,
    OPEN_DATA_WRITE,
    OPEN_DATA_WRITE_DONE,
    OPEN_DATA_READ,
    DIR_READ,
    FILE_READ_OPENING,
    FILE_SAVE_OPENING,
    FILE_READ,
    FILE_SAVE,
    OPEN_FNAME,
    FILE_NOT_FOUND,
    CLOSING
} pdstate;

typedef enum _filedir
{
    FNONE,
    FREAD,
    FWRITE
} filedir;

typedef struct _openFileInfo
{
    char _fileName[FILENAME_MAX_LENGTH];
    filedir _fileDirection;
    int _fileBufferIndex;
    bool _useRemainderByte;
    int _byteIndex;
    unsigned char _remainderByte;
    unsigned char _nextByte;
    bool _opened;
    bool _command;
} openFileInfo;

class PETdisk
{
public:
    PETdisk()
    : _espConn(0)
    , _espHttp(0)
    {
        for (int i = 0; i < 8; i++)
        {
            _dataSources[i] = 0;
        }
    }
    ~PETdisk() {}

    void init(
        FAT32* fat32,
        D64DataSource* d64,
        uint8_t* buffer, 
        uint16_t* bufferSize, 
        EspConn* espConn, 
        EspHttp* espHttp, 
        NetworkDataSource** nds_array,
        IEEE488* ieee,
        SerialLogger* logger);
    void setDataSource(unsigned char id, DataSource* dataSource);
    DataSource* getDataSource(unsigned char id);

    bool readConfigFile(FAT32* fat32, uint8_t* buffer);
    void printConfig(struct pd_config* pdcfg);
    void run();

private:
    DataSource* _dataSources[8];
    EspConn* _espConn;
    EspHttp* _espHttp;
    IEEE488* _ieee;
    FAT32* _fat32;
    D64DataSource* _d64;
    SerialLogger* _logger;

    openFileInfo _openFileInformation[14];

    DataSource* _dataSource;
    unsigned char* progname;
    int _filenamePosition;
    int _fileNotFound;
    int _bytesToSend;
    pdstate _currentState;
    int _bufferFileIndex;
    unsigned char _primaryAddress;
    unsigned char _secondaryAddress;

    uint8_t _directoryEntryIndex;
    uint8_t _directoryEntryByteIndex;
    uint8_t _directoryEntry[32];
    uint16_t _directoryEntryAddress;

    bool configChanged(struct pd_config* pdcfg);
    unsigned char processFilename(unsigned char* filename, unsigned char length, bool* write);
    void writeFile();
    void listFiles();
    unsigned char wait_for_device_address();
    openFileInfo* getFileInfoForAddress(unsigned char address);
    void resetFileInformation(unsigned char address);
    bool isD64(const char* fileName);
};

void PETdisk::init(
    FAT32* fat32,
    D64DataSource* d64,
    uint8_t* buffer, 
    uint16_t* bufferSize, 
    EspConn* espConn, 
    EspHttp* espHttp, 
    NetworkDataSource** nds_array,
    IEEE488* ieee,
    SerialLogger* logger)
{
    _fat32 = fat32;
    _d64 = d64;
    _ieee = ieee;
    _logger = logger;

    // reset state variables
    _dataSource = 0;
    progname = (unsigned char*)&_buffer[1024-FILENAME_MAX_LENGTH];
    _filenamePosition = 0;
    _fileNotFound = 0;
    _bytesToSend = 0;
    _currentState = IDLE;
    _bufferFileIndex = -1;
    _secondaryAddress = 0;

    // clear all open file info structs
    for (int i = 8; i < 16; i++)
    {
        resetFileInformation(i);
    }

    char tmp[32];
    if (readConfigFile(fat32, buffer))
    {
        _logger->logF(PSTR("read config file\r\n"));
    }
    else
    {
        _logger->logF(PSTR("no config read\r\n"));
    }

    struct pd_config *pdcfg = (struct pd_config*)&buffer[512];
    eeprom_read_block(pdcfg, (void*)0, sizeof(struct pd_config));
    printConfig(pdcfg);

    // check validity of config
    if (pdcfg->device_type[0] > DEVICE_END)
    {
        // config not set
        // use defaults
        setDataSource(8, fat32);
        _logger->logF(PSTR("using default\r\n"));
        return;
    }

    _espConn = espConn;
    _espHttp = espHttp;

    bool espConnected = false;

    if (strlen(pdcfg->wifi_ssid) > 0 && strlen(pdcfg->wifi_password) > 0)
    {
        _logger->logF(PSTR("trying to connect\r\n"));
        
        bool device_present = true;
        if (!_espConn->device_present())
        {
            _logger->logF(PSTR("no device!\r\n"));
            _espConn->attempt_baud_rate_setting();
            if (_espConn->device_present())
            {
                _logger->logF(PSTR("device present at 115kbps\r\n"));
            }
            else
            {
                _logger->logF(PSTR("no device present.\r\n"));
                device_present = false;
            }
        }
        
        if (device_present)
        {
            _logger->logF(PSTR("device present\r\n"));
            if (_espConn->init())
            {
                if (_espConn->connect(pdcfg->wifi_ssid, pdcfg->wifi_password))
                {
                    _logger->logF(PSTR("trying to connect\r\n"));
                    _logger->log(pdcfg->wifi_ssid);
                    _logger->log("\r\n");
                    _logger->log(pdcfg->wifi_password);
                    _logger->log("\r\n");
                    _espConn->setDns();
                    espConnected = true;
                    _logger->logF(PSTR("connected\r\n"));
                    blink_led(2, 150, 150);
                }
                else
                {
                    _logger->logF(PSTR("no connect\r\n"));
                    blink_led(5, 150, 150);
                }
            }
            else
            {
                blink_led(4, 150, 150);
                _logger->logF(PSTR("could not connect\r\n"));
            }
        }
        else
        {
            blink_led(5, 150, 150);
        }
    }

    // check each entry in config file
    int network_drive_count = 0;
    for (int i = 0; i < 9; i++)
    {
        sprintf_P(tmp, PSTR("i %d dt %d\r\n"), i, pdcfg->device_type[i]);
        _logger->log(tmp);
        if (pdcfg->device_type[i] == DEVICE_SD0)
        {
            setDataSource(i + MIN_DEVICE_ID, fat32);
            _logger->logF(PSTR("SD0\r\n"));
        }
        else if (pdcfg->device_type[i] >= DEVICE_URL_BASE && network_drive_count < 4)
        {
            int device_id = i + MIN_DEVICE_ID;
            int url_index = pdcfg->device_type[i] - DEVICE_URL_BASE;
            int eeprom_offset = 9 + (64 * url_index);

            // find the host and url portion for this datasource
            char* url = strchr(pdcfg->urls[url_index], '/');
            int url_offset = url - pdcfg->urls[url_index];

            NetworkDataSource* nds = nds_array[network_drive_count];

            nds->setUrlData(
                (void*)eeprom_offset, 
                url_offset, 
                (void*)(eeprom_offset+url_offset), 
                strlen(pdcfg->urls[url_index]) - url_offset);

            sprintf_P(tmp, PSTR("d %d %d %d\r\n"), device_id, eeprom_offset, url_offset);
            _logger->log(tmp);
            
            if (espConnected)
            {
                setDataSource(device_id, nds);
            }
            else
            {
                setDataSource(device_id, 0);
            }

            network_drive_count++;
        }
    }
}

void PETdisk::printConfig(struct pd_config* pdcfg)
{
    char tmp[16];
    for (int i = 0; i < 9; i++)
    {
        sprintf_P(tmp, PSTR("d %d->%d\r\n"), i, pdcfg->device_type[i]);
        _logger->log(tmp);
    }

    for (int i = 0; i < 4; i++)
    {
        sprintf_P(tmp, PSTR("u %d->"), i);
        _logger->log(tmp);
        _logger->log(pdcfg->urls[i]);
        _logger->logF(PSTR("\r\n"));
    }

    _logger->logF(PSTR("ssid: "));
    _logger->log(pdcfg->wifi_ssid);
    _logger->logF(PSTR("\r\n"));

    _logger->logF(PSTR("ssid: "));
    _logger->log(pdcfg->wifi_password);
    _logger->logF(PSTR("\r\n"));
}

bool PETdisk::configChanged(struct pd_config* pdcfg)
{
    // check pdconfig against version in eeprom
    uint8_t byte;
    uint8_t* cfg = (uint8_t*)pdcfg;
    for (int i = 0; i < (int)sizeof(struct pd_config); i++)
    {
        byte = eeprom_read_byte((const uint8_t*)i);
        if (byte != cfg[i])
        {
            return true;
        }
    }
    return false;
}

bool PETdisk::readConfigFile(FAT32* fat32, uint8_t* buffer)
{
    // check for config file
    char cfg_fname[32];
    const char* config_filename = PSTR("PETDISK.CFG");
    if (!fat32->init())
    {
        return false;
    }

    sprintf_P(cfg_fname, config_filename);
    fat32->openCurrentDirectory();
    if (!fat32->openFileForReading((unsigned char*)cfg_fname))
    {
        return false;
    }

    // read config file
    fat32->getNextFileBlock();

    char* token;
    char* next;
    char* str[2];

    struct pd_config* pdcfg = (struct pd_config*)&buffer[512];

    str[0] = (char*)&buffer[512+sizeof(struct pd_config)];
    str[1] = (char*)&buffer[512+sizeof(struct pd_config)+25];

    
    int id;
    int url_index = 0;
    memset(pdcfg, 0, sizeof(struct pd_config));

    token = (char*)buffer;
    next = strchr((char*)buffer, '\n');
    while (token != 0)
    {
        sscanf_P(token, PSTR("%[^,],%s\n"), str[0], str[1]);
        id = atoi(str[0]);
        if (id == 0)
        {
            if (str[0][0] == 's' || str[0][0] == 'S')
            {
                // use next field as ssid
                strcpy(pdcfg->wifi_ssid, str[1]);
            }
            else if (str[0][0] == 'p' || str[0][0] == 'P')
            {
                strcpy(pdcfg->wifi_password, str[1]);
            }
        }
        else
        {
            if (id < MIN_DEVICE_ID || id > MAX_DEVICE_ID)
            {
                _logger->logF("invalid field\r\n");
                return false;
            }

            // check if this is an sd card or a url string
            if (str[1][0] == 'S' && str[1][1] == 'D')
            {
                if (str[1][2] == '0')
                {
                    pdcfg->device_type[id - MIN_DEVICE_ID] = DEVICE_SD0;
                }
                else if (str[1][2] == '1')
                {
                    pdcfg->device_type[id - MIN_DEVICE_ID] = DEVICE_SD1;
                }
                else
                {
                    _logger->logF("invalid sd\r\n");
                }
            }
            else
            {
                // this is a url
                pdcfg->device_type[id - MIN_DEVICE_ID] = DEVICE_URL_BASE + url_index;
                // copy the url string
                strcpy(pdcfg->urls[url_index], str[1]);
                url_index++;
            }
        }

        token = next;
        if (token != 0)
        {
            token++;
            next = strchr(token, '\n');
        }
    }

    // check for change in configuration
    if (configChanged(pdcfg))
    {
        // update eeprom with new config
        _logger->logF(PSTR("config updated\r\n"));
        eeprom_write_block(pdcfg, (void*)0, sizeof(struct pd_config));
        blink_led(3, 150, 150);
    }
    else
    {
        _logger->logF(PSTR("config not updated\r\n"));
        blink_led(4, 150, 150);
    }

    return true;
}

void PETdisk::setDataSource(unsigned char id, DataSource* dataSource)
{
    if (id >= MIN_DEVICE_ID && id <= MAX_DEVICE_ID)
    {
        _dataSources[id-MIN_DEVICE_ID] = dataSource;
    }
}

DataSource* PETdisk::getDataSource(unsigned char id)
{
    if (id < MIN_DEVICE_ID || id > MAX_DEVICE_ID)
    {
        return 0;
    }

    return _dataSources[id-MIN_DEVICE_ID];
}

void PETdisk::writeFile()
{
    unsigned int numBytes;
    unsigned char rdchar;
    unsigned int writeBufferSize = _dataSource->writeBufferSize();
    
    numBytes = 0;
    do
    {
        rdchar = _ieee->get_byte_from_bus();
        _dataSource->getBuffer()[numBytes++] = rdchar;
        
        if (numBytes >= writeBufferSize)
        {
            _dataSource->writeBufferToFile(numBytes);
            numBytes = 0;
        }
        
        _ieee->acknowledge_bus_byte();
        _ieee->signal_ready_for_data();
    }
    while (!_ieee->eoi_is_low());
    
    if (numBytes > 0)
    {
        _dataSource->writeBufferToFile(numBytes);
    }
    
    _dataSource->closeFile();
}

void PETdisk::openDirectory()
{
    _dataSource->openCurrentDirectory();
    _directoryEntryIndex = 0;
    _directoryEntryAddress = 0x041f;
    _directoryEntryByteIndex = 0;

    // copy header into directory entry buffer

}

bool PETdisk::getDirectoryEntry()
{
    bool gotDir;
    unsigned char startline;
    
    if (_directoryFinished)
    {
        startline = 0;
        _directoryEntryAddress += 0x001e;
        _directoryEntry[startline] = (unsigned char)(_directoryEntryAddress & 0x00ff);
        _directoryEntry[startline+1] = (unsigned char)((_directoryEntryAddress & 0xff00) >> 8);
        _directoryEntry[startline+2] = 0xff;
        _directoryEntry[startline+3] = 0xff;
        sprintf_P((char *)&_directoryEntry[startline+4], PSTR("BLOCKS FREE.             "));
        _directoryEntry[startline+29] = 0x00;
        _directoryEntry[startline+30] = 0x00;
        _directoryEntry[startline+31] = 0x00;
        return true;
    }

    do
    {
        // get next directory entry
        gotDir = _dataSource->getNextDirectoryEntry();
        if (gotDir == false)
        {
            _directoryEntryAddress += 0x0020;
            startline = 0;
            memset(_directoryEntry, ' ', 32);
            _directoryEntry[startline] = (unsigned char)(_directoryEntryAddress & 0x00ff);
            _directoryEntry[startline+1] = (unsigned char)((_directoryEntryAddress & 0xff00) >> 8);
            _directoryEntry[startline+2] = _directoryEntryIndex+1;
            _directoryEntry[startline+3] = 0x00;
            _directoryEntry[startline+4] = 0x20;
            _directoryEntry[startline+5] = 0x20;
            pgm_memcpy(&_directoryEntry[startline+6], (unsigned char*)_firmwareString, 6);
            pgm_memcpy(&_directoryEntry[startline+6+6], (unsigned char*)_hash, 7);
            _directoryEntry[startline+31] = 0x00;
            _directoryEntryIndex++;
            _directoryFinished = true;
            return false;
        }
        else
        {
            if (!_dataSource->isHidden() && !_dataSource->isVolumeId())
            {
                // check if this is a file that can be used by petdisk
                // currently .PRG and .SEQ, soon .REL

                int fname_length = 0;
                unsigned char* fileName = _dataSource->getFilename();
                if (fileName == NULL)
                {
                    continue;
                }
                fname_length = strlen((char*)fileName);
                bool valid_file = false;
                // check for correct extension
                if (fname_length > 4)
                {
                    if (toupper(fileName[fname_length-3]) == 'P' &&
                        toupper(fileName[fname_length-2]) == 'R' &&
                        toupper(fileName[fname_length-1]) == 'G')
                    {
                        valid_file = true;
                    }
                    else if (toupper(fileName[fname_length-3]) == 'S' &&
                        toupper(fileName[fname_length-2]) == 'E' &&
                        toupper(fileName[fname_length-1]) == 'Q')
                    {
                        valid_file = true;
                    }
                    else if (toupper(fileName[fname_length-3]) == 'D' &&
                        toupper(fileName[fname_length-2]) == '6' &&
                        toupper(fileName[fname_length-1]) == '4')
                    {
                        valid_file = true;
                    }
                }

                if (!valid_file)
                {
                    // skip this file, continue to next
                    continue;
                }

                _directoryEntryAddress += 0x0020;
                startline = 0;

                _directoryEntry[startline] = (unsigned char)(_directoryEntryAddress & 0x00ff);
                _directoryEntry[startline+1] = (unsigned char)((_directoryEntryAddress & 0xff00) >> 8);
                _directoryEntry[startline+2] = file+1;
                _directoryEntry[startline+3] = 0x00;
                _directoryEntry[startline+4] = 0x20;
                _directoryEntry[startline+5] = 0x20;
                _directoryEntry[startline+6] = 0x22;

                int extensionPos = -1;
                if (fname_length > 5)
                {
                    if (fileName[fname_length-4] == '.')
                    {
                        extensionPos = fname_length-3;
                        fname_length = fname_length-4;
                    }
                }
                
                if (fname_length >= 17)
                {
                    fname_length = 17;
                }
                
                for (int f = 0; f < fname_length; f++)
                {
                    // make sure filename is upper case
                    _directoryEntry[startline+7+f] = toupper(fileName[f]);
                }

                _directoryEntry[startline+7+fname_length] = 0x22;
                for (int f = 0; f < (17 - fname_length); f++)
                {
                    _directoryEntry[startline+7+fname_length+f+1] = ' ';
                }

                if (_dataSource->isDirectory())
                {
                    _directoryEntry[startline+25] = 'D';
                    _directoryEntry[startline+26] = 'I';
                    _directoryEntry[startline+27] = 'R';
                }
                else
                {
                    if (extensionPos > 0)
                    {
                        // make sure extension is upper case
                        _directoryEntry[startline+25] = toupper(fileName[extensionPos]);
                        _directoryEntry[startline+26] = toupper(fileName[extensionPos+1]);
                        _directoryEntry[startline+27] = toupper(fileName[extensionPos+2]);
                    }
                    else
                    {
                        _directoryEntry[startline+25] = ' ';
                        _directoryEntry[startline+26] = ' ';
                        _directoryEntry[startline+27] = ' ';
                    }
                }

                _directoryEntry[startline+28] = ' ';
                _directoryEntry[startline+29] = ' ';
                _directoryEntry[startline+30] = ' ';
                _directoryEntry[startline+31] = 0x00;
                _directoryEntryIndex++;

                return false;
            }
        }
    }
    while (gotDir == true);
}

void PETdisk::listFiles()
{
    bool gotDir;
    unsigned char startline;
    unsigned int dir_start;
    unsigned char entry[32];
    dir_start = 0x041f;
    unsigned int file = 0;

    _dataSource->openCurrentDirectory();

    do
    {
        // get next directory entry
        gotDir = _dataSource->getNextDirectoryEntry();
        if (gotDir == false)
        {
            dir_start += 0x0020;
            startline = 0;
            memset(entry, ' ', 32);
            entry[startline] = (unsigned char)(dir_start & 0x00ff);
            entry[startline+1] = (unsigned char)((dir_start & 0xff00) >> 8);
            entry[startline+2] = file+1;
            entry[startline+3] = 0x00;
            entry[startline+4] = 0x20;
            entry[startline+5] = 0x20;
            pgm_memcpy(&entry[startline+6], (unsigned char*)_firmwareString, 6);
            pgm_memcpy(&entry[startline+6+6], (unsigned char*)_hash, 7);
            entry[startline+31] = 0x00;
            file++;
            _ieee->sendIEEEBytes(entry, 32, 0);

            startline = 0;
            dir_start += 0x001e;
            entry[startline] = (unsigned char)(dir_start & 0x00ff);
            entry[startline+1] = (unsigned char)((dir_start & 0xff00) >> 8);
            entry[startline+2] = 0xff;
            entry[startline+3] = 0xff;
            sprintf_P((char *)&entry[startline+4], PSTR("BLOCKS FREE.             "));
            entry[startline+29] = 0x00;
            entry[startline+30] = 0x00;
            entry[startline+31] = 0x00;
            
            _ieee->sendIEEEBytes(entry, 32, 1);
            return;
        }
        else
        {
            if (!_dataSource->isHidden() && !_dataSource->isVolumeId())
            {
                // check if this is a file that can be used by petdisk
                // currently .PRG files. Soon .SEQ and .REL

                int fname_length = 0;
                unsigned char* fileName = _dataSource->getFilename();
                if (fileName == NULL)
                {
                    continue;
                }
                fname_length = strlen((char*)fileName);
                bool valid_file = false;
                // check for correct extension
                if (fname_length > 4)
                {
                    if (toupper(fileName[fname_length-3]) == 'P' &&
                        toupper(fileName[fname_length-2]) == 'R' &&
                        toupper(fileName[fname_length-1]) == 'G')
                    {
                        valid_file = true;
                    }
                    else if (toupper(fileName[fname_length-3]) == 'S' &&
                        toupper(fileName[fname_length-2]) == 'E' &&
                        toupper(fileName[fname_length-1]) == 'Q')
                    {
                        valid_file = true;
                    }
                    else if (toupper(fileName[fname_length-3]) == 'D' &&
                        toupper(fileName[fname_length-2]) == '6' &&
                        toupper(fileName[fname_length-1]) == '4')
                    {
                        valid_file = true;
                    }
                }

                if (!valid_file)
                {
                    // skip this file, continue to next
                    continue;
                }

                dir_start += 0x0020;
                startline = 0;

                
                entry[startline] = (unsigned char)(dir_start & 0x00ff);
                entry[startline+1] = (unsigned char)((dir_start & 0xff00) >> 8);
                entry[startline+2] = file+1;
                entry[startline+3] = 0x00;
                entry[startline+4] = 0x20;
                entry[startline+5] = 0x20;
                entry[startline+6] = 0x22;

                int extensionPos = -1;
                if (fname_length > 5)
                {
                    if (fileName[fname_length-4] == '.')
                    {
                        extensionPos = fname_length-3;
                        fname_length = fname_length-4;
                    }
                }
                
                if (fname_length >= 17)
                {
                    fname_length = 17;
                }
                
                for (int f = 0; f < fname_length; f++)
                {
                    // make sure filename is upper case
                    entry[startline+7+f] = toupper(fileName[f]);
                }

                entry[startline+7+fname_length] = 0x22;
                for (int f = 0; f < (17 - fname_length); f++)
                {
                    entry[startline+7+fname_length+f+1] = ' ';
                }

                if (_dataSource->isDirectory())
                {
                    entry[startline+25] = 'D';
                    entry[startline+26] = 'I';
                    entry[startline+27] = 'R';
                }
                else
                {
                    if (extensionPos > 0)
                    {
                        // make sure extension is upper case
                        entry[startline+25] = toupper(fileName[extensionPos]);
                        entry[startline+26] = toupper(fileName[extensionPos+1]);
                        entry[startline+27] = toupper(fileName[extensionPos+2]);
                    }
                    else
                    {
                        entry[startline+25] = ' ';
                        entry[startline+26] = ' ';
                        entry[startline+27] = ' ';
                    }
                }

                entry[startline+28] = ' ';
                entry[startline+29] = ' ';
                entry[startline+30] = ' ';
                entry[startline+31] = 0x00;
                file++;

                _ieee->sendIEEEBytes(entry, 32, 0);
            }
        }
    }
    while (gotDir == true);
}

unsigned char PETdisk::wait_for_device_address()
{
    unsigned char ieee_address;
    unsigned char buscmd;
    _dataSource = 0;
    while (_dataSource == 0)
    {
        ieee_address = _ieee->get_device_address(&buscmd);
        _dataSource = getDataSource(ieee_address);

        if (_dataSource == 0)
        {
            _ieee->reject_address();
            continue;
        }

        _ieee->accept_address();
        _primaryAddress = ieee_address;
    }

    return buscmd;
}

openFileInfo* PETdisk::getFileInfoForAddress(unsigned char address)
{
    if (address >= 2 && address < 16)
    {
        return &_openFileInformation[address - 2];
    }
    else
    {
        return NULL;
    }
}

void PETdisk::resetFileInformation(unsigned char address)
{
    openFileInfo* fileInfo = getFileInfoForAddress(address);
    
    memset(fileInfo->_fileName, 0, FILENAME_MAX_LENGTH);
    fileInfo->_fileDirection = FNONE;
    fileInfo->_fileBufferIndex = 0;
    fileInfo->_useRemainderByte = false;
    fileInfo->_remainderByte = 0;
    fileInfo->_byteIndex = 0;
    fileInfo->_nextByte = 0;
    if (address == 15)
    {
        fileInfo->_opened = true;
        fileInfo->_command = true;
    }
    else
    {
        fileInfo->_opened = false;
        fileInfo->_command = false;
    }
}

bool PETdisk::isD64(const char* fileName)
{
    int len = strlen(fileName);
    
    if (len <= 4)
    {
        return false;
    }

    if (strcmp(".D64", &fileName[len - 4]) == 0)
    {
        return true;
    }

    return false;
}

void PETdisk::run()
{
    // start main loop
    while(1)
    {
        if (_currentState == FILE_NOT_FOUND || _currentState == CLOSING)
        {
            _ieee->unlisten();
            _currentState = IDLE;
            _fileNotFound = 0;
            _filenamePosition = 0;
            memset(progname, 0, FILENAME_MAX_LENGTH);
        }

        if (IEEE_CTL == 0x00)
        {
            // if we are in an unlisten state,
            // wait for my address
            unsigned char buscmd = wait_for_device_address();
            if (buscmd == LISTEN)
            {
                _currentState = BUS_LISTEN;
            }
            else
            {
                _currentState = BUS_TALK;
            }
        }

        unsigned char rdchar = _ieee->get_byte_from_bus();

        if (_ieee->atn_is_low()) // check for bus command
        {
            //_logger->printf("A %X\r\n", rdchar);
            if (rdchar == PET_LOAD_FNAME_ADDR)
            {
                _currentState = LOAD_FNAME_READ;
            }
            else if (rdchar == PET_SAVE_FNAME_ADDR)
            {
                _currentState = SAVE_FNAME_READ;
            }
            else if ((rdchar & PET_OPEN_FNAME_MASK) == PET_OPEN_FNAME_MASK) // open command to another address
            {
                _currentState = OPEN_FNAME_READ;
                _secondaryAddress = rdchar & PET_ADDRESS_MASK;

                openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
                if (of != NULL)
                {
                    of->_fileBufferIndex = -1;
                    of->_opened = true;
                }
            }
            else if (rdchar == PET_READ_CMD_ADDR) // read command
            {
                if (_fileNotFound == 1)
                {
                    _currentState = FILE_NOT_FOUND;
                }
                else
                {
                    _currentState = FILE_READ;
                    // open file for reading
                    if (progname[0] == '$')
                    {
                        // copy the directory header
                        pgm_memcpy((unsigned char *)_buffer, (unsigned char *)_dirHeader, 7);

                        // print directory title
                        pgm_memcpy((unsigned char *)&_buffer[7], (unsigned char *)_versionString, 24);
                        _buffer[31] = 0x00;
                    }
                }
            }
            else if (rdchar == PET_SAVE_CMD_ADDR) // save command
            {
                _currentState = FILE_SAVE;
            }
            else if ((rdchar & PET_OPEN_FNAME_MASK) == PET_OPEN_IO_ADDR) // print or input command
            {
                _secondaryAddress = rdchar & PET_ADDRESS_MASK;
                openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
                if (of != NULL && of->_opened == true)
                {
                    if (_currentState == BUS_LISTEN)
                    {
                        if (!of->_command && of->_fileBufferIndex == -1)
                        {
                            _dataSource->openFileForWriting((unsigned char*)of->_fileName);
                            of->_fileBufferIndex = 0;
                        }
                        of->_fileDirection = FWRITE;
                        _currentState = OPEN_DATA_WRITE;
                    }
                    else
                    {
                        // file read
                        if (_fileNotFound == 1)
                        {
                            resetFileInformation(_secondaryAddress);
                            _currentState = FILE_NOT_FOUND;
                        }
                        else
                        {
                            of->_fileDirection = FREAD;
                            _currentState = OPEN_DATA_READ;
                        }
                    }
                }
                else
                {
                    _logger->printf("X %d\r\n", _secondaryAddress);
                }
            }
            else if ((rdchar & PET_OPEN_FNAME_MASK) == PET_CLOSE_FILE)
            {
                unsigned char address = rdchar & PET_ADDRESS_MASK;
                openFileInfo* of = getFileInfoForAddress(address);
                if (of != NULL)
                {
                    if (of->_fileDirection == FWRITE)
                    {
                        if (of->_fileBufferIndex > 0)
                        {
                            _dataSource->writeBufferToFile(of->_fileBufferIndex);
                            of->_fileBufferIndex = 0;
                        }

                        _dataSource->closeFile();
                    }

                    resetFileInformation(address);
                }
                _currentState = CLOSING;
            }
        }
        else if (_currentState == OPEN_DATA_WRITE) // received byte to write to open file
        {
            openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
            if (of != NULL)
            {
                _dataSource->getBuffer()[of->_fileBufferIndex++] = rdchar;
                if (!of->_command && of->_fileBufferIndex >= (int)_dataSource->writeBufferSize())
                {
                    _dataSource->writeBufferToFile(of->_fileBufferIndex);
                    of->_fileBufferIndex = 0;
                }
            }
        }
        else if (_currentState == LOAD_FNAME_READ ||
                 _currentState == SAVE_FNAME_READ ||
                 _currentState == OPEN_FNAME_READ)
        {
            // add character to filename
            progname[_filenamePosition] = rdchar;
            _filenamePosition++;
            progname[_filenamePosition] = 0;

            if (_ieee->eoi_is_low())
            {
                // this is a directory request
                if (progname[0] == '$')
                {
                    _filenamePosition = 0;
                    _currentState = DIR_READ;
                }
                else
                {
                    // process filename, remove drive indicators and file type
                    bool write;
                    _filenamePosition = processFilename(progname, _filenamePosition, &write);

                    // TODO: figure out at this point if this is a read or write using modifiers in filename
                    // this prevents having to try opening the file every time, which is slow.

                    const unsigned char *ext;
                    if (_currentState == OPEN_FNAME_READ)
                    {
                        ext = _seqExtension;
                        if (write == true)
                        {
                            _currentState = OPEN_FNAME_READ_DONE_FOR_WRITING;
                        }
                        else
                        {
                            _currentState = OPEN_FNAME_READ_DONE;
                        }
                    }
                    else
                    {
                        ext = _fileExtension;

                        if (_currentState == LOAD_FNAME_READ)
                        {
                            _currentState = FILE_READ_OPENING;
                        }
                        else if (_currentState == SAVE_FNAME_READ)
                        {
                            _currentState = FILE_SAVE_OPENING;
                        }
                    }

                    // copy the PRG file extension onto the end of the file name
                    pgm_memcpy(&progname[_filenamePosition], (unsigned char*)ext, 5);
                    _filenamePosition = 0;
                    _logger->log((const char*)progname);
                    _logger->logF(PSTR("\r\n"));
                }
            }
        }

        _ieee->acknowledge_bus_byte();

        // === PREPARE FOR READ/WRITE

        if (_currentState == FILE_READ_OPENING ||
            _currentState == FILE_SAVE_OPENING ||
            _currentState == OPEN_FNAME_READ_DONE ||
            _currentState == OPEN_FNAME_READ_DONE_FOR_WRITING ||
            _currentState == DIR_READ)
        {
            // initialize datasource
            if (!_dataSource->init()) 
            {
                _fileNotFound = 1;
                _currentState = IDLE;
            }

            if (_currentState == FILE_SAVE_OPENING)
            {
                // open file
                _dataSource->openFileForWriting(progname);
                _currentState = IDLE;
            }
            else if (_currentState == FILE_READ_OPENING ||
                     _currentState == OPEN_FNAME_READ_DONE) // file read, either LOAD or OPEN command
            {
                // check for direct access command
                openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
                if (of != NULL && of->_command == true)
                {
                    _logger->printf("CMD: %s\r\n", progname);
                }
                else if (!_dataSource->openFileForReading(progname))
                {
                    // file not found
                    _fileNotFound = 1;
                }
                else
                {
                    _bytesToSend = _dataSource->getNextFileBlock();
                    _bufferFileIndex = 0;

                    if (_currentState == OPEN_FNAME_READ_DONE)
                    {
                        _fileNotFound = 0;
                        
                        if (of != NULL)
                        {
                            strcpy(of->_fileName, (const char*)progname);
                            of->_fileBufferIndex = 0;
                            of->_useRemainderByte = false;
                            of->_remainderByte = 0;
                            of->_nextByte = _dataSource->getBuffer()[0];
                        }
                        
                        _bufferFileIndex = _secondaryAddress;
                    }
                }
                _currentState = IDLE;
            }
            else if (_currentState == OPEN_FNAME_READ_DONE_FOR_WRITING)
            {
                openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
                strcpy(of->_fileName, (const char*)progname);
                of->_fileBufferIndex = -1;
                _currentState = IDLE;
            }
        }

        if ((rdchar == UNLISTEN) || (rdchar == UNTALK && _ieee->atn_is_low()))
        {
            openFileInfo* of = getFileInfoForAddress(_secondaryAddress);
            if (of != NULL && of->_command == true)
            {
                _dataSource->getBuffer()[of->_fileBufferIndex] = 0;
                // retrieve the address from parsing command string
                _dataSource->processCommandString(&_bufferFileIndex);
                of = getFileInfoForAddress(_bufferFileIndex);
                if (of != NULL)
                {
                    of->_nextByte = _dataSource->getBuffer()[0];
                    of->_fileBufferIndex = 0;
                }

                resetFileInformation(_secondaryAddress);
            }
            // unlisten or untalk command
            _ieee->signal_ready_for_data();
            _ieee->unlisten();
            _currentState = IDLE;
            continue;
        }

        _ieee->signal_ready_for_data();

        // LOAD requested
        if (_currentState == FILE_READ || _currentState == OPEN_DATA_READ)
        {
            // ==== STARTING LOAD SEQUENCE
            _ieee->begin_output();

            if (_currentState == FILE_READ)
            {
                // get packet
                if (progname[0] == '$')
                {
                    // reading a directory
                    // need to handle both standard load"$" command and DIRECTORY/CATALOG here
                    // on each byte sent, we should check for ATN asserted.
                    // if ATN is asserted, we exit and wait for further commands




                    // OLD directory routine
                    /*
                    _ieee->sendIEEEBytes((unsigned char *)_buffer, 32, 0);

                    // this is a change directory command
                    if (progname[1] == ':')
                    {
                        // change directory command
                        // this can be either a directory name, or a d64 file
                        if (isD64((const char*)&progname[2]))
                        {
                            _logger->printf("d64: %s\r\n", &progname[2]);
                            // this is a d64 file, mount as a datasource
                            // initialize d64 datasource
                            bool success = _d64->initWithDataSource(_dataSource, (const char*)&progname[2], _logger);
                            if (success)
                            {
                                setDataSource(_primaryAddress, _d64);
                                _dataSource = _d64;
                            }
                            else
                            {
                                _logger->printf("not found %s\n", progname);
                            }
                        }
                        else
                        {
                            if (_dataSource == _d64)
                            {
                                if (progname[2] == '.' && progname[3] == '.')
                                {
                                    // unmount this d64 image and return to previous datasource
                                    _dataSource = _d64->getFileDataSource();
                                    setDataSource(_primaryAddress, _dataSource);
                                }
                            }
                            else
                            {
                                // change directory
                                _dataSource->openDirectory((const char*)&progname[2]);
                            }
                        }
                    }
                    // write directory entries
                    listFiles();
                    */
                }
                else // read from file
                {
                    // send blocks of file
                    unsigned char done_sending = 0;
                    while (done_sending == 0)
                    {
                        if (_bytesToSend == 0)
                        {
                            _bytesToSend = _dataSource->getNextFileBlock();
                        }

                        if (_dataSource->isLastBlock())
                        {
                            done_sending = 1;
                        }

                        _ieee->sendIEEEBytes(_dataSource->getBuffer(), _bytesToSend, done_sending);
                        _bytesToSend = 0;
                    }
                }
            }
            else if (_currentState == OPEN_DATA_READ)
            {
                bool done = false;
                unsigned char result = 0;
                
                unsigned char address = rdchar & PET_ADDRESS_MASK;
                openFileInfo* of = getFileInfoForAddress(address);

                while (!done)
                {
                    if (of->_useRemainderByte == true)
                    {
                        result = _ieee->sendIEEEByteCheckForATN(of->_remainderByte);
                    }
                    else
                    {
                        result = _ieee->sendIEEEByteCheckForATN(of->_nextByte);
                    }

                    result = _ieee->wait_for_ndac_high_or_atn_low();

                    if (result == ATN_MASK)
                    {
                        done = true;
                    }
                    else
                    {
                        if (of->_useRemainderByte == true)
                        {
                            of->_useRemainderByte = false;
                        }
                        else
                        {
                            of->_remainderByte = _dataSource->getBuffer()[of->_fileBufferIndex];
                            of->_fileBufferIndex++;
                            of->_byteIndex++;
                        }

                        if (_bufferFileIndex != address)
                        {
                            _dataSource->openFileForReading((unsigned char*)of->_fileName);
                            int blocksToRead = (of->_byteIndex / _dataSource->readBufferSize()) + 1;

                            // read enough blocks to get back to the right block of the file
                            for (int i = 0; i < blocksToRead; i++)
                            {
                                _dataSource->getNextFileBlock();
                            }
                            _bufferFileIndex = address;
                        }

                        if (of->_fileBufferIndex >= (int)_dataSource->readBufferSize())
                        {
                            // get next buffer block
                            _bytesToSend = _dataSource->getNextFileBlock();
                            of->_fileBufferIndex = 0;
                        }

                        _ieee->raise_dav_and_eoi();

                        result = _ieee->wait_for_ndac_low_or_atn_low();
                        
                        if (result == ATN_MASK)
                        {
                            if (of->_fileBufferIndex == 0)
                            {
                                of->_useRemainderByte = true;
                            }
                            else
                            {
                                of->_fileBufferIndex--;
                            }

                            done = true;
                        }

                        of->_nextByte = _dataSource->getBuffer()[of->_fileBufferIndex];
                    }
                }
            }

            // ==== ENDING LOAD SEQUENCE
            _ieee->end_output();
            _currentState = IDLE;

        }
        else if (_currentState == FILE_SAVE)
        {
            // save command
            writeFile();
            _ieee->unlisten();
            _currentState = IDLE;
        }
    }
}

unsigned char PETdisk::processFilename(unsigned char* filename, unsigned char length, bool* write)
{
    _logger->log("pf: ");
    _logger->log(filename, length);
    _logger->log("\r\n");

    *write = false;
    unsigned char drive_separator = ':';
    unsigned char* sepptr = (unsigned char*)memmem(filename, length, &drive_separator, 1);
    if (sepptr)
    {
        // found a drive separator
        int seplen = sepptr - filename + 1;
        memmove(filename, sepptr + 1, length - seplen);
        length -= seplen;
    }

    // find part of string before ','
    drive_separator = ',';
    sepptr = (unsigned char*)memmem(filename, length, &drive_separator, 1);
    if (sepptr)
    {
        // look for write indicator
        unsigned char writeIndicator[2];
        writeIndicator[0] = ',';
        writeIndicator[1] = 'W';
        int fnameLength = sepptr - filename;
        int indicatorsSize = length - fnameLength;

        if (memmem(sepptr, indicatorsSize, writeIndicator, 2) != NULL)
        {
            // mark as a write
            *write = true;
        }

        length = fnameLength;
    }

    return length;
}

int main(void)
{
    _bufferSize = 0;
    prog_init();
    
    SPI spi;
    spi.init();

    Serial0 serial;
    serial.init(0);  

    Serial1 logSerial;
    logSerial.init(0);

    SerialLogger logger(&logSerial);
    logger.init();

    SD sd(&spi, SPI_CS);
    FAT32 fat32(&sd, _buffer, &_buffer[512], &logger);

    checkForFirmware((char*)&_buffer[769], &fat32, &logSerial);

    init_led();
    blink_led(1, 300, 50);

    EspConn espConn(_buffer, &_bufferSize, &serial, &logSerial);
    EspHttp espHttp(&espConn, &logger);
    reset_esp();
    for (int i = 0; i < 50; i++)
    {
        _delay_loop_2(65535);
    }

    IEEE488 ieee(&logger);
    ieee.unlisten();

    // init 4 possible network datasources
    NetworkDataSource nds0(&espHttp, _buffer, &_bufferSize, &logger);
    NetworkDataSource nds1(&espHttp, _buffer, &_bufferSize, &logger);
    NetworkDataSource nds2(&espHttp, _buffer, &_bufferSize, &logger);
    NetworkDataSource nds3(&espHttp, _buffer, &_bufferSize, &logger);

    NetworkDataSource* nds_array[4];
    nds_array[0] = &nds0;
    nds_array[1] = &nds1;
    nds_array[2] = &nds2;
    nds_array[3] = &nds3;

    D64DataSource d64DataSource;

    PETdisk petdisk;
    petdisk.init(
        &fat32,
        &d64DataSource,
        _buffer, 
        &_bufferSize, 
        &espConn, 
        &espHttp, 
        (NetworkDataSource**)nds_array,
        &ieee,
        &logger);

    // TESTING
    // try loading all clusters of a file on sd card
    /*
    fat32.openFileForReading((unsigned char*)"ushergam.d64");
    int b = 0;
    bool done = false;
    char tmp[16];
    logger.log("here\r\n");
    fat32.indexFileForSeeking();
    fat32.seek(1000);
    fat32.seek(2000);
    fat32.seek(100000);
    */

    //D64DataSource d64DataSource;
    //d64DataSource.initWithDataSource(&fat32, "ushergam.d64", &logger);
    //d64DataSource.openFileForReading((unsigned char*)"USHER.PRG");

    //d64DataSource.getNextFileBlock();
    //d64DataSource.getNextFileBlock();

    //petdisk.setDataSource(8, &d64DataSource);

    #ifdef TESTING

    D64DataSource d64DataSource;
    d64DataSource.initWithDataSource(nds_array[0], "ushergam.d64", &logger);
    
    #endif


    logger.log("back\r\n");

    set_led(true);

    // execute run loop
    petdisk.run();

    while(1) {}
    return 0;
}

// can you make the interrupt handler self contained?


// interrupt handler - should read bytes into a buffer.
// this can be read outside the handler to check for end tokens
// would need to be consumed fast enough to prevent buffer overrun
// length can be an atomic (8-bit) value
// you are guaranteed to be able to read this amount of data
// if you allocate enough space to handle any potential reads, you 
// don't even need a ring buffer, just a regular buffer


// turn on serial interrupts to start reading
// turn off when done

ISR(USART0_RX_vect)
{
    // insert character into buffer
    // loop to beginning if needed
    unsigned char a = UDR0;
    _buffer[_bufferSize] = a;
    _bufferSize++;
}