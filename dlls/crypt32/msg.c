/*
 * Copyright 2007 Juan Lang
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */
#include <stdarg.h>
#include "windef.h"
#include "winbase.h"
#include "wincrypt.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(crypt);

/* Called when a message's ref count reaches zero.  Free any message-specific
 * data here.
 */
typedef void (*CryptMsgCloseFunc)(HCRYPTMSG msg);

typedef BOOL (*CryptMsgGetParamFunc)(HCRYPTMSG hCryptMsg, DWORD dwParamType,
 DWORD dwIndex, void *pvData, DWORD *pcbData);

typedef BOOL (*CryptMsgUpdateFunc)(HCRYPTMSG hCryptMsg, const BYTE *pbData,
 DWORD cbData, BOOL fFinal);

typedef struct _CryptMsgBase
{
    LONG                 ref;
    DWORD                open_flags;
    CryptMsgCloseFunc    close;
    CryptMsgUpdateFunc   update;
    CryptMsgGetParamFunc get_param;
} CryptMsgBase;

static inline void CryptMsgBase_Init(CryptMsgBase *msg, DWORD dwFlags)
{
    msg->ref = 1;
    msg->open_flags = dwFlags;
}

typedef struct _CDataEncodeMsg
{
    CryptMsgBase base;
} CDataEncodeMsg;

static HCRYPTMSG CDataEncodeMsg_Open(DWORD dwFlags, const void *pvMsgEncodeInfo,
 LPSTR pszInnerContentObjID, PCMSG_STREAM_INFO pStreamInfo)
{
    CDataEncodeMsg *msg;

    if (pvMsgEncodeInfo)
    {
        SetLastError(E_INVALIDARG);
        return NULL;
    }
    FIXME("semi-stub\n");
    msg = CryptMemAlloc(sizeof(CDataEncodeMsg));
    if (msg)
    {
        CryptMsgBase_Init((CryptMsgBase *)msg, dwFlags);
    }
    return (HCRYPTMSG)msg;
}

static inline const char *MSG_TYPE_STR(DWORD type)
{
    switch (type)
    {
#define _x(x) case (x): return #x
        _x(CMSG_DATA);
        _x(CMSG_SIGNED);
        _x(CMSG_ENVELOPED);
        _x(CMSG_SIGNED_AND_ENVELOPED);
        _x(CMSG_HASHED);
        _x(CMSG_ENCRYPTED);
#undef _x
        default:
            return wine_dbg_sprintf("unknown (%d)", type);
    }
}

HCRYPTMSG WINAPI CryptMsgOpenToEncode(DWORD dwMsgEncodingType, DWORD dwFlags,
 DWORD dwMsgType, const void *pvMsgEncodeInfo, LPSTR pszInnerContentObjID,
 PCMSG_STREAM_INFO pStreamInfo)
{
    HCRYPTMSG msg = NULL;

    TRACE("(%08x, %08x, %08x, %p, %s, %p)\n", dwMsgEncodingType, dwFlags,
     dwMsgType, pvMsgEncodeInfo, debugstr_a(pszInnerContentObjID), pStreamInfo);

    if (GET_CMSG_ENCODING_TYPE(dwMsgEncodingType) != PKCS_7_ASN_ENCODING)
    {
        SetLastError(E_INVALIDARG);
        return NULL;
    }
    switch (dwMsgType)
    {
    case CMSG_DATA:
        msg = CDataEncodeMsg_Open(dwFlags, pvMsgEncodeInfo,
         pszInnerContentObjID, pStreamInfo);
        break;
    case CMSG_SIGNED:
    case CMSG_ENVELOPED:
    case CMSG_HASHED:
        FIXME("unimplemented for type %s\n", MSG_TYPE_STR(dwMsgType));
        break;
    case CMSG_SIGNED_AND_ENVELOPED:
    case CMSG_ENCRYPTED:
        /* defined but invalid, fall through */
    default:
        SetLastError(CRYPT_E_INVALID_MSG_TYPE);
    }
    return msg;
}

HCRYPTMSG WINAPI CryptMsgOpenToDecode(DWORD dwMsgEncodingType, DWORD dwFlags,
 DWORD dwMsgType, HCRYPTPROV hCryptProv, PCERT_INFO pRecipientInfo,
 PCMSG_STREAM_INFO pStreamInfo)
{
    FIXME("(%08x, %08x, %08x, %08lx, %p, %p): stub\n", dwMsgEncodingType,
     dwFlags, dwMsgType, hCryptProv, pRecipientInfo, pStreamInfo);

    if (GET_CMSG_ENCODING_TYPE(dwMsgEncodingType) != PKCS_7_ASN_ENCODING)
    {
        SetLastError(E_INVALIDARG);
        return NULL;
    }
    return NULL;
}

HCRYPTMSG WINAPI CryptMsgDuplicate(HCRYPTMSG hCryptMsg)
{
    TRACE("(%p)\n", hCryptMsg);

    if (hCryptMsg)
    {
        CryptMsgBase *msg = (CryptMsgBase *)hCryptMsg;

        InterlockedIncrement(&msg->ref);
    }
    return hCryptMsg;
}

BOOL WINAPI CryptMsgClose(HCRYPTMSG hCryptMsg)
{
    TRACE("(%p)\n", hCryptMsg);

    if (hCryptMsg)
    {
        CryptMsgBase *msg = (CryptMsgBase *)hCryptMsg;

        if (InterlockedDecrement(&msg->ref) == 0)
        {
            TRACE("freeing %p\n", msg);
            if (msg->close)
                msg->close(msg);
            CryptMemFree(msg);
        }
    }
    return TRUE;
}

BOOL WINAPI CryptMsgUpdate(HCRYPTMSG hCryptMsg, const BYTE *pbData,
 DWORD cbData, BOOL fFinal)
{
    CryptMsgBase *msg = (CryptMsgBase *)hCryptMsg;
    BOOL ret = FALSE;

    TRACE("(%p, %p, %d, %d)\n", hCryptMsg, pbData, cbData, fFinal);
    if (msg && msg->update)
        ret = msg->update(hCryptMsg, pbData, cbData, fFinal);
    return ret;
}

BOOL WINAPI CryptMsgGetParam(HCRYPTMSG hCryptMsg, DWORD dwParamType,
 DWORD dwIndex, void *pvData, DWORD *pcbData)
{
    CryptMsgBase *msg = (CryptMsgBase *)hCryptMsg;
    BOOL ret = FALSE;

    TRACE("(%p, %d, %d, %p, %p)\n", hCryptMsg, dwParamType, dwIndex,
     pvData, pcbData);
    if (msg && msg->get_param)
        ret = msg->get_param(hCryptMsg, dwParamType, dwIndex, pvData, pcbData);
    return ret;
}
