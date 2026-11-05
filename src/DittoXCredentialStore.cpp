// Đọc Ditto credential từ Windows Credential Manager (CredReadW, CRED_TYPE_GENERIC).
// Blob do tools/dittox-share-credential-setup.ps1 ghi dưới dạng UTF-8/ASCII bytes.
// Không có plaintext fallback — thiếu entry / lỗi API đều trả false.

#include "stdafx.h"
#include <wincred.h>
#include "DittoXCredentialStore.h"

#pragma comment(lib, "advapi32.lib")

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

bool CDittoXCredentialStore::ReadCredential(CStringW& keyOut)
{
	keyOut.Empty();

	CREDENTIALW* credential = NULL;
	if (!::CredReadW(DITTOX_CRED_TARGET_NAME, CRED_TYPE_GENERIC, 0, &credential))
	{
		// ERROR_NOT_FOUND = chưa cấu hình; các lỗi khác = bị chặn/đọc hỏng. Cùng một kết luận.
		return false;
	}

	bool ok = false;
	if (credential->CredentialBlob && credential->CredentialBlobSize > 0)
	{
		// Blob là bytes UTF-8/ASCII của key (script ghi không BOM, không CRLF).
		int wideLength = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
			reinterpret_cast<LPCCH>(credential->CredentialBlob),
			static_cast<int>(credential->CredentialBlobSize), NULL, 0);
		if (wideLength > 0)
		{
			wchar_t* buffer = keyOut.GetBuffer(wideLength);
			if (buffer)
			{
				::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
					reinterpret_cast<LPCCH>(credential->CredentialBlob),
					static_cast<int>(credential->CredentialBlobSize), buffer, wideLength);
				keyOut.ReleaseBuffer(wideLength);
				ok = true;
			}
		}
	}

	::CredFree(credential);
	return ok;
}

bool CDittoXCredentialStore::HasCredential()
{
	CREDENTIALW* credential = NULL;
	bool exists = ::CredReadW(DITTOX_CRED_TARGET_NAME, CRED_TYPE_GENERIC, 0, &credential) != 0;
	if (exists && credential)
	{
		::CredFree(credential);
	}
	return exists;
}
