// Cache/pending public clip link — bảng SQLite additive `DittoXPublicLinks`.
// Xem DittoXPublicLinkCache.h cho contract. KHÔNG lưu nội dung clip.

#include "stdafx.h"
#include "DittoXPublicLinkCache.h"
#include "DittoXShareClient.h"
#include "CP_Main.h"
#include "../Shared/TextConvert.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

namespace
{

	const int CACHE_BUSY_TIMEOUT_MS = 3000;
	// Pending quá 2 giờ coi như bỏ dở (session TTL server chỉ 30 phút).
	const long long PENDING_STALE_MS = 2LL * 60 * 60 * 1000;

	void LogCache(const CStringW& message)
	{
		Log(CString(_T("DittoXPublicLinkCache: ")) + message);
	}

	CStringW ToDbText(const CStringW& value)
	{
		// sqlite3mc_open16 dùng UTF-16; CppSQLite3 binding TCHAR xử lý việc này —
		// ở đây chỉ chuẩn hoá empty→"".
		return value;
	}
}

bool CDittoXPublicLinkCache::OpenConnection(CppSQLite3DB& db)
{
	try
	{
		CString dbPath = CGetSetOptions::GetDBPath();
		if (dbPath.IsEmpty())
		{
			LogCache(L"DB path rỗng");
			return false;
		}
		db.close();
		db.open(dbPath);
		db.setBusyTimeout(CACHE_BUSY_TIMEOUT_MS);
		return EnsureTable(db);
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
}

bool CDittoXPublicLinkCache::EnsureTable(CppSQLite3DB& db)
{
	try
	{
		db.execDML(_T("CREATE TABLE IF NOT EXISTS DittoXPublicLinks(")
			_T("id INTEGER PRIMARY KEY AUTOINCREMENT, ")
			_T("content_hash TEXT NOT NULL, ")
			_T("manifest_hash TEXT NOT NULL UNIQUE, ")
			_T("idempotency_key TEXT NOT NULL, ")
			_T("session_id TEXT, ")
			_T("state TEXT NOT NULL, ")			// 'pending' | 'published'
			_T("viewer_url TEXT, ")
			_T("expires_at TEXT, ")				// ISO 8601 UTC
			_T("created_at TEXT NOT NULL, ")
			_T("last_used_at TEXT NOT NULL);"));
		db.execDML(_T("CREATE INDEX IF NOT EXISTS DittoXPublicLinks_State ON DittoXPublicLinks(state, manifest_hash);"));
		return true;
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
}

bool CDittoXPublicLinkCache::FindReusable(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& nowIso,
	CStringW& viewerUrlOut, CStringW& expiresAtOut)
{
	try
	{
		// Compare ISO theo epoch ms (millis optional từ server) để không lệch chuỗi.
		CppSQLite3Query query = db.execQueryEx(
			_T("SELECT viewer_url, expires_at FROM DittoXPublicLinks ")
			_T("WHERE state = 'published' AND manifest_hash = '%s' AND expires_at IS NOT NULL;"),
			ToDbText(manifestHash).GetString());
		while (!query.eof())
		{
			CStringW expires((LPCWSTR)query.fieldValue(_T("expires_at")));
			CStringW viewerUrl((LPCWSTR)query.fieldValue(_T("viewer_url")));
			if (!expires.IsEmpty() && !viewerUrl.IsEmpty() &&
				DittoXIsoToEpochMs(expires) > DittoXIsoToEpochMs(nowIso))
			{
				viewerUrlOut = viewerUrl;
				expiresAtOut = expires;
				return true;
			}
			query.nextRow();
		}
		return false;
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
}

bool CDittoXPublicLinkCache::FindPending(CppSQLite3DB& db, const CStringW& manifestHash,
	CStringW& sessionIdOut, CStringW& idempotencyKeyOut)
{
	try
	{
		CppSQLite3Query query = db.execQueryEx(
			_T("SELECT session_id, idempotency_key FROM DittoXPublicLinks ")
			_T("WHERE state = 'pending' AND manifest_hash = '%s' ")
			_T("ORDER BY id DESC LIMIT 1;"),
			ToDbText(manifestHash).GetString());
		if (!query.eof())
		{
			CStringW sessionId((LPCWSTR)query.fieldValue(_T("session_id")));
			CStringW key((LPCWSTR)query.fieldValue(_T("idempotency_key")));
			if (!sessionId.IsEmpty() && !key.IsEmpty())
			{
				sessionIdOut = sessionId;
				idempotencyKeyOut = key;
				return true;
			}
		}
		return false;
	}
	CATCH_SQLITE_EXCEPTION_AND_RETURN(false)
}

void CDittoXPublicLinkCache::SavePending(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& contentHash,
	const CStringW& idempotencyKey, const CStringW& sessionId, const CStringW& nowIso)
{
	try
	{
		db.execDMLEx(_T("DELETE FROM DittoXPublicLinks WHERE manifest_hash = '%s' AND state = 'pending';"),
			ToDbText(manifestHash).GetString());
		db.execDMLEx(_T("INSERT INTO DittoXPublicLinks ")
			_T("(content_hash, manifest_hash, idempotency_key, session_id, state, viewer_url, expires_at, created_at, last_used_at) ")
			_T("VALUES ('%s', '%s', '%s', '%s', 'pending', NULL, NULL, '%s', '%s');"),
			ToDbText(contentHash).GetString(),
			ToDbText(manifestHash).GetString(),
			ToDbText(idempotencyKey).GetString(),
			ToDbText(sessionId).GetString(),
			nowIso.GetString(),
			nowIso.GetString());
	}
	CATCH_SQLITE_EXCEPTION
}

void CDittoXPublicLinkCache::MarkPublished(CppSQLite3DB& db, const CStringW& idempotencyKey,
	const CStringW& viewerUrl, const CStringW& expiresAt, const CStringW& nowIso)
{
	try
	{
		db.execDMLEx(_T("UPDATE DittoXPublicLinks ")
			_T("SET state = 'published', viewer_url = '%s', expires_at = '%s', last_used_at = '%s' ")
			_T("WHERE idempotency_key = '%s' AND state = 'pending';"),
			viewerUrl.GetString(),
			expiresAt.GetString(),
			nowIso.GetString(),
			idempotencyKey.GetString());
	}
	CATCH_SQLITE_EXCEPTION
}

void CDittoXPublicLinkCache::RemovePending(CppSQLite3DB& db, const CStringW& idempotencyKey)
{
	try
	{
		db.execDMLEx(_T("DELETE FROM DittoXPublicLinks WHERE idempotency_key = '%s' AND state = 'pending';"),
			idempotencyKey.GetString());
	}
	CATCH_SQLITE_EXCEPTION
}

void CDittoXPublicLinkCache::Touch(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& nowIso)
{
	try
	{
		db.execDMLEx(_T("UPDATE DittoXPublicLinks SET last_used_at = '%s' ")
			_T("WHERE manifest_hash = '%s' AND state = 'published';"),
			nowIso.GetString(),
			manifestHash.GetString());
	}
	CATCH_SQLITE_EXCEPTION
}

void CDittoXPublicLinkCache::PruneExpired(CppSQLite3DB& db, const CStringW& nowIso)
{
	try
	{
		// So sánh ISO string đủ đúng cho cùng format UTC (server + local đều 'Z' suffix);
		// mốc pending dùng epoch để tính 2 giờ.
		long long nowMs = DittoXIsoToEpochMs(nowIso);
		CStringW staleIso = DittoXEpochMsToIso(nowMs - PENDING_STALE_MS);
		db.execDMLEx(_T("DELETE FROM DittoXPublicLinks WHERE state = 'published' AND expires_at <= '%s';"),
			nowIso.GetString());
		db.execDMLEx(_T("DELETE FROM DittoXPublicLinks WHERE state = 'pending' AND created_at <= '%s';"),
			staleIso.GetString());
	}
	CATCH_SQLITE_EXCEPTION
}
