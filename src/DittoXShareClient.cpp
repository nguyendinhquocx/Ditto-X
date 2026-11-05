// Ditto-X Share client (SPEC-2026-11-05). Xem DittoXShareClient.h cho contract đầy đủ.
//
// Retry contract (bounded, khớp reference client readerx/Side Browser):
//  - upload-session: network/5xx → retry đúng 1 lần CÙNG key/manifest (idempotent replay).
//  - PUT presigned: network/5xx → retry cùng URL 1 lần; 403 (URL hết hạn) → reissue
//    session cùng key/manifest rồi PUT lại, tối đa MỘT lần reissue mỗi phiên publish.
//  - finalize: tối đa 3 lần POST (response loss + 5xx retry); session_in_progress →
//    chờ 1.5s hỏi lại đúng 1 lần; idempotency_conflict → fail-fast.
//  - finalize ambiguous (hết retry network/5xx) → giữ pending record, báo
//    FinalizeUnconfirmed ("link có thể đã tạo"); lần Ctrl+Z sau replay TRƯỚC khi upload mới.
//
// Protocol (khớp Worker):
//  POST /api/dittox/clip/upload-session  { markdown:{canonicalPath,size}, assets:[...], idempotencyKey }
//    → { sessionId, expiresAt, markdown:{presignedUrl,...}, assets:[{presignedUrl,...}] }
//  PUT presigned URL (HTTPS duy nhất, Content-Type khớp manifest) từng object
//  POST /api/dittox/clip/finalize { sessionId, idempotencyKey }
//    → { viewerUrl: "/clip/<token>", expiresAt }
//
// Threading: DittoXPublish CHỈ chạy trên worker thread. Cache/pending dùng connection
// SQLite riêng của thread (SQLite nhiều connection + busy timeout), KHÔNG đụng
// theApp.m_db của UI thread.

#include "stdafx.h"
#include <winhttp.h>
#include <bcrypt.h>
#include "DittoXShareClient.h"
#include "DittoXCredentialStore.h"
#include "DittoXPublicLinkCache.h"
#include "ImageHelper.h"
#include "CP_Main.h"

#ifdef _DEBUG
#define new DEBUG_NEW
#endif

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")

// Linker: định nghĩa trong ClipEditThread.cpp (encode CImage → PNG bytes trong memory).
std::vector<BYTE> CImageToPNGBytes(const CImage& image, REFGUID guidFileType);

namespace
{

	const wchar_t* const USER_AGENT = L"Ditto-X/1.0";
	const int MAX_RESPONSE_JSON_BYTES = 256 * 1024;
	const int CLIPBOARD_OPEN_ATTEMPTS = 12;

	DittoXClipboardFaultMode g_clipboardFaultMode = DittoXClipboardFaultMode::None;

	// --- RAII cho WinHTTP handles ---
	struct HttpHandle
	{
		HINTERNET h = NULL;
		~HttpHandle() { if (h) WinHttpCloseHandle(h); }
	};

	void LogClient(const CStringW& message)
	{
		// Không bao giờ log key/presigned URL/viewer URL — chỉ event + lỗi đời thường.
		Log(CString(_T("DittoXShareClient: ")) + message);
	}

	// --- SHA-256 qua BCrypt ---
	class BCryptHasher
	{
	public:
		explicit BCryptHasher(bool& ok)
		{
			ok = false;
			if (BCryptOpenAlgorithmProvider(&m_hAlg, BCRYPT_SHA256_ALGORITHM, NULL, 0) != 0) return;
			if (BCryptCreateHash(m_hAlg, &m_hHash, NULL, 0, NULL, 0, 0) != 0) return;
			ok = true;
		}
		~BCryptHasher()
		{
			if (m_hHash) BCryptDestroyHash(m_hHash);
			if (m_hAlg) BCryptCloseAlgorithmProvider(m_hAlg, 0);
		}
		bool Update(const BYTE* data, size_t length)
		{
			// Nạp theo chunk để tránh giới hạn ULONG trên buffer lớn.
			const size_t chunk = 1 << 20;
			size_t offset = 0;
			while (offset < length)
			{
				size_t take = (chunk < length - offset) ? chunk : (length - offset);
				if (BCryptHashData(m_hHash, const_cast<PUCHAR>(data + offset), static_cast<ULONG>(take), 0) != 0)
				{
					return false;
				}
				offset += take;
			}
			return true;
		}
		bool Finish(CStringW& hexOut)
		{
			BYTE digest[32];
			if (BCryptFinishHash(m_hHash, digest, sizeof(digest), 0) != 0) return false;
			hexOut.Empty();
			static const wchar_t* hexDigits = L"0123456789abcdef";
			for (int i = 0; i < 32; i++)
			{
				hexOut += hexDigits[(digest[i] >> 4) & 0xF];
				hexOut += hexDigits[digest[i] & 0xF];
			}
			return true;
		}
	private:
		BCRYPT_ALG_HANDLE m_hAlg = NULL;
		BCRYPT_HASH_HANDLE m_hHash = NULL;
	};

	// --- Thời gian ---

	long long SystemTimeToEpochMs(const SYSTEMTIME& st)
	{
		// Công thức ngày Julius — không phụ thuộc CRT locale.
		int a = (14 - st.wMonth) / 12;
		int y = st.wYear + 4800 - a;
		int m = st.wMonth + 12 * a - 3;
		long long jdn = st.wDay + (153 * m + 2) / 5 + 365LL * y + y / 4 - y / 100 + y / 400 - 32045;
		long long daysSinceEpoch = jdn - 2440588;
		return daysSinceEpoch * 86400000LL
			+ st.wHour * 3600000LL
			+ st.wMinute * 60000LL
			+ st.wSecond * 1000LL
			+ st.wMilliseconds;
	}

	CStringA WideToUtf8(const CStringW& wide)
	{
		return CTextConvert::UnicodeToUTF8(CString(wide));
	}

	CStringW Utf8ToWide(const CStringA& utf8)
	{
		return CTextConvert::Utf8ToUnicode(utf8);
	}

	// --- JSON tối giản: chỉ đủ cho response của Worker mình (không parser ngoài) ---

	void JsonEscape(const CStringW& input, CStringA& out)
	{
		CStringA utf8 = WideToUtf8(input);
		out.Empty();
		for (int i = 0; i < utf8.GetLength(); i++)
		{
			unsigned char ch = static_cast<unsigned char>(utf8[i]);
			switch (ch)
			{
			case '"': out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if (ch < 0x20)
				{
					char buf[8];
					sprintf_s(buf, "\\u%04x", ch);
					out += buf;
				}
				else
				{
					out += static_cast<char>(ch);
				}
			}
		}
	}

	// Tìm vị trí giá trị của "key" trong JSON (đủ dùng cho response shape cố định).
	int JsonValuePos(const CStringA& json, const char* key)
	{
		CStringA needle;
		needle.Format("\"%s\"", key);
		int pos = json.Find(needle);
		if (pos < 0) return -1;
		int cursor = pos + needle.GetLength();
		while (cursor < json.GetLength() && (json[cursor] == ' ' || json[cursor] == ':'))
		{
			cursor++;
		}
		return cursor < json.GetLength() ? cursor : -1;
	}

	// Đọc chuỗi JSON (có escape cơ bản) tại pos; trả "" khi không phải chuỗi hợp lệ.
	CStringA JsonStringAt(const CStringA& json, int pos)
	{
		if (pos < 0 || pos >= json.GetLength() || json[pos] != '"') return "";
		CStringA out;
		for (int i = pos + 1; i < json.GetLength(); i++)
		{
			char ch = json[i];
			if (ch == '"') return out;
			if (ch == '\\' && i + 1 < json.GetLength())
			{
				i++;
				char esc = json[i];
				switch (esc)
				{
				case '"': out += '"'; break;
				case '\\': out += '\\'; break;
				case '/': out += '/'; break;
				case 'n': out += '\n'; break;
				case 'r': out += '\r'; break;
				case 't': out += '\t'; break;
				case 'b': out += '\b'; break;
				case 'f': out += '\f'; break;
				case 'u':
					if (i + 4 < json.GetLength())
					{
						wchar_t wide = static_cast<wchar_t>(strtol(json.Mid(i + 1, 4), NULL, 16));
						out += WideToUtf8(CStringW(wide));
						i += 4;
					}
					break;
				default: out += esc; break;
				}
				continue;
			}
			out += ch;
		}
		return ""; // không đóng quote — response hỏng
	}

	// Lấy substring cân bằng (object {} hoặc array []) bắt đầu tại pos.
	CStringA JsonBalancedAt(const CStringA& json, int pos)
	{
		if (pos < 0 || pos >= json.GetLength()) return "";
		char openCh = json[pos];
		char closeCh = openCh == '{' ? '}' : (openCh == '[' ? ']' : 0);
		if (!closeCh) return "";
		int depth = 0;
		bool inString = false;
		for (int i = pos; i < json.GetLength(); i++)
		{
			char ch = json[i];
			if (inString)
			{
				if (ch == '\\') i++;
				else if (ch == '"') inString = false;
				continue;
			}
			if (ch == '"') { inString = true; continue; }
			if (ch == openCh) depth++;
			else if (ch == closeCh)
			{
				depth--;
				if (depth == 0) return json.Mid(pos, i - pos + 1);
			}
		}
		return "";
	}

	// Tách các object top-level của một array JSON.
	std::vector<CStringA> JsonArrayObjects(const CStringA& arrayJson)
	{
		std::vector<CStringA> items;
		int depth = 0;
		int start = -1;
		bool inString = false;
		for (int i = 0; i < arrayJson.GetLength(); i++)
		{
			char ch = arrayJson[i];
			if (inString)
			{
				if (ch == '\\') i++;
				else if (ch == '"') inString = false;
				continue;
			}
			if (ch == '"') { inString = true; continue; }
			if (ch == '{')
			{
				if (depth == 0) start = i;
				depth++;
			}
			else if (ch == '}')
			{
				depth--;
				if (depth == 0 && start >= 0)
				{
					items.push_back(arrayJson.Mid(start, i - start + 1));
					start = -1;
				}
			}
		}
		return items;
	}

	CStringW NewIdempotencyKey()
	{
		GUID guid;
		if (FAILED(CoCreateGuid(&guid)))
		{
			return L"";
		}
		wchar_t buffer[64] = {};
		StringFromGUID2(guid, buffer, 64);
		CStringW key(buffer);
		key.Trim(L"{}");
		return key;
	}

	// Credential phải là ASCII in được (đi vào Authorization header) — loại key
	// chứa control char thay vì gửi request hỏng.
	bool SanitizeCredential(const CStringW& raw, CStringA& keyOut)
	{
		CStringW trimmed(raw);
		trimmed.Trim();
		if (trimmed.IsEmpty()) return false;
		for (int i = 0; i < trimmed.GetLength(); i++)
		{
			wchar_t ch = trimmed[i];
			if (ch < 0x20 || ch > 0x7E) return false;
		}
		keyOut = WideToUtf8(trimmed);
		return !keyOut.IsEmpty();
	}

	// --- HTTP core (TLS mặc định: WinHTTP validate certificate, không ignore) ---

	struct HttpRequestResult
	{
		bool networkOk = false;		// nhận được status line (kể cả 4xx/5xx)
		DWORD statusCode = 0;
		CStringA body;
	};

	// POST JSON tới API origin (host cố định, HTTPS bắt buộc).
	HttpRequestResult DittoXHttpPost(const CStringW& path, const CStringA& body, const CStringA& bearerKey)
	{
		HttpRequestResult result;

		HttpHandle session;
		session.h = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session.h) return result;
		WinHttpSetTimeouts(session.h, 10000, 15000, 60000, 60000);

		HttpHandle connect;
		connect.h = WinHttpConnect(session.h, DITTOX_SHARE_HOST, INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!connect.h) return result;

		HttpHandle request;
		request.h = WinHttpOpenRequest(connect.h, L"POST", path, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (!request.h) return result;

		CStringW authHeader;
		if (!bearerKey.IsEmpty())
		{
			authHeader = CStringW(L"Authorization: Bearer ") + Utf8ToWide(bearerKey);
		}

		if (!WinHttpSendRequest(request.h,
			authHeader.IsEmpty() ? WINHTTP_NO_ADDITIONAL_HEADERS : authHeader.GetString(),
			static_cast<DWORD>(authHeader.GetLength()),
			const_cast<LPSTR>(body.GetString()), static_cast<DWORD>(body.GetLength()),
			static_cast<DWORD>(body.GetLength()), 0))
		{
			return result;
		}
		if (!WinHttpReceiveResponse(request.h, NULL))
		{
			return result;
		}

		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
		result.statusCode = statusCode;

		char chunk[16 * 1024];
		DWORD total = 0;
		for (;;)
		{
			DWORD available = 0;
			if (!WinHttpQueryDataAvailable(request.h, &available)) return result;
			if (available == 0) break;
			if (total + available > static_cast<DWORD>(MAX_RESPONSE_JSON_BYTES))
			{
				LogClient(L"response JSON quá lớn — bỏ");
				return result;
			}
			DWORD toRead = (available < static_cast<DWORD>(sizeof(chunk))) ? available : static_cast<DWORD>(sizeof(chunk));
			DWORD readBytes = 0;
			if (!WinHttpReadData(request.h, chunk, toRead, &readBytes)) return result;
			result.body.Append(chunk, static_cast<int>(readBytes));
			total += readBytes;
		}

		result.networkOk = true;
		return result;
	}

	// PUT presigned URL — chỉ chấp nhận HTTPS absolute URL; Content-Type phải
	// khớp đúng contentType trong manifest (chữ ký presign sẽ mismatch nếu khác).
	bool DittoXHttpPutPresigned(const CStringW& presignedUrl, const std::vector<BYTE>& bytes,
		const CStringA& contentType, DWORD& statusCodeOut)
	{
		statusCodeOut = 0;
		URL_COMPONENTS parts = {};
		parts.dwStructSize = sizeof(parts);
		wchar_t hostBuffer[256] = {};
		wchar_t pathBuffer[2048] = {};
		wchar_t queryBuffer[4096] = {};
		parts.lpszHostName = hostBuffer;
		parts.dwHostNameLength = 255;
		parts.lpszUrlPath = pathBuffer;
		parts.dwUrlPathLength = 2047;
		parts.lpszExtraInfo = queryBuffer;
		parts.dwExtraInfoLength = 4095;
		if (!WinHttpCrackUrl(presignedUrl.GetString(), static_cast<DWORD>(presignedUrl.GetLength()), 0, &parts))
		{
			LogClient(L"presigned URL không parse được");
			return false;
		}
		if (parts.nScheme != INTERNET_SCHEME_HTTPS)
		{
			LogClient(L"presigned URL không phải HTTPS — từ chối");
			return false;
		}

		HttpHandle session;
		session.h = WinHttpOpen(USER_AGENT, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session.h) return false;
		WinHttpSetTimeouts(session.h, 10000, 15000, 120000, 60000);

		HttpHandle connect;
		connect.h = WinHttpConnect(session.h, hostBuffer, INTERNET_DEFAULT_HTTPS_PORT, 0);
		if (!connect.h) return false;

		CStringW pathAndQuery = pathBuffer;
		if (parts.dwExtraInfoLength > 0 && queryBuffer[0])
		{
			pathAndQuery += queryBuffer;
		}

		HttpHandle request;
		request.h = WinHttpOpenRequest(connect.h, L"PUT", pathAndQuery, NULL, WINHTTP_NO_REFERER,
			WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
		if (!request.h) return false;

		CStringW contentTypeHeader = L"Content-Type: " + Utf8ToWide(contentType);
		if (!WinHttpAddRequestHeaders(request.h, contentTypeHeader.GetString(),
			static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD))
		{
			return false;
		}

		if (!WinHttpSendRequest(request.h, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
			const_cast<LPBYTE>(bytes.data()), static_cast<DWORD>(bytes.size()),
			static_cast<DWORD>(bytes.size()), 0))
		{
			return false;
		}
		if (!WinHttpReceiveResponse(request.h, NULL))
		{
			return false;
		}

		DWORD statusCode = 0;
		DWORD statusSize = sizeof(statusCode);
		WinHttpQueryHeaders(request.h, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
		statusCodeOut = statusCode;
		return statusCode >= 200 && statusCode < 300;
	}

	// --- Response shapes ---

	struct SessionResponse
	{
		bool ok = false;
		CStringA error;			// error code khi server từ chối
		CStringW sessionId;
		CStringW sessionExpiresAt;
		CStringA markdownPresignedUrl;
		struct Asset
		{
			CStringA canonicalPath;
			CStringA presignedUrl;
		};
		std::vector<Asset> assets;
	};

	SessionResponse ParseSessionResponse(const CStringA& body)
	{
		SessionResponse out;
		int errorPos = JsonValuePos(body, "error");
		if (errorPos >= 0)
		{
			out.error = JsonStringAt(body, errorPos);
			return out;
		}
		int sessionIdPos = JsonValuePos(body, "sessionId");
		if (sessionIdPos < 0) return out;
		out.sessionId = Utf8ToWide(JsonStringAt(body, sessionIdPos));
		if (out.sessionId.IsEmpty()) return out;

		int expiresPos = JsonValuePos(body, "expiresAt");
		if (expiresPos >= 0) out.sessionExpiresAt = Utf8ToWide(JsonStringAt(body, expiresPos));

		int markdownPos = JsonValuePos(body, "markdown");
		CStringA markdownObject = JsonBalancedAt(body, markdownPos);
		if (markdownObject.IsEmpty()) return out;
		int urlPos = JsonValuePos(markdownObject, "presignedUrl");
		if (urlPos < 0) return out;
		out.markdownPresignedUrl = JsonStringAt(markdownObject, urlPos);
		if (out.markdownPresignedUrl.IsEmpty()) return out;

		int assetsPos = JsonValuePos(body, "assets");
		CStringA assetsArray = JsonBalancedAt(body, assetsPos);
		for (const CStringA& item : JsonArrayObjects(assetsArray))
		{
			int pathPos = JsonValuePos(item, "canonicalPath");
			int itemUrlPos = JsonValuePos(item, "presignedUrl");
			if (pathPos < 0 || itemUrlPos < 0) continue;
			SessionResponse::Asset asset;
			asset.canonicalPath = JsonStringAt(item, pathPos);
			asset.presignedUrl = JsonStringAt(item, itemUrlPos);
			out.assets.push_back(asset);
		}
		out.ok = true;
		return out;
	}

	struct FinalizeResponse
	{
		bool networkOk = false;
		DWORD statusCode = 0;
		bool ok = false;
		CStringA error;
		CStringW viewerUrl;
		CStringW expiresAt;
	};

	FinalizeResponse ParseFinalizeResponse(const HttpRequestResult& http)
	{
		FinalizeResponse out;
		out.networkOk = http.networkOk;
		out.statusCode = http.statusCode;
		if (!http.networkOk) return out;
		int viewerPos = JsonValuePos(http.body, "viewerUrl");
		if (viewerPos >= 0)
		{
			out.viewerUrl = Utf8ToWide(JsonStringAt(http.body, viewerPos));
			int expiresPos = JsonValuePos(http.body, "expiresAt");
			if (expiresPos >= 0) out.expiresAt = Utf8ToWide(JsonStringAt(http.body, expiresPos));
			out.ok = !out.viewerUrl.IsEmpty();
		}
		else
		{
			int errorPos = JsonValuePos(http.body, "error");
			if (errorPos >= 0) out.error = JsonStringAt(http.body, errorPos);
		}
		return out;
	}

	// Viewer URL phải là relative "/clip/<token>" — reject absolute/route khác
	// trước khi ghép origin cố định (đề phòng server/replay trả URL lạ).
	bool IsValidClipViewerPath(const CStringW& viewerUrl)
	{
		if (viewerUrl.Left(6) != L"/clip/") return false;
		CStringW token = viewerUrl.Mid(6);
		if (token.IsEmpty()) return false;
		for (int i = 0; i < token.GetLength(); i++)
		{
			wchar_t ch = token[i];
			bool ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') ||
				ch == '-' || ch == '.' || ch == '_' || ch == '~' || ch == '%';
			if (!ok) return false;
		}
		return true;
	}

	// --- Publish state machine ---

	DittoXPublishOutcome MapServerError(const CStringA& error)
	{
		DittoXPublishOutcome outcome;
		outcome.status = DittoXPublishStatus::ServerReject;
		outcome.serverError = Utf8ToWide(error);
		CStringW code = outcome.serverError;
		if (code == L"rate_limited") outcome.detail = L"Đã đạt giới hạn upload. Thử lại sau.";
		else if (code == L"concurrent_upload_limited") outcome.detail = L"Có quá nhiều upload đang chạy. Thử lại sau chốc lát.";
		else if (code == L"package_too_large") outcome.detail = L"Nội dung vượt giới hạn 25 MiB của link public.";
		else if (code == L"idempotency_conflict") outcome.detail = L"Nội dung đổi giữa chừng upload. Ctrl+Z lại để tạo link mới.";
		else if (code == L"session_expired") outcome.detail = L"Phiên upload hết hạn. Ctrl+Z lại để tạo link mới.";
		else if (code == L"unauthorized" || code == L"forbidden") outcome.detail = L"Ditto credential không đúng hoặc đã bị thu hồi.";
		else outcome.detail = L"Server từ chối nội dung (" + code + L").";
		return outcome;
	}

	struct PublishContext
	{
		CStringA bearerKey;
		CStringW idempotencyKey;
		CStringW manifestHash;
		CStringW contentHash;
		std::vector<BYTE> markdownBytes;
		const DittoXClipPayload* payload = nullptr;
	};

	// POST upload-session với bounded retry (network/5xx → đúng 1 lần cùng key).
	bool CreateSession(const PublishContext& ctx, SessionResponse& sessionOut)
	{
		CStringA body;
		body.Format("{\"markdown\":{\"canonicalPath\":\"article.md\",\"size\":%d},\"assets\":[",
			static_cast<int>(ctx.markdownBytes.size()));
		for (size_t i = 0; i < ctx.payload->assets.size(); i++)
		{
			const DittoXClipAsset& asset = ctx.payload->assets[i];
			if (i > 0) body += ",";
			CStringA escapedPath, escapedMime;
			JsonEscape(asset.canonicalPath, escapedPath);
			JsonEscape(CStringW(asset.contentType), escapedMime);
			body += "{\"canonicalPath\":\"" + escapedPath + "\",\"contentType\":\"" + escapedMime + "\"";
			body.AppendFormat(",\"size\":%d}", static_cast<int>(asset.bytes.size()));
		}
		CStringA escapedKey;
		JsonEscape(ctx.idempotencyKey, escapedKey);
		body += "],\"idempotencyKey\":\"" + escapedKey + "\"}";

		for (int attempt = 0; attempt < 2; attempt++)
		{
			HttpRequestResult http = DittoXHttpPost(DITTOX_UPLOAD_SESSION_PATH, body, ctx.bearerKey);
			if (!http.networkOk)
			{
				LogClient(L"upload-session network lỗi");
				continue; // response loss → cùng key thử lại đúng 1 lần
			}
			sessionOut = ParseSessionResponse(http.body);
			if (sessionOut.ok) return true;
			if (http.statusCode >= 500 && attempt == 0) continue;
			if (!sessionOut.error.IsEmpty())
			{
				LogClient(CStringW(L"upload-session reject: ") + Utf8ToWide(sessionOut.error));
			}
			return false;
		}
		return false;
	}

	// PUT một object với retry cùng URL; 403 → báo reissue (URL presign hết hạn).
	enum class PutResult { Ok, Reissue, Failed };
	PutResult PutObject(const CStringA& presignedUrl, const std::vector<BYTE>& bytes, const CStringA& contentType)
	{
		CStringW url = Utf8ToWide(presignedUrl);
		for (int attempt = 0; attempt < 2; attempt++)
		{
			DWORD status = 0;
			bool sent = DittoXHttpPutPresigned(url, bytes, contentType, status);
			if (sent && status >= 200 && status < 300) return PutResult::Ok;
			if (!sent) { if (attempt == 0) continue; return PutResult::Failed; }
			if (status >= 500 && attempt == 0) continue;
			if (status == 403) return PutResult::Reissue;
			LogClient(L"PUT trả HTTP không mong đợi");
			return PutResult::Failed;
		}
		return PutResult::Failed;
	}

	const SessionResponse::Asset* FindAssetUrl(const SessionResponse& session, const CStringA& canonicalUtf8)
	{
		for (const SessionResponse::Asset& candidate : session.assets)
		{
			if (candidate.canonicalPath.Compare(canonicalUtf8) == 0) return &candidate;
		}
		return nullptr;
	}

	bool PutAllObjects(const PublishContext& ctx, SessionResponse& session)
	{
		PutResult markdownPut = PutObject(session.markdownPresignedUrl, ctx.markdownBytes, "text/markdown");
		if (markdownPut == PutResult::Reissue)
		{
			// Presigned URL hết hạn — reissue CÙNG key/manifest (idempotent), PUT lại.
			LogClient(L"presigned markdown hết hạn — reissue session");
			SessionResponse fresh;
			if (!CreateSession(ctx, fresh) || fresh.sessionId != session.sessionId)
			{
				LogClient(L"reissue trả session khác — bỏ");
				return false;
			}
			session = fresh;
			markdownPut = PutObject(session.markdownPresignedUrl, ctx.markdownBytes, "text/markdown");
		}
		if (markdownPut != PutResult::Ok) return false;

		for (const DittoXClipAsset& asset : ctx.payload->assets)
		{
			CStringA canonicalUtf8 = WideToUtf8(asset.canonicalPath);
			const SessionResponse::Asset* remote = FindAssetUrl(session, canonicalUtf8);
			if (!remote)
			{
				LogClient(L"session thiếu presigned URL cho asset");
				return false;
			}
			PutResult put = PutObject(remote->presignedUrl, asset.bytes, asset.contentType);
			if (put == PutResult::Reissue)
			{
				SessionResponse fresh;
				if (!CreateSession(ctx, fresh) || fresh.sessionId != session.sessionId)
				{
					return false;
				}
				session = fresh;
				remote = FindAssetUrl(session, canonicalUtf8);
				if (!remote) return false;
				put = PutObject(remote->presignedUrl, asset.bytes, asset.contentType);
			}
			if (put != PutResult::Ok) return false;
		}
		return true;
	}

	// POST finalize, tối đa 3 lần; in_progress chờ 1.5s đúng 1 lần.
	FinalizeResponse FinalizeWithRetry(const PublishContext& ctx, const CStringW& sessionId)
	{
		CStringA body;
		CStringA escapedSession, escapedKey;
		JsonEscape(sessionId, escapedSession);
		JsonEscape(ctx.idempotencyKey, escapedKey);
		body.Format("{\"sessionId\":\"%s\",\"idempotencyKey\":\"%s\"}", escapedSession.GetString(), escapedKey.GetString());

		FinalizeResponse last;
		int waitedInProgress = 0;
		for (int call = 0; call < 3; call++)
		{
			HttpRequestResult http = DittoXHttpPost(DITTOX_FINALIZE_PATH, body, ctx.bearerKey);
			FinalizeResponse parsed = ParseFinalizeResponse(http);
			last = parsed;
			if (parsed.ok) return parsed;
			if (!parsed.networkOk) continue;							// response loss → hỏi lại
			if (parsed.statusCode >= 500) continue;						// 5xx → hỏi lại
			if (parsed.error.CompareNoCase("session_in_progress") == 0 && waitedInProgress < 1)
			{
				waitedInProgress++;
				::Sleep(1500);
				continue;
			}
			break;														// terminal (404/409/410...)
		}
		return last;
	}

	// Replay finalize cho pending record từ lần chạy trước (UI chỉ gọi khi pending match).
	DittoXPublishOutcome TryPendingReplay(const PublishContext& ctx, const CStringW& sessionId, CppSQLite3DB& db)
	{
		DittoXPublishOutcome outcome;
		FinalizeResponse finalize = FinalizeWithRetry(ctx, sessionId);
		if (finalize.ok && IsValidClipViewerPath(finalize.viewerUrl) && !finalize.expiresAt.IsEmpty())
		{
			CDittoXPublicLinkCache::MarkPublished(db, ctx.idempotencyKey,
				finalize.viewerUrl, finalize.expiresAt, DittoXUtcNowIso());
			outcome.status = DittoXPublishStatus::Success;
			outcome.viewerUrl = finalize.viewerUrl;
			outcome.fullUrl = CStringW(DITTOX_SHARE_ORIGIN) + finalize.viewerUrl;
			outcome.expiresAt = finalize.expiresAt;
			return outcome;
		}
		if (!finalize.networkOk || finalize.statusCode >= 500 ||
			finalize.error.CompareNoCase("session_in_progress") == 0)
		{
			// Ambiguous: commit có thể đã xảy ra. Giữ pending, không upload mới.
			outcome.status = DittoXPublishStatus::FinalizeUnconfirmed;
			outcome.detail = L"Không xác nhận được kết quả upload. Link có thể đã tạo — Ctrl+Z lần sau sẽ thử khôi phục.";
			return outcome;
		}
		if (!finalize.error.IsEmpty())
		{
			if (finalize.error.CompareNoCase("idempotency_conflict") == 0)
			{
				// Fail-fast theo spec — không tự xoá pending (đề phòng replay khác).
				return MapServerError(finalize.error);
			}
			// Terminal (session_not_found / expired / receipt_unavailable): pending không còn giá trị.
			CDittoXPublicLinkCache::RemovePending(db, ctx.idempotencyKey);
			outcome.status = DittoXPublishStatus::PendingReplayFailed;
			outcome.detail = L"Link cũ không khôi phục được. Ctrl+Z lại để publish mới.";
			return outcome;
		}
		outcome.status = DittoXPublishStatus::InvalidResponse;
		outcome.detail = L"Server trả kết quả không mong đợi khi khôi phục link.";
		return outcome;
	}

	// --- Clipboard commit helpers ---

	bool OpenClipboardWithRetry(HWND owner)
	{
		for (int attempt = 0; attempt < CLIPBOARD_OPEN_ATTEMPTS; attempt++)
		{
			if (::OpenClipboard(owner)) return true;
			::Sleep(10);
		}
		return false;
	}

	// Snapshot mọi format hiện có vào local copies. Trả false khi có format nào
	// không đọc được — khi đó KHÔNG được mutation clipboard (abort trước).
	bool SnapshotClipboard(CClipFormats& snapshot)
	{
		snapshot.RemoveAll();
		if (!OpenClipboardWithRetry(theApp.m_MainhWnd))
		{
			LogClient(L"không mở được clipboard để snapshot");
			return false;
		}
		bool ok = true;
		UINT format = ::EnumClipboardFormats(0);
		while (format != 0 && ok)
		{
			HGLOBAL global = ::GetClipboardData(format);
			SIZE_T size = global ? ::GlobalSize(global) : 0;
			if (global && size > 0)
			{
				LPVOID data = ::GlobalLock(global);
				if (!data)
				{
					ok = false;
					break;
				}
				CClipFormat copy;
				copy.m_cfType = format;
				copy.m_hgData = NewGlobalP(data, size);
				::GlobalUnlock(global);
				if (!copy.m_hgData)
				{
					ok = false;
					break;
				}
				snapshot.Add(copy);
			}
			format = ::EnumClipboardFormats(format);
		}
		::CloseClipboard();
		if (!ok) snapshot.RemoveAll();
		return ok;
	}

	void FreeSnapshot(CClipFormats& snapshot)
	{
		for (int i = 0; i < snapshot.GetSize(); i++)
		{
			if (snapshot[i].m_hgData)
			{
				::GlobalFree(snapshot[i].m_hgData);
				snapshot[i].m_hgData = NULL;
			}
		}
	}

	bool VerifyClipboardText(HWND owner, const CStringW& expected)
	{
		if (!::OpenClipboard(owner)) return false;
		bool ok = false;
		HGLOBAL handle = ::GetClipboardData(CF_UNICODETEXT);
		if (handle)
		{
			LPVOID data = ::GlobalLock(handle);
			if (data)
			{
				ok = wcscmp(reinterpret_cast<const wchar_t*>(data), expected.GetString()) == 0;
				::GlobalUnlock(handle);
			}
		}
		::CloseClipboard();
		return ok;
	}

	// Restore snapshot sau lỗi commit; SetClipboardData thành công → clipboard sở hữu
	// HGLOBAL (gỡ khỏi snapshot); verify từng format bằng so bytes thật.
	// Trả false khi restore thiếu format hoặc bytes verify sai.
	bool RestoreClipboardSnapshot(CClipFormats& snapshot)
	{
		if (!OpenClipboardWithRetry(theApp.m_MainhWnd))
		{
			LogClient(L"restore: không mở được clipboard");
			return false;
		}
		::EmptyClipboard();
		// Marker để Ditto không tự capture nội dung restore thành clip mới.
		::SetClipboardData(theApp.m_cfIgnoreClipboard, NewGlobalP("Ignore", sizeof("Ignore")));

		for (int i = 0; i < snapshot.GetSize(); i++)
		{
			CClipFormat& format = snapshot[i];
			if (!format.m_hgData || ::GlobalSize(format.m_hgData) == 0) continue;
			if (g_clipboardFaultMode == DittoXClipboardFaultMode::FailRestore && i == 0)
			{
				// Fault injection: format đầu tiên "bị mất" khi restore (giữ HGLOBAL để free sau).
				LogClient(L"fault injection: bỏ qua format đầu khi restore");
				continue;
			}
			if (::SetClipboardData(format.m_cfType, format.m_hgData))
			{
				format.m_hgData = NULL; // clipboard sở hữu dữ liệu
			}
			else
			{
				::GlobalFree(format.m_hgData);
				format.m_hgData = NULL;
			}
		}
		::CloseClipboard();

		// Xác minh từng format snapshot: tồn tại trên clipboard + đúng size + đúng bytes.
		if (!::OpenClipboard(theApp.m_MainhWnd)) return false;
		bool allVerified = true;
		for (int i = 0; i < snapshot.GetSize(); i++)
		{
			CClipFormat& format = snapshot[i];
			if (!format.m_hgData || ::GlobalSize(format.m_hgData) == 0) continue;
			HGLOBAL handle = ::GetClipboardData(format.m_cfType);
			if (!handle)
			{
				allVerified = false;
				break;
			}
			SIZE_T expectedSize = ::GlobalSize(format.m_hgData);
			if (::GlobalSize(handle) != expectedSize)
			{
				allVerified = false;
				break;
			}
			LPVOID expectedData = ::GlobalLock(format.m_hgData);
			LPVOID actualData = ::GlobalLock(handle);
			if (!expectedData || !actualData)
			{
				if (expectedData) ::GlobalUnlock(format.m_hgData);
				if (actualData) ::GlobalUnlock(handle);
				allVerified = false;
				break;
			}
			if (memcmp(expectedData, actualData, expectedSize) != 0)
			{
				allVerified = false;
			}
			::GlobalUnlock(format.m_hgData);
			::GlobalUnlock(handle);
			if (!allVerified) break;
		}
		::CloseClipboard();
		return allVerified;
	}
}

void DittoXSetClipboardFaultInjection(DittoXClipboardFaultMode mode)
{
	g_clipboardFaultMode = mode;
}

// Forward declarations — định nghĩa ở cuối file, dùng ở phía trên.
CStringW DittoXEpochMsToIso(long long epochMs);
long long DittoXSystemTimeToEpochMs(const SYSTEMTIME& st);

// --- Hash / thời gian (public) ---

CStringW DittoXSha256Hex(const BYTE* data, size_t length)
{
	bool ok = false;
	BCryptHasher hasher(ok);
	CStringW hex;
	if (!ok || !hasher.Update(data, length) || !hasher.Finish(hex))
	{
		LogClient(L"SHA-256 lỗi");
		return L"";
	}
	return hex;
}

CStringW DittoXSha256Hex(const std::vector<BYTE>& bytes)
{
	return DittoXSha256Hex(bytes.data(), bytes.size());
}

long long DittoXSystemTimeToEpochMs(const SYSTEMTIME& st)
{
	return SystemTimeToEpochMs(st);
}

long long DittoXUtcNowEpochMs()
{
	SYSTEMTIME st;
	::GetSystemTime(&st);
	return SystemTimeToEpochMs(st);
}

CStringW DittoXUtcNowIso()
{
	return DittoXEpochMsToIso(DittoXUtcNowEpochMs());
}

CStringW DittoXEpochMsToIso(long long epochMs)
{
	// Đổi epoch ms → UTC ISO "YYYY-MM-DDTHH:MM:SSZ" (bỏ millis để cache so sánh gọn).
	long long days = epochMs / 86400000LL;
	long long rem = epochMs % 86400000LL;
	if (rem < 0) { rem += 86400000LL; days -= 1; }

	long long jdn = days + 2440588;
	long long a = jdn + 32044;
	long long b = (4 * a + 3) / 146097;
	long long c = a - (146097 * b) / 4;
	long long d = (4 * c + 3) / 1461;
	long long e = c - (1461 * d) / 4;
	long long m = (5 * e + 2) / 153;
	long long day = e - (153 * m + 2) / 5 + 1;
	long long month = m + 3 - 12 * (m / 10);
	long long year = 100 * b + d - 4800 + m / 10;

	int hours = static_cast<int>(rem / 3600000LL);
	int minutes = static_cast<int>((rem / 60000LL) % 60);
	int seconds = static_cast<int>((rem / 1000LL) % 60);

	CStringW iso;
	iso.Format(L"%04lld-%02lld-%02lldT%02d:%02d:%02dZ", year, month, day, hours, minutes, seconds);
	return iso;
}

long long DittoXIsoToEpochMs(const CStringW& iso)
{
	// Chấp nhận "YYYY-MM-DDTHH:MM:SS(.mmm)Z" (server trả có/không millis).
	int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0, millis = 0;
	swscanf_s(iso.GetString(), L"%4d-%2d-%2dT%2d:%2d:%2d.%3d",
		&year, &month, &day, &hour, &minute, &second, &millis);
	if (year < 1970 || month < 1 || month > 12 || day < 1 || day > 31)
	{
		return LLONG_MIN;
	}
	SYSTEMTIME st = {};
	st.wYear = static_cast<WORD>(year);
	st.wMonth = static_cast<WORD>(month);
	st.wDay = static_cast<WORD>(day);
	st.wHour = static_cast<WORD>(hour);
	st.wMinute = static_cast<WORD>(minute);
	st.wSecond = static_cast<WORD>(second);
	st.wMilliseconds = static_cast<WORD>(millis);
	return SystemTimeToEpochMs(st);
}

// --- Fingerprint ---

CStringW DittoXComputeManifestHash(const DittoXClipPayload& payload)
{
	// Assets theo canonical path order (byte compare) để manifest order ổn định.
	std::vector<const DittoXClipAsset*> ordered;
	ordered.reserve(payload.assets.size());
	for (const DittoXClipAsset& asset : payload.assets) ordered.push_back(&asset);
	for (size_t i = 1; i < ordered.size(); i++)
	{
		for (size_t j = i; j > 0 && ordered[j - 1]->canonicalPath.Compare(ordered[j]->canonicalPath) > 0; j--)
		{
			std::swap(ordered[j - 1], ordered[j]);
		}
	}

	bool ok = false;
	BCryptHasher hasher(ok);
	CStringW hex;
	if (!ok) return L"";

	static const BYTE nul = 0;
	const BYTE* versionTag = reinterpret_cast<const BYTE*>(DITTOX_FINGERPRINT_VERSION);
	if (!hasher.Update(versionTag, strlen(DITTOX_FINGERPRINT_VERSION))) return L"";
	if (!hasher.Update(&nul, 1)) return L"";
	if (!hasher.Update(reinterpret_cast<const BYTE*>(payload.markdownUtf8.GetString()),
		static_cast<size_t>(payload.markdownUtf8.GetLength()))) return L"";
	if (!hasher.Update(&nul, 1)) return L"";

	for (const DittoXClipAsset* asset : ordered)
	{
		CStringA pathUtf8 = WideToUtf8(asset->canonicalPath);
		if (!hasher.Update(reinterpret_cast<const BYTE*>(pathUtf8.GetString()),
			static_cast<size_t>(pathUtf8.GetLength()))) return L"";
		if (!hasher.Update(&nul, 1)) return L"";
		if (!hasher.Update(reinterpret_cast<const BYTE*>(asset->contentType.GetString()),
			static_cast<size_t>(asset->contentType.GetLength()))) return L"";
		if (!hasher.Update(&nul, 1)) return L"";
		CStringW assetHash = DittoXSha256Hex(asset->bytes);
		if (assetHash.IsEmpty()) return L"";
		CStringA assetHashUtf8 = WideToUtf8(assetHash);
		if (!hasher.Update(reinterpret_cast<const BYTE*>(assetHashUtf8.GetString()),
			static_cast<size_t>(assetHashUtf8.GetLength()))) return L"";
		if (!hasher.Update(&nul, 1)) return L"";
	}

	if (!hasher.Finish(hex)) return L"";
	return hex;
}

CStringW DittoXComputeContentHash(const DittoXClipPayload& payload)
{
	return DittoXSha256Hex(reinterpret_cast<const BYTE*>(payload.markdownUtf8.GetString()),
		static_cast<size_t>(payload.markdownUtf8.GetLength()));
}

// --- Payload build ---

bool DittoXBuildPayloadFromClip(CClip& clip, DittoXClipPayload& payload)
{
	payload.assets.clear();
	payload.markdownUtf8.Empty();

	CClipFormat* bitmap = clip.m_Formats.FindFormat(CF_DIB);
	CClipFormat* png = clip.m_Formats.FindFormat(theApp.m_PNG_Format);

	if (bitmap || png)
	{
		// Ảnh thắng text như Ctrl+X: encode PNG trong memory, không tạo file tạm.
		std::shared_ptr<CImage> image = png
			? PNGImageHelper::CImageFromHGLOBAL(png->m_hgData)
			: DIBImageHelper::CImageFromHGLOBAL(bitmap->m_hgData);
		if (!image)
		{
			LogClient(L"không decode được ảnh từ clip");
			return false;
		}
		std::vector<BYTE> pngBytes = CImageToPNGBytes(*image, Gdiplus::ImageFormatPNG);
		if (pngBytes.empty())
		{
			LogClient(L"encode PNG trong memory lỗi");
			return false;
		}
		DittoXClipAsset asset;
		asset.canonicalPath = L"img/clip-0.png";
		asset.contentType = "image/png";
		asset.bytes.swap(pngBytes);
		payload.assets.push_back(asset);
		payload.markdownUtf8 = "![clip](img/clip-0.png)\n";
		return true;
	}

	if (clip.ContainsClipFormat(CF_UNICODETEXT))
	{
		payload.markdownUtf8 = CTextConvert::UnicodeToUTF8(clip.GetUnicodeTextFormat());
		return true;
	}
	if (clip.ContainsClipFormat(CF_TEXT))
	{
		payload.markdownUtf8 = CTextConvert::UnicodeToUTF8(CTextConvert::AnsiToUnicode(clip.GetCFTextTextFormat()));
		return true;
	}

	return false;
}

bool DittoXValidatePayloadLimits(const DittoXClipPayload& payload, CStringW& detailOut)
{
	if (static_cast<DWORD>(payload.markdownUtf8.GetLength()) > DITTOX_MAX_MARKDOWN_BYTES)
	{
		detailOut = L"Nội dung vượt giới hạn 2 MiB của link public.";
		return false;
	}
	DWORD total = static_cast<DWORD>(payload.markdownUtf8.GetLength());
	for (const DittoXClipAsset& asset : payload.assets)
	{
		if (static_cast<DWORD>(asset.bytes.size()) > DITTOX_MAX_ASSET_BYTES)
		{
			detailOut = L"Ảnh vượt giới hạn 10 MiB của link public.";
			return false;
		}
		total += static_cast<DWORD>(asset.bytes.size());
	}
	if (total > DITTOX_MAX_PACKAGE_BYTES)
	{
		detailOut = L"Nội dung vượt giới hạn 25 MiB của link public.";
		return false;
	}
	return true;
}

// --- Publish flow (worker thread only) ---

DittoXPublishOutcome DittoXPublish(const DittoXClipPayload& payload)
{
	DittoXPublishOutcome outcome;

	CStringW detail;
	if (!DittoXValidatePayloadLimits(payload, detail))
	{
		outcome.status = DittoXPublishStatus::TooLarge;
		outcome.detail = detail;
		return outcome;
	}

	// Connection SQLite riêng cho thread này — không đụng theApp.m_db của UI.
	CppSQLite3DB db;
	if (!CDittoXPublicLinkCache::OpenConnection(db))
	{
		outcome.status = DittoXPublishStatus::Network;
		outcome.detail = L"Không mở được database cache của Ditto.";
		return outcome;
	}

	PublishContext ctx;
	ctx.payload = &payload;
	ctx.markdownBytes.assign(reinterpret_cast<const BYTE*>(payload.markdownUtf8.GetString()),
		reinterpret_cast<const BYTE*>(payload.markdownUtf8.GetString()) + payload.markdownUtf8.GetLength());
	ctx.manifestHash = DittoXComputeManifestHash(payload);
	ctx.contentHash = DittoXComputeContentHash(payload);
	if (ctx.manifestHash.IsEmpty() || ctx.contentHash.IsEmpty())
	{
		outcome.status = DittoXPublishStatus::Network;
		outcome.detail = L"Không tính được fingerprint nội dung.";
		return outcome;
	}

	// Credential đọc TRƯỚC khi tạo request nào (fail closed, không fallback anonymous).
	CStringW credential;
	if (!CDittoXCredentialStore::ReadCredential(credential))
	{
		outcome.status = DittoXPublishStatus::CredentialMissing;
		outcome.detail = L"Chưa cấu hình Ditto credential. Chạy tools\\dittox-share-credential-setup.ps1 rồi thử lại.";
		return outcome;
	}
	if (!SanitizeCredential(credential, ctx.bearerKey))
	{
		outcome.status = DittoXPublishStatus::CredentialMissing;
		outcome.detail = L"Ditto credential không hợp lệ. Chạy lại script cấu hình credential.";
		return outcome;
	}

	CStringW nowIso = DittoXUtcNowIso();

	// Cache reuse: cùng fingerprint + expiresAt > now → copy URL, không upload.
	CStringW cachedUrl, cachedExpiry;
	if (CDittoXPublicLinkCache::FindReusable(db, ctx.manifestHash, nowIso, cachedUrl, cachedExpiry))
	{
		CDittoXPublicLinkCache::Touch(db, ctx.manifestHash, nowIso);
		outcome.status = DittoXPublishStatus::ReusedLink;
		outcome.viewerUrl = cachedUrl;
		outcome.fullUrl = CStringW(DITTOX_SHARE_ORIGIN) + cachedUrl;
		outcome.expiresAt = cachedExpiry;
		return outcome;
	}

	CDittoXPublicLinkCache::PruneExpired(db, nowIso);

	// Pending từ lần chạy trước cùng fingerprint → replay finalize TRƯỚC, không upload mới.
	CStringW pendingSession, pendingKey;
	if (CDittoXPublicLinkCache::FindPending(db, ctx.manifestHash, pendingSession, pendingKey))
	{
		ctx.idempotencyKey = pendingKey;
		return TryPendingReplay(ctx, pendingSession, db);
	}

	// Publish mới: idempotency key là random UUID mỗi publish, KHÔNG derive từ content.
	ctx.idempotencyKey = NewIdempotencyKey();
	if (ctx.idempotencyKey.IsEmpty())
	{
		outcome.status = DittoXPublishStatus::Network;
		outcome.detail = L"Không tạo được idempotency key.";
		return outcome;
	}

	SessionResponse session;
	if (!CreateSession(ctx, session))
	{
		if (!session.error.IsEmpty())
		{
			return MapServerError(session.error);
		}
		outcome.status = DittoXPublishStatus::Network;
		outcome.detail = L"Không kết nối được share.quoc.app. Kiểm tra mạng rồi Ctrl+Z lại.";
		return outcome;
	}

	// Persist pending TRƯỚC PUT — crash/restart giữa chừng vẫn replay được.
	CDittoXPublicLinkCache::SavePending(db, ctx.manifestHash, ctx.contentHash,
		ctx.idempotencyKey, session.sessionId, DittoXUtcNowIso());

	if (!PutAllObjects(ctx, session))
	{
		// PUT fail chắc chắn (chưa commit) → xoá pending để lần sau upload lại từ đầu.
		CDittoXPublicLinkCache::RemovePending(db, ctx.idempotencyKey);
		outcome.status = DittoXPublishStatus::Network;
		outcome.detail = L"Upload nội dung lên server lỗi. Ctrl+Z lại để thử.";
		return outcome;
	}

	FinalizeResponse finalize = FinalizeWithRetry(ctx, session.sessionId);
	if (finalize.ok && IsValidClipViewerPath(finalize.viewerUrl) && !finalize.expiresAt.IsEmpty())
	{
		CDittoXPublicLinkCache::MarkPublished(db, ctx.idempotencyKey,
			finalize.viewerUrl, finalize.expiresAt, DittoXUtcNowIso());
		outcome.status = DittoXPublishStatus::Success;
		outcome.viewerUrl = finalize.viewerUrl;
		outcome.fullUrl = CStringW(DITTOX_SHARE_ORIGIN) + finalize.viewerUrl;
		outcome.expiresAt = finalize.expiresAt;
		return outcome;
	}
	if (!finalize.networkOk || finalize.statusCode >= 500 ||
		finalize.error.CompareNoCase("session_in_progress") == 0)
	{
		// Ambiguous — giữ pending record; lần Ctrl+Z sau sẽ replay trước.
		outcome.status = DittoXPublishStatus::FinalizeUnconfirmed;
		outcome.detail = L"Không xác nhận được kết quả upload. Link có thể đã tạo — Ctrl+Z lần sau sẽ thử khôi phục.";
		return outcome;
	}
	if (!finalize.error.IsEmpty())
	{
		CDittoXPublicLinkCache::RemovePending(db, ctx.idempotencyKey);
		return MapServerError(finalize.error);
	}
	CDittoXPublicLinkCache::RemovePending(db, ctx.idempotencyKey);
	outcome.status = DittoXPublishStatus::InvalidResponse;
	outcome.detail = L"Server trả link không mong đợi. Thử lại.";
	return outcome;
}

// --- Clipboard commit (UI thread only) ---

DittoXClipboardCommitResult DittoXCommitUrlToClipboard(const CStringW& url)
{
	// Snapshot TRƯỚC mọi mutation; fail → abort, không đụng clipboard hiện tại.
	CClipFormats snapshot;
	if (!SnapshotClipboard(snapshot))
	{
		return DittoXClipboardCommitResult::SnapshotFailed;
	}

	if (!OpenClipboardWithRetry(theApp.m_MainhWnd))
	{
		FreeSnapshot(snapshot);
		return DittoXClipboardCommitResult::SnapshotFailed;
	}
	::EmptyClipboard();

	HGLOBAL marker = NewGlobalP("Ignore", sizeof("Ignore"));
	if (!::SetClipboardData(theApp.m_cfIgnoreClipboard, marker))
	{
		::GlobalFree(marker);
		LogClient(L"cảnh báo: không set được ignore marker");
	}

	SIZE_T urlBytes = static_cast<SIZE_T>(url.GetLength() + 1) * sizeof(wchar_t);
	HGLOBAL urlGlobal = ::GlobalAlloc(GMEM_MOVEABLE, urlBytes);
	bool urlSet = false;
	if (urlGlobal)
	{
		LPVOID data = ::GlobalLock(urlGlobal);
		if (data)
		{
			wcscpy_s(reinterpret_cast<wchar_t*>(data), url.GetLength() + 1, url.GetString());
			::GlobalUnlock(urlGlobal);
			if (g_clipboardFaultMode == DittoXClipboardFaultMode::FailSetUrl)
			{
				// Fault injection: giả lập SetClipboardData lỗi sau khi đã EmptyClipboard.
				LogClient(L"fault injection: bỏ qua SetClipboardData(URL)");
				::GlobalFree(urlGlobal);
			}
			else
			{
				urlSet = ::SetClipboardData(CF_UNICODETEXT, urlGlobal) != NULL;
				if (!urlSet) ::GlobalFree(urlGlobal);
			}
		}
		else
		{
			::GlobalFree(urlGlobal);
		}
	}
	::CloseClipboard();

	if (urlSet)
	{
		urlSet = VerifyClipboardText(theApp.m_MainhWnd, url);
	}

	if (urlSet)
	{
		FreeSnapshot(snapshot);
		return DittoXClipboardCommitResult::Committed;
	}

	// Commit lỗi sau khi đã EmptyClipboard → restore snapshot + verify từng format.
	LogClient(L"set URL vào clipboard lỗi — restore snapshot");
	if (!RestoreClipboardSnapshot(snapshot))
	{
		FreeSnapshot(snapshot);
		return DittoXClipboardCommitResult::RestoreFailed;
	}
	FreeSnapshot(snapshot);
	return DittoXClipboardCommitResult::CommitFailed;
}
