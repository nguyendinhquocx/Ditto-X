#pragma once

// Cache/pending public clip link cho Ditto-X (SPEC-2026-11-05).
//
// Bảng SQLite `DittoXPublicLinks` (additive, không rewrite clip data hiện có):
//  - KHÔNG lưu nội dung clip. Chỉ giữ fingerprint (content + manifest), idempotency key,
//    sessionId khi pending, và URL/expiry khi published.
//  - Reuse: manifest_hash khớp + state 'published' + expires_at còn hạn → copy URL.
//  - Pending resume: lưu key/sessionId TRƯỚC presigned PUT; crash giữa chừng thì lần
//    publish sau replay finalize với đúng cặp sessionId/key.
//  - Prune: published hết hạn + pending quá cũ (abandoned) bị xoá.
//
// Thread: mọi method có thể chạy trên worker thread qua connection riêng
// (OpenConnection); KHÔNG dùng theApp.m_db từ worker.

#include "sqlite\CppSQLite3.h"

class CDittoXPublicLinkCache
{
public:
	// Mở connection mới tới DB Ditto (đọc path từ options, busy timeout như app).
	// Đảm bảo bảng tồn tại trước khi trả. false = không mở được.
	static bool OpenConnection(CppSQLite3DB& db);

	// CREATE TABLE IF NOT EXISTS — chạy được cả trên connection app lẫn worker.
	static bool EnsureTable(CppSQLite3DB& db);

	// Cache hit: state 'published', manifest_hash khớp, expires_at > now.
	// Trả viewerUrl (relative /clip/...) + expiresAt (ISO) khi hit.
	static bool FindReusable(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& nowIso,
		CStringW& viewerUrlOut, CStringW& expiresAtOut);

	// Pending record cùng manifest fingerprint (session/key đã persist trước PUT).
	static bool FindPending(CppSQLite3DB& db, const CStringW& manifestHash,
		CStringW& sessionIdOut, CStringW& idempotencyKeyOut);

	// Ghi pending record (xoá pending cũ cùng manifest hash trước khi ghi).
	static bool SavePending(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& contentHash,
		const CStringW& idempotencyKey, const CStringW& sessionId, const CStringW& nowIso);

	// Chuyển pending → published (lưu viewerUrl relative + expiresAt ISO từ server).
	static void MarkPublished(CppSQLite3DB& db, const CStringW& idempotencyKey,
		const CStringW& viewerUrl, const CStringW& expiresAt, const CStringW& nowIso);

	// Pending thất bại chắc chắn (chưa commit) → xoá để lần sau upload lại từ đầu.
	static void RemovePending(CppSQLite3DB& db, const CStringW& idempotencyKey);

	// Ghi nhận last_used_at cho cache hit (không đổi expiry — reuse tới đúng hạn).
	static void Touch(CppSQLite3DB& db, const CStringW& manifestHash, const CStringW& nowIso);

	// Dọn entry hết hạn: published có expires_at <= now; pending created_at cũ hơn
	// 2 giờ (session TTL 30 phút — quá đó coi như bỏ dở) .
	static void PruneExpired(CppSQLite3DB& db, const CStringW& nowIso);
};
