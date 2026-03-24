/*------------------------------------------------------------------------*/
/* Sample Code of OS Dependent Functions for FatFs                        */
/* (C)ChaN, 2018                                                          */
/*------------------------------------------------------------------------*/


#include "ff.h"


#if FF_USE_LFN == 3	/* Dynamic memory allocation */

/*------------------------------------------------------------------------*/
/* Allocate a memory block                                                */
/*------------------------------------------------------------------------*/

void* ff_memalloc (	/* Returns pointer to the allocated memory block (null if not enough core) */
	UINT msize		/* Number of bytes to allocate */
)
{
	return malloc(msize);	/* Allocate a new memory block with POSIX API */
}


/*------------------------------------------------------------------------*/
/* Free a memory block                                                    */
/*------------------------------------------------------------------------*/

void ff_memfree (
	void* mblock	/* Pointer to the memory block to free (nothing to do if null) */
)
{
	free(mblock);	/* Free the memory block with POSIX API */
}

#endif



#if FF_FS_REENTRANT	/* Mutual exclusion */

#ifdef USE_FREERTOS

#include "FreeRTOS.h"
#include "semphr.h"

/* Static storage for per-volume mutex (FF_VOLUMES == 1) */
static StaticSemaphore_t sFatFsMutexStorage;
static SemaphoreHandle_t sFatFsMutexHandle = NULL;

int ff_cre_syncobj (BYTE vol, FF_SYNC_t* sobj)
{
	(void)vol;
	*sobj = xSemaphoreCreateMutexStatic(&sFatFsMutexStorage);
	sFatFsMutexHandle = (SemaphoreHandle_t)*sobj;
	return (int)(*sobj != NULL);
}

int ff_del_syncobj (FF_SYNC_t sobj)
{
	(void)sobj;
	/* Static allocation — nothing to free */
	return 1;
}

int ff_req_grant (FF_SYNC_t sobj)
{
	return (int)(xSemaphoreTake(sobj, FF_FS_TIMEOUT) == pdTRUE);
}

void ff_rel_grant (FF_SYNC_t sobj)
{
	xSemaphoreGive(sobj);
}

/* Lock/unlock the FatFS volume mutex from outside FatFS.
 * Used by the cluster loader task to serialize raw disk_read calls
 * against FatFS file operations that use the same SD card. */
void fatfs_lock_volume(void)
{
	if (sFatFsMutexHandle) {
		xSemaphoreTake(sFatFsMutexHandle, portMAX_DELAY);
	}
}

void fatfs_unlock_volume(void)
{
	if (sFatFsMutexHandle) {
		xSemaphoreGive(sFatFsMutexHandle);
	}
}

#else
/* Non-FreeRTOS stub — should not be reached since FF_FS_REENTRANT is
 * only enabled under USE_FREERTOS in ffconf.h */
int ff_cre_syncobj (BYTE vol, FF_SYNC_t* sobj) { (void)vol; (void)sobj; return 1; }
int ff_del_syncobj (FF_SYNC_t sobj) { (void)sobj; return 1; }
int ff_req_grant (FF_SYNC_t sobj) { (void)sobj; return 1; }
void ff_rel_grant (FF_SYNC_t sobj) { (void)sobj; }
#endif

#endif

