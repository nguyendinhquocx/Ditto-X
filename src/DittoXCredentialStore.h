#pragma once

// Ditto-X Share credential (SPEC-2026-11-05): giá trị chỉ sống trong Windows Credential
// Manager; KHÔNG có plaintext fallback trong source/config/log/build. Thiếu key = Ctrl+Z
// từ chối publish TRƯỚC khi tạo request nào (không fallback sang route anonymous).

// Target name trong Credential Manager (script tools/dittox-share-credential-setup.ps1 dùng cùng tên).
#define DITTOX_CRED_TARGET_NAME		L"Ditto-X Share API Credential"

class CDittoXCredentialStore
{
public:
	// true + keyOut khi đọc được credential; false khi thiếu/bị chặn. Không log giá trị.
	static bool ReadCredential(CStringW& keyOut);

	// true khi entry tồn tại (phân biệt "chưa cấu hình" với "lỗi đọc").
	static bool HasCredential();
};
