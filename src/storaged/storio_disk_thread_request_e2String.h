#ifndef __storio_disk_thread_request_e2String_h__
#define __storio_disk_thread_request_e2String_h__
#ifdef __cplusplus
extern "C" {
#endif /*__cplusplus*/

/*___________________________________________________________________
 
   Generated by enum2String.py 
   Date : Monday 2017 November 06, 11:48:28
   Command line : 
 ../../tools/enum2String.py -n storio_disk_thread_request_e -f storio_disk_thread_intf.h -c 19

 ____________________________________________________________________
 */

/*_________________________________________________________________
 * Builds a string from an integer value supposed to be within
 * the enumerated list storio_disk_thread_request_e
 *
 * @param x : value from storio_disk_thread_request_e to translate into a string
 *
 * The input value is translated into a string deduced from the enum
 * definition. When the input value do not fit any of the predefined
 * values, "??" is returned
 *
 * @return A char pointer to the constant string or "??"
 *_________________________________________________________________*/ 
static inline char * storio_disk_thread_request_e2String (storio_disk_thread_request_e x) {

  switch(x) {
    case STORIO_DISK_THREAD_READ                 : return("READ");
    case STORIO_DISK_THREAD_RESIZE               : return("RESIZE");
    case STORIO_DISK_THREAD_WRITE                : return("WRITE");
    case STORIO_DISK_THREAD_TRUNCATE             : return("TRUNCATE");
    case STORIO_DISK_THREAD_WRITE_REPAIR3        : return("WRITE REPAIR3");
    case STORIO_DISK_THREAD_REMOVE               : return("REMOVE");
    case STORIO_DISK_THREAD_REMOVE_CHUNK         : return("REMOVE CHUNK");
    case STORIO_DISK_THREAD_REBUILD_START        : return("REBUILD START");
    case STORIO_DISK_THREAD_REBUILD_STOP         : return("REBUILD STOP");
    case STORIO_DISK_THREAD_MAX_OPCODE           : return("MAX OPCODE");
    /* Unexpected value */
    default: return "??";
  }
}
#ifdef	__cplusplus
}
#endif
#endif
