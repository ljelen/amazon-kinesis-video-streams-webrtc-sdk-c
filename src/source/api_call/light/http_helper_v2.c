/*
 * Copyright 2021 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <inttypes.h>
#include <stddef.h>

/* Thirdparty headers */
#include "azure_c_shared_utility/buffer_.h"
#include "azure_c_shared_utility/httpheaders.h"
#include "azure_c_shared_utility/strings.h"
#include "azure_c_shared_utility/xlogging.h"
#include "llhttp.h"

/* Public headers */
#include "kvs/error.h"
#include "kvs/common_defs.h"

/* Internal headers */
//#include "allocator.h"
#include "http_helper_v2.h"
#include "netio.h"

#define DEFAULT_HTTP_RECV_BUFSIZE 2048

typedef struct {
    llhttp_settings_t xSettings;
    const char* pBodyLoc;
    size_t uBodyLen;
} llhttp_settings_ex_t;

static STRING_HANDLE prvGenerateHttpReq(const char* pcHttpMethod, const char* pcUri, HTTP_HEADERS_HANDLE xHttpReqHeaders, const char* pcBody)
{
    int xRes = STATUS_SUCCESS;
    STRING_HANDLE xStHttpReq = NULL;
    size_t uHeadersCnt = 0;
    size_t i = 0;
    char* pcHeader = NULL;

    if (HTTPHeaders_GetHeaderCount(xHttpReqHeaders, &uHeadersCnt) != HTTP_HEADERS_OK) {
        xRes = STATUS_NULL_ARG;
    } else if ((xStHttpReq = STRING_new()) == NULL || STRING_sprintf(xStHttpReq, "%s %s HTTP/1.1\r\n", pcHttpMethod, pcUri) != 0) {
        xRes = STATUS_NULL_ARG;
    } else {
        for (i = 0; i < uHeadersCnt && xRes == STATUS_SUCCESS; i++) {
            if (HTTPHeaders_GetHeader(xHttpReqHeaders, i, &pcHeader) != HTTP_HEADERS_OK) {
                xRes = STATUS_NULL_ARG;
            } else {
                if (STRING_sprintf(xStHttpReq, "%s\r\n", pcHeader) != 0) {
                    xRes = STATUS_NULL_ARG;
                }
                /* pcHeader was created by HTTPHeaders_GetHeader via malloc */
                free(pcHeader);
            }
        }

        if (xRes == STATUS_SUCCESS) {
            if (STRING_sprintf(xStHttpReq, "\r\n") != 0) {
                xRes = STATUS_NULL_ARG;
            } else if (strlen(pcBody) > 0 && STRING_sprintf(xStHttpReq, "%s", pcBody) != 0) {
                xRes = STATUS_NULL_ARG;
            } else {
                /* nop */
            }
        }
    }

    if (xRes != STATUS_SUCCESS) {
        STRING_delete(xStHttpReq);
        xStHttpReq = NULL;
    }

    return xStHttpReq;
}

static int prvHandleHttpOnBodyComplete(llhttp_t* pHttpParser, const char* at, size_t length)
{
    llhttp_settings_ex_t* pxSettings = (llhttp_settings_ex_t*) (pHttpParser->settings);
    pxSettings->pBodyLoc = at;
    pxSettings->uBodyLen = length;
    return 0;
}

static enum llhttp_errno prvParseHttpResponse(const char* pBuf, size_t uLen, unsigned int* puStatusCode, const char** ppBodyLoc, size_t* puBodyLen)
{
    llhttp_t xHttpParser = {0};
    llhttp_settings_ex_t xSettings = {0};
    enum llhttp_errno xHttpErrno = HPE_OK;

    llhttp_settings_init(&(xSettings.xSettings));
    xSettings.xSettings.on_body = prvHandleHttpOnBodyComplete;
    llhttp_init(&xHttpParser, HTTP_RESPONSE, (llhttp_settings_t*) &xSettings);

    xHttpErrno = llhttp_execute(&xHttpParser, pBuf, (size_t) uLen);
    if (xHttpErrno == HPE_OK) {
        if (puStatusCode != NULL) {
            *puStatusCode = xHttpParser.status_code;
        }
        if (ppBodyLoc != NULL && puBodyLen != NULL) {
            *ppBodyLoc = xSettings.pBodyLoc;
            *puBodyLen = xSettings.uBodyLen;
        }
    }

    return xHttpErrno;
}

int Http_executeHttpReq(NetIoHandle xNetIoHandle, const char* pcHttpMethod, const char* pcUri, HTTP_HEADERS_HANDLE xHttpReqHeaders,
                        const char* pcBody)
{
    int xRes = STATUS_SUCCESS;
    STRING_HANDLE xStHttpReq = NULL;

    if (xNetIoHandle == NULL || pcHttpMethod == NULL || pcUri == NULL || xHttpReqHeaders == NULL || pcBody == NULL) {
        xRes = STATUS_NULL_ARG;
    } else if ((xStHttpReq = prvGenerateHttpReq(pcHttpMethod, pcUri, xHttpReqHeaders, pcBody)) == NULL) {
        xRes = STATUS_NULL_ARG;
    } else if (NetIo_send(xNetIoHandle, (const unsigned char*) STRING_c_str(xStHttpReq), STRING_length(xStHttpReq)) != STATUS_SUCCESS) {
        xRes = STATUS_NULL_ARG;
    } else {
        /* nop */
    }

    STRING_delete(xStHttpReq);

    return xRes;
}

int Http_recvHttpRsp(NetIoHandle xNetIoHandle, unsigned int* puHttpStatus, char** ppRspBody, size_t* puRspBodyLen)
{
    int xRes = STATUS_SUCCESS;
    BUFFER_HANDLE xBufRecv = NULL;
    size_t uBytesReceived = 0;
    size_t uBytesTotalReceived = 0;
    unsigned int uHttpStatusCode = 0;
    const char* pBodyLoc = NULL;
    size_t uBodyLen = 0;
    char* pRspBody = NULL;

    if (xNetIoHandle == NULL || puHttpStatus == NULL || ppRspBody == NULL || puRspBodyLen == NULL) {
        xRes = STATUS_NULL_ARG;
    } else if ((xBufRecv = BUFFER_create_with_size(DEFAULT_HTTP_RECV_BUFSIZE)) == NULL) {
        xRes = STATUS_NULL_ARG;
    } else {
        do {
            /* TODO: Add timeout checking here */

            if (uBytesTotalReceived == BUFFER_length(xBufRecv)) {
                /* If buffer is full, then we double the size of buffer. */
                if (BUFFER_enlarge(xBufRecv, uBytesTotalReceived) != 0) {
                    LogError("OOM: xBufRecv");
                    xRes = STATUS_NULL_ARG;
                    break;
                }
            }
            if (NetIo_recv(xNetIoHandle, BUFFER_u_char(xBufRecv) + uBytesTotalReceived, BUFFER_length(xBufRecv) - uBytesTotalReceived,
                           &uBytesReceived) != STATUS_SUCCESS ||
                uBytesReceived == 0) {
                xRes = STATUS_NULL_ARG;
                break;
            } else {
                uBytesTotalReceived += uBytesReceived;
                if (prvParseHttpResponse((const char*) BUFFER_u_char(xBufRecv), uBytesTotalReceived, &uHttpStatusCode, &pBodyLoc, &uBodyLen) !=
                    HPE_OK) {
                    xRes = STATUS_NULL_ARG;
                }
                /* If it's 100-continue, then we need to discard previous result and do it again. */
                else if (uHttpStatusCode / 100 == 1) {
                    LogInfo("100-continue");
                    uBytesTotalReceived = 0;
                    xRes = STATUS_NULL_ARG;
                } else {
                    xRes = STATUS_SUCCESS;
                    *puHttpStatus = uHttpStatusCode;
                    if (uHttpStatusCode == 200) {
                        if ((pRspBody = (char*) MEMALLOC(uBodyLen + 1)) == NULL) {
                            LogError("OOM: ppBodyLoc");
                            xRes = STATUS_SUCCESS;
                            break;
                        }
                        memcpy(pRspBody, pBodyLoc, uBodyLen);
                        pRspBody[uBodyLen] = '\0';
                        *ppRspBody = pRspBody;
                        *puRspBodyLen = uBodyLen;
                    }
                }
            }
        } while (xRes != STATUS_SUCCESS);
    }

    BUFFER_delete(xBufRecv);

    return xRes;
}