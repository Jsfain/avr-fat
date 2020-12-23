/*
***********************************************************************************************************************
*                                                       AVR-FAT MODULE
*
* File    : FAT.C
* Version : 0.0.0.2 (Previous version commit - Sat Dec 5 22:26:05 2020)
* Author  : Joshua Fain
* Target  : ATMega1280
* License : MIT LICENSE
* Copyright (c) 2020 Joshua Fain
* 
*
*
* DESCRIPTION: 
* Defines functions declared in FAT.H for accessing contents of a FAT32 formatted volume using an AVR microconstroller.
* The fuctions defined here only provide READ access to the volume's contents (i.e. print file, print directory), no 
* WRITE access is currently possible.
*
*
* FUNCTION "PUBLIC":
*  (1) void     fat_set_directory_to_root (FatDir * Dir, BPB * bpb)
*  (2) uint8_t  fat_set_directory (FatDir * Dir, char * newDirStr, BPB * bpb)
*  (3) uint8_t  fat_print_directory (FatDir * Dir, uint8_t entryFilter, BPB * bpb)
*  (4) uint8_t  fat_print_file (FatDir * Dir, char * file, BPB * bpb)
*  (5) void     fat_print_error (uint8_t err)
*
*
* STRUCTS (defined in FAT.H)
*  typedef struct FatDirectory FatDir
***********************************************************************************************************************
*/



#include <string.h>
#include <stdlib.h>
#include <avr/io.h>
#include "../includes/fat_bpb.h"
#include "../includes/fat.h"
#include "../includes/prints.h"
#include "../includes/usart.h"
#include "../includes/fattodisk_interface.h"


/*
-----------------------------------------------------------------------------------------------------------------------
|                                            "PRIVATE" FUNCTION DECLARATIONS
-----------------------------------------------------------------------------------------------------------------------
*/

// Private function descriptions are only included with their definitions at the bottom of this file.

uint8_t pvt_check_valid_name (char * nameStr, FatDir * Dir);
uint8_t pvt_set_directory_to_parent (FatDir * Dir, BPB * bpb);
void pvt_set_directory_to_child (FatDir * Dir, uint8_t * sectorArr, uint16_t snPos, char * childDirStr, BPB * bpb);
void pvt_load_long_name (int lnFirstEntry, int lnLastEntry, uint8_t * sectorArr, char * lnStr, uint8_t * lnStrIndx);
uint32_t pvt_get_next_cluster_index (uint32_t currentClusterIndex, BPB * bpb);
void pvt_print_entry_fields (uint8_t * byte, uint16_t entryPos, uint8_t entryFilter);
void pvt_print_short_name (uint8_t * byte, uint16_t entryPos, uint8_t entryFilter);
uint8_t pvt_print_fat_file (uint16_t entryPos, uint8_t * fileSector, BPB * bpb);
uint8_t pvt_correct_entry_check (uint8_t lnFlags, uint16_t * entryPos, 
                                 uint16_t * snPosCurrSec, uint16_t * snPosNextSec);           
void pvt_set_long_name_flags (uint8_t * lnFlags, uint16_t entryPos, 
                              uint16_t * snPosCurrSec, uint8_t * currSecArr, BPB * bpb);
uint8_t pvt_get_next_sector (uint8_t * nextSecArr, uint32_t currSecNumInClus, 
                             uint32_t currSecNumPhys, uint32_t clusIndx, BPB* bpb);



/*
***********************************************************************************************************************
 *                                SET MEMBERS OF FAT DIRECTORY INSTANCE TO ROOT DIRECTORY
 * 
 * This function should be called before maniupulating/accessing the FatDir instance using the other FAT functions.
 * Call this function to set the members of the FatDir struct's instance to the root directory
 *                                         
 * Description : This function will set the members of a BiosParameterBlock (BPB) struct instance according to the
 *               values specified within the FAT volume's Bios Parameter Block / Boot Sector. 
 * 
 * Arguments   : *Dir          - Pointer to a FatDir struct whose members will be set to point to the root directory.
 *             : *bpb          - Pointer to a BPB struct instance.
***********************************************************************************************************************
*/

void
fat_set_directory_to_root(FatDir * Dir, BPB * bpb)
{
  for (uint8_t i = 0; i < LONG_NAME_STRING_LEN_MAX; i++)
    Dir->longName[i] = '\0';
  for (uint8_t i = 0; i < PATH_STRING_LEN_MAX; i++)
    Dir->longParentPath[i] = '\0';
  for (uint8_t i = 0; i < 9; i++)
    Dir->shortName[i] = '\0';
  for (uint8_t i = 0; i < PATH_STRING_LEN_MAX; i++)
    Dir->shortParentPath[i] = '\0';
  
  Dir->longName[0] = '/';
  Dir->shortName[0] = '/';
  Dir->FATFirstCluster = bpb->rootClus;
}



void
fat_init_entry(FatEntry * ent, BPB * bpb)
{
  for(uint8_t i = 0; i < LONG_NAME_STRING_LEN_MAX; i++)
    ent->longName[i] = '\0';

  for(uint8_t i = 0; i < 13; i++)
    ent->shortName[i] = '\0';

  for(uint8_t i = 0; i < 32; i++)
    ent->shortNameEntry[i] = 0;

  ent->longNameEntryCount = 0;
  ent->shortNameEntryClusIndex = bpb->rootClus;
  ent->shortNameEntrySecNumInClus = 0;
  ent->entryPos = 0;
  ent->lnFlags = 0;
  ent->snPosCurrSec = 0;
  ent->snPosCurrSec = 0;
}




/*
***********************************************************************************************************************
 *                                                   SET FAT DIRECTORY
 *                                        
 * Description : Call this function to set a FatDirectory (FatDir) struct instance to a new directory. The new
 *               directory must be a child, or the parent, of the struct's instance when this function is called. This
 *               function operates by searching the current directory for a name that matches string newDirStr. If a 
 *               matching entryPos is found, the members of the FatDir instance are updated to those corresponding to
 *               the matching entryPos. To set to parent, pass the string ".." as newDirStr.
 *
 * Arguments   : *Dir            - Pointer to an instance of FatDir. The members of *Dir must point to a valid FAT32
 *                                 directory when the function is called. The members of the instance are updated by 
 *                                 this function.
 *             : *newDirStr      - Pointer to a C-string that specifies the name of the intended new directory. This 
 *                                 function will only search the current FatDir instance's directory for a matching  
 *                                 name, thus it is only possible to set FatDir to a child, or the parent (".."), of 
 *                                 the current directory. Paths must not be included in the string. This string is
 *                                 also required to be a long name unless a long name does not exist for a given
 *                                 entryPos, only then can a short name be a valid string. This is case-sensitive.
 *             : *bpb            - Pointer to a valid instance of a BPB struct.
 * 
 * Return      : FAT Error Flag  - The returned value can be read by passing it tp fat_print_error(err). If SUCCESS
 *                                 is returned then the FatDir instance members were successfully updated to point to
 *                                 the new directory. Any other returned value indicates a failure.
 *  
 * Limitation  : This function will not work with absolute paths, it will only set a FatDir instance to a new directory
 *               if the new directory is a child, or the parent, of the current directory.
***********************************************************************************************************************
*/

uint8_t 
fat_next_entry (FatDir * currDir, FatEntry * currEntry, BPB * bpb)
{
  uint16_t bps  = bpb->bytesPerSec;
  uint8_t  spc  = bpb->secPerClus;
  uint32_t drfs = bpb->dataRegionFirstSector;

  uint8_t  currSecArr[bps]; 
  uint8_t  nextSecArr[bps];
  uint8_t  attrByte;       // attribute byte
  uint32_t currSecNumPhys; // physical (disk) sector number
  uint8_t  entCorrFlag = 0;
  char     lnStr[LONG_NAME_STRING_LEN_MAX];

  uint8_t  lnStrIndx = 0;
  uint8_t  err;

  uint32_t clusIndx = currEntry->shortNameEntryClusIndex;
  uint8_t  currSecNumInClus = currEntry->shortNameEntrySecNumInClus;
  uint16_t entryPos = currEntry->entryPos;

  uint8_t  lnExistsFlag        = 0x01;
  uint8_t  lnCrossSecBoundFlag = 0x02;
  uint8_t  lnLastSecEntFlag    = 0x04;

  uint8_t  lnMask = currEntry->lnFlags;
  uint16_t snPosCurrSec = currEntry->snPosCurrSec;
  uint16_t snPosNextSec = currEntry->snPosNextSec;

  uint8_t  currSecNumInClusStart = 1;
  uint8_t  entryPosStart = 1;

  uint8_t longNameOrder;

  do 
    {
      if (currSecNumInClusStart == 0) currSecNumInClus = 0; 
      for (; currSecNumInClus < spc; currSecNumInClus++)
        {
          currSecNumInClusStart = 0;

          // load sector bytes into currSecArr[]
          currSecNumPhys = currSecNumInClus + drfs + ((clusIndx - 2) * spc);
          err = fat_to_disk_read_single_sector (currSecNumPhys, currSecArr);
          if (err == 1) return FAILED_READ_SECTOR;
          
          if (entryPosStart == 0) entryPos = 0; 
          for (; entryPos < bps; entryPos = entryPos + ENTRY_LEN)
            {
              entryPosStart = 0;

              entCorrFlag = pvt_correct_entry_check (lnMask, &entryPos, &snPosCurrSec, &snPosNextSec);
              
              if (entCorrFlag == 1)
                {
                  entCorrFlag = 0;
                  break;    
                }

              // reset long name flags
              lnMask = 0;

              // If first value of entry is 0 then all subsequent entries are empty.
              if (currSecArr[entryPos] == 0) 
                return END_OF_DIRECTORY;

              // Skip and go to next entry if Only continue with current entry if it has not been marked for deletion
              if (currSecArr[entryPos] != 0xE5)
                {
                  attrByte = currSecArr[entryPos + 11];
                  
                  if ((attrByte & LONG_NAME_ATTR_MASK) == LONG_NAME_ATTR_MASK)
                    {
                      if ( !(currSecArr[entryPos] & LONG_NAME_LAST_ENTRY)) return CORRUPT_FAT_ENTRY;

                      for (uint8_t k = 0; k < LONG_NAME_STRING_LEN_MAX; k++) lnStr[k] = '\0';
 
                      lnStrIndx = 0;

                      // pvt_set_long_name_flags ( &lnMask, entryPos, &snPosCurrSec, currSecArr, bpb);
                        lnMask |= lnExistsFlag;

                      // number of entries required by the long name
                      longNameOrder = LONG_NAME_ORDINAL_MASK & currSecArr[entryPos];              
                      snPosCurrSec = entryPos + (ENTRY_LEN * longNameOrder);
                      
                      // If short name position is greater than 511 then the short name is in the next sector.
                      if (snPosCurrSec >= bps)
                        {
                          if (snPosCurrSec > bps) 
                            lnMask |= lnCrossSecBoundFlag;
                          else if (snPosCurrSec == SECTOR_LEN) 
                            lnMask |= lnLastSecEntFlag;
                        }

                      if (lnMask & (lnCrossSecBoundFlag | lnLastSecEntFlag))
                        {
                          err = pvt_get_next_sector (nextSecArr, currSecNumInClus, currSecNumPhys, clusIndx, bpb);

                          if (err == FAILED_READ_SECTOR) return FAILED_READ_SECTOR;
                          snPosNextSec = snPosCurrSec - bps;
                          attrByte = nextSecArr[snPosNextSec + 11];

                          // If snPosNextSec points to a long name entry then something is wrong.
                          if ((attrByte & LONG_NAME_ATTR_MASK) == LONG_NAME_ATTR_MASK)
                            return CORRUPT_FAT_ENTRY;
                          
                          // print long name if filter flag is set.
                          if (lnMask & lnCrossSecBoundFlag)
                            {
                              // Entry immediately preceeding short name must be the long names's first entry.
                              if ((nextSecArr[snPosNextSec - ENTRY_LEN] & LONG_NAME_ORDINAL_MASK) != 1) 
                                return CORRUPT_FAT_ENTRY;                                              

                              // load long name entryPos into lnStr[]
                              pvt_load_long_name (snPosNextSec - ENTRY_LEN, 0, nextSecArr, lnStr, &lnStrIndx);
                              pvt_load_long_name (SECTOR_LEN - ENTRY_LEN, entryPos, currSecArr, lnStr, &lnStrIndx);

                              for (uint8_t i = 0; i < LONG_NAME_STRING_LEN_MAX; i++)
                                {
                                  currEntry->longName[i] = lnStr[i];
                                  if (lnStr == '\0') break;
                                }

                              currEntry->entryPos = entryPos;
                              currEntry->shortNameEntrySecNumInClus = currSecNumInClus;
                              currEntry->shortNameEntryClusIndex = clusIndx;
                              currEntry->snPosCurrSec = snPosCurrSec;
                              currEntry->snPosNextSec = snPosNextSec;
                              currEntry->lnFlags = lnMask;
                              currEntry->longNameEntryCount = currSecArr[entryPos] & 0x3F;

                              for (uint8_t i = 0; i < 32; i++)
                                currEntry->shortNameEntry[i] = nextSecArr[snPosNextSec + i];

                              char sn[13];
                              uint8_t j = 0;
                              for (uint8_t k = 0; k < 8; k++)
                                {
                                  if (nextSecArr[snPosNextSec + k] != ' ')
                                    { 
                                      sn[j] = nextSecArr[snPosNextSec + k];
                                      j++;
                                    }
                                }
                              if (nextSecArr[snPosNextSec + 8] != ' ')
                                {
                                  sn[j] = '.';
                                  j++;
                                  for (uint8_t k = 8; k < 11; k++)
                                    {
                                      if (nextSecArr[snPosNextSec + k] != ' ')
                                        { 
                                          sn[j] = nextSecArr[snPosNextSec + k];
                                          j++;
                                        }
                                    }
                                }
                              sn[j] = '\0';


                              strcpy(currEntry->shortName, sn);

                              return SUCCESS;
                            }

                          else if (lnMask & lnLastSecEntFlag)
                            {
                              
                              // Entry immediately preceeding short name must be the long names's first entry.
                              if ((currSecArr[SECTOR_LEN - ENTRY_LEN] & LONG_NAME_ORDINAL_MASK) != 1) 
                                return CORRUPT_FAT_ENTRY;

                              // load long name entryPos into lnStr[]
                              pvt_load_long_name (SECTOR_LEN - ENTRY_LEN, entryPos, currSecArr, lnStr, &lnStrIndx);
                              
                              for (uint8_t i = 0; i < LONG_NAME_STRING_LEN_MAX; i++)
                                {
                                  currEntry->longName[i] = lnStr[i];
                                  if (lnStr == '\0') break;
                                }
                              
                              currEntry->entryPos = entryPos;
                              currEntry->shortNameEntrySecNumInClus = currSecNumInClus;
                              currEntry->shortNameEntryClusIndex = clusIndx;
                              currEntry->snPosCurrSec = snPosCurrSec;
                              currEntry->snPosNextSec = snPosNextSec;
                              currEntry->lnFlags = lnMask;
                              currEntry->longNameEntryCount = currSecArr[entryPos] & 0x3F;

                              for (uint8_t i = 0; i < 32; i++)
                                currEntry->shortNameEntry[i] = nextSecArr[snPosNextSec + i];

                              char sn[13];
                              uint8_t j = 0;
                              for (uint8_t k = 0; k < 8; k++)
                                {
                                  if (nextSecArr[snPosNextSec + k] != ' ')
                                    { 
                                      sn[j] = nextSecArr[snPosNextSec + k];
                                      j++;
                                    }
                                }
                              if (nextSecArr[snPosNextSec + 8] != ' ')
                                {
                                  sn[j] = '.';
                                  j++;
                                  for (uint8_t k = 8; k < 11; k++)
                                    {
                                      if (nextSecArr[snPosNextSec + k] != ' ')
                                        { 
                                          sn[j] = nextSecArr[snPosNextSec + k];
                                          j++;
                                        }
                                    }
                                }
                              sn[j] = '\0';

                              strcpy(currEntry->shortName, sn);

                              return SUCCESS;
                            }
                          else return CORRUPT_FAT_ENTRY;
                        }

                      // Long name exists and is entirely in current sector along with the short name
                      else
                        {   
                          attrByte = currSecArr[snPosCurrSec + 11];
                          
                          // if snPosCurrSec points to long name entry, then somethine is wrong.
                          if ((attrByte & LONG_NAME_ATTR_MASK) == LONG_NAME_ATTR_MASK) return CORRUPT_FAT_ENTRY;
                 
                          // Confirm entry preceding short name is first entryPos of a long name.
                          if ((currSecArr[snPosCurrSec - ENTRY_LEN] & LONG_NAME_ORDINAL_MASK) != 1) 
                            return CORRUPT_FAT_ENTRY;
                          

                          // load long name entry into lnStr[]
                          pvt_load_long_name (snPosCurrSec - ENTRY_LEN, entryPos, currSecArr, lnStr, &lnStrIndx);

                          for (uint8_t i = 0; i < LONG_NAME_STRING_LEN_MAX; i++)
                            {
                              currEntry->longName[i] = lnStr[i];
                              if (lnStr == '\0') break;
                            }

                          currEntry->entryPos = entryPos;
                          currEntry->shortNameEntrySecNumInClus = currSecNumInClus;
                          currEntry->shortNameEntryClusIndex = clusIndx;
                          currEntry->snPosCurrSec = snPosCurrSec;
                          currEntry->snPosNextSec = snPosNextSec;
                          currEntry->lnFlags = lnMask;
                          currEntry->longNameEntryCount = currSecArr[entryPos] & 0x3F;

                          for (uint8_t i = 0; i < 32; i++)
                            currEntry->shortNameEntry[i] = currSecArr[snPosCurrSec + i];

                          char sn[13];
                          uint8_t j = 0;
                          for (uint8_t k = 0; k < 8; k++)
                            {
                              if (currSecArr[snPosCurrSec + k] != ' ')
                                { 
                                  sn[j] = currSecArr[snPosCurrSec + k];
                                  j++;
                                }
                            }
                          if (currSecArr[snPosCurrSec + 8] != ' ')
                            {
                              sn[j] = '.';
                              j++;
                              for (uint8_t k = 8; k < 11; k++)
                                {
                                  if (currSecArr[snPosCurrSec + k] != ' ')
                                    { 
                                      sn[j] = currSecArr[snPosCurrSec + k];
                                      j++;
                                    }
                                }
                            }
                          sn[j] = '\0';

                          strcpy(currEntry->shortName, sn);

                          return SUCCESS;                                                             
                        }                   
                    }

                  // Long name entry does not exist, use short name instead regardless of SHORT_NAME entryFilter.
                  else
                    {
                      snPosCurrSec = entryPos;

                      attrByte = currSecArr[snPosCurrSec + 11];

                      currEntry->entryPos = snPosCurrSec + ENTRY_LEN; // have to adjust + 32, otherwise gets stuck repeating the same entry.
                      currEntry->shortNameEntrySecNumInClus = currSecNumInClus;
                      currEntry->shortNameEntryClusIndex = clusIndx;
                      currEntry->snPosCurrSec = snPosCurrSec;
                      currEntry->snPosNextSec = snPosNextSec;
                      currEntry->lnFlags = lnMask;
                      currEntry->longNameEntryCount = 0;

                      for (uint8_t i = 0; i < 32; i++)
                        currEntry->shortNameEntry[i] = currSecArr[snPosCurrSec + i];

                      char sn[13];
                      uint8_t j = 0;
                      for (uint8_t k = 0; k < 8; k++)
                        {
                          if (currSecArr[snPosCurrSec + k] != ' ')
                            { 
                              sn[j] = currSecArr[snPosCurrSec + k];
                              j++;
                            }
                        }
                      if (currSecArr[snPosCurrSec + 8] != ' ')
                        {
                          sn[j] = '.';
                          j++;
                          for (uint8_t k = 8; k < 11; k++)
                            {
                              if (currSecArr[snPosCurrSec + k] != ' ')
                                { 
                                  sn[j] = currSecArr[snPosCurrSec + k];
                                  j++;
                                }
                            }
                        }
                      sn[j] = '\0';
                      strcpy(currEntry->shortName, sn);
                      strcpy(currEntry->longName, sn);
                      return SUCCESS;  
                    }
                }
            }
        }
    }
  while ((clusIndx = pvt_get_next_cluster_index(clusIndx, bpb)) != END_CLUSTER);

  return END_OF_DIRECTORY;
}



uint8_t 
fat_set_directory (FatDir * Dir, char * newDirStr, BPB * bpb)
{
  if (pvt_check_valid_name (newDirStr, Dir)) 
    return INVALID_DIR_NAME;

  // newDirStr == 'Current Directory' ?
  if (!strcmp (newDirStr,  ".")) 
    return SUCCESS;
  
  // newDirStr == 'Parent Directory' ?
  if (!strcmp (newDirStr, ".."))
  {
    // returns either FAILED_READ_SECTOR or SUCCESS
    return pvt_set_directory_to_parent (Dir, bpb);
  }

  FatEntry * ent = malloc(sizeof * ent);
  fat_init_entry(ent, bpb);
  ent->shortNameEntryClusIndex = Dir->FATFirstCluster;


  uint8_t err = 0;
  do 
    {
      err = fat_next_entry(Dir, ent, bpb);
      if (err != SUCCESS) return err;
      
      if (!strcmp(ent->longName, newDirStr) && (ent->shortNameEntry[11] & DIRECTORY_ENTRY_ATTR))
        {                                                        
          Dir->FATFirstCluster = ent->shortNameEntry[21];
          Dir->FATFirstCluster <<= 8;
          Dir->FATFirstCluster |= ent->shortNameEntry[20];
          Dir->FATFirstCluster <<= 8;
          Dir->FATFirstCluster |= ent->shortNameEntry[27];
          Dir->FATFirstCluster <<= 8;
          Dir->FATFirstCluster |= ent->shortNameEntry[26];
          
          uint8_t snLen;
          if (strlen(newDirStr) < 8) snLen = strlen(newDirStr);
          else snLen = 8; 

          char sn[9];                                    
          for (uint8_t k = 0; k < snLen; k++)  
            sn[k] = ent->shortNameEntry[k];
          sn[snLen] = '\0';

          strcat (Dir->longParentPath,  Dir->longName );
          strcat (Dir->shortParentPath, Dir->shortName);

          // if current directory is not root then append '/'
          if (Dir->longName[0] != '/') 
            strcat(Dir->longParentPath, "/"); 
          strcpy(Dir->longName, newDirStr);
          
          if (Dir->shortName[0] != '/') 
            strcat(Dir->shortParentPath, "/");
          strcpy(Dir->shortName, sn);
          
          return SUCCESS;
        }
    }
  while (err != END_OF_DIRECTORY);

  return END_OF_DIRECTORY;
}


/*
***********************************************************************************************************************
 *                                       PRINT ENTRIES IN A DIRECTORY TO A SCREEN
 * 
 * Description : Prints a list of the entries (files and directories) contained in the directory pointed to by the
 *               FatDir struct instance. Which entries and fields (e.g. hidden files, creation date, etc...) are
 *               selected by passing the desired combination of Entry Filter Flags as the entryFilter argument. See the
 *               specific Entry Filter Flags that can be passed in the FAT.H header file.
 * 
 * Arguments   : *Dir             - Pointer to a FatDir struct whose members must be associated with a valid FAT32 Dir
 *             : entryFilter      - Byte whose value specifies which entries and fields will be printed (short name, 
 *                                  long name, hidden, creation date, etc...). Any combination of flags can be passed. 
 *                                  If neither LONG_NAME or SHORT_NAME are passed then no entries will be printed.
 *             : *bpb             - Pointer to a valid instance of a BPB struct.
 * 
 * Return      : FAT Error Flag     Returns END_OF_DIRECTORY if the function completed successfully and it was able to
 *                                  read in and print entries until reaching the end of the directory. Any other value
 *                                  returned indicates an error. To read, pass the value to fat_print_error(err).
***********************************************************************************************************************
*/

uint8_t 
fat_print_directory (FatDir * dir, uint8_t entryFilter, BPB * bpb)
{
  // Prints column headers according to entryFilter
  print_str("\n\n\r");
  if (CREATION & entryFilter) print_str(" CREATION DATE & TIME,");
  if (LAST_ACCESS & entryFilter) print_str(" LAST ACCESS DATE,");
  if (LAST_MODIFIED & entryFilter) print_str(" LAST MODIFIED DATE & TIME,");
  if (FILE_SIZE & entryFilter) print_str(" SIZE,");
  if (TYPE & entryFilter) print_str(" TYPE,");
  
  print_str(" NAME");
  print_str("\n\r");

  FatEntry * ent = malloc(sizeof * ent);
  fat_init_entry(ent, bpb);
  ent->shortNameEntryClusIndex = dir->FATFirstCluster;

  while ( fat_next_entry(dir, ent, bpb) != END_OF_DIRECTORY)
    { 
      if ((!(ent->shortNameEntry[11] & HIDDEN_ATTR)) || ((ent->shortNameEntry[11]  & HIDDEN_ATTR) && (entryFilter & HIDDEN)))
        {      
          if ((entryFilter & SHORT_NAME) == SHORT_NAME)
            {
              pvt_print_entry_fields (ent->shortNameEntry, 0, entryFilter);
              pvt_print_short_name(ent->shortNameEntry, 0, entryFilter);
            }

          if ((entryFilter & LONG_NAME) == LONG_NAME)
            {
              pvt_print_entry_fields (ent->shortNameEntry, 0, entryFilter);
              if (ent->shortNameEntry[11] & DIRECTORY_ENTRY_ATTR) 
                { 
                  if ((entryFilter & TYPE) == TYPE) 
                    print_str (" <DIR>   ");
                }
              else 
                { 
                  if ((entryFilter & TYPE) == TYPE) 
                    print_str (" <FILE>  ");
                }
              print_str(ent->longName);
            }
        }        
    }
  return END_OF_DIRECTORY;
}



/*
***********************************************************************************************************************
 *                                               PRINT FILE TO SCREEN
 * 
 * Description : Prints the contents of a FAT file from the current directory to a terminal/screen.
 * 
 * Arguments   : *Dir              - Pointer to a FatDir struct instance pointing to a valid FAT32 directory.
 *             : *fileNameStr      - Pointer to C-string. This is the name of the file to be printed to the screen.
 *                                   This can only be a long name unless there is no long name for a given entry, in
 *                                   which case it must be a short name.
 *             : *bpb              - Pointer to a valid instance of a BPB struct.
 * 
 * Return      : FAT Error Flag     Returns END_OF_FILE if the function completed successfully and was able to read in
 *                                  and print a file's contents to the screen. Any other value returned indicates an
 *                                  an error. Pass the returned value to fat_print_error(err).
***********************************************************************************************************************
*/

uint8_t 
fat_print_file(FatDir * Dir, char * fileNameStr, BPB * bpb)
{
  if (pvt_check_valid_name (fileNameStr, Dir)) 
    return INVALID_DIR_NAME;

  FatEntry * ent = malloc(sizeof * ent);
  fat_init_entry(ent, bpb);
  ent->shortNameEntryClusIndex = Dir->FATFirstCluster;

  uint8_t err = 0;
  do 
    {
      err = fat_next_entry(Dir, ent, bpb);
      if (err != SUCCESS) return err;

      if (!strcmp(ent->longName, fileNameStr)
          && !(ent->shortNameEntry[11] & DIRECTORY_ENTRY_ATTR))
        {                                                        
          print_str("\n\n\r");
          return pvt_print_fat_file (0, ent->shortNameEntry, bpb);
        }
    }
  while (err != END_OF_DIRECTORY);

  return END_OF_DIRECTORY;
}



/*
***********************************************************************************************************************
 *                                        PRINT ERROR RETURNED BY A FAT FUNCTION
 * 
 * Description : Call this function to print an error flag returned by one of the FAT functions to the screen. 
 * 
 * Argument    : err    This should be an error flag returned by one of the FAT file/directory functions.
***********************************************************************************************************************
*/

void
fat_print_error (uint8_t err)
{  
  switch(err)
  {
    case SUCCESS: 
      print_str("\n\rSUCCESS");
      break;
    case END_OF_DIRECTORY:
      print_str("\n\rEND_OF_DIRECTORY");
      break;
    case INVALID_FILE_NAME:
      print_str("\n\rINVALID_FILE_NAME");
      break;
    case FILE_NOT_FOUND:
      print_str("\n\rFILE_NOT_FOUND");
      break;
    case INVALID_DIR_NAME:
      print_str("\n\rINVALID_DIR_NAME");
      break;
    case DIR_NOT_FOUND:
      print_str("\n\rDIR_NOT_FOUND");
      break;
    case CORRUPT_FAT_ENTRY:
      print_str("\n\rCORRUPT_FAT_ENTRY");
      break;
    case END_OF_FILE:
      print_str("\n\rEND_OF_FILE");
      break;
    case FAILED_READ_SECTOR:
      print_str("\n\rFAILED_READ_SECTOR");
      break;
    default:
      print_str("\n\rUNKNOWN_ERROR");
      break;
  }
}






/*
***********************************************************************************************************************
 *                                          "PRIVATE" FUNCTION DEFINITIONS
***********************************************************************************************************************
*/




/*
***********************************************************************************************************************
 *                                          (PRIVATE) CHECK FOR LEGAL NAME
 * 
 * Description : Function used to confirm a 'name' string is legal and valid size based on the current settings. This 
 *               function is called by any FAT function that must match a 'name' string argument to a FAT entry name 
 *               (e.g. fat_print_file() or fat_set_directory() ). 
 * 
 * Argument    : *nameStr    - Pointer to C-string that the calling function needs to verify is a legal FAT name.
 *             : *bpb        - Pointer to a valid instance of a BPB struct.
 *            
 * Return      : 0 if name is LEGAL, 1 if name is ILLEGAL.
***********************************************************************************************************************
*/

uint8_t
pvt_check_valid_name (char * nameStr, FatDir * Dir)
{
  // check that long name and path size are 
  // not too large for current settings.
  if (strlen (nameStr) > LONG_NAME_STRING_LEN_MAX) return 1;
  if (( strlen (nameStr) + strlen (Dir->longParentPath)) > PATH_STRING_LEN_MAX) return 1;
  
  // nameStr is illegal if it is an empty string or begins with a space character 
  if ((strcmp (nameStr, "") == 0) || (nameStr[0] == ' ')) return 1;

  // nameStr is illegal if it contains an illegal character
  char illegalCharacters[] = {'\\','/',':','*','?','"','<','>','|'};
  for (uint8_t k = 0; k < strlen (nameStr); k++)
    {       
      for (uint8_t j = 0; j < 9; j++)
        {
          if (nameStr[k] == illegalCharacters[j]) 
            return 1;
        }
    }

  // nameStr is illegal if it is all space characters.
  for (uint8_t k = 0; k < strlen (nameStr); k++)  
    {
      if (nameStr[k] != ' ') 
        return 0; // name is legal
    }  
  return 1; // illegal name
}



/*
***********************************************************************************************************************
 *                                  (PRIVATE) SET CURRENT DIRECTORY TO ITS PARENT
 * 
 * Description : This function is called by fat_set_directory() if that function has been requested to set the FatDir 
 *               instance to its parent directory. This function will carry out the task of setting the members.
 * 
 * Argument    : *Dir         - Pointer to a FatDir instance whose members will be updated to point to the parent of 
 *                              the directory pointed to by the current instance's members.
 *             : *bpb         - Pointer to a valid instance of a BPB struct.
***********************************************************************************************************************
*/

uint8_t
pvt_set_directory_to_parent (FatDir * Dir, BPB * bpb)
{
  uint32_t parentDirFirstClus;
  uint32_t currSecNumPhys;
  uint8_t  currSecArr[bpb->bytesPerSec];
  uint8_t  err;

  currSecNumPhys = bpb->dataRegionFirstSector + ((Dir->FATFirstCluster - 2) * bpb->secPerClus);

  // function returns either 0 for success for 1 for failed.
  err = fat_to_disk_read_single_sector (currSecNumPhys, currSecArr);
  if (err != 0) return FAILED_READ_SECTOR;

  parentDirFirstClus = currSecArr[53];
  parentDirFirstClus <<= 8;
  parentDirFirstClus |= currSecArr[52];
  parentDirFirstClus <<= 8;
  parentDirFirstClus |= currSecArr[59];
  parentDirFirstClus <<= 8;
  parentDirFirstClus |= currSecArr[58];

  // If Dir is pointing to the root directory, do nothing.
  if (Dir->FATFirstCluster == bpb->rootClus); 

  // If parent of Dir is root directory
  else if (parentDirFirstClus == 0)
    {
      strcpy (Dir->shortName, "/");
      strcpy (Dir->shortParentPath, "");
      strcpy (Dir->longName, "/");
      strcpy (Dir->longParentPath, "");
      Dir->FATFirstCluster = bpb->rootClus;
    }

  // parent directory is not root directory
  else
    {          
      char tmpShortNamePath[PATH_STRING_LEN_MAX];
      char tmpLongNamePath[PATH_STRING_LEN_MAX];

      strlcpy (tmpShortNamePath, Dir->shortParentPath, strlen (Dir->shortParentPath));
      strlcpy (tmpLongNamePath, Dir->longParentPath,   strlen (Dir->longParentPath ));
      
      char *shortNameLastDirInPath = strrchr (tmpShortNamePath, '/');
      char *longNameLastDirInPath  = strrchr (tmpLongNamePath , '/');
      
      strcpy (Dir->shortName, shortNameLastDirInPath + 1);
      strcpy (Dir->longName , longNameLastDirInPath  + 1);

      strlcpy (Dir->shortParentPath, tmpShortNamePath, (shortNameLastDirInPath + 2) - tmpShortNamePath);
      strlcpy (Dir->longParentPath,  tmpLongNamePath,  (longNameLastDirInPath  + 2) -  tmpLongNamePath);

      Dir->FATFirstCluster = parentDirFirstClus;
    }
    return SUCCESS;
}



/*
***********************************************************************************************************************
 *                           (PRIVATE) SET DIRECTORY TO A CHILD DIRECTORY
 * 
 * Description : This function is called by fat_set_directory() if that function has been asked to set an instance of
 *               FatDir to a child directory and a valid matching entry was found in the directory currently pointed at
 *               by the FatDir instance. This function will only update the struct's members to that of the matching 
 *               entry. It does not perform any of the search/compare required to find the matching entry.
 * 
 * Argument    : *Dir            - Pointer to an instance of a FatDir whose members will be updated to point to the 
 *                                 directory whose name matches *childDirStr.
 *             : *sectorArr      - Pointer to an array that holds the physical sector's contents containing the short
 *                                 name of the entry whose name matches *childDirStr.
 *             : snPos           - Integer that specifies the position of the first byte of the 32-byte short name 
 *                                 entry in *sectorArr.
 *             : *childDirStr    - Pointer to a C-string whose name matches an entry in the current directory FatDir is
 *                                 set to. This is the name of the directory FatDir will be set to by this function. 
 *                                 This is a long name, unless a long name does not exist for a given entry. In that
 *                                 case the longName and shortName members of the FatDir instance will both be set to 
 *                                 the short name of the entry.  
 *             : *bpb            - Pointer to a valid instance of a BPB struct.
***********************************************************************************************************************
*/

void
pvt_set_directory_to_child (FatDir * Dir, uint8_t * sectorArr, uint16_t snPos, char * childDirStr, BPB * bpb)
{
  uint32_t dirFirstClus;
  dirFirstClus = sectorArr[snPos + 21];
  dirFirstClus <<= 8;
  dirFirstClus |= sectorArr[snPos + 20];
  dirFirstClus <<= 8;
  dirFirstClus |= sectorArr[snPos + 27];
  dirFirstClus <<= 8;
  dirFirstClus |= sectorArr[snPos + 26];

  Dir->FATFirstCluster = dirFirstClus;
  
  uint8_t snLen;
  if (strlen(childDirStr) < 8) snLen = strlen(childDirStr);
  else snLen = 8; 

  char sn[9];                                    
  for (uint8_t k = 0; k < snLen; k++)  
    sn[k] = sectorArr[snPos + k];
  sn[snLen] = '\0';

  strcat (Dir->longParentPath,  Dir->longName );
  strcat (Dir->shortParentPath, Dir->shortName);

  // if current directory is not root then append '/'
  if (Dir->longName[0] != '/') 
    strcat(Dir->longParentPath, "/"); 
  strcpy(Dir->longName, childDirStr);
  
  if (Dir->shortName[0] != '/') 
    strcat(Dir->shortParentPath, "/");
  strcpy(Dir->shortName, sn);
}



/*
***********************************************************************************************************************
 *                                  (PRIVATE) LOAD A LONG NAME ENTRY INTO A C-STRING
 * 
 * Description : This function is called by a FAT function that must read in a long name from a FAT directory into a
 *               C-string. The function is called twice if a long name crosses a sector boundary, and *lnStrIndx will
 *               point to the position in the string to begin loading the next characters
 * 
 * Arguments   : lnFirstEntry        - Integer that points to the position in *sectorArr that is the lowest order entry
 *                                     of the long name in the sector array.
 *             : lnLastEntry         - Integer that points to the position in *sectorArr that is the highest order
 *                                     entry of the long name in the sector array.
 *             : *sectorArr          - Pointer to an array that holds the physical sector's contents containing the 
 *                                     the entries of the long name that will be loaded into the char arry *lnStr.
 *             : *lnStr              - Pointer to a null terminated char array (C-string) that will be loaded with the
 *                                     long name characters from the sector. Usually starts as an array of NULLs.
 *             : *lnStrIndx          - Pointer to an integer that specifies the position in *lnStr where the first
 *                                     character will be loaded when this function is called. This function will update
 *                                     this value as the characters are loaded. If a subsequent call to this function 
 *                                     is made, as required when a single long name crosses a sector boundary, then 
 *                                     this value will be its final value from the previous call.
***********************************************************************************************************************
*/

void
pvt_load_long_name (int lnFirstEntry, int lnLastEntry, uint8_t * sectorArr, char * lnStr, uint8_t * lnStrIndx)
{
  for (int i = lnFirstEntry; i >= lnLastEntry; i = i - ENTRY_LEN)
    {                                              
      for (uint16_t n = i + 1; n < i + 11; n++)
        {                                  
          if ((sectorArr[n] == 0) || (sectorArr[n] > 126));
          else 
            { 
              lnStr[*lnStrIndx] = sectorArr[n];
              (*lnStrIndx)++;  
            }
        }

      for (uint16_t n = i + 14; n < i + 26; n++)
        {                                  
          if ((sectorArr[n] == 0) || (sectorArr[n] > 126));
          else 
            { 
              lnStr[*lnStrIndx] = sectorArr[n];
              (*lnStrIndx)++;  
            }
        }
      
      for (uint16_t n = i + 28; n < i + 32; n++)
        {                                  
          if ((sectorArr[n] == 0) || (sectorArr[n] > 126));
          else 
            { 
              lnStr[*lnStrIndx] = sectorArr[n];  
              (*lnStrIndx)++;  
            }
        }        
    }
}



/*
***********************************************************************************************************************
 *                                   (PRIVATE) GET THE FAT INDEX OF THE NEXT CLUSTER 
 *
 * Description : Used by the FAT functions to get the location of the next cluster in a directory or file. The value  
 *               returned is an integer that specifies to the cluster's index in the FAT. This value is offset by two 
 *               when counting the clusters in a FATs Data Region. Therefore, to get the cluster number in the data 
 *               region the value returned by this function must be subtracted by 2.
 * 
 * Arguments   : currClusIndx      - A cluster's FAT index. The value at this index in the FAT is the index of the
 *                                   the next cluster of the file or directory.
 *             : *bpb              - Pointer to a valid instance of a BPB struct.
 * 
 * Returns     : The FAT index of the next cluster of a file or Dir. This is the value at the currClusIndx's location  
 *               in the FAT. If a value of 0x0FFFFFFF is returned then the current cluster is the last cluster of the 
 *               file or directory.
***********************************************************************************************************************
*/

uint32_t 
pvt_get_next_cluster_index (uint32_t currClusIndx, BPB * bpb)
{
  uint8_t  bytesPerClusIndx = 4; // for FAT32
  uint16_t numOfIndexedClustersPerSecOfFat = bpb->bytesPerSec / bytesPerClusIndx; // = 128

  uint32_t clusIndx = currClusIndx / numOfIndexedClustersPerSecOfFat;
  uint32_t clusIndxStartByte = 4 * (currClusIndx % numOfIndexedClustersPerSecOfFat);
  uint32_t cluster = 0;

  uint32_t fatSectorToRead = clusIndx + bpb->rsvdSecCnt;

  uint8_t sectorArr[bpb->bytesPerSec];
  
  fat_to_disk_read_single_sector (fatSectorToRead, sectorArr);

  cluster = sectorArr[clusIndxStartByte+3];
  cluster <<= 8;
  cluster |= sectorArr[clusIndxStartByte+2];
  cluster <<= 8;
  cluster |= sectorArr[clusIndxStartByte+1];
  cluster <<= 8;
  cluster |= sectorArr[clusIndxStartByte];

  return cluster;
}



/*
***********************************************************************************************************************
 *                                        (PRIVATE) PRINTS THE FIELDS OF FAT ENTRY
 *
 * Description : Used by fat_print_directory() to print the fields associated with an entry (e.g. creation/last 
 *               modified date/time, file size, etc...). Which fields are printed is determined by the entryFilter 
 *               flag set.
 * 
 * Arguments   : *sectorArr     - Pointer to an array that holds the short name of the entry that is being printed to
 *                                the screen. Only the short name entry of a short name/long name combination holds
 *                                the values of these fields.
 *             : entryPos       - Integer specifying the location in *sectorArr of the first byte of the short name 
 *                                entry whose fields should be printed to the screen.
 *             : entryFilter    - Byte specifying the entryFlag settings. This is used to determined which fields of 
 *                                the entry will be printed.
***********************************************************************************************************************
*/

void 
pvt_print_entry_fields (uint8_t *sectorArr, uint16_t entryPos, uint8_t entryFilter)
{
  uint16_t creationTime;
  uint16_t creationDate;
  uint16_t lastAccessDate;
  uint16_t writeTime;
  uint16_t writeDate;
  uint32_t fileSize;

  // Load fields with values from sectorArr

  if (CREATION & entryFilter)
    {
      creationTime = sectorArr[entryPos + 15];
      creationTime <<= 8;
      creationTime |= sectorArr[entryPos + 14];
      
      creationDate = sectorArr[entryPos + 17];
      creationDate <<= 8;
      creationDate |= sectorArr[entryPos + 16];
    }

  if (LAST_ACCESS & entryFilter)
    {
      lastAccessDate = sectorArr[entryPos + 19];
      lastAccessDate <<= 8;
      lastAccessDate |= sectorArr[entryPos + 18];
    }

  if (LAST_MODIFIED & entryFilter)
    {
      writeTime = sectorArr[entryPos + 23];
      writeTime <<= 8;
      writeTime |= sectorArr[entryPos + 22];

      writeDate = sectorArr[entryPos + 25];
      writeDate <<= 8;
      writeDate |= sectorArr[entryPos + 24];
    }

  fileSize = sectorArr[entryPos + 31];
  fileSize <<= 8;
  fileSize |= sectorArr[entryPos + 30];
  fileSize <<= 8;
  fileSize |= sectorArr[entryPos + 29];
  fileSize <<= 8;
  fileSize |= sectorArr[entryPos + 28];

  print_str ("\n\r");

  // Print fields 

  if (CREATION & entryFilter)
    {
      print_str ("    ");
      if (((creationDate & 0x01E0) >> 5) < 10) 
        {
          print_str ("0");
        }
      print_dec ((creationDate & 0x01E0) >> 5);
      print_str ("/");
      if ((creationDate & 0x001F) < 10)
        {
          print_str ("0");
        }
      print_dec (creationDate & 0x001F);
      print_str ("/");
      print_dec (1980 + ((creationDate & 0xFE00) >> 9));

      print_str ("  ");
      if (((creationTime & 0xF800) >> 11) < 10) 
        {
          print_str ("0");
        }
      print_dec (((creationTime & 0xF800) >> 11));
      print_str (":");
      if (((creationTime & 0x07E0) >> 5) < 10)
        {
          print_str ("0");
        }
      print_dec ((creationTime & 0x07E0) >> 5);
      print_str (":");
      if ((2 * (creationTime & 0x001F)) < 10) 
        {
          print_str ("0");
        }
      print_dec (2 * (creationTime & 0x001F));
    }

  if (LAST_ACCESS & entryFilter)
    {
      print_str ("     ");
      if (((lastAccessDate & 0x01E0) >> 5) < 10)
        {
          print_str ("0");
        }
      print_dec ((lastAccessDate & 0x01E0) >> 5);
      print_str ("/");
      if ((lastAccessDate & 0x001F) < 10) 
        {
          print_str("0");
        }
      print_dec (lastAccessDate & 0x001F);
      print_str ("/");
      print_dec (1980 + ((lastAccessDate & 0xFE00) >> 9));
    }


  if (LAST_MODIFIED & entryFilter)
    {
      print_str ("     ");
      if (((writeDate & 0x01E0) >> 5) < 10) 
        {
          print_str ("0");
        }
      print_dec ((writeDate & 0x01E0) >> 5);
      print_str ("/");
      if ((writeDate & 0x001F) < 10) 
        {
          print_str ("0");
        }
      print_dec (writeDate & 0x001F);
      print_str ("/");
      print_dec (1980 + ((writeDate & 0xFE00) >> 9));

      print_str ("  ");

      if (((writeTime & 0xF800) >> 11) < 10)
       {
         print_str ("0");
       }
      print_dec (((writeTime & 0xF800) >> 11));
      print_str (":");
      
      if (((writeTime & 0x07E0) >> 5) < 10) 
        {
          print_str ("0");
        }
      print_dec ((writeTime & 0x07E0) >> 5);
      print_str (":");
      if ((2 * (writeTime & 0x001F)) < 10) 
        {
          print_str ("0");
        }
      print_dec (2 * (writeTime & 0x001F));
    }

  uint16_t div = 1000;
  print_str ("     ");
  if ((entryFilter & FILE_SIZE) == FILE_SIZE)
  {
        if ((fileSize / div) >= 10000000) { print_str(" "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 1000000) { print_str("  "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 100000) { print_str("   "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 10000) { print_str("    "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 1000) { print_str("     "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 100) { print_str("      "); print_dec(fileSize / div); }
    else if ((fileSize / div) >= 10) { print_str("       "); print_dec(fileSize / div); }
    else                             { print_str("        ");print_dec(fileSize / div);}        
  
  print_str("kB  ");
  }

}



/*
***********************************************************************************************************************
 *                                              (PRIVATE) PRINT SHORT NAME
 *
 * Description : Used by fat_print_directory to print the short name of a FAT file or directory.
 * 
 * Arguments   : *sectorArr     - Pointer to an array that holds the short name of the entry that is to be printed to
 *                                the screen.
 *             : entryPos       - Location in *sectorArr of the first byte of the short name entry that is to be
 *                                printed to the screen.
 *             : entryFilter    - Byte specifying the entryFlag settings. This is used to determined here if the TYPE 
 *                                flag has been passed
***********************************************************************************************************************
*/

void 
pvt_print_short_name (uint8_t *sectorArr, uint16_t entryPos, uint8_t entryFilter)
{
  char sn[9];
  char ext[5];

  for (uint8_t k = 0; k < 8; k++) sn[k] = ' ';
  sn[8] = '\0';

  uint8_t attr = sectorArr[entryPos + 11];
  if (attr & 0x10)
    {
      if ((entryFilter & TYPE) == TYPE) print_str (" <DIR>   ");
      for (uint8_t k = 0; k < 8; k++) sn[k] = sectorArr[entryPos + k];
      print_str (sn);
      print_str ("    ");
    }
  else 
    {
      if ((entryFilter & TYPE) == TYPE) print_str (" <FILE>  ");
      // initialize extension char array
      strcpy (ext, ".   ");
      for (uint8_t k = 1; k < 4; k++) ext[k] = sectorArr[entryPos + 7 + k];
      for (uint8_t k = 0; k < 8; k++) 
        {
          sn[k] = sectorArr[k + entryPos];
          if (sn[k] == ' ') 
            { 
              sn[k] = '\0'; 
              break; 
            };
        }

      print_str (sn);
      if (strcmp (ext, ".   ")) print_str (ext);
      for (uint8_t p = 0; p < 10 - (strlen (sn) + 2); p++) print_str (" ");
    }
}



/*
***********************************************************************************************************************
 *                                              (PRIVATE) PRINT A FAT FILE
 *
 * Description : Used by fat_print_file() to perform that actual print operation.
 * 
 * Arguments   : entryPos       - Integer that points to the location in *fileSector of the first byte of the short 
 *                                name entryPos of the file whose contents will be printed to the screen. This is 
 *                                required as the first cluster index of the file is located in the short name.
 *             : *fileSector      pointer to an array that holds the short name entryPos of the file to be printed to 
 *                                the screen.
 *             : *bpb           - Pointer to a valid instance of a BPB struct.
***********************************************************************************************************************
*/

uint8_t 
pvt_print_fat_file (uint16_t entryPos, uint8_t *fileSector, BPB * bpb)
  {
    uint32_t currSecNumPhys;
    uint32_t cluster;
    uint8_t  err;
    uint8_t  eof = 0; // end of file flag

    //get FAT index for file's first cluster
    cluster =  fileSector[entryPos + 21];
    cluster <<= 8;
    cluster |= fileSector[entryPos + 20];
    cluster <<= 8;
    cluster |= fileSector[entryPos + 27];
    cluster <<= 8;
    cluster |= fileSector[entryPos + 26];

    // read in contents of file starting at relative sector 0 in 'cluster' and print contents to the screen.
    do
      {
        for (uint32_t currSecNumInClus = 0; currSecNumInClus < bpb->secPerClus; currSecNumInClus++) 
          {
            if (eof == 1) break; // end-of-file reached.
            currSecNumPhys = currSecNumInClus + bpb->dataRegionFirstSector + ((cluster - 2) * bpb->secPerClus);

            // function returns either 0 for success for 1 for failed.
            err = fat_to_disk_read_single_sector (currSecNumPhys, fileSector);
            if (err == 1) return FAILED_READ_SECTOR;

            for (uint16_t k = 0; k < bpb->bytesPerSec; k++)
              {
                // for formatting how this shows up on the screen.
                if (fileSector[k] == '\n') print_str ("\n\r");
                else if (fileSector[k] != 0) usart_transmit (fileSector[k]);

                // checks if end of file by checking to see if remaining bytes in sector are zeros.
                else 
                  {
                    eof = 1;
                    for (uint16_t i = k+1; i < bpb->bytesPerSec; i++)
                      {
                        if (fileSector[i] != 0) 
                          { 
                            eof = 0;
                            break;
                          }
                      }
                  }
              }
          }
      } 
    while (((cluster = pvt_get_next_cluster_index(cluster,bpb)) != END_CLUSTER));
    return END_OF_FILE;
  }



/*
***********************************************************************************************************************
 *                                    (PRIVATE) CORRECTION TO ENTRY POSITON POINTER
 *
 * Description : Used by the FAT functions when searching within a directory. Often a correction to entryPos needs to 
 *               be made to ensure it is pointing at the correct location in the sector array and this function will 
 *               ensure this. If this function determines that the actual correct location of the entry is not in the 
 *               current sector being searched when this function is called, then this function will return a 1 to
 *               signal the calling function to break and get the next sector before proceeding. This function will
 *               then be called again to ensure that entryPos is indicating the correct location in the new sector.
 * 
 * Arguments   : lnFlags            - Byte that is holding the setting of the three long name flags: LONG_NAME_EXISTS,
 *                                    LONG_NAME_CROSSES_SECTOR_BOUNDARY, and LONG_NAME_LAST_SECTOR_ENTRY. The long 
 *                                    name flags should be cleared (set to 0) by the calling function after this 
 *                                    function returns with 0.
 *             : *entryPos          - Pointer to an integer that specifies the location in the current sector that will
 *                                    be checked/read-in by the calling function. This value will be updated by this
 *                                    function if a correction to it is required. 
 *             : *snPosCurrSec      - Pointer to an integer that specifies the final value of this variable after
 *                                    the previous entry was checked. Stands for Short Name Position In Current Sector.
 *             : *snPosNextSec      - Pointer to an integer that specifies the final value of this variable after
 *                                    the previous entry was checked. Stands for Short Name Position In Next Sector.
 * 
 * Returns     : 1 if this function determines correct entry is in the next sector when this function is called.
 *             : 0 if the correct entry is in the current sector when this function is called.
***********************************************************************************************************************
*/

uint8_t 
pvt_correct_entry_check (uint8_t lnFlags, uint16_t * entryPos, uint16_t * snPosCurrSec, uint16_t * snPosNextSec)
{  
  if (lnFlags & LONG_NAME_EXISTS)
    {
      if ((*snPosCurrSec) >= (SECTOR_LEN - ENTRY_LEN))
        {
          if ((*entryPos) != 0) return 1; // need to get the next sector
          else (*snPosCurrSec) = -ENTRY_LEN;
        }

      if (lnFlags & (LONG_NAME_CROSSES_SECTOR_BOUNDARY | LONG_NAME_LAST_SECTOR_ENTRY))
        {
          *entryPos = (*snPosNextSec) + ENTRY_LEN; 
          *snPosNextSec = 0;
        }
      else 
        {
          *entryPos = (*snPosCurrSec) + ENTRY_LEN;
          *snPosCurrSec = 0;
        }
    }
  return 0;
}    



/*
***********************************************************************************************************************
 *                                       (PRIVATE) SET THE LONG NAME FLAGS
 *
 * Description : Used by the FAT functions to set the long name flags if it was determined that a long name exists for 
 *               the current entry begin checked/read-in. This also sets the variable snPosCurrSec.
 * 
 * Arguments   : *lnFlags           - Pointer to a byte that will hold the long name flag settings: LONG_NAME_EXISTS,
 *                                    LONG_NAME_CROSSES_SECTOR_BOUNDARY, and LONG_NAME_LAST_SECTOR_ENTRY. This function
 *                                    will determine which flags should be set and then set this byte accordingly.
 *             : entryPos           - Integer that specifies the location in the current sector that will be checked 
 *                                    during the subsequent execution.
 *             : *snPosCurrSec      - This value is set by this function. It is an integer pointer that specifies the 
 *                                    position of the short name relative to the position of the first byte of the 
 *                                    current sector. NOTE: If this is greater than 511 then the short name is in the
 *                                    next sector.
 *             : *snPosNextSec      - Integer pointer. This value is set by this function if it is determined that the
 *                                    short name entry is in the next sector compared to the sector where the last 
 *                                    entry of its corresponding long name resides.
 *             : *bpb               - Pointer to a valid instance of a BPB struct.
***********************************************************************************************************************
*/

void
pvt_set_long_name_flags (uint8_t * lnFlags, uint16_t entryPos, uint16_t * snPosCurrSec,
                         uint8_t * currSecArr, BPB * bpb)
{
  *lnFlags |= LONG_NAME_EXISTS;

  // number of entries required by the long name
  uint8_t longNameOrder = LONG_NAME_ORDINAL_MASK & currSecArr[entryPos];                 
  *snPosCurrSec = entryPos + (ENTRY_LEN * longNameOrder);
  
  // If short name position is greater than 511 then the short name is in the next sector.
  if ((*snPosCurrSec) >= bpb->bytesPerSec)
    {
      if ((*snPosCurrSec) > bpb->bytesPerSec) *lnFlags |= LONG_NAME_CROSSES_SECTOR_BOUNDARY;
      else if (*snPosCurrSec == SECTOR_LEN)   *lnFlags |= LONG_NAME_LAST_SECTOR_ENTRY;
    }
}   



/*
***********************************************************************************************************************
 *                             (PRIVATE) LOAD THE CONTENTS OF THE NEXT SECTOR INTO AN ARRAY
 *
 * Description : Used by the FAT functions to load the contents of the next file or directory sector into the 
 *               *nextSecArr array if it is found that a long/short name combo crosses the sector boundary.
 * 
 * Arguments   : *nextSecArr             - Pointer to an array that will be loaded with the contents of the next sector
 *                                         of a file or directory.
 *             : currSecNumInClus        - Integer that specifies the current sector number relative to the current 
 *                                         cluster. This value is used to determine if the next sector is in the 
 *                                         current or the next cluster.
 *             : currSecNumPhys          - Integer specifying the current sector's physical sector number on the disk 
 *                                         hosting the FAT volume.
 *             : clusIndx                - Integer specifying the current cluster's FAT index. This is required if it
 *                                         is determined that the next sector is in the next cluster.
 *             : *bpb                    - Pointer to a valid instance of a BPB struct.
***********************************************************************************************************************
*/

uint8_t
pvt_get_next_sector (uint8_t * nextSecArr, uint32_t currSecNumInClus, 
                     uint32_t currSecNumPhys, uint32_t clusIndx, BPB * bpb)
{
  uint32_t nextSecNumPhys;
  uint8_t  err = 0;
  
  if (currSecNumInClus >= (bpb->secPerClus - 1)) 
    nextSecNumPhys = bpb->dataRegionFirstSector + ((pvt_get_next_cluster_index (clusIndx, bpb) - 2) * bpb->secPerClus);
  else 
    nextSecNumPhys = 1 + currSecNumPhys;

  // function returns either 0 for success or 1 for failed.
  err = fat_to_disk_read_single_sector (nextSecNumPhys, nextSecArr);
  if (err == 1) 
    return FAILED_READ_SECTOR;
  else 
    return SUCCESS;
}
