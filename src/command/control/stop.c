/***********************************************************************************************************************************
Stop Command
***********************************************************************************************************************************/
#include "build.auto.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#include "command/control/common.h"
#include "common/debug.h"
#include "common/type/convert.h"
#include "config/config.h"
#include "storage/helper.h"
#include "storage/storage.h"
#include "storage/storage.intern.h"

void
cmdStop(void)
{
    FUNCTION_LOG_VOID(logLevelDebug);

    MEM_CONTEXT_TEMP_BEGIN()
    {
        String *stopFile = lockStopFileName(cfgOptionStr(cfgOptStanza));

        // If the stop file does not already exist, then create it
        if (!storageExistsNP(storageLocal(), stopFile))
        {
            // Create the lock path (ignore if already created)
            storagePathCreateP(storageLocalWrite(), strPath(stopFile), .mode = 0770);

            // Create the stop file with Read/Write and Create only - do not use Truncate
            int fileHandle = -1;
            THROW_ON_SYS_ERROR_FMT(
                ((fileHandle = open(strPtr(stopFile), O_WRONLY | O_CREAT, STORAGE_MODE_FILE_DEFAULT)) == -1), FileOpenError,
                "unable to open stop file '%s'", strPtr(stopFile));

            // Close the file
            close(fileHandle);

            // If --force was specified then send term signals to running processes
            if (cfgOptionBool(cfgOptForce))
            {
                const String *lockPath = cfgOptionStr(cfgOptLockPath);
                StringList *lockPathFileList = strLstSort(
                    storageListP(storageLocal(), lockPath, .errorOnMissing = true), sortOrderAsc);

                // Find each lock file and send term signals to the processes
                for (unsigned int lockPathFileIdx = 0; lockPathFileIdx < strLstSize(lockPathFileList); lockPathFileIdx++)
                {
                    String *lockFile = strNewFmt("%s/%s", strPtr(lockPath), strPtr(strLstGet(lockPathFileList, lockPathFileIdx)));

                    // Skip any file that is not a lock file
                    if (!strEndsWithZ(lockFile, LOCK_FILE_EXT))
                        continue;

                    // If we cannot open the lock file for any reason then warn and continue to next file
                    if ((fileHandle = open(strPtr(lockFile), O_RDONLY, 0)) == -1)
                    {
                        LOG_WARN( "unable to open lock file %s", strPtr(lockFile));
                        continue;
                    }

                    // Attempt a lock on the file - if a lock can be acquired that means the original process died without removing
                    // the lock file so remove it now
                    if (flock(fileHandle, LOCK_EX | LOCK_NB) == 0)
                    {
                        unlink(strPtr(lockFile));
                        close(fileHandle);
                        continue;
                    }

                    // The file is locked so that means there is a running process - read the process id and send it a term signal
                    char contents[64];
                    ssize_t actualBytes = read(fileHandle, contents, sizeof(contents));
                    String *processId = actualBytes > 0 ? strTrim(strNewN(contents, (size_t)actualBytes)) : NULL;

                    // If the process id is defined then assume this is a valid lock file
                    if (processId != NULL && strSize(processId) > 0)
                    {
                        if (kill(cvtZToInt(strPtr(processId)), SIGTERM) != 0)
                            LOG_WARN("unable to send term signal to process %s", strPtr(processId));
                        else
                            LOG_INFO("sent term signal to process %s", strPtr(processId));
                    }
                    else
                    {
                        unlink(strPtr(lockFile));
                        close(fileHandle);
                    }
                }
            }
        }
        else
        {
            LOG_WARN(
                "stop file already exists for %s",
                cfgOptionTest(cfgOptStanza) ? strPtr(strNewFmt("stanza %s", strPtr(cfgOptionStr(cfgOptStanza)))) : "all stanzas");
        }
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN_VOID();
}
