/***********************************************************************************************************************************
Remote Storage Protocol Handler
***********************************************************************************************************************************/
#include "build.auto.h"

#include "command/backup/pageChecksum.h"
#include "common/compress/gzip/compress.h"
#include "common/compress/gzip/decompress.h"
#include "common/crypto/cipherBlock.h"
#include "common/crypto/hash.h"
#include "common/debug.h"
#include "common/io/filter/sink.h"
#include "common/io/filter/size.h"
#include "common/io/io.h"
#include "common/log.h"
#include "common/memContext.h"
#include "common/regExp.h"
#include "config/config.h"
#include "storage/remote/protocol.h"
#include "storage/helper.h"
#include "storage/storage.intern.h"

/***********************************************************************************************************************************
Constants
***********************************************************************************************************************************/
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_EXISTS_STR,                  PROTOCOL_COMMAND_STORAGE_EXISTS);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_FEATURE_STR,                 PROTOCOL_COMMAND_STORAGE_FEATURE);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_LIST_STR,                    PROTOCOL_COMMAND_STORAGE_LIST);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_OPEN_READ_STR,               PROTOCOL_COMMAND_STORAGE_OPEN_READ);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_OPEN_WRITE_STR,              PROTOCOL_COMMAND_STORAGE_OPEN_WRITE);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_PATH_CREATE_STR,             PROTOCOL_COMMAND_STORAGE_PATH_CREATE);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_PATH_EXISTS_STR,             PROTOCOL_COMMAND_STORAGE_PATH_EXISTS);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_PATH_REMOVE_STR,             PROTOCOL_COMMAND_STORAGE_PATH_REMOVE);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_PATH_SYNC_STR,               PROTOCOL_COMMAND_STORAGE_PATH_SYNC);
STRING_EXTERN(PROTOCOL_COMMAND_STORAGE_REMOVE_STR,                  PROTOCOL_COMMAND_STORAGE_REMOVE);

/***********************************************************************************************************************************
Regular expressions
***********************************************************************************************************************************/
STRING_STATIC(BLOCK_REG_EXP_STR,                                    PROTOCOL_BLOCK_HEADER "-1|[0-9]+");

/***********************************************************************************************************************************
Local variables
***********************************************************************************************************************************/
static struct
{
    MemContext *memContext;                                         // Mem context
    RegExp *blockRegExp;                                            // Regular expression to check block messages
} storageRemoteProtocolLocal;

/***********************************************************************************************************************************
Set filter group based on passed filters
***********************************************************************************************************************************/
static void
storageRemoteFilterGroup(IoFilterGroup *filterGroup, const Variant *filterList)
{
    FUNCTION_TEST_BEGIN();
        FUNCTION_TEST_PARAM(IO_FILTER_GROUP, filterGroup);
        FUNCTION_TEST_PARAM(VARIANT, filterList);
    FUNCTION_TEST_END();

    ASSERT(filterGroup != NULL);
    ASSERT(filterList != NULL);

    for (unsigned int filterIdx = 0; filterIdx < varLstSize(varVarLst(filterList)); filterIdx++)
    {
        const KeyValue *filterKv = varKv(varLstGet(varVarLst(filterList), filterIdx));
        const String *filterKey = varStr(varLstGet(kvKeyList(filterKv), 0));
        const VariantList *filterParam = varVarLst(kvGet(filterKv, VARSTR(filterKey)));

        if (strEq(filterKey, GZIP_COMPRESS_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, gzipCompressNewVar(filterParam));
        else if (strEq(filterKey, GZIP_DECOMPRESS_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, gzipDecompressNewVar(filterParam));
        else if (strEq(filterKey, CIPHER_BLOCK_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, cipherBlockNewVar(filterParam));
        else if (strEq(filterKey, CRYPTO_HASH_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, cryptoHashNewVar(filterParam));
        else if (strEq(filterKey, PAGE_CHECKSUM_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, pageChecksumNewVar(filterParam));
        else if (strEq(filterKey, SINK_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, ioSinkNew());
        else if (strEq(filterKey, SIZE_FILTER_TYPE_STR))
            ioFilterGroupAdd(filterGroup, ioSizeNew());
        else
            THROW_FMT(AssertError, "unable to add filter '%s'", strPtr(filterKey));
    }

    FUNCTION_TEST_RETURN_VOID();
}

/***********************************************************************************************************************************
Process storage protocol requests
***********************************************************************************************************************************/
bool
storageRemoteProtocol(const String *command, const VariantList *paramList, ProtocolServer *server)
{
    FUNCTION_LOG_BEGIN(logLevelDebug);
        FUNCTION_LOG_PARAM(STRING, command);
        FUNCTION_LOG_PARAM(VARIANT_LIST, paramList);
        FUNCTION_LOG_PARAM(PROTOCOL_SERVER, server);
    FUNCTION_LOG_END();

    ASSERT(command != NULL);

    // Determine which storage should be used
    const Storage *storage = strEqZ(cfgOptionStr(cfgOptType), "backup") ? storageRepo() : storagePg();
    StorageInterface interface = storageInterface(storage);
    void *driver = storageDriver(storage);

    // Attempt to satisfy the request -- we may get requests that are meant for other handlers
    bool found = true;

    MEM_CONTEXT_TEMP_BEGIN()
    {
        if (strEq(command, PROTOCOL_COMMAND_STORAGE_EXISTS_STR))
        {
            protocolServerResponse(server, VARBOOL(             // The unusual line break is to make coverage happy -- not sure why
                interface.exists(driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))))));
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_FEATURE_STR))
        {
            protocolServerResponse(server, varNewUInt64(interface.feature));
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_LIST_STR))
        {
            protocolServerResponse(
                server,
                varNewVarLst(
                    varLstNewStrLst(
                        interface.list(
                            driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varStr(varLstGet(paramList, 1))))));
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_OPEN_READ_STR))
        {
            // Create the read object
            IoRead *fileRead = storageReadIo(
                interface.newRead(
                    driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varBool(varLstGet(paramList, 1)), false));

            // Set filter group based on passed filters
            storageRemoteFilterGroup(ioReadFilterGroup(fileRead), varLstGet(paramList, 2));

            // Check if the file exists
            bool exists = ioReadOpen(fileRead);
            protocolServerResponse(server, VARBOOL(exists));

            // Transfer the file if it exists
            if (exists)
            {
                Buffer *buffer = bufNew(ioBufferSize());

                // Write file out to protocol layer
                do
                {
                    ioRead(fileRead, buffer);

                    if (bufUsed(buffer) > 0)
                    {
                        ioWriteStrLine(protocolServerIoWrite(server), strNewFmt(PROTOCOL_BLOCK_HEADER "%zu", bufUsed(buffer)));
                        ioWrite(protocolServerIoWrite(server), buffer);
                        ioWriteFlush(protocolServerIoWrite(server));

                        bufUsedZero(buffer);
                    }
                }
                while (!ioReadEof(fileRead));

                ioReadClose(fileRead);

                // Write a zero block to show file is complete
                ioWriteLine(protocolServerIoWrite(server), BUFSTRDEF(PROTOCOL_BLOCK_HEADER "0"));
                ioWriteFlush(protocolServerIoWrite(server));

                // Push filter results
                protocolServerResponse(server, ioFilterGroupResultAll(ioReadFilterGroup(fileRead)));
            }
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_OPEN_WRITE_STR))
        {
            // Create the write object
            IoWrite *fileWrite = storageWriteIo(
                interface.newWrite(
                    driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varUIntForce(varLstGet(paramList, 1)),
                    varUIntForce(varLstGet(paramList, 2)), varStr(varLstGet(paramList, 3)), varStr(varLstGet(paramList, 4)),
                    (time_t)varIntForce(varLstGet(paramList, 5)), varBool(varLstGet(paramList, 6)),
                    varBool(varLstGet(paramList, 7)), varBool(varLstGet(paramList, 8)), varBool(varLstGet(paramList, 9)), false));

            // Set filter group based on passed filters
            storageRemoteFilterGroup(ioWriteFilterGroup(fileWrite), varLstGet(paramList, 10));

            // Open file
            ioWriteOpen(fileWrite);
            protocolServerResponse(server, NULL);

            // Write data
            Buffer *buffer = bufNew(ioBufferSize());
            ssize_t remaining;

            do
            {
                // How much data is remaining to write?
                remaining = storageRemoteProtocolBlockSize(ioReadLine(protocolServerIoRead(server)));

                // Write data
                if (remaining > 0)
                {
                    size_t bytesToCopy = (size_t)remaining;

                    do
                    {
                        if (bytesToCopy < bufSize(buffer))
                            bufLimitSet(buffer, bytesToCopy);

                        bytesToCopy -= ioRead(protocolServerIoRead(server), buffer);
                        ioWrite(fileWrite, buffer);

                        bufUsedZero(buffer);
                        bufLimitClear(buffer);
                    }
                    while (bytesToCopy > 0);
                }
                // Close when all data has been written
                else if (remaining == 0)
                {
                    ioWriteClose(fileWrite);

                    // Push filter results
                    protocolServerResponse(server, ioFilterGroupResultAll(ioWriteFilterGroup(fileWrite)));
                }
                // Write was aborted so free the file
                else
                {
                    ioWriteFree(fileWrite);
                    protocolServerResponse(server, NULL);
                }
            }
            while (remaining > 0);

        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_PATH_CREATE_STR))
        {
            interface.pathCreate(
                driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varBool(varLstGet(paramList, 1)),
                varBool(varLstGet(paramList, 2)), varUIntForce(varLstGet(paramList, 3)));

            protocolServerResponse(server, NULL);
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_PATH_EXISTS_STR))
        {
            // Not all drivers implement pathExists()
            CHECK(interface.pathExists != NULL);

            protocolServerResponse(server, VARBOOL(             // The unusual line break is to make coverage happy -- not sure why
                interface.pathExists(driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))))));
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_PATH_REMOVE_STR))
        {
            protocolServerResponse(server,
                varNewBool(
                    interface.pathRemove(
                        driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varBool(varLstGet(paramList, 1)))));
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_PATH_SYNC_STR))
        {
            interface.pathSync(driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))));

            protocolServerResponse(server, NULL);
        }
        else if (strEq(command, PROTOCOL_COMMAND_STORAGE_REMOVE_STR))
        {
            interface.remove(driver, storagePathNP(storage, varStr(varLstGet(paramList, 0))), varBool(varLstGet(paramList, 1)));

            protocolServerResponse(server, NULL);
        }
        else
            found = false;
    }
    MEM_CONTEXT_TEMP_END();

    FUNCTION_LOG_RETURN(BOOL, found);
}

/***********************************************************************************************************************************
Get size of the next transfer block
***********************************************************************************************************************************/
ssize_t
storageRemoteProtocolBlockSize(const String *message)
{
    FUNCTION_LOG_BEGIN(logLevelTrace);
        FUNCTION_LOG_PARAM(STRING, message);
    FUNCTION_LOG_END();

    ASSERT(message != NULL);

    // Create block regular expression if it has not been created yet
    if (storageRemoteProtocolLocal.memContext == NULL)
    {
        MEM_CONTEXT_BEGIN(memContextTop())
        {
            MEM_CONTEXT_NEW_BEGIN("StorageRemoteFileReadLocal")
            {
                storageRemoteProtocolLocal.memContext = memContextCurrent();
                storageRemoteProtocolLocal.blockRegExp = regExpNew(BLOCK_REG_EXP_STR);
            }
            MEM_CONTEXT_NEW_END();
        }
        MEM_CONTEXT_END();
    }

    // Validate the header block size message
    if (!regExpMatch(storageRemoteProtocolLocal.blockRegExp, message))
        THROW_FMT(ProtocolError, "'%s' is not a valid block size message", strPtr(message));

    FUNCTION_LOG_RETURN(SSIZE, (ssize_t)cvtZToInt(strPtr(message) + sizeof(PROTOCOL_BLOCK_HEADER) - 1));
}
