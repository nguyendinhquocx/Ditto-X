#pragma once

// Ditto-X Share client (SPEC-2026-11-05-dittox-quick-handoff-public-clip).
//
// Nhiệm vụ: biến nội dung clipboard (text/ảnh) thành gói Markdown + PNG trong memory,
// publish lên share.quoc.app qua route Ditto-only (credential gate), trả về link public
// 12 giờ; cache/pending được persist trong SQLite qua DittoXPublicLinkCache để:
//  - reuse link cùng nội dung còn hạn (fingerprint khớp + expiresAt > now),
//  - replay finalize khi response mất sau commit (không tạo document trùng).
//
// Ranh giới bảo mật (spec):
//  - Credential chỉ đọc từ Windows Credential Manager (DittoXCredentialStore).
//  - Origin API cố định https://share.quoc.app; presigned PUT chỉ nhận URL HTTPS.
//  - Viewer URL chỉ nhận dạng relative "/clip/<token>" rồi client tự ghép origin.
//  - Không log key, presigned URL hay viewer URL.

#include <vector>
#include "../Shared/TextConvert.h"
#include "Clip.h"

// Origin cố định của Share API. Viewer URL relative được ghép vào đây.
#define DITTOX_SHARE_ORIGIN			L"https://share.quoc.app"
#define DITTOX_SHARE_HOST			L"share.quoc.app"
#define DITTOX_UPLOAD_SESSION_PATH	L"/api/dittox/clip/upload-session"
#define DITTOX_FINALIZE_PATH		L"/api/dittox/clip/finalize"

// Giới hạn phía server (khớp Worker): 2 MiB markdown, 10 MiB/ảnh, 25 MiB package.
const DWORD DITTOX_MAX_MARKDOWN_BYTES	= 2 * 1024 * 1024;
const DWORD DITTOX_MAX_ASSET_BYTES		= 10 * 1024 * 1024;
const DWORD DITTOX_MAX_PACKAGE_BYTES	= 25 * 1024 * 1024;

// Tag version của fingerprint; đổi khi công thức hash đổi để tránh reuse nhầm cache cũ.
#define DITTOX_FINGERPRINT_VERSION	"dittox-clip-fingerprint-v1"

// Một asset trong package (hiện chỉ ảnh PNG).
struct DittoXClipAsset
{
	CStringW canonicalPath;		// vd: "img/clip-0.png" — path canonical, không tuyệt đối
	CStringA contentType;		// "image/png"
	std::vector<BYTE> bytes;	// nội dung PNG trong memory
};

// Gói payload trước khi upload. Markdown là UTF-8 article.md; không chứa absolute path.
struct DittoXClipPayload
{
	CStringA markdownUtf8;
	std::vector<DittoXClipAsset> assets;
};

enum class DittoXPublishStatus
{
	Success,				// publish mới xong, URL đã nằm trong outcome
	ReusedLink,				// cache hit: link còn hạn, không upload
	CredentialMissing,		// Credential Manager không có/không đọc được key
	TooLarge,				// payload vượt giới hạn server — không gửi request
	Network,				// kết nối/response lỗi sau bounded retry
	ServerReject,			// server trả error code (rate limit, invalid manifest...)
	InvalidResponse,		// response sai shape (viewerUrl không phải /clip/...)
	FinalizeUnconfirmed,	// commit có thể đã xảy ra nhưng chưa xác nhận — pending giữ lại
	PendingReplayFailed		// pending cũ replay không thành công và không thể xác nhận
};

struct DittoXPublishOutcome
{
	DittoXPublishStatus status = DittoXPublishStatus::Network;
	CStringW viewerUrl;		// relative "/clip/<token>" khi thành công
	CStringW fullUrl;		// origin + viewerUrl (URL được copy vào clipboard)
	CStringW expiresAt;		// ISO 8601 UTC từ server
	CStringW serverError;	// error code server trả (khi có)
	CStringW detail;		// mô tả ngắn tiếng Việt cho feedback overlay
};

// Kết quả commit clipboard (chạy trên UI thread sau khi có URL).
enum class DittoXClipboardCommitResult
{
	Committed,		// URL đã nằm trong clipboard (verify xong)
	SnapshotFailed,	// không snapshot được clipboard → KHÔNG đụng clipboard hiện tại
	CommitFailed,	// set URL lỗi sau EmptyClipboard → đã restore snapshot thành công
	RestoreFailed	// restore sau lỗi cũng fail — clipboard có thể không còn nguyên
};

// Hook fault-injection cho test clipboard commit/restore (release blocker khi tái hiện).
enum class DittoXClipboardFaultMode
{
	None,				// bình thường
	FailSetUrl,			// giả lập SetClipboardData(URL) lỗi sau khi EmptyClipboard
	FailRestore			// giả lập restore lỗi sau khi commit fail (phải báo RestoreFailed)
};

void DittoXSetClipboardFaultInjection(DittoXClipboardFaultMode mode);

// --- Hash / thời gian helpers (dùng chung với cache) ---

// SHA-256 hex lowercase của buffer. Trả rỗng khi BCrypt lỗi.
CStringW DittoXSha256Hex(const BYTE* data, size_t length);
CStringW DittoXSha256Hex(const std::vector<BYTE>& bytes);

// ISO 8601 UTC hiện tại ("YYYY-MM-DDTHH:MM:SSZ").
CStringW DittoXUtcNowIso();
// Epoch milliseconds hiện tại.
long long DittoXUtcNowEpochMs();
// SYSTEMTIME UTC → epoch ms (dùng chung nội bộ + cache).
long long DittoXSystemTimeToEpochMs(const SYSTEMTIME& st);
// Parse ISO 8601 UTC ("...T...Z", millis optional) sang epoch ms; trả LLONG_MIN khi sai.
long long DittoXIsoToEpochMs(const CStringW& iso);
// Epoch ms → ISO 8601 UTC "YYYY-MM-DDTHH:MM:SSZ" (cache dùng để tính mốc pending stale).
CStringW DittoXEpochMsToIso(long long epochMs);

// Fingerprint toàn gói: version tag + NUL + article.md bytes + từng asset
// theo canonical path order: pathUTF8 + NUL + MIME + NUL + SHA-256(bytes) + NUL.
// Hai ảnh khác bytes phải cho fingerprint khác dù Markdown giống nhau.
CStringW DittoXComputeManifestHash(const DittoXClipPayload& payload);
// Hash của riêng article.md (contentHash lưu trong cache).
CStringW DittoXComputeContentHash(const DittoXClipPayload& payload);

// Dựng payload từ một clip đã LoadFormats. Ảnh (PNG/DIB) thắng text như Ctrl+X:
// ảnh được encode PNG trong memory, markdown tham chiếu "img/clip-0.png".
// Không có ảnh → Unicode/Ansi text thành article.md (UTF-8). Trả false khi không có
// payload nào lưu được; TooLarge được check trong Publish (cần detail đầy đủ).
bool DittoXBuildPayloadFromClip(CClip& clip, DittoXClipPayload& payload);

// Kiểm tra giới hạn server; trả false + detail khi vượt (không gửi request).
bool DittoXValidatePayloadLimits(const DittoXClipPayload& payload, CStringW& detailOut);

// Publish flow đầy đủ (chạy trên worker thread — KHÔNG gọi trên UI thread):
// credential → cache reuse → pending replay → session → PUT → finalize → cache save.
DittoXPublishOutcome DittoXPublish(const DittoXClipPayload& payload);

// Commit URL vào clipboard an toàn (UI thread): snapshot mọi format trước,
// EmptyClipboard chỉ khi snapshot OK, set ignore marker + CF_UNICODETEXT URL,
// verify từng format; lỗi → restore snapshot và verify từng format.
DittoXClipboardCommitResult DittoXCommitUrlToClipboard(const CStringW& url);
